// SPDX-License-Identifier: MIT

// Definitions that the parts of the performer share (performer.cpp, performer_passes.cpp, performer_tick.cpp,
// performer_core.cpp and performer_run.cpp): the helpers of the console's timing and rules, and the hooks of the core.
// The five files build as one translation unit. The helpers therefore keep internal linkage. Only those files include
// this header.

#pragma once

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

} // namespace ntdlib::sequence_export
