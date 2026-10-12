// SPDX-License-Identifier: MIT

// Unit tests for the 3DS's policy of the sound library's model (CtrPolicy in ntdlib/nwsnd/console.h), its envelope
// (CurveAdshr<CtrPolicy>), its LFO (TableLfo<CtrPolicy>) and its channel's update. The expected values are exact bits.
// They're fixed: a change to them is a change of ntdlib's output.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/util.h"
#include "xsflib/testing/check.h"

using xsflib::testing::Check;
using xsflib::testing::failures;

namespace
{

using Ctr = ntdlib::nwsnd::CtrPolicy;

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

uint32_t Bits(float x)
{
    return std::bit_cast<uint32_t>(x);
}

void TestArithmetic()
{
    XSFLIB_CHECK(Bits(Ctr::TrackVolume(100, 90, 127, 1.0f, Ctr::PlayerVolume(100))) == 0x3e7b0db4);
    XSFLIB_CHECK(Bits(Ctr::PanOfValue(-40)) == 0xbf228a29 && Ctr::PanOfValue(-64) == -1.0f);
    XSFLIB_CHECK(Bits(Ctr::SpanOfValue(100)) == 0x3fcb2cb4 && Ctr::SpanOfValue(127) == 2.0f);
    XSFLIB_CHECK(Bits(Ctr::TicksPerMs(48, 120, 1.0f)) == 0x3dc49ba6);
    XSFLIB_CHECK(Bits(Ctr::VelocityVolume(100)) == 0x3f1eb866 && Bits(Ctr::SendOfValue(64)) == 0x3f010204);

    // The ARM's shifts take the amount's low 8 bits.
    XSFLIB_CHECK(Ctr::Shift(0x1234, 3) == 0x91a0 && Ctr::Shift(-4660, -40) == -1);
    XSFLIB_CHECK(Ctr::Shift(5, 300) == 0 && Ctr::Shift(5, 256) == 5);

    XSFLIB_CHECK(Ctr::ToInt32(3.9f) == 3 && Ctr::ToInt32(-1e20f) == INT32_MIN && Ctr::ToInt32(kNan) == 0);

    const Ctr::Build kBuild;

    XSFLIB_CHECK(Ctr::HasCommand(kBuild, 0xC4) && !Ctr::HasCommand(kBuild, 0xDE) && !Ctr::HasCommand(kBuild, 0xE4));
    XSFLIB_CHECK(Ctr::HasCommand(kBuild, 0xF085) && !Ctr::HasCommand(kBuild, 0xF0A0) &&
                 !Ctr::HasCommand(kBuild, 0xF0E6));
    XSFLIB_CHECK(!Ctr::HasCommand(kBuild, 0xF0BF) && Ctr::HasCommand(kBuild, 0xF0E0) &&
                 Ctr::HasCommand(kBuild, 0xF0E7));
}

// The pan curves' gains (Util::CalcPanRatio and CalcSurroundPanRatio): the place in each curve, the surround curves for
// surround output, the scale to unity at the centre, and the holds.
void TestPan()
{
    namespace tables = ntdlib::nwsnd::tables;
    using ntdlib::nwsnd::CalcPanRatio;
    using ntdlib::nwsnd::CalcSurroundPanRatio;
    using ntdlib::nwsnd::OutputMode;
    using ntdlib::nwsnd::PanInfo;

    const PanInfo kSine{.curve = 1};

    XSFLIB_CHECK(CalcPanRatio<Ctr>(0.0f, kSine, OutputMode::kStereo) == tables::kStereoPan[1][128]);
    XSFLIB_CHECK(CalcPanRatio<Ctr>(0.5f, kSine, OutputMode::kStereo) == tables::kStereoPan[1][192]);
    XSFLIB_CHECK(CalcPanRatio<Ctr>(-3.0f, kSine, OutputMode::kStereo) == tables::kStereoPan[1][0]);
    XSFLIB_CHECK(CalcPanRatio<Ctr>(3.0f, kSine, OutputMode::kStereo) == tables::kStereoPan[1][256]);
    XSFLIB_CHECK(CalcPanRatio<Ctr>(kNan, kSine, OutputMode::kStereo) == tables::kStereoPan[1][0]);
    XSFLIB_CHECK(CalcPanRatio<Ctr>(0.5f, kSine, OutputMode::kSurround) == tables::kSurroundPan[1][192]);
    XSFLIB_CHECK(CalcPanRatio<Ctr>(0.5f, PanInfo{.curve = 1, .front_bypass = true}, OutputMode::kSurround) ==
                 tables::kStereoPan[1][192]);

    const float kScaled = tables::kStereoPan[0][0] / tables::kStereoPan[0][128];

    XSFLIB_CHECK(CalcPanRatio<Ctr>(-1.0f, PanInfo{.center_zero_db = true}, OutputMode::kStereo) == kScaled);
    XSFLIB_CHECK(
        CalcPanRatio<Ctr>(-1.0f, PanInfo{.center_zero_db = true, .clamp_to_unity = true}, OutputMode::kStereo) == 1.0f);

    XSFLIB_CHECK(CalcSurroundPanRatio<Ctr>(1.0f, kSine) == tables::kStereoPan[1][128]);
    XSFLIB_CHECK(CalcSurroundPanRatio<Ctr>(-1.0f, kSine) == tables::kStereoPan[1][0]);
    XSFLIB_CHECK(CalcSurroundPanRatio<Ctr>(5.0f, kSine) == tables::kStereoPan[1][256]);
    XSFLIB_CHECK(CalcSurroundPanRatio<Ctr>(kNan, kSine) == tables::kStereoPan[1][0]);

    // The pan curves 0 to 8, and a curve past them.
    const PanInfo centred = ntdlib::nwsnd::PanInfoOf(4);
    XSFLIB_CHECK(centred.curve == 1 && centred.center_zero_db && !centred.clamp_to_unity);
    const PanInfo held = ntdlib::nwsnd::PanInfoOf(8);
    XSFLIB_CHECK(held.curve == 2 && held.center_zero_db && held.clamp_to_unity);
    const PanInfo past = ntdlib::nwsnd::PanInfoOf(9);
    XSFLIB_CHECK(past.curve == 0 && !past.center_zero_db && !past.clamp_to_unity && !past.front_bypass);
}

// An envelope that runs `steps` frames of 5 ms, released at the frame `release_at`.
uint32_t Envelope(int attack, int hold, int decay, int sustain, int release, int steps, int release_at,
                  ntdlib::nwsnd::AdshrStatus& status)
{
    ntdlib::nwsnd::CurveAdshr<Ctr> envelope;
    envelope.SetAttack(attack);
    envelope.SetHold(hold);
    envelope.SetDecay(decay);
    envelope.SetSustain(sustain);
    envelope.SetRelease(release);

    for (int i = 0; i < steps; ++i)
    {
        if (i == release_at)
        {
            envelope.SetStatus(ntdlib::nwsnd::AdshrStatus::kRelease);
        }

        envelope.Update(5);
    }

    status = envelope.GetStatus();

    return Bits(envelope.GetValue());
}

void TestEnvelope()
{
    using Status = ntdlib::nwsnd::AdshrStatus;
    Status status;

    XSFLIB_CHECK(Envelope(127, 0, 100, 90, 110, 60, 40, status) == 0xc1580000 && status == Status::kRelease);
    XSFLIB_CHECK(Envelope(90, 20, 60, 64, 100, 200, 150, status) == 0xc1a7f1ad && status == Status::kRelease);
    XSFLIB_CHECK(Envelope(120, 5, 30, 100, 80, 120, 1000, status) == 0xc0866667 && status == Status::kSustain);
}

// An LFO that runs `steps` frames of 5 ms, with the sequence's settings.
uint32_t Lfo(int depth, int speed, int range, uint32_t delay, int steps)
{
    ntdlib::nwsnd::TableLfo<Ctr> lfo;
    lfo.param.depth = static_cast<float>(depth) * 0.0078125f;
    lfo.param.speed = static_cast<float>(speed) * 0.390625f;
    lfo.param.range = static_cast<uint8_t>(range);
    lfo.param.delay = delay;

    for (int i = 0; i < steps; ++i)
    {
        lfo.Update(5);
    }

    return Bits(lfo.GetValue());
}

void TestLfo()
{
    XSFLIB_CHECK(Lfo(64, 32, 1, 0, 37) == 0x3eebd7af);
    XSFLIB_CHECK(Lfo(128, 100, 12, 50, 101) == 0xc13e7cfa);
    XSFLIB_CHECK(Lfo(255, 255, 127, 0, 13) == 0x42473800);

    // No value before the delay has passed, or without depth.
    XSFLIB_CHECK(Lfo(128, 100, 12, 50, 9) == 0 && Lfo(0, 100, 12, 0, 20) == 0);
}

} // namespace

