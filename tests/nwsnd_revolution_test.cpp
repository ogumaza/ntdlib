// SPDX-License-Identifier: MIT

// Unit tests for the Wii's policy of the sound library's model (RevolutionPolicy in ntdlib/nwsnd/console.h), its LFO
// (TableLfo<RevolutionPolicy>) and its channel frame (RevolutionPolicy::UpdateChannel). The expected values are fixed,
// to the last bit. Where the Wii's code divides and the 3DS's code multiplies by a reciprocal, the last bit can differ.
// The checks detect that difference. The envelope's checks run CurveAdshr<RevolutionPolicy>.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/channel_impl.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/nwsnd/util.h"
#include "xsflib/testing/check.h"

using xsflib::testing::Check;

namespace
{

using Wii = ntdlib::nwsnd::RevolutionPolicy;

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

uint32_t Bits(float x)
{
    return std::bit_cast<uint32_t>(x);
}

// Four of the six kinds of build (SndBuild): the builds of October 2006 (Smooth Moves), June 2007 (Wii Sports), March
// 2008 (Mario Kart Wii) and June 2008 (Wii Music).
const Wii::Build kOctober2006 = {0, true};
const Wii::Build kJune2007 = {20070608, false};
const Wii::Build kMarch2008 = {20080308, false};
const Wii::Build kJune2008 = {20080620, false};

void TestArithmetic()
{
    XSFLIB_CHECK(Bits(Wii::TrackVolume(100, 90, 127, 1.0f, Wii::PlayerVolume(100))) == 0x3e7b0db5);
    XSFLIB_CHECK(Bits(Wii::PanOfValue(-40)) == 0xbf228a29 && Wii::PanOfValue(-64) == -1.0f);
    XSFLIB_CHECK(Bits(Wii::SpanOfValue(100)) == 0x3fcb2cb3 && Wii::SpanOfValue(127) == 2.0f);
    XSFLIB_CHECK(Bits(Wii::TicksPerMs(48, 120, 1.0f)) == 0x3dc49ba6);
    XSFLIB_CHECK(Bits(Wii::VelocityVolume(100)) == 0x3f1eb866 && Bits(Wii::SendOfValue(64)) == 0x3f010204);

    // The track's float volume multiplies after the main volume and before the sound's volume
    // (SeqTrack::UpdateChannelParam, 0x800403e0). The tempo ratio multiplies before the division by 60,000
    // (SeqPlayer::UpdateTick, 0x8003eec4). Both products follow the game's order.
    const float track = 100.0f / 127.0f;
    const float track2 = 90.0f / 127.0f;
    const float main = 120.0f / 127.0f;
    float in_order = 1.0f;
    in_order *= track * track;
    in_order *= track2 * track2;
    in_order *= main * main;
    in_order *= 0.3f;
    in_order *= Wii::PlayerVolume(100);
    XSFLIB_CHECK(Bits(Wii::TrackVolume(100, 90, 120, 0.3f, Wii::PlayerVolume(100))) == Bits(in_order));
    XSFLIB_CHECK(Wii::TrackVolume(100, 90, 127, 0.0f, Wii::PlayerVolume(100)) == 0.0f);
    XSFLIB_CHECK(Bits(Wii::TicksPerMs(48, 120, 1.1f)) == Bits(1.1f * 5760.0f / 60000.0f));

    // The sound's volume is held to 1.
    XSFLIB_CHECK(Wii::PlayerVolume(200) == 1.0f && Wii::PlayerVolume(127) == 1.0f);

    // PowerPC's shifts and conversions, the same as on the Wii U.
    XSFLIB_CHECK(Wii::Shift(0x1234, 3) == 0x91a0 && Wii::Shift(-4660, -40) == -1 && Wii::Shift(5, 64) == 5);
    XSFLIB_CHECK(Wii::ToInt32(3.9f) == 3 && Wii::ToInt32(kNan) == INT32_MIN);
}

void TestBuilds()
{
    // Every build lacks the bank, the LFOs' extras and the front bypass. 0xF0 0xE0 is the game's procedure.
    for (const Wii::Build& build : {kOctober2006, kJune2007, kMarch2008, kJune2008})
    {
        XSFLIB_CHECK(!Wii::HasCommand(build, 0xB6) && !Wii::HasCommand(build, 0xBD) && !Wii::HasCommand(build, 0xE4));
        XSFLIB_CHECK(!Wii::HasCommand(build, 0xBF) && !Wii::HasCommand(build, 0xF0A0) &&
                     !Wii::HasCommand(build, 0xF0E6));
        XSFLIB_CHECK(Wii::HasCommand(build, 0xF085) && Wii::HasCommand(build, 0xF0E0) && Wii::HasCommand(build, 0xDE));
    }

    // 2008 adds the time prefixes, the hold and monophony. June 2008 adds the velocity range, the biquad filter and the
    // envelope's reset.
    XSFLIB_CHECK(!Wii::HasCommand(kJune2007, 0xA3) && Wii::HasCommand(kMarch2008, 0xA5));
    XSFLIB_CHECK(!Wii::HasCommand(kJune2007, 0xB2) && Wii::HasCommand(kMarch2008, 0xB1));
    XSFLIB_CHECK(!Wii::HasCommand(kMarch2008, 0xB3) && !Wii::HasCommand(kMarch2008, 0xFB));
    XSFLIB_CHECK(Wii::HasCommand(kJune2008, 0xB5) && Wii::HasCommand(kJune2008, 0xFB));

    // October 2006 raises a pan of 0 or 1 by one.
    XSFLIB_CHECK(Wii::PanArgument(kOctober2006, 0) == 1 && Wii::PanArgument(kOctober2006, 1) == 2);
    XSFLIB_CHECK(Wii::PanArgument(kOctober2006, 2) == 2 && Wii::PanArgument(kJune2007, 0) == 0);

    // 2008 holds the tempo to 0-1023. Before, a variable's -1 gives 65535.
    XSFLIB_CHECK(Wii::TempoOf(kJune2007, -1) == 65535 && Wii::TempoOf(kMarch2008, -1) == 0);
    XSFLIB_CHECK(Wii::TempoOf(kMarch2008, 2000) == 1023 && Wii::TempoOf(kJune2007, 2000) == 2000);

    // The sweep is in 32nds of a semitone before 2008, and in 64ths from 2008.
    XSFLIB_CHECK(Bits(Wii::SweepPitchOf(kJune2007, -40)) == 0xbfa00000);
    XSFLIB_CHECK(Bits(Wii::SweepPitchOf(kMarch2008, -40)) == 0xbf200000);
}

// Where each channel of a wave goes in the stereo mix, by the pan mode and the build: the pan that the curve takes, the
// sides that the channel reaches, and the curve. The expected values are fixed.
void TestWavePlacement()
{
    using Side = Wii::WaveSide;
    struct Case
    {
        const char* what;
        const Wii::Build* build;
        float pan;
        int channel;
        int channels;
        uint32_t mode;
        uint32_t curve;
        float placed;
        Side side;
        uint32_t placed_curve;
    };
    const Case kCases[] = {
        {"a mono wave", &kJune2007, 0.25f, 0, 1, 0, 3, 0.25f, Side::kBoth, 3},
        {"a mono wave in balance mode", &kJune2008, 0.25f, 0, 1, 1, 3, 0.25f, Side::kBoth, 3},
        {"a stereo wave's first channel in dual mode", &kJune2007, 0.25f, 0, 2, 0, 3, -0.75f, Side::kBoth, 3},
        {"a stereo wave's second channel in dual mode", &kJune2007, 0.25f, 1, 2, 0, 3, 1.25f, Side::kBoth, 3},
        {"a stereo wave's first channel in balance mode", &kMarch2008, 0.25f, 0, 2, 1, 3, 0.25f, Side::kLeftOnly, 3},
        {"a stereo wave's second channel in balance mode", &kMarch2008, 0.25f, 1, 2, 1, 3, 0.25f, Side::kRightOnly, 3},
        {"October 2006: a stereo wave's first channel", &kOctober2006, 0.25f, 0, 2, 1, 3, -0.75f, Side::kBoth, 0},
        {"October 2006: a stereo wave's second channel", &kOctober2006, 0.25f, 1, 2, 1, 3, 1.25f, Side::kBoth, 0},
        {"October 2006: a mono wave", &kOctober2006, -0.5f, 0, 1, 0, 6, -0.5f, Side::kBoth, 0},
    };
    for (const Case& c : kCases)
    {
        const Wii::WavePlacement placement =
            Wii::PlacementOfWaveChannel(c.pan, c.channel, c.channels, c.mode, c.curve, *c.build);
        Check(Bits(placement.pan) == Bits(c.placed) && placement.side == c.side && placement.curve == c.placed_curve,
              std::string("the placement of ") + c.what);
    }
}

// An LFO that runs `steps` frames of 3 ms, with the sequence's settings.
uint32_t Lfo(int depth, int speed, int range, uint32_t delay, int steps)
{
    ntdlib::nwsnd::TableLfo<Wii> lfo;
    lfo.param.depth = static_cast<float>(depth) * 0.0078125f;
    lfo.param.speed = static_cast<float>(speed) * 0.390625f;
    lfo.param.range = static_cast<uint8_t>(range);
    lfo.param.delay = delay;

    for (int i = 0; i < steps; ++i)
    {
        lfo.Update(3);
    }

    return Bits(lfo.GetValue());
}

void TestLfo()
{
    XSFLIB_CHECK(Lfo(64, 32, 1, 0, 37) == 0x3eab56ad);
    XSFLIB_CHECK(Lfo(128, 100, 12, 50, 101) == 0xc1008102);
    XSFLIB_CHECK(Lfo(255, 255, 127, 0, 13) == 0xc32955ff);

    // No value before the delay has passed, or without depth.
    XSFLIB_CHECK(Lfo(128, 100, 12, 50, 9) == 0 && Lfo(0, 100, 12, 0, 20) == 0);
}

// An envelope with a region's settings: its level and stage after `frames` frames of 3 ms, and then `release_frames`
// more in its release (none for -1).
struct Level
{
    uint32_t bits;
    ntdlib::nwsnd::AdshrStatus status;
};

Level Envelope(int attack, int hold, int decay, int sustain, int release, int frames, int release_frames)
{
    ntdlib::nwsnd::CurveAdshr<Wii> envelope;
    envelope.SetAttack(attack);
    envelope.SetHold(hold);
    envelope.SetDecay(decay);
    envelope.SetSustain(sustain);
    envelope.SetRelease(release);
    envelope.Reset(ntdlib::nwsnd::CurveAdshr<Wii>::kInitialDecibel);

    for (int i = 0; i < frames; ++i)
    {
        envelope.Update(3);
    }

    if (release_frames >= 0)
    {
        envelope.SetStatus(ntdlib::nwsnd::AdshrStatus::kRelease);
        for (int i = 0; i < release_frames; ++i)
        {
            envelope.Update(3);
        }
    }

    return {Bits(envelope.GetValue()), envelope.GetStatus()};
}

void TestEnvelope()
{
    using Status = ntdlib::nwsnd::AdshrStatus;

    // A decay that lands on the sustain level starts the sustain at once. Decay 126 falls 7.2 dB a frame. Sustain 84 is
    // -7.2 dB, and sustain 16 is -36 dB.
    const Level landed = Envelope(127, 0, 126, 84, 127, 2, -1);
    XSFLIB_CHECK(landed.bits == 0xc0e66666 && landed.status == Status::kSustain);
    const Level later = Envelope(127, 0, 126, 16, 127, 6, -1);
    XSFLIB_CHECK(later.bits == 0xc2100000 && later.status == Status::kSustain);
    const Level before = Envelope(127, 0, 126, 16, 127, 5, -1);
    XSFLIB_CHECK(before.bits == 0xc1e66666 && before.status == Status::kDecay);

    // An instant attack reaches 0 dB in the first frame. A hold of 20 lasts 110 ms: 37 frames after that one.
    const Level attacked = Envelope(127, 0, 126, 84, 127, 1, -1);
    XSFLIB_CHECK(attacked.bits == 0 && attacked.status == Status::kHold);
    const Level held = Envelope(127, 20, 120, 64, 127, 37, -1);
    XSFLIB_CHECK(held.bits == 0 && held.status == Status::kHold);
    const Level decaying = Envelope(127, 20, 120, 64, 127, 38, -1);
    XSFLIB_CHECK(decaying.bits == 0xbe4ccccd && decaying.status == Status::kDecay);

    // An attack on its way, and two releases.
    XSFLIB_CHECK(Envelope(100, 0, 100, 100, 100, 5, -1).bits == 0xc1a08591);
    XSFLIB_CHECK(Envelope(110, 0, 90, 64, 100, 40, 20).bits == 0xc07e07e2);
    XSFLIB_CHECK(Envelope(120, 0, 60, 30, 20, 25, 50).bits == 0xbfcec81e);
}

// The ratios of a level, a pan and a pitch: Util::CalcVolumeRatio (0x8004e9cc), Util::CalcPanRatio (0x8004ea24) and
// Util::CalcPitchRatio (0x8004e83c).
void TestRatios()
{
    using ntdlib::nwsnd::CalcPanRatio;
    using ntdlib::nwsnd::CalcPitchRatio;
    using ntdlib::nwsnd::CalcVolumeRatio;

    // The table's entry in 0.1 dB steps, held to -90.4 and 6 dB.
    XSFLIB_CHECK(Bits(CalcVolumeRatio<Wii>(-90.4f)) == 0 && Bits(CalcVolumeRatio<Wii>(-100.0f)) == 0);
    XSFLIB_CHECK(Bits(CalcVolumeRatio<Wii>(-45.35f)) == 0x3bb20323 && Bits(CalcVolumeRatio<Wii>(-6.0f)) == 0x3f004dce);
    XSFLIB_CHECK(CalcVolumeRatio<Wii>(-0.05f) == 1.0f && CalcVolumeRatio<Wii>(0.0f) == 1.0f);
    XSFLIB_CHECK(Bits(CalcVolumeRatio<Wii>(3.7f)) == 0x3fc3faad);
    XSFLIB_CHECK(Bits(CalcVolumeRatio<Wii>(6.0f)) == 0x3fff64c1 && Bits(CalcVolumeRatio<Wii>(7.5f)) == 0x3fff64c1);

    // The stereo curves: the square root, the sine and cosine scaled to 0 dB at the centre, and the line scaled and
    // held to 1. A pan past either side is held to it.
    const auto pan = [](float value, uint32_t curve, bool center_zero_db, bool clamp_to_unity)
    {
        ntdlib::nwsnd::PanInfo info;
        info.curve = curve;
        info.center_zero_db = center_zero_db;
        info.clamp_to_unity = clamp_to_unity;
        return Bits(CalcPanRatio<Wii>(value, info, ntdlib::nwsnd::OutputMode::kStereo));
    };

    XSFLIB_CHECK(pan(-2.0f, 0, false, false) == 0x3f800000 && pan(-0.5f, 0, false, false) == 0x3f5db3d7);
    XSFLIB_CHECK(pan(0.0f, 0, false, false) == 0x3f3504f3 && pan(0.3f, 0, false, false) == 0x3f17ca11);
    XSFLIB_CHECK(pan(0.77f, 0, false, false) == 0x3eac5345 && pan(1.0f, 0, false, false) == 0);
    XSFLIB_CHECK(pan(-1.0f, 1, true, false) == 0x3fb504f3 && pan(-0.5f, 1, true, false) == 0x3fa73d74);
    XSFLIB_CHECK(pan(0.3f, 1, true, false) == 0x3f3debf8 && pan(1.5f, 1, true, false) == 0);
    XSFLIB_CHECK(pan(-0.5f, 2, true, true) == 0x3f800000 && pan(0.3f, 2, true, true) == 0x3f340000);
    XSFLIB_CHECK(pan(0.77f, 2, true, true) == 0x3e680000);

    // Whole octaves, then the semitone and the 256th of one.
    XSFLIB_CHECK(Bits(CalcPitchRatio(-7000)) == 0x3e53098b && CalcPitchRatio(-3072) == 0.5f);
    XSFLIB_CHECK(Bits(CalcPitchRatio(-1)) == 0x3f7ff138 && CalcPitchRatio(0) == 1.0f);
    XSFLIB_CHECK(Bits(CalcPitchRatio(1)) == 0x3f800765 && Bits(CalcPitchRatio(255)) == 0x3f8794a8);
    XSFLIB_CHECK(Bits(CalcPitchRatio(3071)) == 0x3ffff138 && CalcPitchRatio(3072) == 2.0f);
    XSFLIB_CHECK(Bits(CalcPitchRatio(9000)) == 0x40f3d290);
}

class ChannelTestEngine;

// A policy with stand-ins for the engine, the waves and the outputs, for the Wii's channel frame.
struct ChannelTestPolicy : Wii
{
    using Console = Wii;
    using Engine = ChannelTestEngine;

