// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/length_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>

#include "xsflib/rip/length.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::ntr
{

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

bool LengthModel::RunFrame()
{
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

bool LengthModel::Ringing() const
{
    return false;
}

std::size_t LengthModel::VoiceCount() const
{
    return snd::kChannelCount;
}

xsflib::rip::VoicePlace LengthModel::Voice(std::size_t index) const
{
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
                                                   const std::set<snd::LoopPlace>& endless_loops)
{
    ArchivePlayer player(archive, {.build = build});
    player.Start(sequence, settings);
    LengthModel model(player, 0, endless_loops);
    const xsflib::rip::LengthRules rules = LengthRules();
    const std::optional<xsflib::rip::Timing> timing = xsflib::rip::MeasureLength(model, rules);
    if (timing && endless_loops.empty() && model.Frames() >= rules.max_frames)
    {
        if (const std::set<snd::LoopPlace> running = player.Driver().OutermostCountedLoops(0); !running.empty())
        {
            return MeasureSequence(archive, sequence, settings, build, running);
        }
    }

    return timing;
}

} // namespace ntdlib::ntr
