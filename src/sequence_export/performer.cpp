// SPDX-License-Identifier: MIT

// Sequence execution (see performer.h). A core of nw::snd (nwsnd/) plays the sequence. The performer follows the core's
// hooks and records the notes and controllers. Addresses without another source come from the Wii U's nw::snd in Animal
// Crossing: amiibo Festival's executable. The Switch's nn::atk in WarioWare: Get It Together!'s executable is the same
// player with 5 ms frames, arguments in the file's byte order and the Switch's audio renderer. Settings selects the
// console.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/lengths.h"
#include "sequence_export/performer_engine.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

namespace
{

constexpr double kMaxTailMs = kMaxTailSeconds * 1000.0; // longest release tail kept after the end (lengths.h)

// The notes that a core has room for at once in a conversion that keeps every note. The 3DS's core always has this many
// channels.
constexpr int kEveryNote = 4096;

// The 3DS's nw::snd counts ticks in thousandths of the ARM's cycles. A sound frame is 160 samples of the DSP's clock,
// 160 * 8192 cycles (SequenceSoundPlayer::UpdateTick, code.bin 0x31ff50; see Performer::NextCoreTick).
constexpr float kArmClock = 268111856.0f;
constexpr uint64_t kFrameMilliCycles = 1310720000;

// The units of a sound frame that ticks are scheduled in: 1/65536 ms on the Wii U and the Switch, in sound frames of
// Settings::frame_ms (SequenceSoundPlayer::UpdateTick, 0x022B7368), or the 3DS's thousandths of ARM cycles.
uint64_t FrameUnits(const Settings& settings)
{
    if (settings.engine == Engine::kCtr)
    {
        return kFrameMilliCycles;
    }

    return static_cast<uint64_t>(settings.frame_ms * 65536.0);
}

// A time in the units of FrameUnits, in milliseconds.
double UnitsToMs(uint64_t units, const Settings& settings)
{
    if (settings.engine == Engine::kCtr)
    {
        return static_cast<double>(units) / kArmClock;
    }

    return static_cast<double>(units) / 65536.0;
}

// The rules of the main loop's passes that this performer plays by (passes.h). A track's first part starts with the SFZ
// low-pass and biquad off. A preset at the loop's first tick doesn't count among the settings there. Without
// Settings::reverb_moves_loops, the comparison of passes leaves CC91 out.
const LoopRules kLoopRules{{kSfzLowPassEvent, kSfzBiquadEvent}, false};
const LoopRules kLoopRulesWithoutReverb{{kSfzLowPassEvent, kSfzBiquadEvent}, false, {EventKind::kReverb}};

const LoopRules& LoopRulesOf(const Settings& settings)
{
    return settings.reverb_moves_loops ? kLoopRules : kLoopRulesWithoutReverb;
}

// How a track's next stretch of commands begins after `command` moved the track elsewhere (Performance::stretches).
Entry EntryAfter(uint32_t command)
{
    switch (command)
    {
    case 0x8a:
        return Entry::kCall;
    case 0xfd:
        return Entry::kReturn;
    case 0xfc:
        return Entry::kLoop;
    default:
        return Entry::kJump;
    }
}

// The report of the new notes that a track starts in one tick past TickNoteCap(). The performer keeps the first
// notes. On the 3DS, the Wii and the Switch, the game keeps the newest. A new note that finds every voice in use takes
// the voice of the lowest priority there, the oldest first (the AllocVoice of each console's voice manager). On the
// Wii U, a new note finds no channel once every channel is in use (ChannelManager::Alloc, 0x022B8A14). The game there
// keeps the first notes too.
const char* TickCapReport(Engine engine)
{
    return engine == Engine::kCafe ? " new channels on a track in one tick (excess omitted; the game keeps the first)"
                                   : " new channels on a track in one tick (excess omitted; the game keeps the newest)";
}

} // namespace