    struct WaveInfo
    {
        int format = 0;
        uint32_t sample_rate = 32000;
        bool loop = false;
    };

    // The track's main out volume, pan, surround pan and sends.
    struct Outputs
    {
        float volume = 1.0f;
        uint32_t mix_mode = 0;
        float pan = 0.0f;
        float span = 0.0f;
        float main_send = 0.0f;
        std::array<float, Wii::kFxSendCount> fx_send{};
    };

    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs...);
    }
};

using Channel = ntdlib::nwsnd::Channel<ChannelTestPolicy>;
using ChannelManager = ntdlib::nwsnd::ChannelManager<ChannelTestPolicy>;

// A voice that keeps what the channel's frame last gave it.
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
    }

    void SetPriority(ChannelTestEngine&, uint32_t)
    {
    }

    void SetParam(const Wii::ChannelVoiceParam& value)
    {
        param = value;
        ++frames;
    }

    Wii::ChannelVoiceParam param;
    int frames = 0;
};

// The Wii's channels with a voice for each, the sound's build, and a count of the owners' callbacks.
class ChannelTestEngine
{
public:
    // The voice of the channel at each place.
    struct VoicePool
    {
        ntdlib::nwsnd::VoiceId AllocVoice(ChannelTestEngine&, int, uint32_t, ntdlib::nwsnd::ChannelId owner)
        {
            return owner;
        }

