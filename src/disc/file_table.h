// SPDX-License-Identifier: MIT

// What the loaders of discs and titles share: reading files from the host, and the file system tables of discs and of
// U8 archives. YAGCD and WiiBrew's "Wii disc" and "U8 archive" pages document the tables.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::disc::detail
{

// The largest file that the loaders read into memory.
constexpr uint64_t kMaxFileSize = 1ull << 30;

// Reads the file at `path` into `data`: a regular file of kMaxFileSize bytes at most. A folder or a pipe of that name
// isn't one. Returns false if there's no such file or it can't be read.
bool ReadHostFile(const std::filesystem::path& path, std::vector<uint8_t>& data);

// A file in a file system table.
struct FileEntry
{
    uint64_t offset; // on a GameCube disc, in a Wii game partition's data, or in a U8 archive
    uint32_t size;
    uint32_t index; // the file's entry in the table
};

// Parses a file system table into its files, by path. The table holds 12-byte entries, then their names. The first
// entry is the root directory. Each entry starts with a type byte and a 24-bit name offset. A file's entry then gives
// the file's offset and its size. A Wii disc's table gives the offset divided by 4 (`offset_shift` 2). The tables of a
// GameCube disc and a U8 archive give it in bytes (`offset_shift` 0). A directory's entry gives its parent and the
// index after its last entry. A directory named "." adds nothing to the paths of the files in it. The names are
// Shift-JIS. The paths are UTF-8 (DecodeShiftJis). Throws Error with the message `damaged` if the table is damaged.
std::map<std::string, FileEntry> ParseFileTable(std::span<const uint8_t> table, int offset_shift,
                                                const std::string& damaged);

} // namespace ntdlib::disc::detail
