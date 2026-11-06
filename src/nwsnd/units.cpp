// SPDX-License-Identifier: MIT

#include "ntdlib/nwsnd/units.h"

#include <cstdint>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"

namespace ntdlib::nwsnd
{

using commands::Fixed;
using commands::Kind;
using commands::kOne;

Scale ScaleOf(Kind kind, const Grammar& grammar)
{
    switch (kind)
    {
    case Kind::kPan:
    case Kind::kInitialPan:
        return {kOne, -64 * kOne}; // the player takes 64 off
    case Kind::kTranspose:
    case Kind::kBendRange:
        return {100 * kOne, 0}; // semitones
    case Kind::kSweepPitch:
        return {100 * kOne / grammar.sweep_steps, 0}; // 64ths of a semitone, or 32nds on the Wii before 2008
    case Kind::kPitchBend:
        return {kOne / 128, 0};
    case Kind::kLowPass:
        return {kOne / 64, -kOne}; // (value - 64) / 64
    case Kind::kLfoDepth:
        return {kOne / 128, 0};
    case Kind::kLfoSpeed:
        return {kOne * 25 / 64, 0}; // 0.390625 Hz
    case Kind::kLfoDelay:
        return {5 * kOne, 0}; // 5 ms
    case Kind::kLfoPeriod:
        return {10 * kOne, 0}; // 10 ms
    default:
        return {};
    }
}

} // namespace ntdlib::nwsnd