        StandInVoice& At(ntdlib::nwsnd::VoiceId voice)
        {
            return voices[static_cast<std::size_t>(voice)];
        }

        std::array<StandInVoice, Wii::kChannelCount> voices;
    };

    explicit ChannelTestEngine(const Wii::Build& build) : build_(build)
    {
    }

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

    const Wii::Build& Build() const
    {
        return build_;
    }

    void ChannelEvent(int, int, Channel&, Channel::CallbackStatus status)
    {
        last_status = status;
        ++callbacks;
    }

    // A note of player 0's track 0 that plays a wave for ever: a note's volume of 0.5 on a track's volume of 0.6, with
    // an instant attack.
    Channel& Note()
    {
        Channel& channel = *channels_.AllocChannel(*this, 1, 64, 0, 0);
        channel.velocity_volume = 0.5f;
        channel.track_volume = 0.6f;
        channel.envelope.SetAttack(127);
        channel.Start(*this, ChannelTestPolicy::WaveInfo{}, -1);

        return channel;
    }

    StandInVoice& VoiceOf(const Channel& channel)
    {
        return voices_.At(channel.voice);
    }

    Channel::CallbackStatus last_status = Channel::CallbackStatus::kStopped;
    int callbacks = 0;

private:
    Wii::Build build_;
    ChannelManager channels_{Wii::kChannelCount};
    VoicePool voices_;
    ntdlib::nwsnd::Random random_;
};

