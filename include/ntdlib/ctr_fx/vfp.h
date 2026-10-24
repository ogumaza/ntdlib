// SPDX-License-Identifier: MIT

// The single-precision arithmetic of the 3DS's ARM11 in the mode that the games' code runs in, FPSCR 0x03000000. In
// that mode, results round to nearest with ties to even, denormal values flush to zero, and a NaN comes out as the
// default NaN. The kernel starts each thread with FPSCR 0x03c00000, toward zero. Each game's startup code then sets
// 0x03000000 (Pokémon X's 0x122a5c, at boot and at the start of each thread). The setup of the 3DS's sound effects
// computes their coefficients in that mode. Each function gives the VFP's result bit for bit, on any host. It computes
// the result to nearest in double precision, and then to nearest in single precision.

#pragma once

#include <cstdint>

namespace ntdlib::ctr_fx::vfp
{

// vadd.f32, vsub.f32, vmul.f32 and vdiv.f32. A multiply-accumulate (vmla.f32 and vmls.f32) rounds twice on the ARM11:
// Add(d, Mul(n, m)) and Sub(d, Mul(n, m)).
float Add(float a, float b);
float Sub(float a, float b);
float Mul(float a, float b);
float Div(float a, float b);

// vcvt.f32.u32 and vcvt.f32.s32.
float FromU32(uint32_t value);
float FromS32(int32_t value);

// vcvt.u32.f32 and vcvt.s32.f32: toward zero whatever the mode, saturated to the integer's range. A NaN gives 0.
uint32_t ToU32(float value);
int32_t ToS32(float value);

// The power function `powf` of the C library that 3DS games link (Pokémon X's at 0x18fb50), in the mode above. It
// covers the arguments that the effects give it: an `x` that is positive and normal, outside the bit patterns
// 0x3f7ff000 to 0x3f801000 next to 1, and a `y` of 0 or less, minus infinity included. It throws std::domain_error for
// other arguments. The function takes 16·log2(x) from a table of 16 entries and a series, in two parts of 12
// significant bits and the rest, multiplies it by `y` in the same two parts, and takes the power of 2 from another
// table of 16 entries and a polynomial.
float PowF(float x, float y);

} // namespace ntdlib::ctr_fx::vfp
