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

// The scale and its conversions are the commands' (commands/units.h).
using commands::FromUnit;
using commands::Scale;
using commands::ToUnit;

// The scale of the value of a command of `kind` (commands::UnitOf) on the grammar's console. A note's velocity takes
// kLevelScale below, and a time kTicksScale.
Scale ScaleOf(commands::Kind kind, const Grammar& grammar);

// The scales of a note's velocity and of a time: unchanged.
inline constexpr Scale kLevelScale{};
inline constexpr Scale kTicksScale{};

} // namespace ntdlib::nwsnd
