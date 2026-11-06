// SPDX-License-Identifier: MIT

#include "ntdlib/dse/length_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "xsflib/rip/length.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::dse
{

xsflib::rip::LengthRules LengthRules()
{
    constexpr double kSilentMs = 180000.0;
    xsflib::rip::LengthRules rules;
    rules.frame_ms = kTickMs;
    rules.max_frames = static_cast<uint64_t>(rules.max_ms / kTickMs);
    rules.silent_frames = static_cast<uint64_t>(kSilentMs / kTickMs);
    return rules;
}

LengthModel::LengthModel(Player& player) : player_(player)
{
}

bool LengthModel::RunFrame()
{
    samples_.clear();
    output_.clear();
    player_.RunTick(samples_);
    ntr::AppendPcm16(samples_, output_);
    ++frames_;

    // A change of a channel's settings changes the sound, and so does an envelope that moves.
    const Driver& driver = player_.Driver();
    const auto count = static_cast<std::size_t>(driver.ChannelCount());
    if (channels_.size() != count)
    {
        channels_.resize(count);
        ++changes_;
    }
    for (std::size_t c = 0; c < count; c++)
    {
        const ChannelView view = driver.GetChannel(static_cast<int>(c));
        if (view != channels_[c])
        {
            channels_[c] = view;
            ++changes_;
        }
    }
    envelopes_moved_ = false;
    for (std::size_t v = 0; v < voices_.size(); v++)
    {
        const VoiceView view = driver.GetVoice(static_cast<int>(v));
        const VoiceView& last = voices_[v];
        if (view.allocated != last.allocated || view.started != last.started || view.releasing != last.releasing ||
            view.level != last.level)
        {
            envelopes_moved_ = true;
        }
        voices_[v] = view;
    }
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
    return voices_.size();
}

xsflib::rip::VoicePlace LengthModel::Voice(std::size_t index) const
{
    const ntr::SoundUnit::ChannelPlace place = player_.Unit().PlaceOf(static_cast<int>(index));
    return {place.playing, place.loops, static_cast<int64_t>(place.position)};
}

bool LengthModel::Active() const
{
    return player_.Driver().SequencePlays() || ActiveChannels() > 0;
}

bool LengthModel::Settled() const
{
    const Driver& driver = player_.Driver();
    for (int t = 0; t < driver.TrackCount(); t++)
    {
        if (driver.TrackPlays(t))
        {
            return false;
        }
    }
    return true;
}

bool LengthModel::Stuck() const
{
    return Settled();
}

bool LengthModel::Moving() const
{
    return false;
}

uint64_t LengthModel::ChangedAt() const
{
    return changes_;
}

bool LengthModel::Busy() const
{
    return player_.Driver().SequencePlays() || player_.BusyChannels() != 0;
}

bool LengthModel::ChannelsChanging() const
{
    return envelopes_moved_;
}

int LengthModel::ActiveChannels() const
{
    int count = 0;
    for (const VoiceView& voice : voices_)
    {
        count += voice.allocated && voice.followed ? 1 : 0;
    }
    return count;
}

bool LengthModel::OthersLooped(int track) const
{
    const Driver& driver = player_.Driver();
    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        if (static_cast<int>(t) != track && tracks_[t].loops && driver.TrackPlays(static_cast<int>(t)) &&
            tracks_[t].passes < xsflib::rip::kLengthLoops)
        {
            return false;
        }
    }
    return true;
}

LengthModel::TrackFollow& LengthModel::Follow(int track)
{
    const auto index = static_cast<std::size_t>(track);
    if (index >= tracks_.size())
    {
        tracks_.resize(index + 1);
    }
    return tracks_[index];
}

int LengthModel::MainTrack() const
{
    const Driver& driver = player_.Driver();
    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        if (tracks_[t].loops && driver.TrackPlays(static_cast<int>(t)))
        {
            return static_cast<int>(t);
        }
    }
    return -1;
}

std::optional<xsflib::rip::Timing> MeasureSequence(const SequenceFiles& files, Build build)
{
    Player player({.build = build});
    player.LoadBanks(files);
    if (!player.Start(files))
    {
        return std::nullopt;
    }
    LengthModel model(player);
    return xsflib::rip::MeasureLength(model, LengthRules());
}

} // namespace ntdlib::dse