// The test's types are in a named namespace. Templates explicitly instantiated on a type in an unnamed namespace have
// internal linkage, and GCC then warns about each of their members that no test calls.
namespace nwsnd_ctr_test
{

class ChannelTestEngine;

// A policy with stand-ins for the 3DS's engine, waves and outputs, for the channel's update.
struct ChannelTestPolicy
{
    using Console = Ctr;
    using Engine = ChannelTestEngine;

    struct WaveInfo
    {
        int format = 0;
        uint32_t sample_rate = 32728;
        bool loop = false;
    };

    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, 2> fx_send{};
    };

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }
};

using Channel = ntdlib::nwsnd::Channel<ChannelTestPolicy>;
using ChannelManager = ntdlib::nwsnd::ChannelManager<ChannelTestPolicy>;

// A voice that keeps what the channel last gave it.
struct StandInVoice
{
    void SetSampleFormat(int)
    {
    }

    void SetSampleRate(uint32_t)
    {
    }

    void SetInterpolationType(uint8_t)
    {
    }

    void AppendWave(ChannelTestEngine&, const ChannelTestPolicy::WaveInfo&)
    {
    }

    void Start()
    {
    }

    void Stop(ChannelTestEngine&)
    {
    }

    void Free(ChannelTestEngine&)
    {
        freed = true;
    }

