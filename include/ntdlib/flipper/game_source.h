// SPDX-License-Identifier: MIT

// What game mode does with a set around a GameCube or Wii machine: the disc made of the set's files, main memory loaded
// from its MEM chunks, the samples of the audio interface's DMA, the status word of the set's driver, and the messages
// for a CPU that stops a render.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/file_disc.h"
#include "ntdlib/ppc/cpu.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/program.h"

namespace ntdlib::flipper
{

// A disc made of a set's FILE chunks. The chunk at `header_path` gives the disc's header. Without one, the header is
// blank apart from the console's disc magic: `magic` at `magic_offset` (see disc/header.h). A chunk replaces an earlier
// one with the same path. The header and the table give offsets and sizes in units of `offset_unit` bytes: 1 on the
// GameCube and 4 on the Wii. The chunks must outlive the disc.
std::unique_ptr<FileDisc> DiscFromFiles(const std::vector<xsflib::psf::FileChunk>& files,
                                        const std::string& header_path, uint32_t magic_offset, uint32_t magic,
                                        uint32_t offset_unit);

// Copies a set's MEM chunks into `memory`, at their CPU addresses. Returns false at the first chunk that lies outside
// it.
template <typename Memory>
bool LoadMemChunks(Memory& memory, const std::vector<xsflib::psf::MemChunk>& chunks)
{
    for (const xsflib::psf::MemChunk& chunk : chunks)
    {
        uint8_t* to = memory.At(dsp::ToPhysical(chunk.address), chunk.data.size());
        if (to == nullptr)
        {
            return false;
        }

        std::copy(chunk.data.begin(), chunk.data.end(), to);
    }

    return true;
}

// Appends a 32-byte block of the audio interface's DMA to `out` as eight stereo samples, left then right. Each of the
// block's samples holds the right channel's 16 bits, then the left's.
void AppendAudioBlock(std::vector<int16_t>& out, std::span<const uint8_t, 32> block);

// The status word of the set's driver at the CPU address `address`: 0 while it boots, 1 while the sound plays and 2
// once it has finished. After an error, bit 31 is set and the low byte holds the step that failed. A set without a
// status word (`address` 0), or with one outside memory, plays: 1.
template <typename Memory>
uint32_t DriverStatus(Memory& memory, uint32_t address)
{
    if (!address)
    {
        return 1;
    }

    const uint8_t* word = memory.At(dsp::ToPhysical(address), 4);
    return word != nullptr ? xsflib::LoadBe32(word) : 1;
}

// The message for a CPU stop that ends a render (a fault or an instruction that the CPU doesn't support), or nothing.
std::optional<std::string> StopMessage(ppc::StopReason reason);

} // namespace ntdlib::flipper
