// SPDX-License-Identifier: MIT

// Tests of a policy that gives the channel's whole frame (UpdateChannel): the frame runs in place of the template's
// frame, and a channel that it stops goes back to the pool. A stand-in voice has the methods that a channel calls, and
// it plays nothing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "xsflib/testing/check.h"

namespace
{

class TestEngine;

// The Wii U's steps, with the policy's frame (see the definition of UpdateChannel below).
struct FramePolicy : ntdlib::nwsnd::CafePolicy
{
    using Engine = TestEngine;

    struct WaveInfo
    {
        uint32_t format = 0;
        uint32_t sample_rate = 32000;
        bool loop = false;
    };

    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, 3> fx_send{};
    };

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }

    static void UpdateChannel(ntdlib::nwsnd::Channel<FramePolicy>& channel, Engine& engine, bool periodic);
};

using Channel = ntdlib::nwsnd::Channel<FramePolicy>;
using ChannelManager = ntdlib::nwsnd::ChannelManager<FramePolicy>;
using ntdlib::nwsnd::ChannelId;
using ntdlib::nwsnd::VoiceId;

// A voice with the methods that a channel calls. It plays nothing. The template's frame would also call a voice's
// setters. A policy's frame calls only the methods that its voices have.
struct StandInVoice
{
    void SetSampleFormat(uint32_t)
    {
    }

    void SetSampleRate(uint32_t)
    {
    }

    void SetInterpolationType(uint8_t)
    {
    }

    void AppendWave(TestEngine&, const FramePolicy::WaveInfo&)
    {
    }

    void Start()
    {
    }

    void Stop(TestEngine&)
    {
    }

    void Free(TestEngine&)
    {
    }

    void SetPriority(TestEngine&, uint32_t)
    {
    }
};

// The voices: a new one for each channel.
class StandInVoices
{
public:
    VoiceId AllocVoice(TestEngine&, int, uint32_t, ChannelId)
    {
        voices_.push_back({});
        return static_cast<VoiceId>(voices_.size() - 1);
    }

    StandInVoice& At(VoiceId place)
    {
        return voices_[static_cast<std::size_t>(place)];
    }

private:
    std::vector<StandInVoice> voices_;
};

// An engine with `channels` channels. It records the channels' callbacks.
class TestEngine
{
public:
    // A callback from a channel to the engine.
    struct Event
    {
        int track = -1;
        Channel::CallbackStatus status = Channel::CallbackStatus::kStopped;
    };

    explicit TestEngine(int channels) : channels_(channels)
    {
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    StandInVoices& Voices()
    {
        return voices_;
    }

    ntdlib::nwsnd::Random& Rng()
    {
        return random_;
    }

    void ChannelEvent(int, int track, Channel&, Channel::CallbackStatus status)
    {
        events.push_back({track, status});
    }

    // A note on track `track`: a channel that plays a wave for ever.
    Channel* Note(int track)
    {
        Channel* channel = channels_.AllocChannel(*this, 1, 64, 0, track);
        if (channel)
        {
            channel->Start(*this, FramePolicy::WaveInfo{}, -1);
        }

        return channel;
    }

    std::vector<Event> events;

private:
    ChannelManager channels_;
    StandInVoices voices_;
    ntdlib::nwsnd::Random random_;
};

// The frames that FramePolicy's UpdateChannel has run.
int frames = 0;

// FramePolicy's frame: it counts the frames, and stops the channel at its second periodic one.
void FramePolicy::UpdateChannel(Channel& channel, Engine& engine, bool periodic)
{
    ++frames;
    if (periodic && frames == 2)
    {
        channel.Stop(engine);
    }
}

void TestFrame()
{
    // A policy's UpdateChannel runs in place of the template's frame. The template's frame would have moved the
    // envelope. A channel that the policy's frame stops goes back to the pool, and the engine receives a callback with
    // kStopped.
    TestEngine engine(4);
    const Channel* channel = engine.Note(1);
    const float level = channel->envelope.Level();

    engine.Channels().UpdateAllChannel(engine);

    xsflib::testing::Check(frames == 1 && channel->envelope.Level() == level && engine.Channels().ActiveCount() == 1,
                           "frame: the policy's frame runs in place of the template's frame");

    engine.Channels().UpdateAllChannel(engine);

    xsflib::testing::Check(frames == 2 && engine.Channels().ActiveCount() == 0 && engine.events.size() == 1 &&
                               engine.events[0].track == 1 &&
                               engine.events[0].status == Channel::CallbackStatus::kStopped,
                           "frame: a channel that the policy's frame stops is free");
}

} // namespace

int main()
{
    TestFrame();

    return xsflib::testing::Summary("nwsnd_channel");
}
