// SPDX-License-Identifier: MIT

#include "ntdlib/nwsnd/envelope.h"

#include <algorithm>
#include <cstdint>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/tables.h"

namespace ntdlib::nwsnd
{
namespace
{

// The LFOs' delay: `msec` milliseconds more of it. Returns whether the delay has ended. `msec` then holds the
// milliseconds after its end.
bool PassDelay(uint32_t& counter, uint32_t delay, int& msec)
{
    if (counter < delay)
    {
        if (counter + static_cast<uint32_t>(msec) <= delay)
        {
            counter += static_cast<uint32_t>(msec);
            return false;
        }

        msec -= static_cast<int>(delay - counter);
        counter = delay;
    }

    return true;
}

} // namespace

float DecibelSquare(int sustain)
{
    return static_cast<float>(tables::kDecibelSquare[std::clamp(sustain, 0, 127)]);
}

int HoldMs(int hold)
{
    return (hold + 1) * (hold + 1) / 4;
}

template <typename Policy>
void CurveAdshr<Policy>::Initialize(float decibel)
{
    SetAttack(127);
    SetHold(0);
    SetDecay(127);
    SetSustain(127);
    SetRelease(127);
    Reset(decibel);
}

template <typename Policy>
void CurveAdshr<Policy>::SetAttack(int attack)
{
    // Archives give 0 to 127.
    attack_ = tables::kAttack[std::clamp(attack, 0, 127)];
}

template <typename Policy>
void CurveAdshr<Policy>::SetHold(int hold)
{
    hold_ = static_cast<uint16_t>(HoldMs(hold));
}

template <typename Policy>
void CurveAdshr<Policy>::SetDecay(int decay)
{
    decay_ = CalcRelease(decay);
}

template <typename Policy>
void CurveAdshr<Policy>::SetSustain(int sustain)
{
    sustain_ = static_cast<uint8_t>(sustain);
}

template <typename Policy>
void CurveAdshr<Policy>::SetRelease(int release)
{
    release_ = CalcRelease(release);
}

template <typename Policy>
float CurveAdshr<Policy>::CalcRelease(int release)
{
    if (release == 127)
    {
        return 65535.0f;
    }
    if (release == 126)
    {
        return 24.0f;
    }
    if (release < 50)
    {
        return Policy::ReleaseRate(static_cast<float>(release * 2 + 1) * 0.0078125f);
    }

    return Policy::ReleaseRate(60.0f / static_cast<float>(126 - release));
}

template <typename Policy>
float CurveAdshr<Policy>::GetValue() const
{
    if (status_ == Status::kAttack && attack_ == 0.0f)
    {
        return 0.0f;
    }

    return Policy::EnvelopeLevel(value_);
}

template <typename Policy>
void CurveAdshr<Policy>::Update(int msec)
{
    switch (status_)
    {
    case Status::kAttack:
        {
            // The level is multiplied by the attack each millisecond. The hold starts once the level is within 1/32 of
            // 0.1 dB of 0.
            float value = value_;
            for (int i = 0; i < msec; ++i)
            {
                value *= attack_;
                if (value > -0.03125f)
                {
                    value_ = 0.0f;
                    hold_counter_ = hold_;
                    status_ = Status::kHold;
                    return;
                }
            }

            value_ = value;
            return;
        }

    case Status::kHold:
        if (msec < hold_counter_)
        {
            hold_counter_ = static_cast<uint16_t>(hold_counter_ - msec);
            return;
        }

        msec -= hold_counter_;
        hold_counter_ = 0;
        status_ = Status::kDecay;
        [[fallthrough]];

    case Status::kDecay:
        {
            // The decay and the release fall by their rate times the milliseconds (see the policy's Fall).
            const float sustain = DecibelSquare(sustain_);
            const float value = Policy::Fall(value_, decay_, msec);
            if (Policy::ReachesSustain(value, sustain))
            {
                value_ = sustain;
                status_ = Status::kSustain;
                return;
            }

            value_ = value;
            return;
        }

    case Status::kRelease:
        value_ = Policy::Fall(value_, release_, msec);
        return;

    default:
        return;
    }
}

template <typename Policy>
void CurveLfo<Policy>::Reset()
{
    delay_counter_ = 0;
    counter_ = 0.0f;
    random_value_ = 1.0f;
    started_ = false;
    wrapped_ = false;
}

template <typename Policy>
void CurveLfo<Policy>::Update(int msec)
{
    if (!PassDelay(delay_counter_, param.delay, msec) || !(param.speed > 0.0f))
    {
        return;
    }

    // The cycle starts at the phase, and advances by the speed times the seconds.
    if (!started_)
    {
        started_ = true;
        counter_ = static_cast<float>(param.phase) / 127.0f;
    }
    const float counter = counter_ + param.speed * static_cast<float>(msec) / 1000.0f;
    wrapped_ = Policy::Wrapped(counter);
    counter_ = counter - static_cast<float>(Policy::ToInt32(counter));
}

template <typename Policy>
float CurveLfo<Policy>::GetValue(Random& random)
{
    if (param.depth == 0.0f || delay_counter_ < param.delay)
    {
        return 0.0f;
    }

    float value = 1.0f;
    switch (param.curve)
    {
    case 0:
        value = Policy::Sine(counter_);
        break;

    case 1:
        value = Policy::Triangle(counter_);
        break;

    case 2:
        value = counter_;
        break;

    case 3:
        value = Policy::Square(counter_);
        break;

    case 4:
        // CurveRandom (0x022BB610): a number from 0 to 1. The number is drawn while the cycle has just wrapped.
        if (wrapped_)
        {
            random_value_ = static_cast<float>(random.Next()) / 65535.0f;
        }
        value = random_value_;
        break;

    default:
        break;
    }

    return value * param.depth * static_cast<float>(param.range);
}

template class CurveAdshr<CafePolicy>;
template class CurveAdshr<NxPolicy>;
template class CurveAdshr<CtrPolicy>;
template class CurveAdshr<RevolutionPolicy>;
template class CurveLfo<CafePolicy>;
template class CurveLfo<NxPolicy>;

template <typename Policy>
void TableLfo<Policy>::Reset()
{
    delay_counter_ = 0;
    counter_ = 0.0f;
}

template <typename Policy>
void TableLfo<Policy>::Update(int msec)
{
    if (!PassDelay(delay_counter_, param.delay, msec))
    {
        return;
    }

    const float counter = counter_ + Policy::LfoPhaseStep(param.speed, msec);
    counter_ = counter - static_cast<float>(static_cast<int>(counter));
}

template <typename Policy>
float TableLfo<Policy>::GetValue() const
{
    if (param.depth == 0.0f || param.delay > delay_counter_)
    {
        return 0.0f;
    }

    // The place in the cycle in 128ths, and the sine from the quarter wave.
    const int place = static_cast<int>(counter_ * 128.0f);
    const auto sine = [](int step)
    {
        return static_cast<int>(tables::kLfoSine[std::clamp(step, 0, 32)]);
    };

    int value;
    if (place < 32)
    {
        value = sine(place);
    }
    else if (place < 64)
    {
        value = sine(64 - place);
    }
    else if (place < 96)
    {
        value = -sine(place - 64);
    }
    else
    {
        value = -sine(128 - place);
    }

    return Policy::LfoLevel(value, param.depth, param.range);
}

template class TableLfo<CtrPolicy>;
template class TableLfo<RevolutionPolicy>;

} // namespace ntdlib::nwsnd
