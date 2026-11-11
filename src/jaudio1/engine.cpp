// SPDX-License-Identifier: MIT

// JAudio1's engine for one piece: its start, the game's frames, the tracks' requests to the game's code and the
// sub-frames. engine_effects.cpp contains the effects, and sound_files.cpp the sound files. The three files build
// as one translation unit. The compiler can then inline the small helpers across them.

#include "ntdlib/jaudio1/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "jaudio1/engine_internal.h"
#include "ntdlib/jaudio1/channel.h"
#include "ntdlib/jaudio1/conductor.h"
#include "ntdlib/jaudio1/oscillator.h"
#include "ntdlib/jaudio1/track.h"
#include "ntdlib/jaudio2/dsp.h"

namespace ntdlib::jaudio1
{

namespace
{

// The order in which a port command sets its levels: each flag of the command, and the outer parameter that it sets.
constexpr std::array<std::pair<uint8_t, uint16_t>, 6> kPortOrder = {{
    {kPortVolume, OuterParam::kVolume},
    {kPortPitch, OuterParam::kPitch},
    {kPortPan, OuterParam::kPan},
    {kPortFxmix, OuterParam::kFxmix},
    {kPortTempo, OuterParam::kTempo},
    {kPortDolby, OuterParam::kDolby},
}};

// The levels that outerInit switches on. A piece's kept levels use the same bits.
constexpr std::array<uint16_t, 5> kOuterLevels = {OuterParam::kVolume, OuterParam::kPitch, OuterParam::kFxmix,
                                                  OuterParam::kPan, OuterParam::kDolby};

// BgmSeq::setConfigVolume gives line 3 of a piece's volume.
constexpr float kMusicLevel = 0.8f;

// JAI's work for a piece runs once in each of the game's frames. The model takes 30 frames a second.
constexpr double kGameFrameRate = 30.0;

// The registers of a track that hold the game's handle of the track's object (PSSystem::setObject). The model writes
// 0 for the handle.
constexpr uint32_t kObjectRegister = 18;

// The requests of the tracks to the game's code (PSGame's seqCpuSync) that change the piece.
constexpr uint32_t kSyncLevels = 0x0;
constexpr uint32_t kSyncSeLevels = 0x1;
constexpr uint32_t kSyncRootUpdate = 0x1e00;
constexpr uint32_t kSyncChildUpdate = 0x1f00;
constexpr uint32_t kSyncBeat = 0x2e00;
constexpr uint32_t kSyncStartPoint = 0x4000;
constexpr uint32_t kSyncJump = 0x4001;
constexpr uint32_t kSyncRootInit = 0xe00;
constexpr uint32_t kSyncChildInit = 0xf00;

// A request that the conductor has answered. No track's request has this value: requests are 16 bits.
constexpr uint32_t kSyncConducted = 0x10000;
constexpr uint16_t kNoJump = 0xffff;

// The requests of the horns' tunes (PSGame::Rappa, one for each captain): the horn's place in its tune, and the wait
// after each note. The game's start makes the horns, at place 0. The wait comes from the tilt of the stick: 3 ticks
// (Rappa::cBaseWaitTime) at full tilt. The horn's place is the track's port 11 at the last wait.
constexpr uint32_t kSyncHornPlace = 0x3000;
constexpr uint32_t kSyncHornWait = 0x3001;
constexpr uint32_t kSyncSecondHornPlace = 0x3002;
constexpr uint32_t kSyncSecondHornWait = 0x3003;
constexpr uint32_t kHornPlacePort = 11;
constexpr uint16_t kHornWait = 3;

// The request for the story's flag of the paid debt (Game::playData). The model plays the story's start: the debt
// unpaid.
constexpr uint32_t kSyncDebtPaid = 0x5000;

// TrackOnDirectorBase::onPlayInit: a muted layer's pause status. Its notes stop as it mutes, and none start.
constexpr uint8_t kMutedLayerStatus = 0x60;

// Whether an oscillator keeps its level or repeats a cycle. It keeps its level at its sustain, and from the note's
// start when it has no attack table. The game's step then gives the full scale and leaves the state as it is
// (JASOscillator::incCounter). A vibrato and a tremolo repeat a part of their tables (Oscillator::Cycling).
bool OscillatorHeld(const Oscillator& oscillator)
{
    const uint8_t state = oscillator.State().state;
    return state == Oscillator::kHold || (state == Oscillator::kStart && oscillator.Data()->attack.Null()) ||
           oscillator.Cycling();
}

// Whether a channel holds its note: it plays on a looping wave, and each of its envelopes keeps its level or repeats a
// cycle.
bool NoteHeld(const Channel& channel)
{
    if (!channel.alive || channel.status != Channel::kPlay ||
        (channel.wave_format == Channel::kWave && channel.wave.loop_flags == 0))
    {
        return false;
    }

    return std::all_of(channel.oscillators.begin(), channel.oscillators.end(),
                       [](const Oscillator& oscillator) { return !oscillator.Valid() || OscillatorHeld(oscillator); });
}

// Whether a track has opened a track that is still open. A track that closes leaves its parent's slot.
bool HasChildren(const Tracks& tracks, uint32_t index)
{
    const Track& track = tracks.TrackAt(index);
    return std::any_of(track.children.begin(), track.children.end(),
                       [](uint32_t child) { return child != Track::kNone; });
}

// The port command's request for an interrupt of its track (JASPortArgs's flag 0x40), and the interrupt that stops an
// effect's slot track.
constexpr uint8_t kPortInterrupt = 0x40;
constexpr uint32_t kStopInterrupt = 5;

} // namespace

Engine::Engine(const SoundFiles& files, const Settings& settings)
    : files_(files), settings_(settings), memory_(std::make_unique<DspMemory>(files.Memory().Size()))
{
    std::copy(files.Memory().Bytes().begin(), files.Memory().Bytes().end(), memory_->Bytes().begin());
}

Engine::~Engine() = default;

std::optional<std::string> Engine::Start(uint16_t id, const PieceSettings& piece)
{
    model_.reset();
    const SoundFiles::SequenceFile* file = files_.FindSequence(id);
    if (file == nullptr)
    {
        return std::string("the sequence archive has no such file");
    }

    std::unique_ptr<Conductor> conductor;
    if (piece.kind == PieceSettings::kAuto)
    {
        try
        {
            conductor = std::make_unique<Conductor>(ConductorFile::Parse(piece.conductor));
        }
        catch (const xsflib::FormatError& e)
        {
            return std::string("the conductor file can't be read: ") + e.what();
        }
    }

    // JAI's start of the piece (checkReadSeq): the volume's lines, the sequence's set-up (setSeqData), the root
    // track's levels (rootInit) and its start (startSeq). The lines multiply in their order: the piece's volume (line
    // 1), the music's level (line 3) and the music's volume of the options (line 8). The others hold 1.
    float volume = 1.0f;
    if ((files_.SequenceFormat() & kFormatVolume) != 0)
    {
        volume = volume * (static_cast<float>(piece.volume) / 127.0f);
    }
    volume = volume * kMusicLevel;
    volume = volume * settings_.music_volume;
    if (auto error = StartSequence(*file, volume))
    {
        return error;
    }

    model_->piece = piece;
    model_->conductor = std::move(conductor);

    return std::nullopt;
}

void Engine::DrawSequenceNumbers(uint32_t count)
{
    if (model_)
    {
        model_->tracks.DrawRandom(count);
    }
}

uint32_t Engine::SequenceDraws() const
{
    return model_ ? model_->tracks.RandomDraws() : 0;
}

std::optional<std::string> Engine::StartSequence(const SoundFiles::SequenceFile& file, float volume)
{
    // The memory below the waves starts again from zeros.
    std::fill(memory_->Bytes().begin(), memory_->Bytes().begin() + SoundFiles::kWaves, uint8_t{0});
    model_ = std::make_unique<Model>(*memory_);
    Model& m = *model_;

    // The DSP's start-up (DsetupTable, DsetVARAM), and the FX lines of the first FX scene (JASDsp::setFXLine).
    m.dsp.SetFxLayout(jaudio2::Dsp::FxLayout::kJaudio1);
    m.dsp.Setup(static_cast<uint16_t>(Voices::kDspChannels), kBlocks, kFxSettings);
    m.dsp.SetVaram(0);
    for (std::size_t line = 0; line < files_.FxLines().size(); line++)
    {
        const FxLine& fx = files_.FxLines()[line];
        if (fx.buffer_subframes == 0)
        {
            continue;
        }

        const uint32_t at = kFxSettings + static_cast<uint32_t>(line) * kFxLineBytes;
        const uint32_t buffer = kFxBuffers + static_cast<uint32_t>(line) * kFxBufferBytes;
        jaudio2::WriteFxLine<jaudio2::Dsp::FxLayout::kJaudio1>(*memory_, at, buffer, fx.subframes, fx);
    }

    m.voices.SetChannelLevel(settings_.channel_level);
    m.voices.SetAutoLevel(settings_.auto_level);
    for (uint32_t i = 0; i < files_.Banks().size(); i++)
    {
        m.tracks.SetBank(i, files_.Banks()[i].get());
        m.tracks.SetVirtualBank(files_.Banks()[i]->Number(), i);
    }
    m.tracks.InstGenerator() = files_.InstGenerator();
    m.tracks.SetSyncCallback([this](uint32_t track, uint32_t value) { return Sync(track, value); });
    if (loop_hook_)
    {
        m.tracks.SetLoopHook(loop_hook_);
    }

    m.volume = volume;
    m.root = m.tracks.StartSequence(*file.sequence, &m.root_outer);
    if (m.root == Track::kNone)
    {
        model_.reset();
        return std::string("no root track is free");
    }
    OuterInit(kRootNumber, 0xffff);

    // The sequence starts in one of the game's frames. JAI's next frame falls due a frame later.
    m.frame_subframes = m.voices.DacRate() / jaudio2::Dsp::kSubframeSamples / kGameFrameRate;
    m.next_frame = m.frame_subframes;
    output_.assign(2 * jaudio2::Dsp::kSubframeSamples, int16_t{0});

    return std::nullopt;
}

uint32_t Engine::JaiTrack(uint32_t number) const
{
    // JAInter::SystemInterface::trackToSeqp: the root track, or the root's child in the slot. The effect sequence's ID
    // has the flag 0x800: its numbers name the children of the root's children.
    const Model& m = *model_;
    if (number == kRootNumber)
    {
        return m.root;
    }

    const Track& root = m.tracks.TrackAt(m.root);
    if (!m.effect)
    {
        return root.children[number & 0xf];
    }

    const uint32_t child = root.children[(number >> 4) & 0xf];
    return child == Track::kNone ? Track::kNone : m.tracks.TrackAt(child).children[number & 0xf];
}

void Engine::OuterInit(uint32_t number, uint16_t kept)
{
    // JAInter::SystemInterface::outerInit. The track's levels switch on at once. Each level that `kept` leaves out
    // drops to 0 at once. The port command then sets the levels at the next sub-frame's start. A child also takes its
    // mute from JAI's mute bits, which stay off while the game leaves the piece alone.
    Model& m = *model_;
    const uint32_t index = JaiTrack(number);
    if (index == Track::kNone)
    {
        return;
    }

    Model::PortArgs& args = m.ports[number];
    OuterParam* outer = m.tracks.TrackAt(index).outer;
    if (number == kRootNumber)
    {
        // SeqUpdateData's levels for the sequence: the volume's product, and the defaults of the others.
        args.volume = m.volume;
        args.pitch = 1.0f;
        args.fxmix = 0.0f;
        args.pan = 0.5f;
        args.dolby = 0.0f;
        args.tempo = 1.0f;
        args.flags = 0xff;
        outer->OnSwitch(OuterParam::kTempo);
    }
    else
    {
        // JAISequence's levels for the child: 1 for the volume and the pitch, 0.5 for the pan and 0 for the others. An
        // AutoBgm piece's start gives each of the conductor's tracks its effect send.
        args.volume = 1.0f;
        args.pitch = 1.0f;
        args.fxmix = m.conductor ? m.conductor->Fxmix(number).value_or(0.0f) : 0.0f;
        args.pan = 0.5f;
        args.dolby = 0.0f;
        args.interrupt = false;
        args.flags = 0x7f;
        m.tracks.MuteTrack(index, false);
    }

    for (const uint16_t level : kOuterLevels)
    {
        outer->OnSwitch(level);
    }
    for (const uint16_t level : kOuterLevels)
    {
        if ((kept & level) == 0)
        {
            outer->SetParam(static_cast<uint8_t>(level), 0.0f);
        }
    }

    if (!args.queued)
    {
        args.queued = true;
        m.port_queue.push_back(number);
    }
}

void Engine::RunPortCommands()
{
    // JASPortCmd::execAllCommand runs each command in the order that it joined the list. A command finds its track
    // again by its number, and does nothing if the track has gone (setSePortParameter).
    Model& m = *model_;
    for (const uint32_t number : m.port_queue)
    {
        Model::PortArgs& args = m.ports[number];
        args.queued = false;
        const uint32_t index = JaiTrack(number);
        if (index == Track::kNone)
        {
            continue;
        }

        OuterParam* outer = m.tracks.TrackAt(index).outer;
        const std::array<float, 6> values = {args.volume, args.pitch, args.pan, args.fxmix, args.tempo, args.dolby};
        for (std::size_t i = 0; i < kPortOrder.size(); i++)
        {
            if ((args.flags & kPortOrder[i].first) != 0)
            {
                outer->SetParam(static_cast<uint8_t>(kPortOrder[i].second), values[i]);
                args.flags = static_cast<uint8_t>(args.flags ^ kPortOrder[i].first);
            }
        }

        // The request for the stop keeps its flag.
        if ((args.flags & kPortInterrupt) != 0 && args.interrupt)
        {
            m.tracks.SetInterrupt(index, kStopInterrupt);
        }
    }
    m.port_queue.clear();
}

void Engine::GameFrame()
{
    // checkPlayingSeq: the stop of a released effect's slot track (the interrupt switch), by a port command for a
    // track that has asked for its levels.
    Model& m = *model_;
    if (m.effect && m.started && m.effect->stop)
    {
        m.effect->stop = false;
        Model::PortArgs& args = m.ports[m.effect->slot];
        args.interrupt = true;
        if ((m.asked & (1u << m.effect->slot)) != 0)
        {
            args.flags = kPortInterrupt;
            if (!args.queued)
            {
                args.queued = true;
                m.port_queue.push_back(m.effect->slot);
            }
        }
    }

    // checkStartedSeq. Once the root track plays and has opened a child, JAI gives each track that it numbers and that
    // hasn't asked for its levels its levels (trackInit). JAI's other work changes nothing in a piece that the game
    // leaves alone. Neither does the directors' work (DirectorMgrBase::exec).
    if (m.started || !m.tracks.Playing(m.root) || !HasChildren(m.tracks, m.root))
    {
        return;
    }

    m.started = true;
    const uint32_t numbers = m.effect ? kRootNumber : kChildNumbers;
    for (uint32_t number = 0; number < numbers; number++)
    {
        if ((m.asked & (1u << number)) == 0)
        {
            OuterInit(number, 0xffff);
        }
    }
}

uint16_t Engine::Sync(uint32_t index, uint32_t value)
{
    // PSGame's seqCpuSync, for a piece whose handle the game holds.
    Model& m = *model_;
    const bool directed = m.piece.kind != PieceSettings::kBgm;
    const bool jump = m.piece.kind == PieceSettings::kJump;
    uint16_t answer = 0;
    const std::optional<uint16_t> conducted = m.conductor ? m.conductor->Sync(m.tracks, index, value) : std::nullopt;
    switch (conducted ? kSyncConducted : value)
    {
    case kSyncConducted:
        answer = *conducted;
        break;

    case kSyncLevels:
        SyncLevels(index);
        break;

    case kSyncRootUpdate:
    case kSyncChildUpdate:
        // The update of the track's object (SeqTrackBase::update) runs the directors' tasks. A piece that the game
        // leaves alone has none. A piece without objects has no object to update.
        if (!directed)
        {
            m.unmodelled++;
        }
        break;

    case kSyncBeat:
        answer = SyncBeat(directed);
        break;

    case kSyncStartPoint:
        // JumpBgmSeq::getSeqStartPoint.
        if (!jump)
        {
            m.unmodelled++;
            break;
        }
        answer = m.piece.start_point;
        break;

    case kSyncJump:
        // JumpBgmSeq::outputJumpRequest: no request.
        if (!jump)
        {
            m.unmodelled++;
            break;
        }
        answer = kNoJump;
        break;

    case kSyncRootInit:
    case kSyncChildInit:
        SyncObjectInit(index, value, directed);
        break;

    case kSyncHornPlace:
    case kSyncSecondHornPlace:
        answer = m.horn_places[value == kSyncHornPlace ? 0 : 1];
        break;

    case kSyncHornWait:
    case kSyncSecondHornWait:
        m.horn_places[value == kSyncHornWait ? 0 : 1] = m.tracks.ReadPortAppDirect(index, kHornPlacePort);
        answer = kHornWait;
        break;

    case kSyncDebtPaid:
        answer = 0;
        break;

    case kSyncSeLevels:
        SyncSeLevels(index);
        break;

    default:
        // The game's other code. Any other request changes nothing and gets 0. A request that reaches the cave
        // music's conductor (PSAutoBgm) from a piece without one counts as a step that the model leaves out.
        if (Conductor::TakesRequest(value))
        {
            m.unmodelled++;
        }
        break;
    }

    return answer;
}

void Engine::SyncLevels(uint32_t index)
{
    // A child of the root track asks for its levels: outerInit with the levels that the piece keeps. On the effect
    // sequence, a child of the root's children asks. A request from another track finds no sequence.
    Model& m = *model_;
    const Track& track = m.tracks.TrackAt(index);
    if (!m.effect && track.parent == m.root)
    {
        const uint32_t number = track.route & 0xf;
        OuterInit(number, m.piece.kept_levels);
        m.asked |= 1u << number;
    }
    else if (m.effect && track.parent != Track::kNone && m.tracks.TrackAt(track.parent).parent == m.root)
    {
        const uint32_t number = track.route & 0xff;
        if (number >= kRootNumber)
        {
            m.unmodelled++;
            return;
        }
        OuterInit(number, m.piece.kept_levels);
        m.asked |= 1u << number;
    }
}

uint16_t Engine::SyncBeat(bool directed)
{
    // SeqTrackRoot::beatUpdate: the root track takes its standard tempo again. The swing stays off. It plays only
    // while the player leads the second captain. A jump piece's requests at the beat's top (onBeatTop) stay empty.
    Model& m = *model_;
    if (!directed)
    {
        m.unmodelled++;
        return 0;
    }
    m.tracks.SetTempo(m.root, m.standard_tempo);
    uint16_t answer = m.piece.beat_interval;
    m.odd_beat = !m.odd_beat;
    if (m.conductor)
    {
        // The conductor's root object counts every other beat, and answers the conductor's beat interval.
        if (m.odd_beat)
        {
            m.conductor->CountBeat();
        }
        answer = m.conductor->BeatInterval();
    }

    return answer;
}

void Engine::SyncObjectInit(uint32_t index, uint32_t value, bool directed)
{
    Model& m = *model_;
    const Track& track = m.tracks.TrackAt(index);
    switch (value)
    {
    case kSyncRootInit:
        // DirectedBgm::initRootTrack_onPlaying. The track's registers take its object's handle (setObject), and the
        // object takes the track's tempo as its standard tempo (SeqTrackRoot::init). The directors' start (playInit)
        // mutes no root track.
        if (!directed)
        {
            m.unmodelled++;
            break;
        }
        m.tracks.WriteRegDirect(index, kObjectRegister, 0);
        m.tracks.WriteRegDirect(index, kObjectRegister + 1, 0);
        m.standard_tempo = track.tempo;
        m.odd_beat = false;
        break;

    case kSyncChildInit:
        // DirectedBgm::initChildTrack_onPlaying for the slot in the track's route: setObject, then the directors'
        // start. The director of a layer mutes its track (TrackOnDirectorBase::onPlayInit).
        if (!directed)
        {
            m.unmodelled++;
            break;
        }
        m.tracks.WriteRegDirect(index, kObjectRegister, 0);
        m.tracks.WriteRegDirect(index, kObjectRegister + 1, 0);
        if ((m.piece.muted_tracks & (1u << (track.route & 0xf))) != 0)
        {
            m.tracks.TrackAt(index).pause_status |= kMutedLayerStatus;
            m.tracks.MuteTrack(index, true);
        }
        break;

    default:
        break;
    }
}

void Engine::SyncSeLevels(uint32_t index)
{
    // JAIBasic::setParameterSeqSync gives an effect's slot track, by the low byte of its route, the levels that it
    // last took. The other slots hold their first levels. The surround level stays 0 for a stereo output. A request
    // from a piece of music, or past the slots, reads outside the levels.
    Model& m = *model_;
    const Track& track = m.tracks.TrackAt(index);
    if (!m.effect || (track.route & 0xff) >= m.effect->slots)
    {
        m.unmodelled++;
        return;
    }

    const Effect& e = *m.effect;
    const bool effect_slot = (track.route & 0xff) == e.slot;
    OuterParam* outer = m.tracks.TrackAt(index).outer;
    outer->SetParam(static_cast<uint8_t>(OuterParam::kVolume), effect_slot ? e.volume : 1.0f);
    outer->SetParam(static_cast<uint8_t>(OuterParam::kPan), effect_slot ? e.pan : 0.5f);
    outer->SetParam(static_cast<uint8_t>(OuterParam::kPitch), effect_slot ? e.pitch : 1.0f);
    outer->SetParam(static_cast<uint8_t>(OuterParam::kFxmix), effect_slot ? e.fxmix : 0.0f);
    outer->SetParam(static_cast<uint8_t>(OuterParam::kDolby), 0.0f);
}

void Engine::RunSubframe()
{
    Model& m = *model_;
    const auto in_frame = static_cast<uint32_t>(m.subframe % kSubframesPerFrame);
    if (in_frame == 0)
    {
        m.dsp.StartFrame(kSubframesPerFrame, settings_.dsp_level, kLeft, kRight);
    }

    m.dsp.RenderSubframe();
    jaudio2::CopyOutput(*memory_, kLeft, kRight, in_frame * 2 * jaudio2::Dsp::kSubframeSamples, output_);

    // JAI's frame runs on the game's thread, between two sub-frames of the audio thread: the effects' work, then the
    // sequences'.
    if (static_cast<double>(m.subframe) >= m.next_frame)
    {
        if (m.effect)
        {
            EffectFrame();
        }
        GameFrame();
        m.next_frame += m.frame_subframes;
    }

    // JASDriver::updateDSP: the port commands, the DSP channels' update, then the sub-frame's callbacks.
    RunPortCommands();
    m.voices.UpdateDspChannels(*memory_, &m.tracks);
    m.tracks.RunSubframe();
    m.subframe++;
}

bool Engine::Playing() const
{
    if (!model_ || !model_->tracks.Playing(model_->root))
    {
        return false;
    }

    return !model_->effect || model_->effect->state != Effect::kInactive;
}

int Engine::ActiveChannels() const
{
    if (!model_)
    {
        return 0;
    }

    return jaudio2::ActiveDspChannels(model_->voices);
}

bool Engine::Busy() const
{
    return Playing() || ActiveChannels() != 0 || FxRinging();
}

bool Engine::FxRinging() const
{
    if (!model_)
    {
        return false;
    }

    return jaudio2::FxBuffersRinging(memory_->Bytes().subspan(kFxBuffers, 4 * kFxBufferBytes),
                                     model_->dsp.Delay().samples);
}

uint32_t Engine::MissingBankNotes() const
{
    return model_ ? model_->tracks.MissingBankNotes() : 0;
}

uint32_t Engine::Unmodelled() const
{
    if (!model_)
    {
        return 0;
    }

    return model_->tracks.Unmodelled() + model_->unmodelled + (model_->conductor ? model_->conductor->Unmodelled() : 0);
}

void Engine::SetLoopHook(LoopHook hook)
{
    loop_hook_ = std::move(hook);
    if (model_)
    {
        model_->tracks.SetLoopHook(loop_hook_);
    }
}

std::vector<uint32_t> Engine::TreeOrder() const
{
    if (!model_ || model_->root == Track::kNone)
    {
        return {};
    }

    const Tracks& tracks = model_->tracks;
    const auto track_at = [&tracks](uint32_t index) -> const Track&
    {
        return tracks.TrackAt(index);
    };
    const auto closed = [](uint32_t, const Track& track, const std::vector<uint32_t>&)
    {
        return track.status == Track::kClosed;
    };

    return jaudio2::WalkTrees(std::span<const uint32_t>(&model_->root, 1), track_at, closed);
}

bool Engine::PlayedNotes(uint32_t track) const
{
    return model_ && model_->tracks.PlayedNotes(track);
}

bool Engine::QuietPass(uint32_t track) const
{
    return model_ && model_->tracks.QuietPass(track);
}

bool Engine::Held() const
{
    if (!model_)
    {
        return false;
    }

    // A track that waits for its first note holds the count back.
    const Tracks& tracks = model_->tracks;
    for (const uint32_t index : TreeOrder())
    {
        if (!HasChildren(tracks, index) && !tracks.PlayedNotes(index) && !tracks.Looped(index))
        {
            return false;
        }
    }

    bool held = false;
    for (uint32_t i = 0; i < Voices::kChannels; i++)
    {
        const Channel& channel = model_->voices.ChannelAt(i);
        if (!channel.alive || channel.status == Channel::kStop)
        {
            continue;
        }

        if (!NoteHeld(channel))
        {
            return false;
        }

        held = true;
    }

    return held;
}

bool Engine::Moving() const
{
    if (!model_)
    {
        return false;
    }

    // A level that a command sets at once takes one step that changes nothing.
    for (const uint32_t index : TreeOrder())
    {
        const Track& track = model_->tracks.TrackAt(index);
        if (std::any_of(track.timed.begin(), track.timed.end(),
                        [](const MoveParam& p) { return p.time > 0.0f && p.amount != 0.0f; }))
        {
            return true;
        }
    }

    for (uint32_t i = 0; i < Voices::kChannels; i++)
    {
        const Channel& channel = model_->voices.ChannelAt(i);
        if (channel.alive && channel.sweep_steps != 0)
        {
            return true;
        }
    }

    return false;
}

bool Engine::WaitsForNotes(uint32_t track) const
{
    return model_ && model_->tracks.TrackAt(track).wait == -1;
}

bool Engine::WaitsForEver(uint32_t track) const
{
    if (!WaitsForNotes(track))
    {
        return false;
    }

    // The track waits for the note of its first channel to end.
    const uint32_t channel = model_->tracks.TrackAt(track).channels[0];
    return channel != Track::kNone && NoteHeld(model_->voices.ChannelAt(channel));
}

bool Engine::Waiting() const
{
    if (!model_)
    {
        return false;
    }

    // A track that has started no note and has jumped back idles: it polls the game. The effects sequence's other
    // slot tracks do that.
    const Tracks& tracks = model_->tracks;
    const auto idles = [this, &tracks](uint32_t index)
    {
        return HasChildren(tracks, index) || WaitsForNotes(index) ||
               (!tracks.PlayedNotes(index) && tracks.Looped(index));
    };
    const std::vector<uint32_t> order = TreeOrder();
    return !order.empty() && std::all_of(order.begin(), order.end(), idles);
}

Engine::WavePlace Engine::DspChannelPlace(uint32_t index) const
{
    WavePlace place;
    if (!model_ || index >= Voices::kDspChannels)
    {
        return place;
    }

    // An oscillator's channel plays no wave.
    const DspChannel& dsp_channel = model_->voices.DspChannelAt(index);
    if (dsp_channel.status != DspChannel::kActive || dsp_channel.channel == Channel::kNone ||
        model_->voices.ChannelAt(dsp_channel.channel).wave_format != Channel::kWave)
    {
        return place;
    }

    return PlayingWavePlace(*memory_, kBlocks + index * static_cast<uint32_t>(DspChannelBlock::kBytes));
}

} // namespace ntdlib::jaudio1