    void SetVolume(float value)
    {
        volume = value;
    }

    void SetPitch(float value)
    {
        pitch = value;
    }

    void SetPanMode(uint32_t)
    {
    }

    void SetPanCurve(uint32_t)
    {
    }

    void SetLpfFreq(float)
    {
    }

    void SetBiquadFilter(int, float)
    {
    }

    void SetOutputLine(uint32_t)
    {
    }

    void SetOutputs(const ChannelTestPolicy::Outputs&)
    {
    }

    void SetPriority(ChannelTestEngine&, uint32_t)
    {
    }

    float volume = 0.0f;
    float pitch = 0.0f;
    bool freed = false;
};

// One voice, and a count of the channels' callbacks.
class ChannelTestEngine
{
public:
    struct VoicePool
    {
        ntdlib::nwsnd::VoiceId AllocVoice(ChannelTestEngine&, int, uint32_t, ntdlib::nwsnd::ChannelId)
        {
            return 0;
        }

        StandInVoice& At(ntdlib::nwsnd::VoiceId)
        {
            return voice;
        }

        StandInVoice voice;
    };

    ChannelManager& Channels()
    {
        return channels_;
    }

    VoicePool& Voices()
    {
        return voices_;
    }

    ntdlib::nwsnd::Random& Rng()
    {
        return random_;
    }

    void ChannelEvent(int, int, Channel&, Channel::CallbackStatus status)
    {
        last_status = status;
        ++callbacks;
    }

    Channel::CallbackStatus last_status = Channel::CallbackStatus::kStopped;
    int callbacks = 0;

private:
    ChannelManager channels_{24};
    VoicePool voices_;
    ntdlib::nwsnd::Random random_;
};

} // namespace nwsnd_ctr_test

using nwsnd_ctr_test::Channel;
using nwsnd_ctr_test::ChannelTestEngine;
using nwsnd_ctr_test::ChannelTestPolicy;
using nwsnd_ctr_test::StandInVoice;