void TestChannelVolume()
{
    // A silence ramp from 0 to 255 over 2 frames stands at 127 after the first frame's step. The Wii divides it by 255.
    // A product with 1/255 would give 0x3e18ff67. The ramp ends at 255 in the second frame.
    ChannelTestEngine engine(kJune2007);
    Channel& channel = engine.Note();
    channel.silence_from = 0;
    channel.silence_to = 255;
    channel.silence_duration = 2;
    const StandInVoice& voice = engine.VoiceOf(channel);

    engine.Channels().UpdateAllChannel(engine);
    const uint32_t first = Bits(voice.param.volume);
    engine.Channels().UpdateAllChannel(engine);

    XSFLIB_CHECK(first == 0x3e18ff66 && Bits(voice.param.volume) == 0x3e99999a && voice.frames == 2);
    XSFLIB_CHECK(Wii::kChannelCount == 97);
}

// A note whose envelope has reached 0 dB, released with its track's volume at 0, then one more frame. The note's volume
// times the envelope's gain is then 0, and the gain alone isn't. The count of channels left, and of the owner's
// callbacks.
struct Ended
{
    int channels;
    int callbacks;
};

Ended ReleaseSilentTrack(const Wii::Build& build, bool owned)
{
    ChannelTestEngine engine(build);
    Channel& channel = engine.Note();
    engine.Channels().UpdateAllChannel(engine);

    if (!owned)
    {
        channel.Detach();
    }

    channel.track_volume = 0.0f;
    channel.Release(engine);
    engine.Channels().UpdateAllChannel(engine);

    return {engine.Channels().ActiveCount(), engine.callbacks};
}

