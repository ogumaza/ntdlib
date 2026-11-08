// SPDX-License-Identifier: MIT

#pragma once

namespace ntdlib::alphadream
{

// The routine that turns a sample's rate and a pitch into the mixer's step (0x0819A204). Its two adr instructions
// give the table of keys and the table of steps.
constexpr const char* kStepRoutine = "4684 A31A 5CC9 A30C 0708 0E80 18C0 6803 6840 1AC0 4350 0A00 181B 0909 40CB 4Axx";

} // namespace ntdlib::alphadream
