// SPDX-License-Identifier: MIT

// Tests of the channel and voice models on a stand-in renderer, with the policies of the Wii U and the Switch: the
// allocation of voices by priority, the drop of the lowest voice, a released channel's priority, a one-shot wave's end,
// and copies of the models. Then the steps where the Switch differs: the frame's step, the LFOs' types, a note-off
// before a channel's first update, the drop that makes room for all of a note's channels, the second try when the
// renderer has no voice left, the clamps, the receiver, and the output parameters that each mix mode takes.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/nwsnd/voice_impl.h"
#include "xsflib/testing/check.h"

namespace
{

using ntdlib::nwsnd::CafePolicy;
using ntdlib::nwsnd::NxPolicy;
using xsflib::testing::Check;

template <typename Console>
class TestEngine;

// A policy on a console's policy, with stand-ins for the waves, the outputs, the mixes and the voices' parameters. The
// stand-in has one output device. A mix is a number.
template <typename Console>
struct TestPolicy : Console
{
    using Engine = TestEngine<Console>;
    using Receiver = int;

    enum class SampleFormat : uint32_t
    {
        kPcm8 = 0,
        kPcm16 = 1,
        kDspAdpcm = 2,
    };

    struct WaveInfo
    {
        SampleFormat format = SampleFormat::kPcm16;
        uint32_t sample_rate = 32000;
        bool loop = false;
    };

    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        std::array<std::array<float, 6>, 2> mix_parameter{};
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, 3> fx_send{};
    };

    struct VoiceParam
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        uint8_t interpolation = 0;
    };

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }

    static VoiceParam InitialVoiceParam()
    {
        return {};
    }
};

template <typename Console>
using Channel = ntdlib::nwsnd::Channel<TestPolicy<Console>>;
template <typename Console>
using ChannelManager = ntdlib::nwsnd::ChannelManager<TestPolicy<Console>>;
template <typename Console>
using MultiVoice = ntdlib::nwsnd::MultiVoice<TestPolicy<Console>>;
template <typename Console>
using MultiVoiceManager = ntdlib::nwsnd::MultiVoiceManager<TestPolicy<Console>>;

// A check's message, after the console's name.
template <typename Console>
std::string Name(const std::string& what)
{
    return (std::is_same_v<Console, NxPolicy> ? "Switch: " : "Wii U: ") + what;
}

// A renderer that plays nothing, with `count` voices. The renderer hands out the first free voice and keeps the
// parameters that each voice was last given. The test sets the MultiVoice whose one-shot wave has ended.
template <typename Console>
class StandInRenderer
{
public:
    using Voice = typename MultiVoice<Console>::Voice;
    using VoiceParam = typename TestPolicy<Console>::VoiceParam;

    explicit StandInRenderer(int count) : used_(static_cast<std::size_t>(count))
    {
    }