void TestReleasedNotes()
{
    // With an owner, a released note stops when its envelope's gain is 0, whatever its volume. Without one, it stops
    // when its volume times that gain is 0. The build of October 2006 takes the product either way. A stopped channel
    // calls its owner back and goes back to the pool.
    const Ended owned = ReleaseSilentTrack(kJune2007, true);
    const Ended ownerless = ReleaseSilentTrack(kJune2007, false);
    const Ended october = ReleaseSilentTrack(kOctober2006, true);

    XSFLIB_CHECK(owned.channels == 1 && owned.callbacks == 0);
    XSFLIB_CHECK(ownerless.channels == 0 && ownerless.callbacks == 0);
    XSFLIB_CHECK(october.channels == 0 && october.callbacks == 1);

    // The fastest release takes an owned note's envelope to -90.4 dB, a gain of 0, in one frame. The note stops in the
    // frame after that one.
    ChannelTestEngine engine(kJune2007);
    Channel& channel = engine.Note();
    channel.envelope.SetRelease(127);
    engine.Channels().UpdateAllChannel(engine);
    channel.Release(engine);

    int frames = 0;
    bool silent = false;
    while (engine.Channels().ActiveCount() == 1 && frames < 1000)
    {
        silent = ntdlib::nwsnd::CalcVolumeRatio<Wii>(channel.envelope.GetValue()) == 0.0f;
        engine.Channels().UpdateAllChannel(engine);
        ++frames;
    }

    XSFLIB_CHECK(silent && frames == 2 && engine.callbacks == 1 &&
                 engine.last_status == Channel::CallbackStatus::kStopped);
}

