// SPDX-License-Identifier: MIT

// NintendoWare's envelope (ADSHR) timing and its SoundFont 2 and SFZ equivalents. The timing comes from the core's
// CurveAdshr (nwsnd/envelope.h), in the arithmetic of the console policy for Settings' engine. Settings also gives the
// step.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

// nw::snd's silence threshold, -90.4 dB, in 0.1 dB units. Attack starts here; release stops the channel below it.
constexpr float kSilentLevel = -904.0f;

// Attack duration from -90.4 dB to full level, in milliseconds.
double AttackMs(int attack);

// Decay or release rate for an MML value, in 0.1 dB per millisecond, in the engine's single-precision arithmetic.
float DecayRate(int value, Engine engine);

// MML envelope parameters, with nw::snd's defaults for regions without an envelope.
struct EnvelopeValues
{
    int attack = 127, decay = 127, sustain = 127, hold = 127, release = 127;
};

// Release starting level after `frames` envelope updates, in 0.1 dB units. Before the first update it is -90.4 dB, even
// for an instant attack.
float HeldLevel(const EnvelopeValues& v, int64_t frames, const Settings& settings);

// The levels HeldLevel gives after 1 to `frames` updates, worked out in one pass.
std::vector<float> HeldLevels(const EnvelopeValues& v, int64_t frames, const Settings& settings);

// Level after `frames` release updates with the game's stepping. `level` is in 0.1 dB units; `rate` is the DecayRate
// result, in 0.1 dB per millisecond.
float ReleasedLevel(float level, float rate, int64_t frames, const Settings& settings);

// Counts release updates until the channel falls below -90.4 dB and stops. `level` is in 0.1 dB units; `rate` is
// positive, in 0.1 dB per millisecond (see DecayRate).
int64_t ReleaseFrames(float level, float rate, const Settings& settings);

// Counts envelope updates until a held note has attacked and then decayed to `level` or below (0.1 dB units). Returns
// -1 if it never reaches that level.
int64_t FramesToLevel(const EnvelopeValues& v, float level, const Settings& settings);

// Sustain attenuation at or above this value counts as inaudible (centibels).
constexpr int kInaudibleSustain = 400;

// The attenuation in centibels down to which the MIDI note of a channel that its length can't release follows the
// channel's fade. A loud note can still be heard at kInaudibleSustain.
constexpr int kFadedAttenuation = 600;

// SoundFont volume envelope generators (timecents; sustain in centibels of attenuation).
struct Sf2Envelope
{
    int attack = -12000, hold = -12000, decay = -12000, sustain = 0, release = -12000;
};

// Converts the game's envelope timing. The SoundFont attack is linear, fitted to the game's attack curve.
Sf2Envelope ConvertEnvelope(const EnvelopeValues& v, const Settings& settings);

// The time the SoundFont and SFZ attacks take to reach full level, in milliseconds. Both rise linearly in amplitude.
double Sf2AttackMs(const EnvelopeValues& v, const Settings& settings);

// SFZ amplitude envelope (ampeg_*): times in seconds, sustain as a percentage of full amplitude.
struct SfzEnvelope
{
    double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 100.0, release = 0.0;
};

// Converts to an SFZ envelope, with the same attack fit as SoundFont. As in the game, decay and release are linear in
// dB. They fall by a factor of e^-8 (69.49 dB) over their time. sfzformat.com's ampeg_attack page gives that curve.
SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& v, const Settings& settings);

} // namespace ntdlib::sequence_export
