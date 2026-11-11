// SPDX-License-Identifier: MIT

// Attributes that force the compiler to inline. Only the files in src/dsp and src/ppc include this header.

#pragma once

// Forces the compiler to inline a function. The DSP's instruction handlers rely on this to fold their decoding away.
// In the PowerPC, Run's loop calls Step for every instruction. Inline, the loop dispatches each instruction with no
// call.
#if defined(_MSC_VER) && !defined(__clang__)
#define NTDLIB_FORCE_INLINE __forceinline
#else
#define NTDLIB_FORCE_INLINE inline __attribute__((always_inline))
#endif

// The same for a lambda. It goes after the lambda's parameter list. The DSP's generic decoder calls its helper lambdas
// from many cases. The compiler would otherwise call them out of line there. The PowerPC's opcode functions call their
// helper lambdas from many cases too. Out of line, each call of such a function first stored the lambdas' captures on
// the stack, whichever case ran.
#if defined(_MSC_VER) && !defined(__clang__)
#define NTDLIB_INLINE_LAMBDA [[msvc::forceinline]]
#else
#define NTDLIB_INLINE_LAMBDA __attribute__((always_inline))
#endif
