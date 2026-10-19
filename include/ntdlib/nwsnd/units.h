// SPDX-License-Identifier: MIT

// NintendoWare's values in the commands' units (ntdlib/commands/units.h). Each value converts by a scale: its unit's
// value is the console's value times a step, plus an offset. A volume of 100 is a level of 100, a pan of 0 is -64
// 63rds, a transpose of -1 is -100 cents, and a pitch bend of 64 is half the bend range. Every console value converts
// exactly, and back.

#pragma once

#include <cstdint>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"

namespace ntdlib::nwsnd
{

struct Scale
{
    friend bool operator==(const Scale&, const Scale&) = default;

    commands::Fixed step = commands::kOne;
    commands::Fixed offset = 0;
};

// The scale of the value of a command of `kind` (commands::UnitOf) on the grammar's console. A note's velocity takes
// kLevelScale below, and a time kTicksScale.
Scale ScaleOf(commands::Kind kind, const Grammar& grammar);

// The scales of a note's velocity and of a time: unchanged.
inline constexpr Scale kLevelScale{};
inline constexpr Scale kTicksScale{};

// The console's value `value` in its unit.
commands::Fixed ToUnit(int64_t value, const Scale& scale);

// The console's value nearest `unit`. Of two values at the same distance, the one whose unit lies further from zero.
// The value that gave `unit` comes back.
int64_t FromUnit(commands::Fixed unit, const Scale& scale);

} // namespace ntdlib::nwsnd
