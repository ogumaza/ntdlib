// SPDX-License-Identifier: MIT

// The instrument records of Nintendo R&D1's driver.

#include "ntdlib/rd1/instruments.h"

#include <algorithm>
#include <cstdint>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"

namespace ntdlib::rd1
{

using ntdlib::agb::Rom;

int KeyPitch(const Rom& rom, const DriverInfo& info, int key)
{
    return rom.U16(info.pitch_table + 2 * uint32_t(std::clamp(key, 0, 127)));
}

int PsgSetting(int hz)
{
    if (hz <= 0)
    {
        return 0;
    }

    return std::clamp(2048 - 131072 / hz, 0, 2047);
}

bool Sweeps(const InstrumentRecord& r)
{
    return r.kind == InstrumentKind::kPsg && r.psg_channel == 0 && (r.psg_sweep & 0x70) != 0;
}

InstrumentRecord ReadInstrument(const Rom& rom, uint32_t address)
{
    InstrumentRecord r;
    r.address = address;
    if (!rom.Contains(address, 8))
    {
        return r;
    }

    const uint8_t type = rom.U8(address);
    r.key = rom.U8(address + 1) & 0x7F;
    switch (type)
    {
    case 0x52:
        r.kind = InstrumentKind::kDrums;
        r.table = rom.U32(address + 4);
        return r;

    case 0x53:
        r.kind = rom.Contains(address, 12) ? InstrumentKind::kKeySplit : InstrumentKind::kNone;
        r.table = rom.U32(address + 4);
        r.parts = rom.U32(address + 8);
        return r;

    case 0x50:
    case 0x51:
        r.kind = InstrumentKind::kPsg;
        break;

    case 0x46:
        r.kind = InstrumentKind::kFixed;
        break;

    default:
        r.kind = InstrumentKind::kSample;
        break;
    }

    if (!rom.Contains(address, r.kind == InstrumentKind::kPsg ? 36 : 32))
    {
        r.kind = InstrumentKind::kNone;
        return r;
    }

    r.pan = int16_t(rom.U16(address + 2));
    r.sample = rom.U32(address + 4);
    r.start = rom.U32(address + 8);
    r.sustain = rom.U32(address + 12);
    r.attack = rom.U32(address + 16);
    r.decay = rom.U32(address + 20);
    r.fall = rom.U32(address + 24);
    r.release = rom.U32(address + 28);

    if (r.kind == InstrumentKind::kPsg)
    {
        const uint32_t word = rom.U32(address + 32);
        r.psg_channel = int(word & 3);
        r.psg_length = int((word >> 2) & 0xFF);
        r.psg_sweep = int((word >> 10) & 0x7F);
        r.psg_setting = rom.U16(address + 34);
    }

    return r;
}

uint32_t ProgramInstrument(const Rom& rom, const DriverInfo& info, int bank, int program)
{
    if (bank < 0 || bank >= info.bank_count || program < 0 || program > 127)
    {
        return 0;
    }

    const uint32_t table = rom.U32(info.bank_table + 4 * uint32_t(bank));
    return table ? rom.U32(table + 4 * uint32_t(program)) : 0;
}

InstrumentRecord PlayedInstrument(const Rom& rom, const InstrumentRecord& program, int key)
{
    const int index = key - program.key;
    uint32_t part = 0;
    if (program.kind == InstrumentKind::kDrums)
    {
        part = index >= 0 ? rom.U32(program.table + 4 * uint32_t(index)) : 0;
    }
    else if (program.kind == InstrumentKind::kKeySplit)
    {
        part = index >= 0 ? rom.U32(program.parts + 4 * uint32_t(rom.U8(program.table + uint32_t(index)))) : 0;
    }
    else
    {
        return program;
    }

    const InstrumentRecord r = ReadInstrument(rom, part);
    if (r.kind == InstrumentKind::kDrums || r.kind == InstrumentKind::kKeySplit)
    {
        return InstrumentRecord();
    }

    return r;
}

} // namespace ntdlib::rd1
