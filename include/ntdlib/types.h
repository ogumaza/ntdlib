// SPDX-License-Identifier: MIT

// Address types for the memory of an emulated console.

#pragma once

#include <cstdint>

namespace ntdlib
{

// A physical address. On the Wii, MEM1 is at 0 and MEM2 at 0x10000000.
using PAddr = uint32_t;

} // namespace ntdlib
