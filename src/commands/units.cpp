// SPDX-License-Identifier: MIT

#include "ntdlib/commands/units.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "ntdlib/commands/sequence.h"

namespace ntdlib::commands
{
namespace
{

// The units' names, in the order of Unit.
constexpr std::array<std::string_view, kUnitCount> kNames = {
    "number", "ticks", "level", "send", "cents", "bend", "pan", "bpm", "ms", "hz", "fraction",
};

} // namespace

std::optional<Unit> UnitOf(Kind kind)
{
    switch (kind)
    {
    case Kind::kNote:
    case Kind::kWait:
        return Unit::kTicks;
    case Kind::kVolume:
    case Kind::kMainVolume:
    case Kind::kVolume2:
        return Unit::kLevel;
    case Kind::kSend:
    case Kind::kMainSend:
        return Unit::kSend;
    case Kind::kTranspose:
    case Kind::kBendRange:
    case Kind::kSweepPitch:
    case Kind::kTune:
        return Unit::kCents;
    case Kind::kPitchBend:
        return Unit::kBend;
    case Kind::kPan:
    case Kind::kInitialPan:
    case Kind::kSurroundPan:
        return Unit::kPan;
    case Kind::kTempo:
        return Unit::kBpm;
    case Kind::kLfoDelay:
    case Kind::kLfoPeriod:
        return Unit::kMs;
    case Kind::kLfoSpeed:
        return Unit::kHz;
    case Kind::kLfoDepth:
    case Kind::kLowPass:
        return Unit::kFraction;
    case Kind::kOpenTrack:
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kReturn:
    case Kind::kLoopEnd:
    case Kind::kEnd:
    case Kind::kEnvelopeReset:
    case Kind::kNoteOff:
    case Kind::kData:
        return std::nullopt;
    default:
        return Unit::kNumber;
    }
}

Fixed ToUnit(int64_t value, const Scale& scale)
{
    return value * scale.step + scale.offset;
}

int64_t FromUnit(Fixed unit, const Scale& scale)
{
    // The console's values on either side of the unit. Of two at the same distance, FromUnit takes the one further
    // from zero.
    const Fixed above = unit - scale.offset;
    int64_t value = above / scale.step;
    if (above % scale.step != 0 && above < 0)
    {
        --value;
    }

    const Fixed low = ToUnit(value, scale);
    const Fixed high = ToUnit(value + 1, scale);
    const Fixed down = unit - low;
    const Fixed up = high - unit;
    if (up < down || (up == down && (high < 0 ? -high : high) > (low < 0 ? -low : low)))
    {
        ++value;
    }

    return value;
}

std::string_view Name(Unit unit)
{
    return kNames[static_cast<std::size_t>(unit)];
}

std::optional<Unit> UnitNamed(std::string_view name)
{
    for (std::size_t i = 0; i < kNames.size(); ++i)
    {
        if (kNames[i] == name)
        {
            return static_cast<Unit>(i);
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::commands
