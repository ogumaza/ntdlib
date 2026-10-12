// SPDX-License-Identifier: MIT

// The sound library's envelope (CurveAdshr) and LFO (CurveLfo), for a console policy (see console.h). Each function's
// comment gives the address of the routine it follows in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <cstdint>

#include "ntdlib/nwsnd/util.h"

namespace ntdlib::nwsnd
{

// An envelope's stage.
enum class AdshrStatus : uint32_t
{
    kAttack = 0,
    kHold = 1,
    kDecay = 2,
    kSustain = 3,
    kRelease = 4,
};

// The level in 0.1 dB of (sustain / 127)^2 (CurveAdshr::CalcDecibelSquare, 0x022BAF48). Archives give 0 to 127. A
// larger value gives 127's level.
float DecibelSquare(int sustain);

// The hold's length in milliseconds: (hold + 1)^2 / 4 (CurveAdshr::SetHold, 0x022B877C).
int HoldMs(int hold);

// An envelope of attack, hold, decay, sustain and release, in 0.1 dB. The envelope advances a millisecond at a time.
template <typename Policy>
class CurveAdshr
{
public:
    using Status = AdshrStatus;

    // The level a new envelope starts from, in dB.
    static constexpr float kInitialDecibel = -90.4f;

    // 0x022B8838.
    CurveAdshr()
    {
        Initialize(kInitialDecibel);
    }

    // The fastest attack, decay and release, and the highest sustain, then Reset (0x022B87E4).
    void Initialize(float decibel);

    // Back to the attack, from `decibel` (0x022B87C8).
    void Reset(float decibel)
    {
        status_ = Status::kAttack;
        value_ = decibel * 10.0f;
    }

    // The attack multiplies the level by a table's entry each millisecond (0x022B8764).
    void SetAttack(int attack);

    // The hold lasts HoldMs(hold) milliseconds (0x022B877C).
    void SetHold(int hold);

    void SetDecay(int decay);     // 0x022B8794
    void SetSustain(int sustain); // 0x022B87C0
    void SetRelease(int release); // 0x022B5FFC

    Status GetStatus() const
    {
        return status_;
    }

    void SetStatus(Status status)
    {
        status_ = status;
    }

    // The level in dB (0x022BB160): 0 during an attack that takes no time.
    float GetValue() const;

    // The level as the envelope keeps it, in 0.1 dB (+0x4).
    float Level() const
    {
        return value_;
    }

    // Advances `msec` milliseconds (0x022BAF5C).
    void Update(int msec);

    // The fall in 0.1 dB a millisecond that a decay or release value gives (0x022B5F34).
    static float CalcRelease(int release);

private:
    Status status_ = Status::kAttack;
    float value_ = 0.0f;
    float decay_ = 0.0f;
    float release_ = 0.0f;
    float attack_ = 0.0f;
    uint16_t hold_ = 0;
    uint16_t hold_counter_ = 0;
    uint8_t sustain_ = 0;
};

// An LFO's settings (CurveLfoParam, set by 0x022B8880).
struct CurveLfoParam
{
    bool operator==(const CurveLfoParam&) const = default;

    float depth = 0.0f;
    float speed = 6.25f; // cycles a second
    uint32_t delay = 0;  // in milliseconds
    uint8_t range = 1;
    uint8_t curve = 0; // 0 sine, 1 triangle, 2 saw, 3 square, 4 random
    uint8_t phase = 0; // the start, in 127ths of a cycle
};

// A low-frequency oscillator. The oscillator advances a millisecond at a time. Its value is the curve's value at the
// oscillator's place in its cycle, times the depth and the range.
template <typename Policy>
class CurveLfo
{
public:
    // 0x022B96D0.
    void Reset();

    // 0x022BB194.
    void Update(int msec);

    // 0x022BB2BC. The random curve draws a number from `random` when the cycle has just wrapped, and holds it until the
    // next wrap. A curve with no function (5 and up, for a game to register) gives 1.
    float GetValue(Random& random);

    CurveLfoParam param;

private:
    uint32_t delay_counter_ = 0;
    float counter_ = 0.0f;
    float random_value_ = 1.0f;
    bool started_ = false;
    bool wrapped_ = false;
};

// The table LFO of the 3DS's nw::snd (Lfo::Update, 0x1811c8, and Lfo::GetValue, 0x181434, in Pokemon X's code.bin) and
// the Wii's NW4R SND. This LFO plays one curve, a sine from kLfoSine's quarter wave in 32 steps. The LFO has no phase
// or random value, and ignores those settings. The policy gives the LFO's phase step (LfoPhaseStep) and level
// (LfoLevel).
template <typename Policy>
class TableLfo
{
public:
    // 0x321064.
    void Reset();

    void Update(int msec);
    float GetValue() const;

    // The template's channels pass the random generator. This LFO draws no numbers from it.
    float GetValue(Random&) const
    {
        return GetValue();
    }

    CurveLfoParam param;

private:
    uint32_t delay_counter_ = 0;
    float counter_ = 0.0f;
};

struct CafePolicy;
struct NxPolicy;
struct CtrPolicy;
struct RevolutionPolicy;

// The library builds the envelope for the four consoles, the LFO for the Wii U and the Switch, and the table LFO for
// the 3DS and the Wii.
extern template class CurveAdshr<CafePolicy>;
extern template class CurveAdshr<NxPolicy>;
extern template class CurveAdshr<CtrPolicy>;
extern template class CurveAdshr<RevolutionPolicy>;
extern template class TableLfo<CtrPolicy>;
extern template class TableLfo<RevolutionPolicy>;
extern template class CurveLfo<CafePolicy>;
extern template class CurveLfo<NxPolicy>;

} // namespace ntdlib::nwsnd
