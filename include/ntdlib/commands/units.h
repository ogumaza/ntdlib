// SPDX-License-Identifier: MIT

// The units of the commands' values, common to every console. Each console's values convert to them by the console's
// curves, and back. A value then means the same on every console: a level is a gain and a pitch is in cents, whatever
// the console that stored it. The units take the Switch's scales where a quantity has no usual unit: a Switch value
// converts to its unit without rounding.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ntdlib/commands/sequence.h"

namespace ntdlib::commands
{

// What a value measures.
enum class Unit : uint8_t
{
    kNumber,   // a count, a number, or a setting as the Switch's library stores it
    kTicks,    // ticks of the sequence's timebase
    kLevel,    // a level on MIDI's volume curve: a gain of (level / 127) squared
    kSend,     // a send's level: a gain of send / 127
    kCents,    // a pitch in cents
    kBend,     // a fraction of the bend range
    kPan,      // a position in 63rds: from the centre for a pan, from the front for a surround pan
    kBpm,      // beats a minute
    kMs,       // milliseconds
    kHz,       // hertz
    kFraction, // a fraction of the setting's range
};

inline constexpr int kUnitCount = static_cast<int>(Unit::kFraction) + 1;

// A value in its unit, in fixed point with 16 fraction bits.
using Fixed = int64_t;
inline constexpr Fixed kOne = 65536;

// The unit of the value of a command of `kind`, or nothing for a kind without a value. A note's value is its length,
// in kTicks, and its velocity is a kLevel. A time is in kTicks.
std::optional<Unit> UnitOf(Kind kind);

// A unit's name in the text form, such as "cents".
std::string_view Name(Unit unit);

// The unit with `name`, or nothing.
std::optional<Unit> UnitNamed(std::string_view name);

} // namespace ntdlib::commands
