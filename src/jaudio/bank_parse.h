// SPDX-License-Identifier: MIT

// Bank::Parse with the numbers of the wave systems that a bank's waves name. A bank file (bx.h) names each wave's
// system in the high half of its velocity region's wave id.

#pragma once

#include <cstdint>
#include <set>
#include <span>

#include "ntdlib/jaudio/bank.h"

namespace ntdlib::jaudio::detail
{

// A bank, and the high halves of its velocity regions' wave ids, each once.
struct ParsedBank
{
    Bank bank;
    std::set<uint16_t> wave_systems;
};

// Reads the bank at the start of `file` as Bank::Parse does.
ParsedBank ParseBank(std::span<const uint8_t> file, uint32_t wave_system);

} // namespace ntdlib::jaudio::detail
