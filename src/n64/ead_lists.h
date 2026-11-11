// SPDX-License-Identifier: MIT

// The commands of EAD's audio microcodes, as the models of EAD's engines append them, and the big-endian halfwords that
// the engines keep in the model's memory. A DMA command of these microcodes gives its size in bytes. The first form
// (Mario Kart 64) has segments at opcode 7. The second and third forms (Ocarina of Time, Majora's Mask) have a filter
// there, and more commands.

#pragma once

#include <array>
#include <cstdint>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"

namespace ntdlib::n64::ead_lists
{

constexpr uint32_t kFlagContinue = 0;
constexpr uint32_t kFlagInit = 1;
constexpr uint32_t kFlagLoop = 2;

// The microcodes' opcodes.
constexpr uint32_t kOpAdpcm = 0x01;
constexpr uint32_t kOpClear = 0x02;
constexpr uint32_t kOpMultiply = 0x03;
constexpr uint32_t kOpAddMixer = 0x04;
constexpr uint32_t kOpResample = 0x05;
constexpr uint32_t kOpResampleNearest = 0x06;
constexpr uint32_t kOpSegment = 0x07; // the first form
constexpr uint32_t kOpFilter = 0x07;  // the second and third forms
constexpr uint32_t kOpSetBuffer = 0x08;
constexpr uint32_t kOpDuplicate = 0x09;
constexpr uint32_t kOpMove = 0x0A;
constexpr uint32_t kOpLoadBook = 0x0B;
constexpr uint32_t kOpMixer = 0x0C;
constexpr uint32_t kOpInterleave = 0x0D;
constexpr uint32_t kOpGain = 0x0E;
constexpr uint32_t kOpSetLoop = 0x0F;
constexpr uint32_t kOpHalve = 0x11;
constexpr uint32_t kOpEnvSetup1 = 0x12;
constexpr uint32_t kOpEnvMixer = 0x13;
constexpr uint32_t kOpLoad = 0x14;
constexpr uint32_t kOpSave = 0x15;
constexpr uint32_t kOpEnvSetup2 = 0x16;
constexpr uint32_t kOpDecodeBytes = 0x17;
constexpr uint32_t kOpNegate = 0x19;

constexpr int Align8(int value)
{
    return (value + 7) & ~7;
}

constexpr int Align16(int value)
{
    return (value + 15) & ~15;
}

// A command's first word: the opcode, a byte and a halfword.
inline uint32_t Word0(uint32_t opcode, uint32_t middle, uint32_t low)
{
    return (opcode << 24) | ((middle & 0xFF) << 16) | (low & 0xFFFF);
}

// Two halfwords in a word, the first in the high half.
inline uint32_t Pair(uint32_t high, uint32_t low)
{
    return (high << 16) | (low & 0xFFFF);
}

inline void Clear(AudioList& list, uint32_t dmem, uint32_t size)
{
    list.commands.push_back({Word0(kOpClear, 0, dmem), size});
}

inline void SetBuffer(AudioList& list, uint32_t in, uint32_t out, uint32_t size)
{
    list.commands.push_back({Word0(kOpSetBuffer, 0, in), Pair(out, size)});
}

inline void Load(AudioList& list, uint32_t address, uint32_t dmem, uint32_t size)
{
    list.commands.push_back({Word0(kOpLoad, size >> 4, dmem), address});
}

inline void Save(AudioList& list, uint32_t dmem, uint32_t address, uint32_t size)
{
    list.commands.push_back({Word0(kOpSave, size >> 4, dmem), address});
}

inline void Move(AudioList& list, uint32_t in, uint32_t out, uint32_t size)
{
    list.commands.push_back({(kOpMove << 24) | (in & 0xFFFFFF), Pair(out, size)});
}

inline void Mix(AudioList& list, uint32_t size, uint32_t gain, uint32_t in, uint32_t out)
{
    list.commands.push_back({Word0(kOpMixer, size >> 4, gain), Pair(in, out)});
}

inline void Resample(AudioList& list, uint32_t flags, uint32_t pitch, uint32_t state)
{
    list.commands.push_back({Word0(kOpResample, flags, pitch), state});
}

inline void WriteBe16(AudioMemory& memory, uint32_t address, int16_t value)
{
    const std::array<uint8_t, 2> bytes = {static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8),
                                          static_cast<uint8_t>(static_cast<uint16_t>(value))};
    memory.Write(address, bytes.data(), bytes.size());
}

inline int16_t ReadBe16(const AudioMemory& memory, uint32_t address)
{
    std::array<uint8_t, 2> bytes = {};
    memory.Read(address, bytes.data(), bytes.size());

    return static_cast<int16_t>((uint32_t{bytes[0]} << 8) | bytes[1]);
}

} // namespace ntdlib::n64::ead_lists