namespace
{

void TestChannelFormulas()
{
    // The 3DS adds the sweep to the key's distance, then the bend. The Wii U adds the bend first, and here its last bit
    // differs.
    const float kBend = std::bit_cast<float>(0x3dc985f0u);
    const float kSweep = std::bit_cast<float>(0xc0455555u);

    XSFLIB_CHECK(Bits(Ctr::NotePitch(7.0f, kBend, kSweep)) == 0x40807b6d);
    XSFLIB_CHECK(Bits(ntdlib::nwsnd::CafePolicy::NotePitch(7.0f, kBend, kSweep)) == 0x40807b6e);

    // The 3DS multiplies the silence level in 255ths by the product of the note's volume and the track's volume:
    // velocity 1 in a region of volume 127, on a track at 0.6.
    XSFLIB_CHECK(Bits(Ctr::ChannelVolume(std::bit_cast<float>(0x38820610u), 1.0f, 0.6f, 255)) == 0x381c0747);

    // The level of a volume holds to -90.4 dB with the ARM's compare. The compare keeps NaN.
    XSFLIB_CHECK(Ctr::Max(-100.0f, -90.4f) == -90.4f && Ctr::Max(3.0f, -90.4f) == 3.0f);
    XSFLIB_CHECK(std::isnan(Ctr::Max(NAN, -90.4f)));
}

void TestChannelUpdate()
{
    ChannelTestEngine engine;
    Channel* channel = engine.Channels().AllocChannel(engine, 1, 64, 0, 3);
    XSFLIB_CHECK(channel != nullptr);
    if (!channel)
    {
        return;
    }

    channel->key = 64;
    channel->velocity_volume = std::bit_cast<float>(0x38820610u);
    channel->track_volume = 0.6f;
    channel->envelope.SetAttack(100);
    channel->envelope.SetDecay(90);
    channel->envelope.SetSustain(100);
    channel->Start(engine, ChannelTestPolicy::WaveInfo{}, -1);

    // Each update moves the envelope on 5 ms.
    ntdlib::nwsnd::CurveAdshr<Ctr> envelope;
    envelope.Initialize(ntdlib::nwsnd::CurveAdshr<Ctr>::kInitialDecibel);
    envelope.SetAttack(100);
    envelope.SetDecay(90);
    envelope.SetSustain(100);
    envelope.Reset(ntdlib::nwsnd::CurveAdshr<Ctr>::kInitialDecibel);

    for (int i = 0; i < 12; ++i)
    {
        engine.Channels().UpdateAllChannel(engine);
        envelope.Update(5);
    }

    const StandInVoice& voice = engine.Voices().voice;
    XSFLIB_CHECK(Bits(channel->envelope.GetValue()) == Bits(envelope.GetValue()));
    const float volume = Ctr::ChannelVolume(channel->velocity_volume, 1.0f, 0.6f, 255) *
                         ntdlib::nwsnd::CalcVolumeRatio<Ctr>(envelope.GetValue());
    XSFLIB_CHECK(Bits(voice.volume) == Bits(volume));
    XSFLIB_CHECK(Bits(voice.pitch) == Bits(ntdlib::nwsnd::CalcPitchRatio(4 * 256)));

    // The voice's end frees the channel at once, after the owner's callback.
    channel->OnVoiceEvent(engine, ntdlib::nwsnd::VoiceCallbackStatus::kFinishWave);

    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0 && voice.freed);
    XSFLIB_CHECK(engine.callbacks == 1 && engine.last_status == Channel::CallbackStatus::kFinish);
}

} // namespace

template class ntdlib::nwsnd::Channel<ChannelTestPolicy>;
template class ntdlib::nwsnd::ChannelManager<ChannelTestPolicy>;

int main()
{
    TestArithmetic();
    TestPan();
    TestEnvelope();
    TestLfo();
    TestChannelFormulas();
    TestChannelUpdate();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("3DS policy tests passed\n");

    return 0;
}
