// SPDX-License-Identifier: MIT

// The bank file of Pikmin's JAudio (.bx): its wave systems (WSYS) and its banks (IBNK) in one file. A 16-byte header
// gives two tables: the offset of the wave systems' table and its number of entries, then the same for the banks'
// table. Each entry gives a block's offset and size. An entry of size 0 is an empty place. The game registers each
// wave system and each bank by its place in its table (0x279d90 in the main of the Switch's release). The GameCube's
// release stores the file big-endian. The Switch's release stores it little-endian, and widens each offset of its
// wave systems and banks to 64 bits (bank.h and wave_system.h). Its tables keep 32-bit entries.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/wave_system.h"

namespace ntdlib::jaudio
{

// The file's wave systems, in the order of the table. A system's number is its place in the table. A system's records
// of its archives hold the number of waves at kWaveCountAt in a big-endian file, and at kWideWaveCountAt in a
// little-endian one. Throws FormatError if the file or a wave system is damaged.
std::vector<WaveSystem> ReadBxWaveSystems(std::span<const uint8_t> bx);

// The file's banks, in the order of the table. A sequence selects a bank by the number in the bank's header. The game
// maps each number up to 0xff to the first bank in the table that gives it (0x272e10). Bank::number holds that number.
//
// A velocity region names the wave system of its wave in the high half of its wave id (0x273570). The value 0xffff
// names the system at the bank's place in the table. Another value names the first system in the table whose header
// gives that number. The game maps the numbers up to 0xff (0x272de0). Bank::wave_system holds the place of the system
// that the bank's waves name. A bank whose waves name more than one system, or a system that the file lacks, is left
// out, and so is a bank that can't be read. `warnings` gets a message for each. Throws FormatError if the file is
// damaged.
std::vector<Bank> ReadBxBanks(std::span<const uint8_t> bx, std::vector<std::string>& warnings);

} // namespace ntdlib::jaudio
