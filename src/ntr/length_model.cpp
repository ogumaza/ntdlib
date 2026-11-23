// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/length_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "xsflib/rip/length.h"

#include "ntdlib/first_sample.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::ntr
{
namespace
{

// A measure's timing, and the outermost counted loops that the sequence ran in at its end.
struct Measured
{
    std::optional<xsflib::rip::Timing> timing;
    std::set<snd::LoopPlace> counted_loops;
};

// Measures `sequence` once with `rules`, with `commands` after the start and the lists of `tick_commands` once their
// ticks come. The counted loops are read only at the limit.
Measured MeasureOnce(const sdat::Archive& archive, const SequenceRef& sequence, const GameSettings& settings,
                     const snd::CommandList& commands, const std::vector<TickCommands>& tick_commands,
                     const snd::Build& build, const xsflib::rip::LengthRules& rules,
                     const std::set<snd::LoopPlace>& endless_loops)
{
    ArchivePlayer player(archive, {.build = build});
    player.Start(sequence, settings);
    if (!commands.empty())
    {
        player.Send(commands);
    }

    LengthModel model(player, 0, endless_loops);
    model.SendTickCommands(tick_commands);
    Measured measured{.timing = xsflib::rip::MeasureLength(model, rules)};
    if (measured.timing && measured.timing->at_limit)
    {
        measured.counted_loops = player.Driver().OutermostCountedLoops(0);
    }

    return measured;
}

} // namespace

xsflib::rip::LengthRules LengthRules()
{
    constexpr double kSilentMs = 180000.0;
    constexpr uint64_t kSoundsAfter = 2;
    xsflib::rip::LengthRules rules;
    rules.frame_ms = kFrameMs;
    rules.max_frames = static_cast<uint64_t>(rules.max_ms / kFrameMs);
    rules.silent_frames = static_cast<uint64_t>(kSilentMs / kFrameMs);
    rules.loop_offset = kSoundsAfter;
    rules.sound_offset = kSoundsAfter;
    rules.steady_offset = kSoundsAfter;

    return rules;
}

LengthModel::LengthModel(ArchivePlayer& player, int sequence_player, std::set<snd::LoopPlace> endless_loops)
    : player_(player), sequence_player_(sequence_player), endless_loops_(std::move(endless_loops))
{
}

void LengthModel::SendTickCommands(std::vector<TickCommands> tick_commands)
{
    tick_commands_ = std::move(tick_commands);
    tick_progress_ = TickListProgress(tick_commands_.size());
    if (tick_commands_.empty())
    {
        return;
    }

    player_.on_tick = [this](int player)
    {
        if (player == sequence_player_)
        {
            ++ticks_;
        }
    };
}

void LengthModel::SendDueLists()
{
    const bool finished = tick_progress_.SendDue(tick_commands_, frames_ * snd::kFrameClocks, ticks_,
                                                 [this](const snd::CommandList& list) { player_.Send(list); });
    if (finished)
    {
        // The search for the main loop starts again after a last list, as after a change of a variable.
        ++loop_search_;
    }
}

bool LengthModel::RunFrame()
{
    SendDueLists();
    samples_.clear();
    output_.clear();
    player_.RunFrame(samples_);
    AppendPcm16(samples_, output_);
    ++frames_;

    return true;
}

uint64_t LengthModel::Frames() const
{
    return frames_;
}

bool LengthModel::Sounded() const
{
    return xsflib::rip::Audible(output_);
}

std::optional<double> LengthModel::FirstSampleMs() const
{
    return ntdlib::FirstSampleMs(output_, 1000.0 / kOutputRate);
}

bool LengthModel::Ringing() const
{
    return player_.Unit().CapturesRun() && Sounded();
}

std::size_t LengthModel::VoiceCount() const
{
    return snd::kChannelCount;
}

xsflib::rip::VoicePlace LengthModel::Voice(std::size_t index) const
{
    // The capture buffer's loop on channels 1 and 3 (see VoiceCount).
    if ((index == 1 || index == 3) && player_.Unit().CapturesRun() && player_.Driver().ActiveChannels() == 0)
    {
        return {};
    }

    const SoundUnit::ChannelPlace place = player_.Unit().PlaceOf(static_cast<int>(index));
    return {place.playing, place.loops, static_cast<int64_t>(place.position)};
}

bool LengthModel::Active() const
{
    return player_.Driver().GetPlayer(sequence_player_).active;
}

bool LengthModel::Settled() const
{
    return player_.Driver().IsSettled(sequence_player_);
}

bool LengthModel::Stuck() const
{
    return player_.Driver().IsStuck(sequence_player_);
}

bool LengthModel::Moving() const
{
    return false;
}

uint64_t LengthModel::ChangedAt() const
{
    return player_.Driver().ChangedAt(sequence_player_);
}

bool LengthModel::Busy() const
{
    return player_.Busy();
}

bool LengthModel::ChannelsChanging() const
{
    return player_.Driver().AnyChannelChanging();
}

int LengthModel::ActiveChannels() const
{
    return player_.Driver().ActiveChannels();
}

bool LengthModel::OthersLooped(int track) const
{
    if (!tick_progress_.Done(tick_commands_) || looped_in_search_ != loop_search_)
    {
        return false;
    }

    const snd::Driver& driver = player_.Driver();
    for (int i = 0; i < snd::kTracksPerPlayer; ++i)
    {
        if (i != track && driver.IsTrackOpen(sequence_player_, i) && !driver.IsTrackIdle(sequence_player_, i) &&
            !driver.WaitsForEver(sequence_player_, i) &&
            passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
        {
            return false;
        }
    }

    return true;
}

std::optional<xsflib::rip::Timing> MeasureSequence(const sdat::Archive& archive, const SequenceRef& sequence,
                                                   const GameSettings& settings, const snd::Build& build,
                                                   const std::set<snd::LoopPlace>& endless_loops,
                                                   xsflib::rip::Limit limit, const snd::CommandList& commands,
                                                   const std::vector<TickCommands>& tick_commands)
{
    const xsflib::rip::LengthRules rules = LengthRules();
    const Measured first =
        MeasureOnce(archive, sequence, settings, commands, tick_commands, build, rules, endless_loops);
    if (!first.timing || !first.timing->at_limit)
    {
        return first.timing;
    }

    // At the limit: the counted loops that the sequence ran in become endless, and the long limit applies.
    const bool long_limit = limit == xsflib::rip::Limit::kLong;
    const std::set<snd::LoopPlace>& endless = endless_loops.empty() ? first.counted_loops : endless_loops;
    if (!long_limit && endless == endless_loops)
    {
        return first.timing;
    }

    return MeasureOnce(archive, sequence, settings, commands, tick_commands, build,
                       long_limit ? xsflib::rip::WithLongLimit(rules) : rules, endless)
        .timing;
}

} // namespace ntdlib::ntr