template <typename Console, template <typename> class VoiceLayer>
bool SequenceCore<Console, VoiceLayer>::OnParseLimit(Player& player, int track)
{
    const auto t = static_cast<std::size_t>(track);
    const int64_t tick = player.TickCount();
    if (limit_ticks_[t] != tick - 1)
    {
        run_start_ms_[t] = now_ms_;
    }
    limit_ticks_[t] = tick;
    const bool stop = now_ms_ - run_start_ms_[t] >= kHoldSeconds * 1000.0;
    if (stop && !performer_->stop_)
    {
        performer_->midi_.OnRunaway(*performer_, track);
    }

    return stop;
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnParseLimitEnd(Player&, int track)
{
    if (!performer_->stop_)
    {
        performer_->midi_.OnRunaway(*performer_, track);
    }
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnCommandStart(Player&, int track, uint32_t offset, uint32_t command,
                                                       bool conditional)
{
    performer_->FollowCommandStart(track, offset, command, conditional);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnCommand(Player& player, int track, uint32_t command, int32_t arg1,
                                                  int32_t arg2)
{
    performer_->FollowCommand(track, command, arg1, arg2, player.Track(track)->CommandEnd());
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnExtendedCommand(Player& player, int track, uint32_t command, int32_t arg1,
                                                          int32_t arg2)
{
    performer_->FollowCommand(track, command, arg1, arg2, player.Track(track)->CommandEnd());
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnRandomDraw(Player&, int)
{
    performer_->FollowRandomDraw();
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnVariableRead(Player&, int track, int index)
{
    performer_->FollowVariableRead(track, index);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnNoteCommand(Player&, int track, int key, int velocity, int32_t length)
{
    performer_->FollowNoteCommand(track, key, velocity, length);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnNoteOn(Player&, int track, Channel& channel)
{
    const int key = channel.voice != nwsnd::kNoVoice ? this->PositionKey(channel.voice) : -1;
    performer_->FollowNoteOn(track, channel.place, key);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnLegato(Player&, int track, Channel& channel)
{
    performer_->FollowLegato(track, channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnRelease(Player&, int, Channel& channel)
{
    performer_->FollowRelease(channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnLengthsCounted(Player&, int track)
{
    performer_->FollowLengths(track);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnNoChannel(int track)
{
    performer_->FollowNoChannel(track);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnChannelDropped(int track, Channel& channel)
{
    performer_->FollowChannelDrop(track, channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnGainAboveFull(Channel& channel)
{
    performer_->FollowGainAboveFull(channel.owner_track, channel.place);
}

template <typename Console, template <typename> class VoiceLayer>
void SequenceCore<Console, VoiceLayer>::OnVolumeLfo(Channel& channel, float envelope_db, float lfo_db)
{
    performer_->FollowVolumeLfo(channel.owner_track, channel.place, envelope_db, lfo_db);
}

template <typename Console, template <typename> class VoiceLayer>
bool SequenceCore<Console, VoiceLayer>::StopsTick(Player&, int)
{
    return performer_->stop_;
}

template <typename Console, template <typename> class VoiceLayer>
bool SequenceCore<Console, VoiceLayer>::LoopsForEver(const Player&, int track, uint32_t address) const
{
    return performer_->endless_loops_.contains({track, address});
}

Performer::Performer(const Settings& settings, const SoundInfo& sound, const Sequence& seq, Regions& banks,
                     const PerformOptions& options, const PassFindings& earlier, double level_scale,
                     const std::array<uint8_t, kTrackCount>& part_limits, const std::vector<LoopChoice>& loop_choices)
    : settings_(settings), sound_(sound), seq_(seq), banks_(banks), options_(options), main_loop_(loop_choices)
{
    midi_.rec_.SetLevelScale(level_scale);
    midi_.earlier_ = earlier;
    midi_.rec_.SetPartLimits(part_limits);
    for (Track& tr : tracks_)
    {
        tr.engine = settings_.engine;
    }

    // The game's changes come in the order of their frames, and so do its writes in user procedures.
    std::ranges::stable_sort(options_.changes, {}, &VariableChange::frame);
    std::ranges::stable_sort(options_.procedure_changes, {}, &ProcedureChange::frame);
    procedure_taken_.assign(options_.procedure_changes.size(), false);

    main_volume_.Init(127);

    // The Wii's stereo waves play in balance mode. The build of October 2006 has only dual mode.
    if (Wii())
    {
        balance_ = !WiiOctober2006() && sound.pan_mode == 1;
        midi_.out_.since_march_2008 = settings_.wii_build.SinceMarch2008();
        midi_.out_.balance = balance_;
    }

    // The core plays the sequence (MakeCore). On the Wii U and the Switch, each channel has a voice. With voice
    // drops, the cores of the Wii U, the Switch and the Wii have the game's voices. Without, the cores have room
    // for kEveryNote notes at once. With voice drops, the Wii's core has NW4R SND's channels: one per voice, and
    // one more. The 3DS's core has room for kEveryNote channels. A channel there plays only while it holds a voice.
    // Only the voices run out. With voice drops, the 3DS's core has the game's voices. A new note then takes the
    // voice of the lowest priority.
    const int count = VoiceLimit() > 0 ? VoiceLimit() : kEveryNote;
    if (settings_.engine == Engine::kCafe)
    {
        MakeCore<CafeCore>(banks, count, count);
    }
    else if (settings_.engine == Engine::kSwitch)
    {
        MakeCore<NxCore>(banks, count, count);
    }
    else if (settings_.engine == Engine::kCtr)
    {
        MakeCore<CtrCore>(banks, kEveryNote, VoiceLimit() > 0 ? VoiceLimit() : kEveryNote);
    }
    else if (Wii())
    {
        MakeCore<RevolutionCore>(banks, VoiceLimit() > 0 ? nwsnd::RevolutionPolicy::kChannelCount : count, count);
    }
}

int16_t* Performer::Variable(int t, uint8_t index)
{
    // Player variables 0-15, global 16-31 and track 32-47 (MmlParser::GetVariablePtr, 0x022CB600).
    if (index < 16)
    {
        return &player_vars_[index];
    }
    if (index < 32)
    {
        return &global_vars_[index - 16];
    }
    if (index < 48)
    {
        return &tracks_[t].vars[index - 32];
    }

    return nullptr;
}

template <typename Core>
void Performer::SetTrackVariables(Core& core, uint8_t index, int16_t value)
{
    core.GetPlayer().SetVariable(core, index, value);
    for (int t = 0; t < kTrackCount; t++)
    {
        if (core.GetPlayer().Track(t))
        {
            tracks_[t].vars[index - 32] = value;
        }
    }
}

PresetKey Performer::NoteKey(int t, const Note& n) const
{
    PresetKey pk = ChannelPresetKey(tracks_[t], n);
    pk.attack_timecents = n.midi.attack_timecents;
    pk.lift = n.midi.lift;
    pk.lfo_swing = n.midi.lfo_fit.swing;
    pk.lfo_cut = n.midi.lfo_fit.cut;
    pk.at_pitch_limits = n.midi.at_limits;
    if (midi_.earlier_.filter_moves[t])
    {
        pk.filter_controller = true;
    }
    else
    {
        pk.filter = midi_.tracks_[t].filter;
    }

    return pk;
}

void Performer::SetFilter(int t)
{
    Track& tr = tracks_[t];
    const SoundFontFilter filter =
        ToSoundFont(TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value, settings_), settings_);
    if (filter != midi_.tracks_[t].filter && TrackBusy(t))
    {
        midi_.filter_moves_[t] = true;
    }

    midi_.tracks_[t].filter = filter;
}

void Performer::OpenTrack(int t, uint32_t offset)
{
    // The rest of the track's state comes from the core (TakeCoreTrack). Open (0x022B69B8) clears the wait, the call
    // stack and the wait for notes to end on every console but the Wii (kOpenClearsState in nwsnd's policies).
    Track& tr = tracks_[t];
    tr.current = offset;
    tr.open = true;
    tr.opened = tick_;
    tr.next_stretch = Entry::kOpen;
}

void Performer::ReleaseNote(int t, Note& n, int release_value)
{
    // Channel::Release (0x022B6028) applies a new rate even to channels already releasing. Key groups depend on that.
    //
    // Envelopes advance Settings::env_step_ms per frame, from the note-on frame on. A channel stops in the frame it
    // falls below -90.4 dB and is gone before the next frame's ticks. While the core still has the channel, the core's
    // channel gives the envelope's level.
    const int64_t frame = static_cast<int64_t>(frame_);

    std::optional<float> core_level;
    const auto level_on_core = [&](auto& core)
    {
        if (const auto* channel = CoreChannelOf(core, n))
        {
            core_level = channel->envelope.Level();
        }
    };
    WithCore(level_on_core);

    n.release_pending = false;
    if (n.released)
    {
        if (release_value >= 0 && n.release_end_ms > now_ms_)
        {
            const int64_t steps = frame - static_cast<int64_t>(n.released_frame);
            const float level =
                core_level ? *core_level : ReleasedLevel(n.release_level, n.release_rate, steps, settings_);
            n.release_level = level;
            n.released_frame = frame_;
            n.release_rate = DecayRate(release_value, settings_.engine);
            n.release_end_ms =
                static_cast<double>(frame + ReleaseFrames(level, n.release_rate, settings_) + 1) * settings_.frame_ms;
        }

        return;
    }

    n.released = true;
    n.released_frame = frame_;
    n.release_level = core_level ? *core_level : HeldLevel(n.env, frame - static_cast<int64_t>(n.on_frame), settings_);
    n.release_rate = DecayRate(release_value >= 0 ? release_value : n.env.release, settings_.engine);
    n.release_end_ms =
        static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate, settings_) + 1) * settings_.frame_ms;
    midi_.OnRelease(*this, t, n);
}

bool Performer::Unheard(const Note& n) const
{
    return frame_ == n.on_frame;
}

bool Performer::StillAudible(const Note& n) const
{
    // A note-off that waits for the channel's first update starts the release in the frame after the tick's frame
    // (ReleasePending). Before the next tick, the release has taken no step.
    const int64_t frames = static_cast<int64_t>(frame_) - static_cast<int64_t>(n.released_frame);
    const double steps = static_cast<double>(std::max<int64_t>(frames, 0));
    const bool double_rate = settings_.engine == Engine::kCtr || settings_.engine == Engine::kRevolution;
    const double level = double_rate
                             ? n.release_level - static_cast<double>(n.release_rate) * settings_.env_step_ms * steps
                             : n.release_level - n.release_rate * settings_.env_step_ms * steps;
    return n.EndMs() > now_ms_ && level > -kInaudibleSustain;
}

bool Performer::Heard(const Note& n) const
{
    return n.released ? StillAudible(n) : n.EndMs() > now_ms_;
}

bool Performer::Alone(int t, const Note& n) const
{
    const Track& tr = tracks_[t];
    auto other_heard = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.midi.part == midi_.tracks_[t].output.part && Heard(other);
    };

    return std::ranges::none_of(tr.notes, other_heard) && std::ranges::none_of(tr.detached, other_heard);
}

bool Performer::PartSounds(int t, uint8_t part, const Note* except) const
{
    const Track& tr = tracks_[t];
    auto sounds = [&](const Note& n)
    {
        return &n != except && n.HasMidiNote() && n.midi.part == part && n.EndMs() > now_ms_;
    };

    return std::ranges::any_of(tr.notes, sounds) || std::ranges::any_of(tr.detached, sounds);
}

void Performer::ReleaseAndDetach(int t)
{
    // SequenceTrack::ReleaseAllChannel (0x022B608C) gives the channels the track's settings before it releases them.
    // The end of a tick does the same. So the MIDI part the notes sound on gets the tick's changes so far before the
    // track leaves it.
    midi_.OnReleaseAll(*this, t);
    for (Note& n : tracks_[t].notes)
    {
        ReleaseNote(t, n);
    }
    midi_.OnReleasedAll(*this, t);
}

bool Performer::OthersSounding(int t, const Note* except) const
{
    const Track& tr = tracks_[t];
    for (const Note& n : tr.notes)
    {
        if (&n != except && n.EndMs() > now_ms_ && !BelowHearing(n))
        {
            return true;
        }
    }

    for (const Note& n : tr.detached)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

void Performer::StopNote(int t, Note& n)
{
    // Channel::Stop (0x022C4278) silences immediately.
    midi_.OnStop(*this, t, n);
    n.released = true;
    n.release_end_ms = now_ms_;
}

void Performer::CloseTrack(int t)
{
    // Like the end of a tick, SequenceTrack::Close (0x022B6158) first gives the channels the track's settings. The
    // release tails start from the closing tick's volume, pan, pitch and filter. Close then releases every channel,
    // even one that ignores note-off.
    Track& tr = tracks_[t];
    midi_.OnTrackClosing(*this, t);
    for (Note& n : tr.notes)
    {
        ReleaseNote(t, n);
    }

    midi_.OnTrackClosed(*this, t);
    tr.open = false;
}

bool Performer::TrackBusy(int t) const
{
    for (const Note& n : tracks_[t].notes)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

bool Performer::Quiet(int t) const
{
    const Track& tr = tracks_[t];
    const uint32_t since = std::max(tr.last_change.value_or(0), tr.opened);
    return !OthersSounding(t, nullptr) && now_ms_ - tick_ms_[since] > kHoldSeconds * 1000.0;
}

bool Performer::WaitsForever(int t) const
{
    // A track that waits a negative number of ticks never reads on. That happens to a note or wait whose length comes
    // from a variable left at -1. A tick counts down only a positive wait. The track reads commands only while its
    // wait is 0 (SequenceTrack::ParseNextTick, 0x022B6F14).
    const Track& tr = tracks_[t];
    if (tr.held_wait || tr.wait < 0)
    {
        return true;
    }
    if (!tr.note_finish_wait || tr.notes.empty())
    {
        return false;
    }

    for (const Note& n : tr.notes)
    {
        if (n.length > 0 || n.EndMs() < kNever)
        {
            return false;
        }
    }

    return true;
}

bool Performer::Stuck() const
{
    // A note that hasn't been released sounds here until it has faded (MidiNote::faded_ms). The MIDI note of a tie ends
    // at the same point. The sequence's sound then ends where the note's MIDI note ends (EndPerformance).
    const auto sounding = [this](const Note& n)
    {
        return n.EndMs() > now_ms_ && (n.released || now_ms_ < n.midi.faded_ms);
    };
    const auto ends_later = [this](const Note& n)
    {
        return n.EndMs() > now_ms_;
    };
    for (int t = 0; t < kTrackCount; t++)
    {
        const Track& tr = tracks_[t];
        if ((tr.open && tr.wait >= 0 && !tr.held_wait) || std::ranges::any_of(tr.notes, sounding) ||
            std::ranges::any_of(tr.detached, ends_later))
        {
            return false;
        }
    }

    return true;
}

bool Performer::Steady() const
{
    // The main volume and volume2 ramp on the Wii U and the Switch (SequenceSoundPlayer::ParseNextTick, 0x022B70AC, and
    // SequenceTrack::ParseNextTick, 0x022B6F14).
    if (main_volume_.Moving())
    {
        return false;
    }

    bool any = false;
    for (int t = 0; t < kTrackCount; t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }

        any = true;
        if (!WaitsForever(t) || tr.vol.Moving() || tr.volume2.Moving() || tr.pan.Moving() || tr.bend.Moving())
        {
            return false;
        }

        for (const Note& n : tr.notes)
        {
            if (n.SweepAt(frame_) != 0.0f)
            {
                return false;
            }
        }
    }

    return any;
}

void Performer::PruneNotes(int t)
{
    // Remove channels when their one-shot wave or release finishes (SequenceTrack::ChannelCallbackFunc, 0x022CB06C).
    auto ended = [&](Note& n)
    {
        if (n.EndMs() > now_ms_)
        {
            return false;
        }

        midi_.OnEnd(*this, t, n);
        Ended(n);

        return true;
    };
    std::erase_if(tracks_[t].notes, ended);
    std::erase_if(tracks_[t].detached, ended);
}

void Performer::FollowRelease(nwsnd::ChannelId channel)
{
    if (counting_lengths_)
    {
        length_releases_.push_back(channel);
    }
}

void Performer::FollowLengths(int t)
{
    // The count runs a note-off on a channel whose length has run out if the track's damper doesn't hold it. A region
    // that ignores note-offs plays on. Its MIDI note ends on the channel's release or stop, the wave's end, or where it
    // falls below hearing. In the game, silent channels keep their voices until the voice allocator reuses them.
    //
    // A note can be without a channel in the core. A one-shot wave can end in the frames before the tick in which the
    // note's length runs out. The core frees the channel at the wave's end. The performer also keeps some notes that
    // have no voice in the core (FollowNoChannel, FollowChannelDrop). FollowLengths counts the length and the sweep's
    // ticks of such a note itself. The note then ends in its place among the tick's note-offs. Without that count, a
    // note whose wave has ended would end after them (PruneNotes).
    counting_lengths_ = false;
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        bool has_channel = false;
        bool ran_out = false;
        const auto take_length = [&](auto& core)
        {
            if (const auto* channel = CoreChannelOf(core, n))
            {
                has_channel = true;
                n.length = channel->length;
                n.sweep_ticks = channel->sweep_counter;
                ran_out = std::ranges::find(length_releases_, n.core_channel) != length_releases_.end();
            }
        };
        WithCore(take_length);
        if (!has_channel)
        {
            if (n.length > 0)
            {
                n.length--;
            }
            ran_out = n.length == 0 && !tr.damper;

            if (!n.sweep_in_ms)
            {
                n.sweep_ticks++;
            }
        }

        if (ran_out && !n.released && !n.release_pending && !n.ignore_note_off)
        {
            if (settings_.note_off_waits_for_update && frame_ == n.on_frame)
            {
                n.release_pending = true;
            }
            else
            {
                ReleaseNote(t, n);
            }
        }

        midi_.OnLengthUpdated(*this, t, n);
    }

    length_releases_.clear();
    PruneNotes(t);
}

bool Performer::SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const
{
    const Track& tr = tracks_[t];
    auto moves = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == part &&
               ((n.sweep_in_ms && n.SweepAt(frame) != 0.0f) || n.midi.lfo_bends) && n.EndMs() > ms;
    };

    return std::ranges::any_of(tr.notes, moves) || std::ranges::any_of(tr.detached, moves);
}

void Performer::SweepAfterEnd()
{
    // As MsToTick counts, MIDI goes on at the last tick's tempo. Follow the sweeps for as long as the release tails
    // last (EndPerformance).
    const double per_tick = MsPerTick();
    if (per_tick <= 0)
    {
        return;
    }

    for (uint64_t f = frame_ + 1; static_cast<double>(f) * settings_.frame_ms < now_ms_ + kMaxTailMs; f++)
    {
        const double ms = static_cast<double>(f) * settings_.frame_ms;
        bool any = false;
        for (int t = 0; t < kTrackCount; t++)
        {
            for (uint8_t part = 0; part < kPartCount; part++)
            {
                if (SweepMoves(t, part, f - 1, ms))
                {
                    midi_.SendPitch(
                        *this, t, part, f,
                        WiiOctober2006() ? (ms - nominal_ms_) / MidiMsPerTick() : (ms - now_ms_) / per_tick);
                    any = true;
                }
            }
        }
        if (!any)
        {
            break;
        }
    }
}

float Performer::UserVolume(int t) const
{
    const Track& tr = tracks_[t];
    const float sound = std::clamp(static_cast<float>(sound_.volume) / 127.0f, 0.0f, 1.0f);

    return nwsnd::RevolutionPolicy::TrackVolume(tr.vol.Get(), tr.volume2.Get(), main_volume_.Get(), tr.handle_volume,
                                                sound) *
           options_.volume;
}

int Performer::EffectSend(int t, int bus) const
{
    const Track& tr = tracks_[t];
    const std::array<uint8_t, 3> track_sends = {tr.fx_send_a, tr.fx_send_b, tr.fx_send_c};
    const double send =
        track_sends[static_cast<std::size_t>(bus)] / 127.0 + options_.fx_sends[static_cast<std::size_t>(bus)];

    return static_cast<int>(std::lround(127.0 * std::clamp(send, 0.0, 1.0)));
}

void Performer::StartSweep(Note& n)
{
    // SequenceTrack::NoteOn has given the channel its sweep (Channel::SetSweepParam, 0x022C487C): the track's sweep
    // pitch, plus the distance from the porta key with portamento, over the note's length or over the porta time.
    n.sweep = 0.0f;
    n.sweep_length = 0;
    n.sweep_in_ms = false;
    n.sweep_ticks = 0;
    n.sweep_frame = frame_;

    const auto take_sweep = [&](auto& core)
    {
        if (settling_channel_ != nwsnd::kNoChannel)
        {
            const auto& channel = core.Channels().At(settling_channel_);
            n.sweep = channel.sweep_pitch;
            n.sweep_length = channel.sweep_length;
            n.sweep_in_ms = channel.auto_sweep;
        }
    };
    WithCore(take_sweep);
}

bool Performer::NoteOn(int t, int key, int velocity, int32_t length, bool drop)
{
    // SequenceTrack::NoteOn (0x022C9A4C).
    Track& tr = tracks_[t];

    // At most TickNoteCap() new channels can sound in one tick, fewer with stereo waves. The cap avoids thousands of
    // MIDI notes from loops without waits. Legatos past Settings::voices in one tick continue their existing MIDI note,
    // with or without --voices.
    if (tr.note_tick != tick_)
    {
        tr.note_tick = tick_;
        tr.tick_notes = 0;
        tr.tick_legatos = 0;
    }

    const int vel = velocity * tr.velocity_range / 127;
    midi_.OnNoteOnStart(*this, t);

    // Tie/mono continue the newest channel without another attack (see Legato for MIDI handling).
    if (!tr.notes.empty() && (tr.tie || tr.mono))
    {
        Note& head = tr.notes.back();
        if (!tr.mono || !head.released)
        {
            return Legato(t, head, key, vel, length, ++tr.tick_legatos > settings_.voices);
        }

        StopNote(t, head); // nw::snd stops a releasing channel and starts a new one
        Ended(head);
        tr.notes.pop_back();
    }

    // The game found no free channel for the new note (FollowNoChannel).
    if (drop)
    {
        return false;
    }

    // Bank::NoteOn (0x022C4A0C) finds the new channel's region and wave, or leaves it silent.
    const VelocityRegion* region = banks_.FindRegion(tr.bank_slot, tr.program, key, vel);
    if (!region)
    {
        return true;
    }

    const Pcm* pcm = banks_.GetWave(tr.bank_slot, region->wave_id_index);
    if (!pcm)
    {
        return true;
    }

    if (++tr.tick_notes > TickNoteCap())
    {
        const std::string what = "more than " + std::to_string(TickNoteCap()) + TickCapReport(settings_.engine);
        midi_.Approximate(what.c_str());
        return true;
    }

    const Note n = NewNote(t, key, vel, length, *region, *pcm);

    // Release other channels in the key group at rate 126. SoundFont uses exclusiveClass for this. From 2008, the Wii
    // releases each of them at its release rate (Mario Kart Wii, 0x8009bfcc).
    if (n.key_group != 0)
    {
        for (Note& other : tr.notes)
        {
            if (other.key_group == n.key_group)
            {
                ReleaseNote(t, other, Wii() ? -1 : kKeyGroupRelease);
            }
        }
    }

    tr.notes.push_back(n);
    notes_started_++;
    midi_.OnNoteOn(*this, t, tr.notes.back(), vel, region, n.stereo);

    return true;
}

Note Performer::NewNote(int t, int key, int vel, int32_t length, const VelocityRegion& region, const Pcm& pcm)
{
    const Track& tr = tracks_[t];
    Note n;
    n.settings = &settings_;
    n.key = static_cast<uint8_t>(key);
    n.length = tr.tie ? -1 : length;
    n.on_frame = frame_;
    n.ignore_note_off = region.ignore_note_off;
    n.key_group = region.key_group;
    n.bank_slot = tr.bank_slot;
    n.program = tr.program;
    n.init_pan = tr.init_pan;
    n.stereo = pcm.channels.size() > 1;
    const float init_volume = static_cast<float>(vel) / 127.0f;
    n.init_volume = init_volume * init_volume * (static_cast<float>(region.volume) / 127.0f);
    n.env = OverrideEnvelope(RegionEnvelope(region), TrackPresetKey(tr));

    n.midi.quiet_ms = n.QuietMs();
    n.midi.faded_ms = n.FadedMs();

    n.midi.volume = region.volume;
    n.region_volume = region.volume;
    if (!pcm.loop)
    {
        const float ratio = nwsnd::CalcPitchRatio((key - region.original_key) * 256) * region.pitch * options_.pitch;
        n.one_shot = true;
        WithCore([&](auto& core) { n.core_end = core.PredictEnd(frame_, pcm.Length(), ratio, pcm.sample_rate); });
    }

    StartSweep(n);

    return n;
}

bool Performer::Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap)
{
    Track& tr = tracks_[t];
    const bool heard = !BelowHearing(n);
    n.key = static_cast<uint8_t>(key);
    if (tr.mono)
    {
        n.length = length;
    }

    // The channel takes the velocity squared as its volume (the Wii's Note::init_volume).
    const float init_volume = static_cast<float>(velocity) / 127.0f;
    n.init_volume = init_volume * init_volume;
    n.env = OverrideEnvelope(n.env, TrackPresetKey(tr));
    StartSweep(n);

    // A legato on a channel below hearing changes nothing that can be heard. Count every other legato as a change,
    // regardless of MIDI representation: that can differ between passes.
    if (heard)
    {
        tr.last_change = tick_;
    }

    return midi_.OnLegato(*this, t, n, key, velocity, past_cap, heard);
}

void Performer::NoteVariableRead(int t, uint8_t index)
{
    if (index < 32)
    {
        variable_readers_[index] |= static_cast<uint16_t>(1u << t);
    }
}

void Performer::NoteVariableChange(int t, uint8_t index)
{
    tracks_[t].last_variable_change = commands_run_;
    if (index >= 32)
    {
        return;
    }

    for (int i = 0; i < kTrackCount; i++)
    {
        if (i != t && (variable_readers_[index] >> i & 1) && tracks_[i].open && !WaitsForever(i))
        {
            tracks_[t].last_signal = commands_run_;
            return;
        }
    }
}

void Performer::MuteNotes(int t, uint8_t mode)
{
    Track& tr = tracks_[t];
    if (mode == 2)
    {
        ReleaseAndDetach(t);
    }
    else if (mode == 3)
    {
        midi_.OnStopAll(*this, t);
        for (Note& n : tr.notes)
        {
            n.released = true;
            n.release_end_ms = now_ms_;
            Ended(n);
        }

        tr.notes.clear();
    }
}

bool Performer::Leavable(int t, uint64_t first_command) const
{
    const Track& tr = tracks_[t];
    const auto call = [](const nwsnd::CallStackEntry& e)
    {
        return !e.is_loop;
    };
    if (std::none_of(tr.calls.begin(), tr.calls.begin() + tr.call_depth, call) || tr.leave_at <= first_command ||
        tr.note_at > first_command)
    {
        return false;
    }

    // A note of kHeldTicks or more on a looping wave sounds until the game stops it. A note below hearing doesn't
    // count.
    const auto sustained = [&](const Note& n)
    {
        return !n.released && !n.one_shot && !BelowHearing(n);
    };
    return !tr.holds || std::ranges::none_of(tr.notes, sustained);
}

void Performer::NumberCalls(int t)
{
    Track& tr = tracks_[t];
    tr.numbered_calls = std::min(tr.numbered_calls, tr.call_depth);
    while (tr.numbered_calls < tr.call_depth)
    {
        tr.call_ids[tr.numbered_calls++] = ++call_serial_;
    }
}

uint64_t Performer::InnermostCall(int t) const
{
    const Track& tr = tracks_[t];
    for (int i = tr.call_depth - 1; i >= 0; i--)
    {
        if (!tr.calls[i].is_loop)
        {
            return tr.call_ids[i];
        }
    }

    return 0;
}

bool Performer::ReturnedFrom(int t, uint64_t call) const
{
    const Track& tr = tracks_[t];
    return call != 0 && std::find(tr.call_ids.begin(), tr.call_ids.begin() + tr.call_depth, call) ==
                            tr.call_ids.begin() + tr.call_depth;
}

void Performer::OnLoop(int t, uint32_t target, bool jump)
{
    if (const std::optional<uint32_t> start = CountedPassStart(t, target, jump))
    {
        MarkPass(t, target, *start);
    }
}

std::optional<uint32_t> Performer::CountedPassStart(int t, uint32_t target, bool jump)
{
    // A backward jump to unvisited commands is not a loop; shared code may precede its callers. Also ignore passes
    // without waits, such as random retries or variable polling. A jump back to commands that last ran in a call that
    // the track has since returned from doesn't end a pass either. A random retry can jump into another subroutine's
    // copy of its draw. That copy last ran in an earlier pass.
    const auto run = last_run_[t].find(target);
    Track& tr = tracks_[t];
    const std::optional<uint32_t> last_wait = tr.last_wait;
    if (run == last_run_[t].end() || !last_wait || *last_wait < run->second.tick || ReturnedFrom(t, run->second.call))
    {
        return std::nullopt;
    }

    // A loop that the track jumps back to during a pass of another loop is an inner loop, such as a countdown of waits
    // between the notes of an outer loop. The outer loop repeats the inner loop with everything else. Later passes of
    // an inner loop therefore don't count. The first pass of the outer loop shows which loops are inner. These passes,
    // and passes that can leave their loop (below), don't set Track::idle. It keeps the value from the other passes of
    // the track. An inner loop counts again once the track has played no pass of another loop for kMinLoopSeconds. The
    // outer loop may then take minutes to repeat. A countdown can compare a counter that no command resets with a
    // constant. Its later passes then run until the counter wraps.
    const uint32_t start = run->second.tick;
    const uint64_t first_command = run->second.order;
    if (InnerLoopPass(t, target, first_command))
    {
        return std::nullopt;
    }

    // A pass that changed nothing is idle: the track only waited, polled or counted (ChangedSince).

    // A jump back inside a subroutine isn't a loop when its pass ran a conditional command that can leave the loop and
    // played no note. A subroutine that polls a variable after a one-shot sound does that. A stricter rule would leave
    // out every pass that runs a conditional command. But a game may loop a melody, or move the pitch of a note it has
    // played, in such a subroutine until it stops the sound. And a conditional setvar can't leave the loop.
    // Such a pass is still idle when it changed nothing, changed no variable and drew no random number. The track then
    // does the same on every pass until another track or the game changes a variable. A track that polls the game's
    // variables once a tick, while later tracks play the music, does that for as long as the sound plays. A countdown
    // between a track's notes changes its counter. Its passes keep Track::idle as the track's other passes set it.
    if (jump && Leavable(t, first_command))
    {
        if (!ChangedSince(t, start, first_command) && tr.last_variable_change <= first_command &&
            last_draw_order_ <= first_command)
        {
            tr.idle = true;
        }

        return std::nullopt;
    }

    tr.last_pass_ms = now_ms_;
    tr.idle = !ChangedSince(t, start, first_command);

    // Use the lowest-numbered track that can still advance: track 0 while active, or a later track for continuing sound
    // effects. A pass counts only if no earlier track contributed during it. The marked section then contains only
    // repeating material. A track whose last pass was idle doesn't count as advancing. An idle pass of this track
    // counts only if no later track advances or contributed either. A later track that has been quiet for kHoldSeconds
    // doesn't count as advancing: it may wait minutes in silence before it loops or closes.
    auto plays_on = [&](int i)
    {
        return tracks_[i].open && !WaitsForever(i) && !tracks_[i].idle && (i < t || !Quiet(i));
    };
    for (int i = 0; i < kTrackCount; i++)
    {
        if (i != t && (i < t || tr.idle) && (plays_on(i) || ChangedSince(i, start, first_command)))
        {
            return std::nullopt;
        }
    }

    // A pass that started before the game's last change of a variable can play otherwise than the passes after it
    // (ChangeVariables). It doesn't count.
    if (start < changed_at_)
    {
        return std::nullopt;
    }

    return start;
}

bool Performer::InnerLoopPass(int t, uint32_t target, uint64_t first_command)
{
    Track& tr = tracks_[t];
    for (const auto& [other, at] : tr.jump_backs)
    {
        if (other != target && at > first_command)
        {
            tr.inner_loops.insert(other);
        }
    }

    tr.jump_backs[target] = commands_run_;
    if (tr.inner_loops.contains(target))
    {
        if (now_ms_ - tr.last_pass_ms < kMinLoopSeconds * 1000.0)
        {
            return true;
        }

        tr.inner_loops.erase(target);
    }

    return false;
}

bool Performer::ChangedSince(int t, uint32_t start, uint64_t first_command) const
{
    // Changing a variable that another open track reads counts as a change (Track::last_signal). It belongs to the
    // pass if it came after the pass's first command. The tick doesn't show that: a loop's last commands run in the
    // tick that its next pass starts in.
    const Track& tr = tracks_[t];
    return (tr.last_change && *tr.last_change >= start) || (tr.last_signal && *tr.last_signal > first_command);
}

void Performer::MarkPass(int t, uint32_t target, uint32_t start)
{
    Track& tr = tracks_[t];

    // A pass that changed something, of another loop than the marked one, means the marked pass doesn't repeat what
    // plays now. That happens when an earlier track has gone idle while a later track's loop plays on. Mark this pass
    // instead. An idle pass counts only while nothing else changes. Any idle pass then repeats the sound. Such a pass
    // therefore keeps the marked loop. It doesn't when the pass plays on the track of the marked loop, or when that
    // track has closed. The marked loop then no longer plays.
    const std::pair<int, uint32_t> owner{t, target};
    const std::optional<std::pair<int, uint32_t>>& marked = main_loop_.Owner();
    const int marked_track = marked ? marked->first : t;
    if (!marked || (*marked != owner && (!tr.idle || marked_track == t || !tracks_[marked_track].open)))
    {
        main_loop_.Restart(midi_.out_, owner);
    }

    main_loop_.CountPass(midi_.out_, owner, start, tick_);

    // The performance stops at a pass that repeats, once it has played the passes asked for and kMinLoopSeconds. It
    // plays on while a change of a variable is still to come. The change can change what the loop plays.
    const bool may_stop = midi_.out_.loops >= options_.loops &&
                          now_ms_ >= kMinLoopSeconds * 1000.0 - settings_.early_ticks_ms &&
                          next_change_ == options_.changes.size() && ProcedureChangesArmed();

    // A silent pass that ran a conditional command that can leave its loop can be a tick of a countdown between the
    // notes of an outer loop. The outer loop's first pass shows such a loop as inner (CountedPassStart). When that pass
    // ends after kMinLoopSeconds, the countdown's passes count first. A copy of the performer therefore plays on for
    // kMinLoopSeconds to see whether the loop turns inner. If it does, it's inner from now on, and the performance
    // plays on.
    const auto run = last_run_[t].find(target);
    if (may_stop && run != last_run_[t].end() && tr.leave_at > run->second.order && tr.note_at <= run->second.order &&
        TurnsInner(t, target))
    {
        tr.inner_loops.insert(target);
        return;
    }

    if (may_stop && MarkSteadyPass(t, target))
    {
        stop_ = true;
        stopped_by_ = t;
        held_sound_end_ms_ = HeldSoundEnd(start);
    }
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    if (now_ms_ - tick_ms_[start] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // The last change has to lie more than kHoldSeconds back.
    const uint32_t last = LastChangeTick();
    if (now_ms_ - tick_ms_[last] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // Every sounding note must be endless, unreleased and the newest note of an open track with tie or mono on.
    bool sounding = false;
    for (const Track& tr : tracks_)
    {
        for (const Note& n : tr.detached)
        {
            if (n.EndMs() > now_ms_)
            {
                return std::nullopt;
            }
        }

        for (std::size_t k = 0; k < tr.notes.size(); k++)
        {
            const Note& n = tr.notes[k];
            if (n.EndMs() <= now_ms_ || BelowHearing(n))
            {
                continue;
            }
            if (!tr.open || !(tr.tie || tr.mono) || n.released || n.EndMs() < kNever || k + 1 != tr.notes.size())
            {
                return std::nullopt;
            }

            sounding = true;
        }
    }
    if (!sounding)
    {
        return std::nullopt;
    }

    return tick_ms_[last] + kHoldSeconds * 1000.0;
}

uint32_t Performer::LastChangeTick() const
{
    if (settings_.reverb_moves_loops)
    {
        return midi_.out_.LastEventTick();
    }

    // The last tempo change, and each track's last event that isn't CC91.
    const auto is_change = [](const TrackEvent& e)
    {
        return e.kind != EventKind::kReverb;
    };
    uint32_t last = midi_.out_.tempo.empty() ? 0 : midi_.out_.tempo.back().tick;
    for (const std::vector<TrackEvent>& events : midi_.out_.tracks)
    {
        const auto change = std::find_if(events.rbegin(), events.rend(), is_change);
        if (change != events.rend())
        {
            last = std::max(last, change->tick);
        }
    }

    return last;
}

bool Performer::SilentLoop() const
{
    const uint32_t start = midi_.out_.loop->start;
    for (const std::vector<TrackEvent>& events : midi_.out_.tracks)
    {
        const auto starts_note = [start](const TrackEvent& e)
        {
            return e.kind == EventKind::kNoteOn && e.tick >= start;
        };
        if (std::ranges::any_of(events, starts_note))
        {
            return false;
        }
    }

    // A sunk channel (MidiNote::sunk) has ended its MIDI note, and its sound ends at its fade.
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.midi.on || n.SoundEndMs() >= kNever)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

void Performer::AllocateTracks()
{
    for (int t = 0; t < kTrackCount; t++)
    {
        tracks_[t].Init();
        tracks_[t].allocated = (sound_.allocate_track_flags >> t) & 1;
    }

    // The Wii allocates only the tracks that the sound's flags select (SeqPlayer::Setup, main.dol 0x8003e1f4).
    if (Wii())
    {
        return;
    }

    tracks_[0].allocated = true;

    // A leading alloctrack (0xfe, 16-bit mask) also allocates tracks. The start offset can be 0xffffffff. Guard against
    // addition wrapping.
    const std::size_t start = sound_.start_offset;
    if (start < seq_.data.size() && seq_.data.size() - start > 2 && seq_.data[start] == 0xfe)
    {
        const uint32_t first = seq_.data[start + 1], second = seq_.data[start + 2];
        const uint32_t mask = ArgumentsBigEndian() ? first << 8 | second : second << 8 | first;
        for (int t = 0; t < kTrackCount; t++)
        {
            if ((mask >> t) & 1)
            {
                tracks_[t].allocated = true;
            }
        }
    }
}

template <typename Core>
void Performer::MakeCore(Regions& banks, int channels, int voices)
{
    Core& core = core_.template emplace<Core>(banks, channels, voices);

    // The seed is the random state when the game starts the sound. On the Switch, a sound starts in the second sound
    // frame after that: the first takes the start to the sound thread. The state steps once a frame
    // (SoundThread::FrameProcess, 0xc18590). The first tick draws from the state one step on.
    core.Rng() = nwsnd::Random(options_.seed);
    for (int i = 0; i < settings_.random_steps_before_start; i++)
    {
        core.Rng().Next();
    }

    core.GlobalVariables().fill(Wii() ? 0 : -1);
    if constexpr (std::is_same_v<Core, RevolutionCore>)
    {
        core.build_ = settings_.wii_build;
    }
}

void Performer::StartTick()
{
    ChangeVariables();

    // The core begins its tick: SequenceSoundPlayer::ParseNextTick (0x022B70AC) moves the main volume a tick on before
    // the tracks' ticks. The performer takes the main volume from the core.
    const auto begin_tick = [&](auto& core)
    {
        core.now_ms_ = now_ms_;
        core.GetPlayer().BeginTick();
        core_open_ = false;
        main_volume_ = core.GetPlayer().MainVolume();
    };
    WithCore(begin_tick);
}

void Performer::ChangeVariables()
{
    // A write in a user procedure waits from its frame for a call. A pass that started before the frame plays without
    // the write, and the passes after it can take the write. As after a change, the earlier pass doesn't count.
    const std::vector<ProcedureChange>& waits = options_.procedure_changes;
    if (armed_procedure_changes_ < waits.size() && waits[armed_procedure_changes_].frame <= frame_)
    {
        while (armed_procedure_changes_ < waits.size() && waits[armed_procedure_changes_].frame <= frame_)
        {
            ++armed_procedure_changes_;
        }
        changed_at_ = tick_;
        main_loop_.Restart(midi_.out_, std::nullopt);
    }

    const std::vector<VariableChange>& changes = options_.changes;
    if (next_change_ == changes.size() || changes[next_change_].frame > frame_)
    {
        return;
    }

    // The core and the performer take each change of a player or a global variable.
    while (next_change_ < changes.size() && changes[next_change_].frame <= frame_)
    {
        const VariableChange& change = changes[next_change_++];
        if (change.index < 32)
        {
            *Variable(0, change.index) = change.value;
            WithCore([&](auto& core) { core.GetPlayer().SetVariable(core, change.index, change.value); });
        }
        else if (change.index < 48)
        {
            WithCore([&](auto& core) { SetTrackVariables(core, change.index, change.value); });
        }
    }

    // The passes of the main loop so far can play otherwise than the passes after the change. The search for the main
    // loop starts again with the first pass that starts after it (OnLoop).
    changed_at_ = tick_;
    main_loop_.Restart(midi_.out_, std::nullopt);
}

void Performer::ChangeInProcedure(uint16_t procedure)
{
    const std::vector<ProcedureChange>& changes = options_.procedure_changes;
    bool changed = false;
    for (std::size_t i = 0; i < armed_procedure_changes_; ++i)
    {
        const ProcedureChange& change = changes[i];
        if (procedure_taken_[i] || change.procedure != procedure)
        {
            continue;
        }

        procedure_taken_[i] = true;
        changed = true;
        if (change.index < 32)
        {
            *Variable(0, change.index) = change.value;
            WithCore([&](auto& core) { core.GetPlayer().SetVariable(core, change.index, change.value); });
        }
    }

    // As after a change before a frame (ChangeVariables).
    if (changed)
    {
        changed_at_ = tick_;
        main_loop_.Restart(midi_.out_, std::nullopt);
    }
}

void Performer::RunUserProcedure(int t, uint16_t procedure)
{
    if (!options_.user_procedures)
    {
        return;
    }

    // The core holds the variables as the track's commands left them. The performer takes them from it after each
    // command.
    std::array<int16_t, 16> local = player_vars_;
    std::array<int16_t, 16> global = global_vars_;
    std::array<int16_t, 16> track = tracks_[t].vars;
    const auto take_variables = [&](auto& core)
    {
        local = core.GetPlayer().LocalVariables();
        global = core.GlobalVariables();
        if (const auto* core_track = core.GetPlayer().Track(t))
        {
            track = core_track->Variables();
        }
    };
    WithCore(take_variables);

    const std::array<std::array<int16_t, 16>, 3> before = {local, global, track};
    if (options_.user_procedures(procedure, local, global, track))
    {
        last_draw_ = tick_;
        last_draw_order_ = commands_run_;
    }

    const std::array<const std::array<int16_t, 16>*, 3> after = {&local, &global, &track};
    for (std::size_t set = 0; set < after.size(); ++set)
    {
        for (std::size_t i = 0; i < 16; ++i)
        {
            const int16_t value = (*after[set])[i];
            if (value == before[set][i])
            {
                continue;
            }

            const auto index = static_cast<uint8_t>(set * 16 + i);
            *Variable(t, index) = value;
            const auto set_variable = [&](auto& core)
            {
                if (index < 32)
                {
                    core.GetPlayer().SetVariable(core, index, value);
                }
                else
                {
                    core.GetPlayer().SetTrackVariable(t, index - 32, value);
                }
            };
            WithCore(set_variable);
            NoteVariableChange(t, index);
        }
    }
}

bool Performer::ParseTracks(int first)
{
    bool any_open = false;
    WithCore([&](auto& core) { any_open = ParseCoreTracks(core, first); });

    return any_open;
}

template <typename Core>
bool Performer::ParseCoreTracks(Core& core, int first)
{
    core.performer_ = this;
    bool any_open = false;
    for (int t = first; t < kTrackCount && !stop_; t++)
    {
        // The core's tick of the track counts its channels' lengths down (FollowLengths), moves the track's ramps on
        // and runs the commands. The performer takes the ramps from the core (FollowCommandStart).
        Track& tr = tracks_[t];
        const bool was_open = tr.open;
        const uint64_t commands = commands_run_;
        counting_lengths_ = true;
        const bool open = core.GetPlayer().RunTrackTick(core, t);
        counting_lengths_ = false;
        core_open_ = open || core_open_;

        // A pass that stops the performance stops in the middle of the track's tick, at a jump (Run). The core's track
        // stops at the same command (StopsTick). The performer's track keeps its state.
        if (stop_)
        {
            break;
        }

        any_open = FinishCoreTrackTick(core, t, was_open, open, commands_run_ != commands) || any_open;
    }

    return any_open;
}

template <typename Core>
void Performer::ContinueCoreTrack(Core& core, int t)
{
    core.performer_ = this;
    const uint64_t commands = commands_run_;
    const bool open = core.GetPlayer().ContinueTrackTick(core, t);
    core_open_ = open || core_open_;
    FinishCoreTrackTick(core, t, true, open, commands_run_ != commands);
}

template <typename Core>
bool Performer::FinishCoreTrackTick(Core& core, int t, bool was_open, bool open, bool ran)
{
    SettleNote(false);
    Track& tr = tracks_[t];
    if (was_open && !open)
    {
        CloseTrack(t);
        tr.allocated = false; // the player frees a finished track
    }
    else if (ran)
    {
        // A track that ran its commands up to a wait waits again. One that stopped at the parse limit reads on in the
        // next tick.
        const auto* track = core.GetPlayer().Track(t);
        if (track && (track->Wait() != 0 || track->NoteFinishWait()))
        {
            tr.last_wait = tick_;
        }
    }

    TakeCoreTrack(core, t);
    TakeCorePlayer(core);

    return was_open && open;
}

void Performer::FollowCommandStart(int t, uint32_t offset, uint32_t command, bool conditional)
{
    SettleNote(false);
    if (stop_)
    {
        return;
    }

    Track& tr = tracks_[t];
    NumberCalls(t);
    last_run_[t][offset] = {tick_, commands_run_++, InnermostCall(t)};

    // The command runs on in the track's stretch of commands, or begins the next stretch (Performance::stretches).
    std::vector<Stretch>& stretches = midi_.out_.stretches[t];
    tr.command = offset;
    if (tr.next_stretch)
    {
        stretches.push_back({.tick = tick_,
                             .first = offset,
                             .last = offset,
                             .entry = *tr.next_stretch,
                             .depth = static_cast<uint8_t>(tr.call_depth),
                             .note_wait = tr.note_wait});
        tr.next_stretch.reset();
    }
    else
    {
        stretches.back().last = offset;
    }

    // A conditional jump, call, return or fin can leave a loop (see OnLoop).
    if (conditional && (command == 0x89 || command == 0x8a || command == 0xfd || command == 0xff))
    {
        tr.leave_at = commands_run_;
    }

    // The core moved the track's ramps on at the start of its tick. The note before the command set the track's waits,
    // and the command's start has ended the wait for a held note.
    const auto take_ramps_and_waits = [&](auto& core)
    {
        TakeCoreRamps(core, t);
        TakeCoreWaits(core, t);
    };
    WithCore(take_ramps_and_waits);
}

void Performer::FollowCommand(int t, uint32_t command, int32_t arg1, int32_t arg2, uint32_t next)
{
    // The extended command 0xe0 calls into the game. The performer records nothing for it. The model of the game's
    // procedure runs (RunUserProcedure), and then the game's writes in the procedure take effect (ChangeInProcedure).
    if (!stop_ && command == 0xf0e0)
    {
        RunUserProcedure(t, static_cast<uint16_t>(arg1));
        ChangeInProcedure(static_cast<uint16_t>(arg1));
    }
    if (stop_ || command == 0xf0e0)
    {
        return;
    }

    tracks_[t].current = next;
    WithCore([&](auto& core) { RecordCoreCommand(core, t, command, arg1, arg2); });

    // A command that moved the track elsewhere ends the track's stretch of commands.
    if (tracks_[t].current != next)
    {
        tracks_[t].next_stretch = EntryAfter(command);
    }
}

void Performer::ClosePass(int t, uint32_t cmd, int32_t arg1)
{
    const Track& tr = tracks_[t];
    if (cmd == 0x89 && static_cast<uint32_t>(arg1) < tr.current)
    {
        OnLoop(t, static_cast<uint32_t>(arg1), true);
    }
    if (cmd == 0xfc && tr.call_depth != 0)
    {
        // The Wii takes the top of the call stack as the loop, whether a loop start or a call put it there
        // (kLoopEndIgnoresCalls in nwsnd's policies). A counted loop in endless_loops_ closes a pass like a loop of
        // count 0.
        const nwsnd::CallStackEntry& e = tr.calls[tr.call_depth - 1];
        if ((e.is_loop || Wii()) && (e.loop_count == 0 || endless_loops_.contains({t, e.address})))
        {
            OnLoop(t, e.address, false);
        }
    }
}

void Performer::RecordVariableCommand(int t, uint32_t op, uint8_t index, bool changed)
{
    if (op >= 0x90)
    {
        NoteVariableRead(t, index);
    }
    if (changed)
    {
        NoteVariableChange(t, index);
    }
}

template <typename Core>
void Performer::RecordCoreCommand(Core& core, int t, uint32_t cmd, int32_t arg1, int32_t arg2)
{
    // A jump back, and the end of an endless loop, close a pass of a loop (OnLoop). The analysis takes the track's
    // position and loops from before the command.
    Track& tr = tracks_[t];
    ClosePass(t, cmd, arg1);

    // A variable command's variable before the command, and the track's LFOs.
    const uint32_t op = cmd & 0xff;
    const bool lfo_command = (op >= 0xa0 && op <= 0xb1) || (op >= 0xe1 && op <= 0xe6);
    const uint8_t index = static_cast<uint8_t>(arg1);
    const int16_t* variable = cmd > 0xff && !lfo_command ? Variable(t, index) : nullptr;
    const int16_t before = variable ? *variable : 0;
    const PresetKey preset_before = TrackPresetKey(tr);

    TakeCoreTrack(core, t);
    TakeCorePlayer(core);
    if (cmd > 0xff)
    {
        if (variable)
        {
            RecordVariableCommand(t, op, index, *variable != before);
        }

        return;
    }

    switch (cmd)
    {
    case 0x88: // opentrack: a track that reached FIN was freed and can't be opened again
        if (arg1 < kTrackCount && arg1 != t && tracks_[arg1].allocated)
        {
            CloseTrack(arg1);
            OpenTrack(arg1, static_cast<uint32_t>(arg2));
            TakeCoreTrack(core, arg1);
        }
        break;

    case 0xb0: // timebase: the Wii's build of October 2006 has no such command
        if (!WiiOctober2006())
        {
            midi_.OnTimebase(*this);
        }
        break;

    case 0xb2:
        if (tr.mono)
        {
            ReleaseAndDetach(t);
        }
        break;

    case 0xc8:
        ReleaseAndDetach(t);
        break;

    case 0xb4:
    case 0xb5:
    case 0xd8:
        SetFilter(t);
        break;

    case 0xbd:
    case 0xbe:
    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xe0:
    case 0xe4:
        if (!SameLfo(TrackPresetKey(tr), preset_before))
        {
            midi_.GiveStruckNotesLfo(*this, t);
        }
        break;

    case 0xdd:
        MuteNotes(t, static_cast<uint8_t>(arg1));
        break;

    case 0xe1:
        midi_.OnTempo(*this);
        break;

    default:
        break;
    }

    midi_.OnCommand(*this, t, cmd, arg1);
}

void Performer::FollowNoteOn(int t, nwsnd::ChannelId channel, int key)
{
    if (pending_note_ && pending_note_->t == t)
    {
        settling_channel_ = channel;
        SettleNote(false);
        settling_channel_ = nwsnd::kNoChannel;
    }

    if (note_to_pair_ == t && !tracks_[t].notes.empty())
    {
        tracks_[t].notes.back().core_channel = channel;
        tracks_[t].notes.back().core_key = key;
    }
    note_to_pair_ = -1;
}

void Performer::FollowLegato(int t, nwsnd::ChannelId channel)
{
    if (pending_note_ && pending_note_->t == t)
    {
        settling_channel_ = channel;
        SettleNote(false);
        settling_channel_ = nwsnd::kNoChannel;
    }
}

void Performer::FollowNoChannel(int t)
{
    if (pending_note_ && pending_note_->t == t)
    {
        SettleNote(VoiceLimit() > 0);
    }
}

void Performer::FollowChannelDrop(int t, nwsnd::ChannelId channel)
{
    if (stop_ || VoiceLimit() == 0)
    {
        return;
    }

    // A note past its track's cap for one tick takes a voice from a note that the performer keeps. NoteOn omits the
    // newer note: the cap keeps a tick's first notes. Where a new note takes the voice of the lowest priority, the game
    // keeps the newest (TickCapReport).
    if (pending_note_)
    {
        const Track& taker = tracks_[pending_note_->t];
        const int started = taker.note_tick == tick_ ? taker.tick_notes : 0;
        if (started >= TickNoteCap())
        {
            return;
        }
    }

    // The track's newest note on the channel. An older note's channel has ended, and a newer channel can take its
    // place.
    auto& notes = tracks_[t].notes;
    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    const auto found = std::find_if(notes.rbegin(), notes.rend(), on_channel);
    if (found != notes.rend())
    {
        StopNote(t, *found);
        found->core_channel = nwsnd::kNoChannel;
    }
}

void Performer::FollowGainAboveFull(int t, nwsnd::ChannelId channel)
{
    Note* note = NewestNoteOn(t, channel);
    if (note == nullptr || note->midi.lfo_above_full)
    {
        return;
    }

    note->midi.lfo_above_full = true;
    midi_.Approximate(note->midi.lfo_fit != VolumeLfoFit{}
                          ? "volume LFO above full level (limited in the game; the SoundFont and SFZ swing less, from "
                            "a lower level)"
                          : "volume LFO above full level (limited in the game; the SoundFont and SFZ swing above it)");
}

void Performer::FollowVolumeLfo(int t, nwsnd::ChannelId channel, float envelope_db, float lfo_db)
{
    // A note without a MIDI note has id 0.
    const Note* note = NewestNoteOn(t, channel);
    if (note == nullptr || note->midi.id == 0)
    {
        return;
    }

    const Track& tr = tracks_[t];
    const int depth = LfoDepth(tr.lfos[0]);
    const double scale = depth != 0 ? 2.0 * ModulationValue(tr.lfos[0], tr.lfo_types[0]) / depth : 0.0;
    midi_.OnVolumeLfo(note->midi.id, envelope_db, lfo_db, scale);
}

Note* Performer::NewestNoteOn(int t, nwsnd::ChannelId channel)
{
    if (t < 0 || static_cast<std::size_t>(t) >= tracks_.size())
    {
        return nullptr;
    }

    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    for (std::vector<Note>* list : {&tracks_[t].notes, &tracks_[t].detached})
    {
        const auto found = std::find_if(list->rbegin(), list->rend(), on_channel);
        if (found != list->rend())
        {
            return &*found;
        }
    }

    return nullptr;
}

template <typename Core>
const typename Core::Channel* Performer::CoreChannelOf(Core& core, const Note& n)
{
    if (n.core_channel == nwsnd::kNoChannel || n.core_key < 0)
    {
        return nullptr;
    }

    // A freed channel's place can hold a newer channel. The note's channel still plays the voice with the note's key.
    const auto& channels = core.Channels();
    const auto& in_use = channels.InUse();
    if (std::find(in_use.begin(), in_use.end(), n.core_channel) == in_use.end())
    {
        return nullptr;
    }

    const auto& channel = channels.At(n.core_channel);
    if (channel.voice == nwsnd::kNoVoice)
    {
        return nullptr;
    }

    return core.PositionKey(channel.voice) == n.core_key ? &channel : nullptr;
}

template <typename Core>
void Performer::TakeCoreWaves(Core& core)
{
    for (Track& tr : tracks_)
    {
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                const auto end = n.core_key >= 0 ? core.PositionOf(n.core_key) : std::nullopt;
                if (end)
                {
                    n.one_shot = end->one_shot;
                    n.core_end = end->frame;
                }
            }
        }
    }
}

template <typename Core>
void Performer::TakeCoreLfos(Core& core, uint64_t frame)
{
    if (midi_.earlier_.lfo_changes.empty())
    {
        return;
    }

    for (Track& tr : tracks_)
    {
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                const auto* channel = n.midi.lfo_bends ? CoreChannelOf(core, n) : nullptr;
                if (!channel)
                {
                    continue;
                }

                TakeNoteLfo(core, n, *channel, frame);
            }
        }
    }
}

template <typename Core>
void Performer::TakeNoteLfo(Core& core, Note& n, const typename Core::Channel& channel, uint64_t frame)
{
    // Channel::Update adds the pitch LFO's value to the frame's pitch before the LFO steps on. A copy of
    // the LFO gives the value. The copy draws from a copy of the random generator. The core's generator
    // stays as it was. A random curve's numbers come from that copy. They can differ from those in the
    // game. A note reports such a curve once, when the curve first moves the note's pitch.
    float value = 0.0f;
    if (channel.lfo_types[0] == 0)
    {
        auto lfo = channel.lfos[0];
        nwsnd::Random random = core.Rng();
        value = lfo.GetValue(random);
        if (lfo.param.curve == 4 && value != 0.0f && !n.midi.lfo_random)
        {
            n.midi.lfo_random = true;
            midi_.Approximate("random LFO curve in pitch bends (its numbers can differ from those in the game)");
        }
    }

    // The note keeps the values from frame_ - 1 on.
    const uint64_t kept = frame_ > 0 ? frame_ - 1 : 0;
    if (n.lfo_pitch.empty() || n.lfo_frame + n.lfo_pitch.size() != frame)
    {
        n.lfo_pitch.clear();
        n.lfo_frame = frame;
    }
    else if (n.lfo_frame < kept)
    {
        const uint64_t gone = std::min<uint64_t>(kept - n.lfo_frame, n.lfo_pitch.size());
        n.lfo_pitch.erase(n.lfo_pitch.begin(), n.lfo_pitch.begin() + static_cast<std::ptrdiff_t>(gone));
        n.lfo_frame += gone;
    }
    n.lfo_pitch.push_back(value);
}

template <typename Core>
void Performer::RunCoreTail(Core& core, uint64_t until)
{
    const auto playing = [this, &core](uint64_t frame)
    {
        for (const Track& tr : tracks_)
        {
            for (const auto* list : {&tr.notes, &tr.detached})
            {
                for (const Note& n : *list)
                {
                    if (n.core_key >= 0 && n.one_shot && !n.WaveEndedBy(frame))
                    {
                        return true;
                    }
                    if (n.midi.lfo_bends && static_cast<double>(frame) * settings_.frame_ms < n.EndMs() &&
                        CoreChannelOf(core, n))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    };

    for (uint64_t frame = frame_; frame < until && playing(frame); frame++)
    {
        if (frame > frame_)
        {
            core.BeginFrame();
        }
        if (core.GetPlayer().IsActive())
        {
            core.GetPlayer().UpdateTrackParams(core);
        }
        TakeCoreLfos(core, frame);
        core.FinishFrame();
        TakeCoreWaves(core);
    }
}

void Performer::FollowRandomDraw()
{
    if (stop_)
    {
        return;
    }

    last_draw_ = tick_;
    midi_.out_.draws = true;
    last_draw_order_ = commands_run_;
}

void Performer::FollowVariableRead(int t, int index)
{
    if (stop_)
    {
        return;
    }

    NoteVariableRead(t, static_cast<uint8_t>(index));
}

void Performer::FollowNoteCommand(int t, int key, int velocity, int32_t length)
{
    if (stop_)
    {
        return;
    }

    // The note waits for the core's verdict on its channel (SettleNote). The performer takes the track's waits after
    // the note from the core (TakeCoreWaits).
    const Track& tr = tracks_[t];
    note_to_pair_ = -1;
    if (!tr.mute)
    {
        const auto stretch = static_cast<uint32_t>(midi_.out_.stretches[t].size() - 1);
        pending_note_ = PendingNote{t, key, velocity, length, {tr.command, stretch}};
    }
}

void Performer::SettleNote(bool drop)
{
    if (!pending_note_)
    {
        return;
    }

    // A note that NoteOn starts waits for the core's channel (FollowNoteOn).
    const PendingNote note = *pending_note_;
    pending_note_.reset();
    settling_command_ = note.command;
    const uint64_t started = notes_started_;
    if (NoteOn(note.t, note.key, note.velocity, note.length > 0 ? note.length : -1, drop))
    {
        tracks_[note.t].note_at = commands_run_;
    }
    if (notes_started_ != started)
    {
        note_to_pair_ = note.t;
    }
}

template <typename Core>
void Performer::TakeCoreTrack(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.open = track->IsOpen();
    tr.current = track->Current();
    TakeCoreWaits(core, t);
    const auto calls = track->CallStack();
    tr.call_depth = static_cast<int>(calls.size());
    std::ranges::copy(calls, tr.calls.begin());

    tr.tie = track->Tie();
    tr.mono = track->Mono();
    tr.bank_slot = track->BankIndex();
    tr.program = track->ProgramNumber();
    TakeCoreRamps(core, t);
    tr.bend_range = track->BendRange();
    tr.init_pan = track->InitPan();
    tr.lfos = track->LfoParams();
    tr.lfo_types = track->LfoTypes();

    tr.attack = track->Attack();
    tr.decay = track->Decay();
    tr.sustain = track->Sustain();
    tr.release = track->Release();
    tr.hold = static_cast<uint8_t>(track->Hold());
    tr.main_send = track->MainSend();
    tr.fx_send_a = track->FxSends()[0];
    tr.fx_send_b = track->FxSends()[1];
    tr.fx_send_c = track->FxSends()[2];
    tr.handle_volume = track->HandleVolume();

    // The core keeps the filters' values as nw::snd uses them. The performer keeps the commands' arguments.
    tr.low_pass = static_cast<int32_t>(track->Lpf() * 64.0f) + 0x40;
    tr.biquad_type = static_cast<uint8_t>(track->BiquadType());
    tr.biquad_value = static_cast<int32_t>(std::lround(track->BiquadValue() * 127.0f));

    tr.note_wait = track->NoteWait();
    tr.mute = track->IsMuted();
    tr.damper = track->Damper();
    tr.velocity_range = track->VelocityRange();
    tr.vars = track->Variables();
}

template <typename Core>
void Performer::TakeCoreRamps(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.vol = track->Volume();
    tr.volume2 = track->Volume2();
    tr.pan = track->Pan();
    tr.bend = track->Bend();
}

template <typename Core>
void Performer::TakeCoreWaits(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.wait = track->Wait();
    tr.note_finish_wait = track->NoteFinishWait();
    tr.holds = track->Holds();
    tr.held_wait = track->HeldWait();
}

template <typename Core>
void Performer::TakeCorePlayer(Core& core)
{
    const auto& player = core.GetPlayer();
    tempo_ = player.Tempo();
    timebase_ = player.Timebase();
    main_volume_ = player.MainVolume();
    player_vars_ = player.LocalVariables();
    global_vars_ = core.GlobalVariables();
}

void Performer::FinishTick()
{
    WithCore([&](auto& core) { core.GetPlayer().EndTick(core, core_open_); });

    for (int t = 0; t < kTrackCount; t++)
    {
        midi_.OnTickEnd(*this, t);
        if (!tracks_[t].open)
        {
            PruneNotes(t);
        }
    }

    // The Wii's build of October 2006 stops a released note once its volume times its envelope is 0, whether a track
    // owns it or not (Channel::Update, 0x800e039c in Smooth Moves' main.dol). The later builds stop a note that a track
    // owns only when its envelope is silent. The channels update after the frame's last tick. The performer stops only
    // the notes that a track owns.
    if (WiiOctober2006() && frame_ticks_left_ == 0)
    {
        for (int t = 0; t < kTrackCount; t++)
        {
            const float user = UserVolume(t);
            for (Note& n : tracks_[t].notes)
            {
                if (n.released && n.EndMs() > now_ms_ && n.init_volume * user == 0.0f)
                {
                    StopNote(t, n);
                }
            }
        }
    }
}

void Performer::ReleasePending(const TickTime& next)
{
    if (!settings_.note_off_waits_for_update || next.frame == frame_)
    {
        return;
    }

    const uint64_t frame = frame_ + 1;
    const double after = (static_cast<double>(frame) * settings_.frame_ms - now_ms_) / (next.ms - now_ms_);
    for (int t = 0; t < kTrackCount; t++)
    {
        for (Note& n : tracks_[t].notes)
        {
            if (!n.release_pending)
            {
                continue;
            }

            n.release_pending = false;
            n.released = true;
            n.released_frame = frame;
            n.release_level = HeldLevel(n.env, 1, settings_);
            n.release_rate = DecayRate(n.env.release, settings_.engine);
            n.release_end_ms =
                static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate, settings_) + 1) *
                settings_.frame_ms;
            tracks_[t].last_change = tick_;
            midi_.OnDeferredRelease(*this, t, n, after);
        }
    }
}

void Performer::AdvanceTo(const TickTime& next)
{
    // The core ends the last tick's frame, plays the frames without a tick, and begins the next tick's frame.
    const auto play_frames = [&](auto& core)
    {
        for (uint64_t frame = frame_; frame < next.frame; frame++)
        {
            if (frame > frame_)
            {
                core.BeginFrame();
            }
            if (core.GetPlayer().IsActive())
            {
                core.GetPlayer().UpdateTrackParams(core);
            }
            TakeCoreLfos(core, frame);
            core.FinishFrame();
        }

        TakeCoreWaves(core);
        core.BeginFrame();
    };
    if (next.frame > frame_)
    {
        WithCore(play_frames);
    }

    ReleasePending(next);
    midi_.OnBetweenTicks(*this, next);

    frame_ = next.frame;
    now_ms_ = next.ms;
    if (WiiOctober2006())
    {
        frame_ticks_left_ = next.ticks_left;
        nominal_ms_ = next.nominal_ms;
    }

    tick_++;
    midi_.RecordTickTime(*this);
}

Performance Performer::Run()
{
    // The game's player opens track 0, if the sound allocates it, at the sound's start offset (on the Wii,
    // SeqPlayer::Start, main.dol 0x8003e504). The others always allocate it.
    AllocateTracks();
    if (tracks_[0].allocated)
    {
        OpenTrack(0, sound_.start_offset);
    }

    midi_.OnTempo(*this);
    midi_.RecordTickTime(*this);
    StartCore();

    bool finished = false;
    double steady_since = -1.0;
    PassStart next_pass;
    for (;;)
    {
        StartTick();
        const std::array<int16_t, 16> globals = global_vars_;
        const bool any_open = ParseTracks(0);
        RecordGlobalChanges(globals);
        if (stop_)
        {
            finished = EndAtStoppingPass(next_pass);
            break;
        }

        FinishTick();
        if (const std::optional<bool> ended = EndAfterTick(any_open, steady_since))
        {
            finished = *ended;
            break;
        }

        const std::optional<TickTime> next = NextTick();
        if (!next)
        {
            midi_.rec_.Warn(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                           : "tempo 0: the sequence stops advancing");
            PlayOutStopped();
            break;
        }

        AdvanceTo(*next);
    }

    EndPerformance(finished);
    if (midi_.out_.loop)
    {
        RestoreAtLoopStart(midi_.out_, midi_.rec_.PartSwitches(), next_pass, LoopRulesOf(settings_));
    }

    midi_.out_.approximations = midi_.rec_.Approximations();
    midi_.out_.warnings = midi_.rec_.Warnings();
    for (int i = 0; i < 16; i++)
    {
        midi_.out_.globals_read |= static_cast<uint16_t>(variable_readers_[16 + i] != 0 ? 1u << i : 0u);
    }

    return midi_.out_;
}

bool Performer::EndAtStoppingPass(PassStart& next_pass)
{
    // A main loop that sounds nothing is no loop. The sequence ends with its last sound, like one that ends.
    if (midi_.out_.loop && !held_sound_end_ms_ && SilentLoop())
    {
        silent_loop_ = true;
        midi_.out_.DropLoop();
        return true;
    }
    if (midi_.out_.loop && !held_sound_end_ms_)
    {
        next_pass = tick_ > midi_.out_.loop->end ? PassStartAt(midi_.out_.loop->end)
                                                 : NextPassStart(stopped_by_, tracks_[stopped_by_].current);
    }

    return false;
}

std::optional<bool> Performer::EndAfterTick(bool any_open, double& steady_since)
{
    // Every track has ended. The game repeats none of the passes before the end, such as a state machine's jumps back
    // to its dispatcher. Drop the loop marked on the way. On the Wii, a sequence also ends when no track will read on
    // and no note sounds (Stuck).
    if (!any_open || (Wii() && Stuck()))
    {
        midi_.out_.DropLoop();
        return true;
    }

    // A performance with a stop plays to it (SetStop).
    if (steady_since < 0 && !stop_ms_ && Steady())
    {
        steady_since = now_ms_;
    }
    if (steady_since >= 0 && now_ms_ - steady_since >= kHoldSeconds * 1000.0)
    {
        midi_.out_.holds = true;
        return false;
    }

    if (now_ms_ >= kMaxSeconds * 1000.0)
    {
        return EndAtTimeLimit();
    }
    if (stop_ms_ && now_ms_ >= *stop_ms_)
    {
        midi_.out_.DropLoop();
        return false;
    }

    return std::nullopt;
}

void Performer::RecordGlobalChanges(const std::array<int16_t, 16>& before)
{
    for (int i = 0; i < 16; i++)
    {
        if (global_vars_[i] != before[i])
        {
            midi_.out_.global_changes.push_back(
                {.frame = frame_, .index = static_cast<uint8_t>(16 + i), .value = global_vars_[i]});
        }
    }
}

void Performer::StartCore()
{
    // The core's sound starts in the first tick's frame. The core's clock gives the first tick at the frame's start.
    // The core allocates the tracks that AllocateTracks does. The core alone would allocate only those that the sound's
    // flags select. A sound archive's flags give the tracks of the sequence's alloctrack.
    SoundInfo info = sound_;
    info.allocate_track_flags = 0;
    for (int t = 0; t < kTrackCount; t++)
    {
        info.allocate_track_flags |= tracks_[t].allocated ? 1u << t : 0u;
    }

    const auto start = [&](auto& core)
    {
        core.Play(info, seq_.data, 0);
        core.GetPlayer().SetBuild(core.Build());
        core.GetPlayer().SetTempoRatio(options_.tempo_ratio);
        core.GetPlayer().SetPitch(options_.pitch);
        for (const auto& [index, value] : options_.variables)
        {
            if (index < 32)
            {
                core.GetPlayer().SetVariable(core, static_cast<int>(index), value);
            }
        }
        for (const TrackVariable& variable : options_.track_variables)
        {
            core.GetPlayer().SetTrackVariable(variable.track, variable.index, variable.value);
        }

        // The frame starts the sound, and the player allocates its tracks. The game's code then silences the tracks
        // that PerformOptions::tracks leaves out, gives the others their volumes, and sets the tracks' variables.
        core.BeginFrame();
        core.GetPlayer().SetTrackVolume(~static_cast<uint32_t>(options_.tracks) & 0xffffu, 0.0f);
        for (std::size_t t = 0; t < kSequenceTracks; ++t)
        {
            const uint32_t bit = 1u << t;
            if ((options_.tracks & bit) != 0 && options_.track_volumes[t] != 1.0f)
            {
                core.GetPlayer().SetTrackVolume(bit, options_.track_volumes[t]);
            }
        }
        for (const auto& [index, value] : options_.variables)
        {
            if (index >= 32 && index < 48)
            {
                SetTrackVariables(core, index, value);
            }
        }
        core.GetPlayer().StartClock(1);
        core.GetPlayer().NextTickTime();
        player_vars_ = core.GetPlayer().LocalVariables();
        global_vars_ = core.GlobalVariables();
    };
    WithCore(start);

    // The Wii's build of October 2006 starts its count at a whole tick, and its first frame runs one tick.
    if (WiiOctober2006())
    {
        frame_ticks_left_ = std::get<RevolutionCore>(core_).GetPlayer().ClockTicksLeft();
    }
}

bool Performer::EndAtTimeLimit()
{
    // Treat a sequence that has been silent for longer than kHoldSeconds at the time limit as finished. Such a
    // sequence ends with its last sound and, like a sequence that ends, has no main loop.
    bool sounding = false;
    for (int t = 0; t < kTrackCount; t++)
    {
        sounding = sounding || OthersSounding(t, nullptr);
    }
    const bool finished = !sounding && now_ms_ - sound_end_ms_ > kHoldSeconds * 1000.0;
    midi_.out_.truncated = !finished;

    // When the performance is cut here, the outermost loop that each open track is in, where that loop is a counted one
    // (LimitLoops). The length analysis takes the same loops (nwsnd::OutermostCountedLoops). calls[0] is the outermost
    // entry. A track whose outermost loop is endless loops already, and keeps its inner loops. An inner loop still
    // ends, and repeats in each pass of the outer loop. A performance that counts as finished ends with its last sound.
    for (int t = 0; midi_.out_.truncated && t < kTrackCount; t++)
    {
        const Track& tr = tracks_[t];
        for (int i = 0; tr.open && i < tr.call_depth; i++)
        {
            if (tr.calls[i].is_loop)
            {
                if (tr.calls[i].loop_count != 0)
                {
                    limit_loops_.insert({t, tr.calls[i].address});
                }
                break;
            }
        }
    }

    // A sequence that finishes drops its loop.
    if (finished)
    {
        midi_.out_.DropLoop();
    }

    return finished;
}

Performer::PassStart Performer::PassStartAt(uint32_t tick) const
{
    return sequence_export::PassStartAt(midi_.out_, midi_.rec_.PartSwitches(), tick);
}

Performer Performer::ResumeAfterJump(int t, uint32_t target) const
{
    // Track `t` has jumped back, and the tracks after it haven't run this tick. The copy runs them with no limit on the
    // loops. No pass then stops the copy again.
    Performer probe = *this;
    probe.stop_ = false;
    probe.options_.loops = std::numeric_limits<int>::max();
    probe.tracks_[t].current = target;
    probe.WithCore([&](auto& core) { probe.ContinueCoreTrack(core, t); });
    probe.ParseTracks(t + 1);
    probe.FinishTick();

    return probe;
}

bool Performer::PlayNextTick()
{
    const std::optional<TickTime> next = NextTick();
    if (!next)
    {
        return false;
    }

    AdvanceTo(*next);
    StartTick();
    const bool any_open = ParseTracks(0);
    FinishTick();

    return any_open;
}

Performer Performer::PlayOn(int t, uint32_t target, uint32_t until) const
{
    Performer probe = ResumeAfterJump(t, target);
    while (probe.tick_ + 1 < until)
    {
        if (!probe.PlayNextTick())
        {
            break;
        }
    }

    return probe;
}

bool Performer::TurnsInner(int t, uint32_t target) const
{
    // The copy stops at the first tick that shows the loop inner.
    Performer probe = ResumeAfterJump(t, target);
    const double end_ms = now_ms_ + kMinLoopSeconds * 1000.0;
    while (probe.now_ms_ < end_ms && !probe.tracks_[t].inner_loops.contains(target))
    {
        if (!probe.PlayNextTick())
        {
            break;
        }
    }

    return probe.tracks_[t].inner_loops.contains(target);
}

Performer::PassStart Performer::NextPassStart(int t, uint32_t target) const
{
    return PlayOn(t, target, tick_ + 1).PassStartAt(tick_);
}

bool Performer::RepeatsAsTheGame(uint32_t start, uint32_t end) const
{
    return sequence_export::RepeatsAsTheGame(midi_.out_, midi_.rec_.PartSwitches(), start, end, LoopRulesOf(settings_));
}

bool Performer::MarkSteadyPass(int t, uint32_t target)
{
    // A copy of this performer plays on from the jump back. Its passes are compared. A later run of the sequence makes
    // the choices of the first (Perform). The first run compared its passes without the details that later runs give
    // the notes it played, such as the attacks of early releases (PassFindings). Notes that only a copy played lack
    // those details.
    auto play_on = [&](uint32_t until)
    {
        return [probe = PlayOn(t, target, until)](uint32_t start, uint32_t end)
        {
            return probe.RepeatsAsTheGame(start, end);
        };
    };
    return main_loop_.MarkSteadyPass(midi_.out_, last_draw_, options_.loops, play_on);
}

std::optional<TickTime> Performer::NextTick()
{
    std::optional<TickTime> next;
    WithCore([&](auto& core) { next = NextCoreTick(core); });

    return next;
}

template <typename Core>
std::optional<TickTime> Performer::NextCoreTick(Core& core)
{
    // The clock goes on from the last tick through the frames after it, a frame at a time, until a tick falls in one.
    auto& player = core.GetPlayer();
    TickTime next{frame_};
    std::optional<uint64_t> left = player.NextTickTime();
    while (!left)
    {
        if (player.ClockStopped())
        {
            return std::nullopt;
        }

        next.frame++;
        player.StartClock(1);
        left = player.NextTickTime();
    }

    // On the Wii, the tick's time comes from the milliseconds left in its frame, in the clock's floats. The build of
    // October 2006 runs its ticks at the frame's start.
    if constexpr (std::is_same_v<Core, RevolutionCore>)
    {
        next.ms = static_cast<double>(next.frame) * settings_.frame_ms +
                  (settings_.frame_ms - static_cast<double>(player.ClockRestMs()));
        if (WiiOctober2006())
        {
            next.ticks_left = player.ClockTicksLeft();
            next.nominal_ms = nominal_ms_ + MidiMsPerTick();
        }
    }
    else
    {
        const uint64_t frame_units = FrameUnits(settings_);
        next.ms = UnitsToMs(next.frame * frame_units + (frame_units - *left), settings_);
    }

    return next;
}

void Performer::PlayOutStopped()
{
    // The frame's channel update makes the note-offs still pending. Their MIDI note-offs come at most a tick after the
    // last, at the MIDI tempo. As in EndPerformance, the core's frames finish the one-shot waves.
    const double ticks_per_second = midi_.out_.tempo.back().bpm * midi_.out_.timebase / 60.0;
    const double next_frame_ms = static_cast<double>(frame_ + 1) * settings_.frame_ms;
    ReleasePending({frame_ + 1, std::max(now_ms_ + 1000.0 / ticks_per_second, next_frame_ms)});
    WithCore([&](auto& core) { RunCoreTail(core, FrameOf(now_ms_ + kMaxTailMs) + 1); });

    double end_ms = now_ms_;
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                midi_.out_.holds = midi_.out_.holds || n.EndMs() >= kNever;
                end_ms = std::max(end_ms, std::min(n.EndMs(), now_ms_ + kMaxTailMs));
            }
        }
    }
    if (midi_.out_.holds)
    {
        end_ms = now_ms_ + kHoldSeconds * 1000.0;
    }

    // MIDI ticks continue after sequence timing stops. Millisecond sweeps keep moving; note-length sweeps remain frozen
    // with the sequence's ticks (UpdateOutputs).
    const auto ticks = static_cast<uint32_t>(std::ceil((end_ms - now_ms_) / 1000.0 * ticks_per_second));
    const double start_ms = now_ms_;
    for (uint32_t i = 1; i <= ticks; i++)
    {
        const double ms = start_ms + i / ticks_per_second * 1000.0;
        midi_.SweepBetweenTicks(*this, {FrameOf(ms), ms});

        tick_++;
        now_ms_ = ms;
        if (Wii())
        {
            nominal_ms_ += 1000.0 / ticks_per_second;
        }
        frame_ = FrameOf(now_ms_);

        for (int t = 0; t < kTrackCount; t++)
        {
            PruneNotes(t); // finished notes have no active sweep
            midi_.UpdateOutputs(*this, t);
        }
    }
}

void Performer::EndPerformance(bool finished)
{
    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers. Nothing changed in
    // the ticks after that.
    if (held_sound_end_ms_)
    {
        tick_ = std::min(MsToTick(*held_sound_end_ms_), tick_);
        now_ms_ = tick_ms_[tick_];
        midi_.out_.holds = true;
        midi_.out_.DropLoop();
    }

    // Stop immediately at a loop boundary, time limit or held-sound limit. A naturally finished sequence ends with its
    // final sound, including release tails; nw::snd can leave finished tracks waiting long after silence. The core's
    // frames finish the one-shot waves.
    if (finished)
    {
        WithCore([&](auto& core) { RunCoreTail(core, FrameOf(now_ms_ + kMaxTailMs) + 1); });
    }

    EndNotes(finished);
    if (finished)
    {
        SetFinishedEnd();
    }
    else
    {
        midi_.out_.end_tick = tick_;
        midi_.out_.seconds = now_ms_ / 1000.0;
    }

    // Without Settings::reverb_moves_loops, a change of CC91 doesn't move the end. A MIDI track ends after its last
    // event. The changes of CC91 after the end are therefore dropped. Nothing sounds there.
    if (!settings_.reverb_moves_loops)
    {
        const uint32_t end = midi_.out_.end_tick;
        const auto after_end = [end](const TrackEvent& e)
        {
            return e.kind == EventKind::kReverb && e.tick > end;
        };
        for (std::vector<TrackEvent>& events : midi_.out_.tracks)
        {
            std::erase_if(events, after_end);
        }
    }
}

void Performer::EndNotes(bool finished)
{
    for (int t = 0; t < kTrackCount; t++)
    {
        Track& tr = tracks_[t];
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                // On the Wii, a note below hearing ends the sequence's sound where it has faded (MidiNote::faded_ms).
                // Stuck can end a sequence there while its tracks wait out held notes.
                if (finished)
                {
                    const double end = Wii() && BelowHearing(n) ? n.midi.faded_ms : n.SoundEndMs();
                    sound_end_ms_ = std::max(sound_end_ms_, std::min(end, now_ms_ + kMaxTailMs));
                }
                midi_.MidiNoteOff(*this, t, n);
            }
        }
    }

    // The tails' timed sweeps go on after the last tick, and their pitch events follow its note-offs.
    if (finished)
    {
        SweepAfterEnd();
    }

    for (Track& tr : tracks_)
    {
        tr.notes.clear();
        tr.detached.clear();
    }
}

void Performer::SetFinishedEnd()
{
    // After a main loop that sounded nothing (SilentLoop), what comes after the last sound can't be heard.
    if (silent_loop_)
    {
        const uint32_t end = MsToTick(sound_end_ms_);
        for (std::vector<TrackEvent>& events : midi_.out_.tracks)
        {
            std::erase_if(events, [end](const TrackEvent& e) { return e.tick > end; });
        }
        std::erase_if(midi_.out_.tempo, [end](const Performance::Tempo& t) { return t.tick > end; });
    }
    else if (EndsAtLastSound(sound_end_ms_ / 1000.0, tick_ms_[LastChangeTick()] / 1000.0))
    {
        // Changes that come more than kHoldSeconds after the last sound can't be heard either (EndsAtLastSound).
        // The performance then ends with the sound. A note below hearing that sounds on keeps its note-off, at the
        // end.
        const uint32_t end = MsToTick(sound_end_ms_);
        seqlib::midi::ClampNoteOffs(midi_.out_, end);
        seqlib::midi::CutAt(midi_.out_, end);
        std::erase_if(midi_.out_.tempo, [end](const Performance::Tempo& t) { return t.tick > end; });
    }

    const uint32_t last_change = LastChangeTick();
    midi_.out_.end_tick = std::max(last_change, MsToTick(sound_end_ms_));
    midi_.out_.seconds = std::max(sound_end_ms_, tick_ms_[last_change]) / 1000.0;
}

uint32_t Performer::MsToTick(double ms) const
{
    const auto it = std::lower_bound(tick_ms_.begin(), tick_ms_.end(), ms);
    if (it != tick_ms_.end())
    {
        return static_cast<uint32_t>(it - tick_ms_.begin());
    }

    // Extrapolate at the last tempo, or the MIDI tempo if sequence timing stopped.
    const double per_tick = MsPerTick() > 0 ? MsPerTick() : MidiMsPerTick();
    return static_cast<uint32_t>(tick_ms_.size() - 1 + std::ceil((ms - tick_ms_.back()) / per_tick));
}

} // namespace ntdlib::sequence_export