void TestVolumeLfo()
{
    // A volume LFO scales the envelope's gain at both ends of a frame: by its value before the frame's step, and by its
    // value after it. The envelope stays at 0 dB.
    using ntdlib::nwsnd::CalcVolumeRatio;

    ChannelTestEngine engine(kJune2007);
    Channel& channel = engine.Note();
    channel.lfo_types[0] = 1;
    channel.lfos[0].param.depth = 1.0f;
    channel.lfos[0].param.speed = 50.0f;

    for (int i = 0; i < 9; ++i)
    {
        engine.Channels().UpdateAllChannel(engine);
    }

    const float before = channel.lfos[0].GetValue();
    engine.Channels().UpdateAllChannel(engine);
    const float after = channel.lfos[0].GetValue();

    const Wii::ChannelVoiceParam& param = engine.VoiceOf(channel).param;
    XSFLIB_CHECK(channel.envelope.GetValue() == 0.0f && before != after);
    XSFLIB_CHECK(Bits(param.ve_init_volume) == Bits(CalcVolumeRatio<Wii>(6.0f * before)));
    XSFLIB_CHECK(Bits(param.ve_target_volume) == Bits(CalcVolumeRatio<Wii>(6.0f * after)));

    // The LFO moves neither the volume nor the pitch.
    XSFLIB_CHECK(Bits(param.volume) == 0x3e99999a && param.pitch == 1.0f);
}

