// SPDX-License-Identifier: MIT

// Finds the sound data of Nintendo SPD's sound driver in a DS cartridge image.
//
// The driver reads one file of sound data (song.h). Its first block holds set A: the programs, the samples and the
// sounds that every scene of the game loads. Each other block is a set B: the game loads one at a time, beside set A.
// A song here is one sound of one block. Block 0's sounds play with set A alone, and each other block's sounds with
// set A and that block.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom.h"
#include "ntdlib/spd/song.h"

namespace ntdlib::spd
{

// The two sets that the driver keeps. `b` may be null when no sound plays a program of set B.
struct Sets
{
    const Block* a = nullptr;
    const Block* b = nullptr;
};

// A song: a sound of a block.
struct SongEntry
{
    int block = 0; // an index into DriverInfo::blocks
    int sound = 0; // the sound's index in the block's table
};

struct DriverInfo
{
    uint32_t file_id = 0;
    std::string path; // the file's path in the image's file system
    std::vector<BlockPlace> blocks;
    std::vector<SongEntry> songs; // every sound of every block, block by block, empty entries left out
};

// The driver's sound data in `rom`: the first file of the file system that starts with the magic word and whose
// header names at least one block. None when no file does.
std::optional<DriverInfo> FindDriver(const ntdlib::ntr::Rom& rom);

// The bytes of the sound data file.
std::span<const uint8_t> SoundData(const ntdlib::ntr::Rom& rom, const DriverInfo& info);

// A song's name: the block's slot and the sound's index, such as "03_012".
std::string SongName(const DriverInfo& info, int song);

// The blocks of the sound data, read. They keep views of `data`.
std::vector<Block> ReadBlockData(std::span<const uint8_t> data, const DriverInfo& info);

// The sets that play `song`, from the blocks that ReadBlockData() gives.
Sets SetsOf(const std::vector<Block>& blocks, const DriverInfo& info, int song);

} // namespace ntdlib::spd
