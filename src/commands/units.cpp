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
    case Kind::kData:
        return std::nullopt;
    default:
        return Unit::kNumber;
    }
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
