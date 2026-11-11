// SPDX-License-Identifier: MIT

// Loads a sound archive's files into the DS's main memory as the SDK's sound archive library does, for the ARM7's sound
// driver to play.
//
// The library loads a bank with the wave archives of its four slots, and links each wave archive into the bank: the
// bank's link names the wave archive, and the wave archive's list of links starts with it. A wave archive whose flag
// bit 24 is set loads its waves one at a time. The library then loads the archive's header with a table twice the
// table's length. The second half holds the waves' offsets in the file, and the first half the address of each wave
// that is loaded, or 0. With each bank, the library loads the waves that the bank's PCM instruments play from that
// slot. The driver takes an entry below main memory's start as an offset from the wave archive's start, and a larger
// entry as the wave's address.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>

#include "ntdlib/sdat/formats.h"

namespace ntdlib::sdat
{

// The error that SoundHeap throws when the files don't fit. Its message is "the files don't fit in the sound heap".
class HeapFullError : public std::runtime_error
{
public:
    HeapFullError();
};

class SoundHeap
{
public:
    // `memory` holds main memory from `base` on. The heap takes the addresses from `start` up to `end`, and puts each
    // file and each wave on a multiple of 32 bytes.
    SoundHeap(std::span<uint8_t> memory, uint32_t base, uint32_t start, uint32_t end);

    // Loads file `id` of `archive`, and returns its address. A file that the heap holds already isn't loaded again.
    // Throws HeapFullError when the file doesn't fit.
    uint32_t LoadFile(const Archive& archive, uint32_t id);

    // Loads bank `bank` of `archive` with its wave archives, and returns the bank's address. A slot that names no wave
    // archive of `archive` stays empty. The SDK's load of the bank fails there instead. Throws xsflib::FormatError when
    // the bank can't be read or a wave lies outside its file, and HeapFullError when the files don't fit.
    uint32_t LoadBank(const Archive& archive, uint32_t bank);

    // The address after the last byte that the heap holds.
    uint32_t End() const;

private:
    uint32_t Allocate(std::size_t size);
    uint32_t LoadWaveTable(const Archive& archive, uint32_t id);
    void LoadWave(const Archive& archive, uint32_t id, uint32_t table, uint32_t wave);
    void Link(uint32_t bank, uint32_t slot, uint32_t wave_archive);
    uint32_t Read32(uint32_t address) const;
    void Write32(uint32_t address, uint32_t value);

    std::span<uint8_t> memory_;
    uint32_t base_;
    uint32_t next_;
    uint32_t end_;
    std::map<uint32_t, uint32_t> files_; // the address of each file that the heap holds, by ID
};

} // namespace ntdlib::sdat