void TestChannelPitch()
{
    // Key 64 on a wave of key 60, a sweep from 2 semitones down over 48 ms, and a bend of half a semitone: 2.5
    // semitones in the first frame, and 2.625 in the second. The ratio of the pitch is multiplied by the tune times the
    // sound's pitch. The Wii U multiplies by the tune first. Its ratios would be 0x3f6b04af and 0x3f6cb8a8.
    ChannelTestEngine engine(kJune2007);
    Channel& channel = engine.Note();
    channel.key = 64;
    channel.original_key = 60;
    channel.bend = 0.5f;
    channel.tune = 1.0594631f;
    channel.user_pitch = 0.75f;
    channel.SetSweepParam(-2.0f, 48, true);
    const StandInVoice& voice = engine.VoiceOf(channel);

    engine.Channels().UpdateAllChannel(engine);
    const uint32_t first = Bits(voice.param.pitch);
    engine.Channels().UpdateAllChannel(engine);

    XSFLIB_CHECK(first == 0x3f6b04b0 && Bits(voice.param.pitch) == 0x3f6cb8a9);
}

void TestChannelOutputs()
{
    // The pan is the note's initial pan plus the track's pan. The surround pan adds up the same way. The low-pass
    // filter's frequency is 1 more than the track's value. The main out volume and the sends come from the track. The
    // sums start from 0. Two negative zeros then give a positive pan.
    ChannelTestEngine engine(kJune2007);
    Channel& channel = engine.Note();
    channel.init_pan = -0.0f;
    channel.outputs.pan = -0.0f;
    channel.init_span = 0.25f;
    channel.outputs.span = 0.5f;
    channel.lpf = -0.5f;
    channel.outputs.volume = 0.75f;
    channel.outputs.main_send = -0.25f;
    channel.outputs.fx_send = {0.125f, 0.25f, 0.375f};
    channel.pan_mode = 1;
    channel.pan_curve = 2;
    channel.biquad_type = 3;
    channel.biquad_value = 0.5f;
    const Wii::ChannelVoiceParam& param = engine.VoiceOf(channel).param;

    engine.Channels().UpdateAllChannel(engine);

    XSFLIB_CHECK(param.pan == 0.0f && !std::signbit(param.pan));
    XSFLIB_CHECK(param.surround_pan == 0.75f && param.lpf_freq == 0.5f && param.main_out_volume == 0.75f);
    XSFLIB_CHECK(param.main_send == -0.25f && param.fx_send == (std::array<float, 3>{0.125f, 0.25f, 0.375f}));
    XSFLIB_CHECK(param.pan_mode == 1 && param.pan_curve == 2 && param.biquad_type == 3 && param.biquad_value == 0.5f);

    // A pan LFO adds its value before the frame's step.
    channel.lfo_types[0] = 2;
    channel.lfos[0].param.depth = 1.0f;
    channel.lfos[0].param.speed = 50.0f;
    engine.Channels().UpdateAllChannel(engine);

    const float lfo = channel.lfos[0].GetValue();
    engine.Channels().UpdateAllChannel(engine);

    XSFLIB_CHECK(lfo != 0.0f && Bits(param.pan) == Bits(0.0f + lfo));

    // A frame that isn't periodic moves nothing on.
    const float held = channel.lfos[0].GetValue();
    const int frames = engine.VoiceOf(channel).frames;

    channel.Update(engine, false);

    XSFLIB_CHECK(channel.lfos[0].GetValue() == held && Bits(param.pan) == Bits(0.0f + held));

    // A channel without a voice takes the frame's steps, and the voice gets nothing.
    const ntdlib::nwsnd::VoiceId voice = channel.voice;
    channel.voice = ntdlib::nwsnd::kNoVoice;

    channel.Update(engine, true);

    XSFLIB_CHECK(channel.lfos[0].GetValue() != held && engine.Voices().At(voice).frames == frames + 1);

    channel.voice = voice;
}

} // namespace

int main()
{
    TestArithmetic();
    TestBuilds();
    TestWavePlacement();
    TestLfo();
    TestEnvelope();
    TestRatios();
    TestChannelVolume();
    TestReleasedNotes();
    TestVolumeLfo();
    TestChannelPitch();
    TestChannelOutputs();

    return xsflib::testing::Summary("Wii policy");
}
