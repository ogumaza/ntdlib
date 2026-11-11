// SPDX-License-Identifier: MIT

// What the files of the MP2K sequencer share: the PSG channel registers and the decoder of compressed samples.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::mp2k
{

// The most points that a frame of the mixer holds: those of its highest rate.
constexpr int kMostFramePoints = 704;

// The registers of the PSG's channels, NRx0 to NRx4. Channels 2 and 4 have no NRx0. The driver takes the byte after the
// previous channel's NRx0 there, and doesn't write it.
struct PsgRegisters
{
    uint32_t nrx0, nrx1, nrx2, nrx3, nrx4;
};

constexpr PsgRegisters kPsgRegisters[4] = {
    {0x04000060, 0x04000062, 0x04000063, 0x04000064, 0x04000065},
    {0x04000061, 0x04000068, 0x04000069, 0x0400006C, 0x0400006D},
    {0x04000070, 0x04000072, 0x04000073, 0x04000074, 0x04000075},
    {0x04000071, 0x04000078, 0x04000079, 0x0400007C, 0x0400007D},
};

// The steps of a compressed sample's points, by the 4 bits that code each one.
constexpr int8_t kDeltas[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};

// A compressed sample's points come in blocks of 64 points in 33 bytes. A block starts with its first point. Then the
// low 4 bits of the next byte give the second point's step from the first. The two halves of each later byte give the
// next two steps, the high half first. The points wrap round at 8 bits.
constexpr uint32_t kBlockPoints = 64;

constexpr uint32_t kBlockSize = 33;

// The decoded block of a compressed sample that the mixer last read.
struct DecodedBlock
{
    uint32_t index = ~0u;
    std::array<int8_t, kBlockPoints> points = {};
};

// Returns point `index` of the compressed sample at `wave`. Decodes the point's block into `block` first, unless
// `block` holds it already.
inline int32_t CompressedPoint(const Rom& rom, uint32_t wave, uint32_t index, DecodedBlock& block)
{
    if (block.index != index / kBlockPoints)
    {
        block.index = index / kBlockPoints;
        const uint32_t data = wave + 16 + kBlockSize * block.index;
        uint8_t point = rom.U8(data);
        block.points[0] = int8_t(point);
        for (uint32_t i = 1; i < kBlockPoints; i++)
        {
            const uint8_t byte = rom.U8(data + i / 2 + 1);
            point = uint8_t(point + kDeltas[i % 2 ? byte & 0xF : byte >> 4]);
            block.points[i] = int8_t(point);
        }
    }

    return block.points[index % kBlockPoints];
}

} // namespace ntdlib::mp2k
