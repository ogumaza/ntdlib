// SPDX-License-Identifier: MIT

// The engine of the C JAudio for one piece: the sound files, the model's set-up, the DSP's frames and the
// engine's state. engine_game.cpp contains the game's code above the C JAudio. The two files build as one
// translation unit. The compiler can then inline the small helpers across them.

#include "ntdlib/jac/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "jac/engine_internal.h"
#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/bx.h"

namespace ntdlib::jac
{

namespace
{

namespace nj = ntdlib::jaudio;

// The DSP's memory below ARAM: the 64 channels' blocks from Engine::kBlocks, the FX lines' settings, the outputs of a
// frame, and the FX lines' buffers. A line's settings take 0x20 bytes, as in JAudio1: the flags, the delay's length,
// the buffer at +4, the two sends from +8 and the filter from +0x10.
constexpr uint32_t kFxSettings = 0x6000;
constexpr uint32_t kFxLineBytes = 0x20;
constexpr uint32_t kLeft = 0x6100;
constexpr uint32_t kRight = 0x6800;
constexpr uint32_t kFxBuffers = 0x8000;
constexpr uint32_t kMaxFxSubframes = 128;
constexpr uint32_t kFxBufferBytes = kMaxFxSubframes * 0xa0;
static_assert(kFxBuffers + 4 * kFxBufferBytes <= Engine::kAram);

// A frame of the Jac is one sub-frame (JAC_SUBFRAMES).
constexpr uint16_t kSubframesPerFrame = 1;

// TrackReceive's request that closes scene 6 of wave system 13 (WaveScene_Erase).
constexpr uint16_t kEraseShipPart = 0x1000;

// TrackReceive's request that stops the actions of an event that its port 2 doesn't list (MML_StopEventAll).
constexpr uint16_t kStopEvents = 0x9000;

// Whether an envelope keeps its level: at its sustain, once it has stopped, or from the note's start when it has no
// attack table. The game's step then gives the level 1 and leaves the state as it is.
bool OscHeld(const OscData& data, const OscState& state)
{
    switch (state.state)
    {
    case OscState::kHold:
    case OscState::kStop:
        return true;
    case OscState::kStart:
    case OscState::kAttack:
        return data.attack.Null();
    default:
        return false;
    }
}

// Whether a channel holds its note: each of its envelopes keeps its level, and it plays a looping wave or an
// oscillator.
bool NoteHeld(const Channel& channel)
{
    if (channel.key == 0xff || channel.dsp_channel == kNone)
    {
        return false;
    }
    if (channel.kind == Channel::kWave && (channel.wave == nullptr || channel.wave->info.loop_flags == 0))
    {
        return false;
    }

    for (std::size_t i = 0; i < channel.oscillators.size(); i++)
    {
        if (channel.oscillators[i] != nullptr && !OscHeld(*channel.oscillators[i], channel.osc_states[i]))
        {
            return false;
        }
    }

    return true;
}

// Whether a track has opened a track that is still open. An unheard track (Engine::QueueDemoEvents) doesn't count.
bool HasChildren(const Tracks& tracks, uint32_t index)
{
    const auto open = [&tracks](uint32_t child)
    {
        return child != Track::kNone && tracks.At(child).state != Track::kClosed && !tracks.At(child).unheard;
    };
    const Track& track = tracks.At(index);
    return std::any_of(track.children.begin(), track.children.end(), open);
}

// Whether a DSP channel plays an unheard note (Engine::QueueDemoEvents).
bool UnheardDsp(const Channels& channels, const DspChannel& dsp_channel)
{
    return dsp_channel.channel != kNone && channels.At(dsp_channel.channel).unheard;
}

} // namespace

SoundFiles::SoundFiles(std::span<const uint8_t> bank_file, std::span<const uint8_t> sequences,
                       std::span<const uint8_t> index, WaveArchives waves)
    : waves_(std::move(waves))
{
    systems_ = nj::ReadBxWaveSystems(bank_file);

    // ntdlib's warnings about the banks that it leaves out go unshown. The game's release gives none.
    std::vector<std::string> warnings;
    banks_ = nj::ReadBxBanks(bank_file, warnings);

    // An entry that shares another entry's data plays the data at the end of its chain of shares (data_id). The
    // entries' numbers are their places in the index.
    const std::vector<nj::ArchiveFile> files = nj::ReadBarcSequenceFiles(index, sequences, false);
    for (const nj::ArchiveFile& file : files)
    {
        const std::span<const uint8_t> data = files.at(file.data_id.value_or(file.id)).data;
        SequenceFile entry;
        entry.number = file.id;
        entry.name = file.name;
        entry.sequence = std::make_unique<Sequence>(std::vector<uint8_t>(data.begin(), data.end()));
        sequences_.push_back(std::move(entry));
    }
}

const SoundFiles::SequenceFile* SoundFiles::FindSequence(uint16_t number) const
{
    for (const SequenceFile& file : sequences_)
    {
        if (file.number == number)
        {
            return &file;
        }
    }

    return nullptr;
}

Engine::Engine(const SoundFiles& files, const Settings& settings)
    : files_(files), settings_(settings), memory_(std::make_unique<jaudio2::DspMemory>(kAram + kAramSize))
{
    model_ = std::make_unique<Model>(files_, *memory_);
    Model& m = *model_;

    // The DSP's start-up (DsetupTable, DsetVARAM), and the FX lines as AuxBusInit sets them up.
    m.dsp.SetFxLayout(jaudio2::Dsp::FxLayout::kJaudio1);
    m.dsp.Setup(static_cast<uint16_t>(Channels::kDspChannels), kBlocks, kFxSettings);
    m.dsp.SetVaram(kAram);
    for (std::size_t line = 0; line < settings_.fx_lines.size(); line++)
    {
        const FxLine& fx = settings_.fx_lines[line];
        if (fx.flags == 0)
        {
            continue;
        }

        const uint32_t at = kFxSettings + static_cast<uint32_t>(line) * kFxLineBytes;
        const uint32_t buffer = kFxBuffers + static_cast<uint32_t>(line) * kFxBufferBytes;
        jaudio2::WriteFxLine<jaudio2::Dsp::FxLayout::kJaudio1>(*memory_, at, buffer, fx.subframes, fx);
    }

    // Jac_SetMixerLevel, and the game's answer to the tracks' requests (TrackReceive, 0x267060 in the release's
    // main): 0x1000 closes the scene of the ship's part. A request with bits 12 and 15 set reads the track's port 2
    // (Jam_ReadPortAppDirect). The game's other work clears its records of the events' actions and sets a flag in
    // the game's state. It changes no track.
    m.channels.SetMixerLevel(settings_.channel_level);
    const auto receive = [this](uint32_t track, uint16_t value) -> uint16_t
    {
        if (value == kEraseShipPart)
        {
            model_->banks.CloseScene(kPartSystem, kShipPartScene);
            return 0;
        }

        if ((value & kStopEvents) == kStopEvents)
        {
            uint16_t mask = 0;
            model_->tracks.ReadPortAppDirect(track, 2, mask);
        }

        return 0;
    };
    m.tracks.SetTrackCallback(receive);

    output_.assign(2 * jaudio2::Dsp::kSubframeSamples, int16_t{0});
}

Engine::~Engine() = default;

void Engine::RenderFrame()
{
    Model& m = *model_;
    m.dsp.StartFrame(kSubframesPerFrame, settings_.dsp_level, kLeft, kRight);
    m.dsp.RenderSubframe();
    jaudio2::CopyOutput(*memory_, kLeft, kRight, 0, output_);
}

void Engine::UpdateDsp()
{
    // UpdateDSP (0x268390): the DSP player's callbacks (none in the player's list), UpdateDSPchannelAll, then the
    // player's callbacks.
    Model& m = *model_;
    m.channels.UpdateDspChannels();
    m.tracks.RunPlayerCallbacks();
}

int Engine::ActiveChannels() const
{
    int count = 0;
    for (uint32_t i = 0; i < Channels::kDspChannels; i++)
    {
        const DspChannel& dsp_channel = model_->channels.Dsp(i);
        count += dsp_channel.state != DspChannel::kFree && !UnheardDsp(model_->channels, dsp_channel) ? 1 : 0;
    }

    return count;
}

bool Engine::Busy() const
{
    return Playing() || ActiveChannels() != 0 || FxRinging();
}

bool Engine::FxRinging() const
{
    return jaudio2::FxBuffersRinging(memory_->Bytes().subspan(kFxBuffers, 4 * kFxBufferBytes),
                                     model_->dsp.Delay().samples);
}

uint32_t Engine::MissingBankNotes() const
{
    return model_->tracks.MissingNotes();
}

uint32_t Engine::Unmodelled() const
{
    return model_->tracks.Unmodelled() + model_->unmodelled;
}

void Engine::SetLoopHook(LoopHook hook)
{
    model_->tracks.SetLoopHook(std::move(hook));
}

std::vector<uint32_t> Engine::TreeOrder() const
{
    // A piece's tree is its root's. A part or an effect has the trees of the tracks whose ports its code writes, in the
    // order of the writes.
    const Model& m = *model_;
    std::vector<uint32_t> roots;
    if (m.sound == Model::Sound::kPiece)
    {
        roots.push_back(m.root);
    }
    for (const std::pair<uint32_t, uint32_t>& place : m.sound_ports)
    {
        if (std::find(roots.begin(), roots.end(), place.first) == roots.end())
        {
            roots.push_back(place.first);
        }
    }

    // A track that two roots' trees share is walked once.
    const auto track_at = [&m](uint32_t index) -> const Track&
    {
        return m.tracks.At(index);
    };
    const auto skip = [](uint32_t index, const Track& track, const std::vector<uint32_t>& order)
    {
        return track.state == Track::kClosed || track.unheard ||
               std::find(order.begin(), order.end(), index) != order.end();
    };

    return jaudio2::WalkTrees(roots, track_at, skip);
}

bool Engine::PlayedNotes(uint32_t track) const
{
    return model_->tracks.PlayedNotes(track);
}

bool Engine::QuietPass(uint32_t track) const
{
    return model_->tracks.QuietPass(track);
}

bool Engine::Held() const
{
    const Model& m = *model_;
    for (const uint32_t index : TreeOrder())
    {
        if (!HasChildren(m.tracks, index) && !m.tracks.PlayedNotes(index) && !m.tracks.Looped(index))
        {
            return false;
        }
    }

    bool held = false;
    for (uint32_t i = 0; i < Channels::kChannels; i++)
    {
        const Channel& channel = m.channels.At(i);
        if (channel.dsp_channel == kNone || channel.unheard)
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
    const Model& m = *model_;
    for (const uint32_t index : TreeOrder())
    {
        const Track& track = m.tracks.At(index);
        const auto moving = [](const MoveParam& p)
        {
            return p.time > 0.0f && p.step != 0.0f;
        };
        if (std::any_of(track.timed.begin(), track.timed.end(), moving))
        {
            return true;
        }
    }

    for (uint32_t i = 0; i < Channels::kChannels; i++)
    {
        const Channel& channel = m.channels.At(i);
        if (channel.dsp_channel != kNone && !channel.unheard && channel.sweep_steps != 0)
        {
            return true;
        }
    }

    return false;
}

bool Engine::WaitsForNotes(uint32_t track) const
{
    return model_->tracks.At(track).wait == -1;
}

bool Engine::WaitsForEver(uint32_t track) const
{
    if (!WaitsForNotes(track))
    {
        return false;
    }

    // The track waits for the note of its first channel to end.
    const uint32_t channel = model_->tracks.At(track).channels[0];
    return channel != Track::kNone && NoteHeld(model_->channels.At(channel));
}

bool Engine::Waiting() const
{
    const std::vector<uint32_t> order = TreeOrder();
    const auto waits = [this](uint32_t index)
    {
        return HasChildren(model_->tracks, index) || WaitsForNotes(index);
    };

    return !order.empty() && std::all_of(order.begin(), order.end(), waits);
}

Engine::WavePlace Engine::DspChannelPlace(uint32_t index) const
{
    WavePlace place;
    if (index >= Channels::kDspChannels)
    {
        return place;
    }

    // An oscillator's channel plays no wave.
    const DspChannel& dsp_channel = model_->channels.Dsp(index);
    if (dsp_channel.state == DspChannel::kFree || dsp_channel.channel == kNone ||
        model_->channels.At(dsp_channel.channel).kind != Channel::kWave || UnheardDsp(model_->channels, dsp_channel))
    {
        return place;
    }

    return jaudio2::PlayingWavePlace(
        *memory_, kBlocks + dsp_channel.block * static_cast<uint32_t>(jaudio2::DspChannelBlock::kBytes));
}

Tracks& Engine::TrackModel()
{
    return model_->tracks;
}

const Tracks& Engine::TrackModel() const
{
    return model_->tracks;
}

Channels& Engine::ChannelModel()
{
    return model_->channels;
}

const Channels& Engine::ChannelModel() const
{
    return model_->channels;
}

Banks& Engine::BankModel()
{
    return model_->banks;
}

const Banks& Engine::BankModel() const
{
    return model_->banks;
}

Random& Engine::Generator()
{
    return model_->random;
}

} // namespace ntdlib::jac