    int Acquire(uint32_t, Voice&)
    {
        for (std::size_t i = 0; i < used_.size(); ++i)
        {
            if (!used_[i])
            {
                used_[i] = true;
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    void Free(int renderer)
    {
        used_[static_cast<std::size_t>(renderer)] = false;
    }

    bool IsAvailable(int) const
    {
        return true;
    }

    void SetPriority(int, uint32_t)
    {
    }

    void SetState(int, const Voice&)
    {
    }

    void SetParam(int renderer, const VoiceParam& param)
    {
        if (params.size() <= static_cast<std::size_t>(renderer))
        {
            params.resize(static_cast<std::size_t>(renderer) + 1);
        }

        params[static_cast<std::size_t>(renderer)] = param;
    }

    void AppendWave(ntdlib::nwsnd::VoiceId, std::span<Voice>, const typename TestPolicy<Console>::WaveInfo&)
    {
    }

    bool IsPlayFinished(ntdlib::nwsnd::VoiceId place) const
    {
        return place == finished;
    }

    float OutputVolume() const
    {
        return 1.0f;
    }

    void CalcMix(MultiVoice<Console>&) const
    {
    }

    void CalcLpf(MultiVoice<Console>&) const
    {
    }

    void CalcBiquadFilter(MultiVoice<Console>&) const
    {
    }

    std::vector<VoiceParam> params;                            // by renderer voice
    ntdlib::nwsnd::VoiceId finished = ntdlib::nwsnd::kNoVoice; // the MultiVoice whose wave has ended

private:
    std::vector<bool> used_;
};

// What a channel reported to its owner.
template <typename Console>
struct Event
{
    int track = -1;
    ntdlib::nwsnd::ChannelId place = ntdlib::nwsnd::kNoChannel;
    typename Channel<Console>::CallbackStatus status = Channel<Console>::CallbackStatus::kStopped;
};

// An engine with `channels` channels, `voices` MultiVoices and `renderer_voices` voices of the renderer: two for each
// MultiVoice unless the test gives a number. The engine keeps the channels' callbacks.
template <typename Console>
class TestEngine
{
public:
    TestEngine(int channels, int voices, int renderer_voices = -1)
        : channels_(channels), voices_(voices), renderer_(renderer_voices >= 0 ? renderer_voices : 2 * voices)
    {
    }

    ChannelManager<Console>& Channels()
    {
        return channels_;
    }

    MultiVoiceManager<Console>& Voices()
    {
        return voices_;
    }

    StandInRenderer<Console>& Renderer()
    {
        return renderer_;
    }

    ntdlib::nwsnd::Random& Rng()
    {
        return random_;
    }

    void ChannelEvent(int, int track, Channel<Console>& channel, typename Channel<Console>::CallbackStatus status)
    {
        events.push_back({track, channel.place, status});
    }

    // A note on track `track` at `priority`: a channel that plays a one-shot wave of `channels` channels, for ever.
    Channel<Console>* Note(int track, uint32_t priority, int channels = 1)
    {
        Channel<Console>* channel = channels_.AllocChannel(*this, channels, priority, 0, track);
        if (channel)
        {
            channel->Start(*this, typename TestPolicy<Console>::WaveInfo{}, -1);
        }

        return channel;
    }

    // A sound frame's work on the voices and the channels, in the game's order.
    void Frame()
    {
        voices_.UpdateAllVoiceStatus(*this);
        channels_.UpdateAllChannel(*this);
        voices_.UpdateAllVoices(*this);
    }

    // The MultiVoice that plays a channel.
    MultiVoice<Console>& VoiceOf(const Channel<Console>& channel)
    {
        return voices_.At(channel.voice);
    }

    std::vector<Event<Console>> events;

private:
    ChannelManager<Console> channels_;
    MultiVoiceManager<Console> voices_;
    StandInRenderer<Console> renderer_;
    ntdlib::nwsnd::Random random_;
};

template <typename Console>
void TestDrop()
{
    // Three channels on two voices: the third note drops the voice of the lowest priority, and its channel ends.
    TestEngine<Console> engine(3, 2);
    Channel<Console>* low = engine.Note(0, 10);
    Channel<Console>* high = engine.Note(1, 20);
    Check(low && high, Name<Console>("drop: two notes get voices"));
    const ntdlib::nwsnd::ChannelId low_place = low->place;

    Channel<Console>* third = engine.Note(2, 30);

    Check(third != nullptr, Name<Console>("drop: the third note gets a voice"));
    Check(engine.events.size() == 1 && engine.events[0].track == 0 && engine.events[0].place == low_place &&
              engine.events[0].status == Channel<Console>::CallbackStatus::kDrop,
          Name<Console>("drop: the note of the lowest priority is dropped"));
    Check(!engine.Channels().At(low_place).active, Name<Console>("drop: the dropped channel stops"));

    // A note of a lower priority than every voice gets none.
    TestEngine<Console> full(3, 2);
    full.Note(0, 30);
    full.Note(1, 30);

    Check(full.Note(2, 10) == nullptr, Name<Console>("drop: a note of lower priority gets no voice"));
}

template <typename Console>
void TestRelease()
{
    // A released channel's voice falls to priority 1, below a newer voice of a higher priority. The next drop takes it.
    TestEngine<Console> engine(3, 2);
    Channel<Console>* first = engine.Note(0, 64);
    engine.Note(1, 64);
    first->Release(engine);
    const ntdlib::nwsnd::ChannelId first_place = first->place;

    engine.Note(2, 64);

    Check(engine.events.size() == 1 && engine.events[0].place == first_place,
          Name<Console>("release: the released channel's voice is dropped first"));
}

template <typename Console>
void TestOneShot()
{
    // The wave plays to its end: the voice calls its channel back, the owner receives a callback that the wave
    // finished, and the channel is freed at the next frame.
    TestEngine<Console> engine(4, 4);
    Channel<Console>* channel = engine.Note(5, 64);
    const ntdlib::nwsnd::ChannelId place = channel->place;
    const ntdlib::nwsnd::VoiceId voice = channel->voice;

    engine.Frame();

    Check(engine.Channels().ActiveCount() == 1 && engine.Voices().ActiveCount() == 1,
          Name<Console>("one-shot: the note sounds"));
    Check(!engine.Renderer().params.empty() && engine.Renderer().params[0].pitch == 1.0f,
          Name<Console>("one-shot: the voice gets the channel's pitch"));

    engine.Renderer().finished = voice;

    engine.Frame();

    Check(engine.events.size() == 1 && engine.events[0].track == 5 && engine.events[0].place == place &&
              engine.events[0].status == Channel<Console>::CallbackStatus::kFinish,
          Name<Console>("one-shot: the owner receives a callback that the wave finished"));
    Check(engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0,
          Name<Console>("one-shot: the channel and its voice are free"));
}

template <typename Console>
void TestCopy()
{
    // The channels and the voices copy as values. Their links are places. A copy then keeps them.
    static_assert(std::is_copy_constructible_v<ChannelManager<Console>> &&
                  std::is_copy_constructible_v<MultiVoiceManager<Console>>);
    TestEngine<Console> engine(4, 4);
    engine.Note(0, 64);
    engine.Note(1, 64);
    engine.Frame();

    const ChannelManager<Console> channels = engine.Channels();
    const MultiVoiceManager<Console> voices = engine.Voices();

    Check(channels.InUse() == engine.Channels().InUse(), Name<Console>("copy: the same channels are in use"));
    bool same = true;
    for (const ntdlib::nwsnd::ChannelId place : channels.InUse())
    {
        same = same && channels.At(place).voice == engine.Channels().At(place).voice;
    }
    Check(same && voices.ActiveCount() == 2, Name<Console>("copy: each channel keeps its voice"));
}

template <typename Console>
void TestFrameStep(int ms)
{
    // A periodic update steps the sweep by a sound frame: 3 ms on the Wii U, 5 ms on the Switch.
    TestEngine<Console> engine(1, 1);
    Channel<Console>* channel = engine.Note(0, 64);
    channel->SetSweepParam(12.0f, 1000, true);

    engine.Frame();

    Check(channel->sweep_counter == ms, Name<Console>("frame step: the sweep steps by a sound frame"));
}

template <typename Console>
void TestLfoTypes(bool untyped)
{
    // A channel's LFOs start as pitch LFOs on the Wii U, and every LFO steps. On the Switch they start without a type,
    // and only an LFO with a type steps. Here LFO 0 gets a type, and LFO 1 keeps its first one.
    TestEngine<Console> engine(1, 1);
    Channel<Console>* channel = engine.Note(0, 64);
    const uint8_t first = untyped ? Channel<Console>::kUntypedLfo : 0;
    Check(channel->lfo_types[0] == first && channel->lfo_types[1] == first, Name<Console>("LFO types: the first type"));

    for (int i = 0; i < 2; ++i)
    {
        channel->lfos[i].param.depth = 1.0f;
        channel->lfos[i].param.speed = 10.0f;
        channel->lfos[i].param.curve = 2; // a saw: the value is the place in the cycle
    }

    channel->lfo_types[0] = 1;

    engine.Frame();

    ntdlib::nwsnd::Random random;
    const float typed = channel->lfos[0].GetValue(random);
    const float kept = channel->lfos[1].GetValue(random);
    Check(typed > 0.0f && (untyped ? kept == 0.0f : kept == typed),
          Name<Console>("LFO types: an LFO without a type doesn't step"));
}

template <typename Console>
void TestNoteOff(bool deferred)
{
    // A note-off before the channel's first update releases the channel at once on the Wii U. On the Switch the release
    // waits for the next periodic update.
    TestEngine<Console> engine(1, 1);
    Channel<Console>* channel = engine.Note(0, 64);

    channel->NoteOff(engine);
    const bool at_once = channel->envelope.GetStatus() == ntdlib::nwsnd::AdshrStatus::kRelease;
    engine.Frame();

    Check(at_once != deferred && channel->envelope.GetStatus() == ntdlib::nwsnd::AdshrStatus::kRelease,
          Name<Console>("note-off: the release before the first update"));

    // After the first update, a note-off releases the channel at once.
    TestEngine<Console> later(1, 1);
    Channel<Console>* note = later.Note(0, 64);
    later.Frame();

    note->NoteOff(later);

    Check(note->envelope.GetStatus() == ntdlib::nwsnd::AdshrStatus::kRelease,
          Name<Console>("note-off: the release after the first update"));
}

void TestDropForChannels()
{
    // A stereo note on the Switch drops the two voices of the lowest priority. Together they make room for the note's
    // channels. The Wii U drops the lowest voice alone.
    TestEngine<NxPolicy> engine(4, 2);
    engine.Note(0, 10);
    engine.Note(1, 20);

    Check(engine.Note(2, 30, 2) != nullptr && engine.events.size() == 2 && engine.events[0].track == 0 &&
              engine.events[1].track == 1,
          "Switch: drop: a stereo note drops the two voices of the lowest priority");

    TestEngine<CafePolicy> cafe(4, 2);
    cafe.Note(0, 10);
    cafe.Note(1, 20);

    Check(cafe.Note(2, 30, 2) != nullptr && cafe.events.size() == 1 && cafe.events[0].track == 0,
          "Wii U: drop: a stereo note drops the voice of the lowest priority alone");

    // Nothing is dropped unless each voice that would make room has a priority below 255.
    TestEngine<NxPolicy> full(3, 2);
    full.Note(0, 10);
    full.Note(1, 255);

    Check(full.Note(2, 255, 2) == nullptr && full.events.empty(), "Switch: drop: a voice of priority 255 stays");
}

void TestRetry()
{
    // Four MultiVoices on two voices of the renderer. When the renderer has no voice left for a free MultiVoice, the
    // Switch drops the voice of the lowest priority and allocates again. On the Wii U, the allocation fails.
    TestEngine<NxPolicy> engine(4, 4, 2);
    engine.Note(0, 10);
    engine.Note(1, 20);

    Check(engine.Note(2, 30) != nullptr && engine.events.size() == 1 && engine.events[0].track == 0 &&
              engine.events[0].status == Channel<NxPolicy>::CallbackStatus::kDrop,
          "Switch: retry: the voice of the lowest priority makes way when the renderer has none left");

    TestEngine<CafePolicy> cafe(4, 4, 2);
    cafe.Note(0, 10);
    cafe.Note(1, 20);

    Check(cafe.Note(2, 30) == nullptr && cafe.events.empty(), "Wii U: retry: no voice when the renderer has none left");
}

void TestClamps()
{
    // A volume below 0 becomes 0. A NaN volume stays NaN on the Switch (fmax) and becomes 0 on the Wii U (fsel).
    TestEngine<NxPolicy> nx(1, 1);
    MultiVoice<NxPolicy>& switch_voice = nx.VoiceOf(*nx.Note(0, 64));

    switch_voice.SetVolume(std::nanf(""));
    const bool switch_nan = std::isnan(switch_voice.Volume());
    switch_voice.SetVolume(-1.0f);

    Check(switch_nan && switch_voice.Volume() == 0.0f, "Switch: clamps: a NaN volume stays, and -1 becomes 0");

    TestEngine<CafePolicy> cafe(1, 1);
    MultiVoice<CafePolicy>& cafe_voice = cafe.VoiceOf(*cafe.Note(0, 64));

    cafe_voice.SetVolume(std::nanf(""));

    Check(cafe_voice.Volume() == 0.0f, "Wii U: clamps: a NaN volume becomes 0");
}

void TestReceiver()
{
    // A MultiVoice and its voices output to the mix that they're given. The MultiVoice keeps the mix when it's
    // allocated again. The voices start without one.
    TestEngine<NxPolicy> engine(2, 1);
    const int kMix = 7;
    Channel<NxPolicy>* first = engine.Note(0, 64);
    MultiVoice<NxPolicy>& voice = engine.VoiceOf(*first);

    voice.SetOutputReceiver(&kMix);

    Check(voice.GetOutputReceiver() == &kMix && voice.Voices()[0].receiver == &kMix,
          "Switch: receiver: the voices output to the mix");

    first->Stop(engine);
    Channel<NxPolicy>* second = engine.Note(1, 64);

    Check(second != nullptr && &engine.VoiceOf(*second) == &voice && voice.GetOutputReceiver() == &kMix &&
              voice.Voices()[0].receiver == nullptr,
          "Switch: receiver: the MultiVoice keeps the mix, and its voices start without one");
}

template <typename Console>
void TestOutputParam(bool by_mode)
{
    // On the Switch, mix mode 0 takes the pans and leaves the mix parameters. Mode 1 does the opposite. The Wii U takes
    // both in either mode.
    TestEngine<Console> engine(1, 1);
    MultiVoice<Console>& voice = engine.VoiceOf(*engine.Note(0, 64));
    typename TestPolicy<Console>::Outputs outputs;
    outputs.pan = 0.5f;
    outputs.mix_parameter[0][0] = 0.25f;

    voice.SetOutputs(outputs);

    const bool by_pan =
        voice.GetOutputs().pan == 0.5f && voice.GetOutputs().mix_parameter[0][0] == (by_mode ? 0.0f : 0.25f);

    outputs.mix_mode = 1;
    outputs.pan = -0.5f;
    outputs.mix_parameter[0][0] = 0.75f;

    voice.SetOutputs(outputs);

    const bool by_parameters =
        voice.GetOutputs().pan == (by_mode ? 0.5f : -0.5f) && voice.GetOutputs().mix_parameter[0][0] == 0.75f;
    Check(by_pan && by_parameters, Name<Console>("output parameters: each mix mode takes its inputs"));
}

} // namespace

int main()
{
    TestDrop<CafePolicy>();
    TestDrop<NxPolicy>();
    TestRelease<CafePolicy>();
    TestRelease<NxPolicy>();
    TestOneShot<CafePolicy>();
    TestOneShot<NxPolicy>();
    TestCopy<CafePolicy>();
    TestCopy<NxPolicy>();
    TestFrameStep<CafePolicy>(3);
    TestFrameStep<NxPolicy>(5);
    TestLfoTypes<CafePolicy>(false);
    TestLfoTypes<NxPolicy>(true);
    TestNoteOff<CafePolicy>(false);
    TestNoteOff<NxPolicy>(true);
    TestDropForChannels();
    TestRetry();
    TestClamps();
    TestReceiver();
    TestOutputParam<CafePolicy>(false);
    TestOutputParam<NxPolicy>(true);

    return xsflib::testing::Summary("nwsnd_voice");
}
