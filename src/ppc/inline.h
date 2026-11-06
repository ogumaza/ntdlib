// SPDX-License-Identifier: MIT

// Attributes that force the compiler to inline. Only the files in src/ppc include this header.

#pragma once

// For a function. Run's loop calls Step for every instruction. Inline, the loop dispatches each instruction with no
// call.
#if defined(_MSC_VER) && !defined(__clang__)
#define NTDLIB_PPC_FORCE_INLINE __forceinline
#else
#define NTDLIB_PPC_FORCE_INLINE inline __attribute__((always_inline))
#endif

// For a lambda. It goes after the lambda's parameter list. The opcode functions call their helper lambdas from many
// cases. Out of line, each call of such a function first stored the lambdas' captures on the stack, whichever case
// ran.
#if defined(_MSC_VER) && !defined(__clang__)
#define NTDLIB_PPC_INLINE_LAMBDA [[msvc::forceinline]]
#else
#define NTDLIB_PPC_INLINE_LAMBDA __attribute__((always_inline))
#endif
