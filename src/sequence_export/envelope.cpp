// SPDX-License-Identifier: MIT

// nw::snd ADSHR envelopes (CurveAdshr) and their SoundFont 2/SFZ conversions. The timing, the arithmetic and the tables
// come from the core's CurveAdshr (nwsnd/envelope.h), for the console policy of the engine that Settings names.

#include "ntdlib/sequence_export/envelope.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "seqlib/sf2/units.h"

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{
namespace
{

// SoundFont attack is linear in amplitude; nw::snd attack is exponential in dB. The game's attack stays quiet longer
// before rising sharply. A ramp lasting 0.55 of the game's attack gives the lowest mean dB error at floors of -40, -60
// and -90 dB. The curve has the same shape for every attack value. The fit therefore applies to all of them.
//
// At that point, nw::snd is within 0.5 dB of full level. The remaining time goes to the hold. The decay then starts on
// time. An initial envelope delay would fit the start better, but FluidSynth delays sample playback too.
constexpr double kAttackRamp = 0.55;

// SoundFont's slowest decay or release, 8000 timecents, in milliseconds for a 100 dB fall. MML values 0 to 2 are
// slower.
constexpr double kSlowestSf2DecayMs = 101593.6673259648;

// Calls `function` with the std::type_identity of the console policy (nwsnd/console.h) for `engine`.
template <typename Function>
auto OnPolicy(Engine engine, const Function& function)
{
    switch (engine)
    {
    case Engine::kRevolution:
        return function(std::type_identity<nwsnd::RevolutionPolicy>{});
    case Engine::kCtr:
        return function(std::type_identity<nwsnd::CtrPolicy>{});
    case Engine::kCafe:
        return function(std::type_identity<nwsnd::CafePolicy>{});
    default:
        return function(std::type_identity<nwsnd::NxPolicy>{});
    }
}

// Whether a released envelope at `level` (0.1 dB) still sounds on the core: the channel's update stops one below
// -90.4 dB.
template <typename Policy>
bool CoreAudible(float level)
{
    return !(Policy::EnvelopeLevel(level) < nwsnd::CurveAdshr<Policy>::kInitialDecibel);
}

// One release or decay step of `msec` milliseconds: the level falls by the rate times the milliseconds, in the policy's
// arithmetic. The Wii U does it in one fused multiply-add (CurveAdshr::Update, 0x022BAF5C). The other consoles round
// the product and the difference apart (the Switch's CurveAdshr::Update, 0xc19ab0).
float Fall(float level, float rate, int msec, Engine engine)
{
    const auto fall = [=]<typename Policy>(std::type_identity<Policy>)
    {
        return Policy::Fall(level, rate, msec);
    };
    return OnPolicy(engine, fall);
}

// The channel's level is audible: above -90.4 dB in the comparison of the core's channel update (Channel::Update,
// 0x022C42C8). The Wii U and the Switch divide the envelope's value by 10, and the 3DS multiplies it by 0.1 in single
// precision. The Wii truncates its tenths of a dB (EnvGenerator::GetValue, main.dol 0x80038fb0).
bool Audible(float level, Engine engine)
{
    if (engine == Engine::kRevolution)
    {
        return static_cast<int>(10.0f * std::max(level / 10.0f, -90.4f)) > -904;
    }

    const auto audible = [level]<typename Policy>(std::type_identity<Policy>)
    {
        return CoreAudible<Policy>(level);
    };
    return OnPolicy(engine, audible);
}

// The core's envelope at the start of a note with the values `v`.
template <typename Policy>
nwsnd::CurveAdshr<Policy> CoreEnvelope(const EnvelopeValues& v)
{
    nwsnd::CurveAdshr<Policy> envelope;
    envelope.SetAttack(v.attack);
    envelope.SetHold(v.hold);
    envelope.SetDecay(v.decay);
    envelope.SetSustain(v.sustain);
    envelope.SetRelease(v.release);
    envelope.Reset(nwsnd::CurveAdshr<Policy>::kInitialDecibel);

    return envelope;
}

// The core envelope's level in 0.1 dB: 0 during an attack that takes no time. As in nw::snd, an instant attack's level
// is full before the first update, but the envelope still holds -90.4 dB (CurveAdshr::Level).
template <typename Policy>
float CoreValue(const nwsnd::CurveAdshr<Policy>& envelope)
{
    const bool instant = envelope.GetStatus() == nwsnd::AdshrStatus::kAttack && envelope.GetValue() == 0.0f;

    return instant ? 0.0f : envelope.Level();
}

// HeldLevel, HeldLevels and FramesToLevel for one console policy.
template <typename Policy>
float HeldLevelOn(const EnvelopeValues& v, int64_t frames, const Settings& settings)
{
    nwsnd::CurveAdshr<Policy> envelope = CoreEnvelope<Policy>(v);
    const float sustain = nwsnd::DecibelSquare(v.sustain);
    for (int64_t i = 0; i < frames; i++)
    {
        envelope.Update(settings.env_step_ms);
        if (CoreValue(envelope) == sustain)
        {
            break; // sustaining: the level no longer changes
        }
    }

    return envelope.Level();
}

template <typename Policy>
std::vector<float> HeldLevelsOn(const EnvelopeValues& v, int64_t frames, const Settings& settings)
{
    nwsnd::CurveAdshr<Policy> envelope = CoreEnvelope<Policy>(v);
    std::vector<float> levels;
    levels.reserve(static_cast<std::size_t>(std::max<int64_t>(frames, 0)));
    for (int64_t i = 0; i < frames; i++)
    {
        envelope.Update(settings.env_step_ms);
        levels.push_back(envelope.Level());
    }

    return levels;
}

template <typename Policy>
int64_t FramesToLevelOn(const EnvelopeValues& v, float level, const Settings& settings)
{
    // The attack starts at -90.4 dB. It must rise above `level` before a later fall counts as decay to that level.
    nwsnd::CurveAdshr<Policy> envelope = CoreEnvelope<Policy>(v);
    bool risen = false;
    for (int64_t i = 1; i < 1000000; i++)
    {
        envelope.Update(settings.env_step_ms);
        if (CoreValue(envelope) > level)
        {
            risen = true;
        }
        else if (risen)
        {
            return i;
        }
    }

    return -1;
}

} // namespace

double AttackMs(int attack)
{
    // The core's envelope attacks from -90.4 dB a millisecond at a time until the hold starts. Every attack multiplier
    // is below 1. The attack therefore always finishes. The attack takes nothing from the policy. The Wii U's policy
    // stands for every console.
    nwsnd::CurveAdshr<nwsnd::CafePolicy> envelope;
    envelope.SetAttack(attack);
    int ms = 0;
    while (envelope.GetStatus() == nwsnd::AdshrStatus::kAttack)
    {
        envelope.Update(1);
        ms++;
    }

    return ms;
}

float DecayRate(int value, Engine engine)
{
    // The rate in 0.1 dB per millisecond comes from the core's CalcRelease (0x022B5F34), in the arithmetic of the
    // console's policy. The Wii U's code divides where the 3DS's code multiplies by reciprocals
    // (EnvGenerator::CalcRelease, code.bin 0x319730). A value above 127 takes 127's rate.
    const auto rate = [value]<typename Policy>(std::type_identity<Policy>)
    {
        return nwsnd::CurveAdshr<Policy>::CalcRelease(std::min(value, 127));
    };
    return OnPolicy(engine, rate);
}

float HeldLevel(const EnvelopeValues& v, int64_t frames, const Settings& settings)
{
    const auto held = [&]<typename Policy>(std::type_identity<Policy>)
    {
        return HeldLevelOn<Policy>(v, frames, settings);
    };
    return OnPolicy(settings.engine, held);
}

std::vector<float> HeldLevels(const EnvelopeValues& v, int64_t frames, const Settings& settings)
{
    const auto held = [&]<typename Policy>(std::type_identity<Policy>)
    {
        return HeldLevelsOn<Policy>(v, frames, settings);
    };
    return OnPolicy(settings.engine, held);
}

int64_t FramesToLevel(const EnvelopeValues& v, float level, const Settings& settings)
{
    if (nwsnd::DecibelSquare(v.sustain) > level)
    {
        return -1;
    }

    const auto frames = [&]<typename Policy>(std::type_identity<Policy>)
    {
        return FramesToLevelOn<Policy>(v, level, settings);
    };
    return OnPolicy(settings.engine, frames);
}

float ReleasedLevel(float level, float rate, int64_t frames, const Settings& settings)
{
    for (int64_t i = 0; i < frames; i++)
    {
        level = Fall(level, rate, settings.env_step_ms, settings.engine);
    }

    return level;
}

int64_t ReleaseFrames(float level, float rate, const Settings& settings)
{
    // Channel::Update (0x022C42C8) stops a releasing channel below -90.4 dB. The comparison takes the envelope's value,
    // divided by 10 (CurveAdshr::GetValue, 0x022BB160), in single precision. Release 127 crosses the entire range in
    // one frame; release 0 takes about 193,000 frames, almost ten minutes.
    if (!(rate > 0.0f))
    {
        return std::numeric_limits<int32_t>::max();
    }

    int64_t frames = 0;
    while (Audible(level, settings.engine))
    {
        level = Fall(level, rate, settings.env_step_ms, settings.engine);
        frames++;
    }

    return frames;
}

Sf2Envelope ConvertEnvelope(const EnvelopeValues& v, const Settings& settings)
{
    // The 3DS's envelope steps 5 ms a frame, and its frames are a little shorter. Its times scale by env_time_scale.
    using seqlib::sf2::ToTimecents;
    const double scale = settings.env_time_scale;
    const Engine engine = settings.engine;
    Sf2Envelope e;
    const double attack_ms = v.attack < 127 ? AttackMs(v.attack) * scale : 0.0;
    e.attack = ToTimecents(Sf2AttackMs(v, settings), -12000, 8000);

    // SoundFont decay and release times specify a 100 dB fall, linear in dB. A decay slower than SoundFont allows gets
    // a longer hold, up to the longest hold SoundFont allows. Holding until the faster decay reaches the sustain level
    // when the game's decay does would leave the note too loud until then. The error would be largest at the end of the
    // hold. Half that hold splits the error evenly: as loud at the end of the hold as quiet when the decay reaches the
    // sustain level.
    const double decay_ms = 1000.0 / DecayRate(v.decay, engine) * scale;
    double hold_ms = nwsnd::HoldMs(v.hold) * scale + attack_ms * (1.0 - kAttackRamp);
    if (decay_ms > kSlowestSf2DecayMs)
    {
        hold_ms += -nwsnd::DecibelSquare(v.sustain) / 1000.0 * (decay_ms - kSlowestSf2DecayMs) / 2.0;
    }

    e.hold = ToTimecents(hold_ms, -12000, 5000);
    e.decay = ToTimecents(decay_ms, -12000, 8000);
    e.sustain = std::clamp(-static_cast<int>(nwsnd::DecibelSquare(v.sustain)), 0, 1440);
    e.release = ToTimecents(1000.0 / DecayRate(v.release, engine) * scale, -12000, 8000);

    return e;
}

double Sf2AttackMs(const EnvelopeValues& v, const Settings& settings)
{
    return v.attack < 127 ? AttackMs(v.attack) * settings.env_time_scale * kAttackRamp : 0.0;
}

SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& v, const Settings& settings)
{
    // sfzformat.com's ampeg_attack page gives a decay's or release's step as close to exp(-8 / (time * rate)) a sample.
    // The stage then falls 8 * 200 / ln(10) tenths of a dB over its time. sfizz 1.2.3 steps by exp(-9 / (time * rate)).
    // Its decays and releases fall 12.5 % faster than the page gives.
    constexpr double kSfzFall = 8.0 * 200.0 / 2.302585092994046;

    const double scale = settings.env_time_scale;
    SfzEnvelope e;
    const double attack_ms = v.attack < 127 ? AttackMs(v.attack) * scale : 0.0;
    e.attack = Sf2AttackMs(v, settings) / 1000.0;
    e.hold = (nwsnd::HoldMs(v.hold) * scale + attack_ms * (1.0 - kAttackRamp)) / 1000.0;
    e.decay = kSfzFall / DecayRate(v.decay, settings.engine) * scale / 1000.0;
    e.sustain = 100.0 * std::pow(10.0, nwsnd::DecibelSquare(v.sustain) / 200.0);
    e.release = kSfzFall / DecayRate(v.release, settings.engine) * scale / 1000.0;

    return e;
}

} // namespace ntdlib::sequence_export
