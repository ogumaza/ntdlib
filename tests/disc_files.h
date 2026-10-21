// SPDX-License-Identifier: MIT

// The helpers that the disc tests share: a pseudorandom fill, big-endian stores into a buffer, the writing of a file,
// text as bytes, an empty AppleDouble file, and checks of what LoadGame loads and rejects. The header includes no
// compression library. The tests that link only ntdlib::disc use it too.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "ntdlib/disc/game.h"

namespace disc_files
{

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// Fills data with a pseudorandom pattern (xorshift) so that it doesn't compress to nothing.
inline void Fill(Bytes& data, size_t offset, size_t size, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < size; ++i)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[offset + i] = static_cast<uint8_t>(x);
    }
}

// Stores `value` big-endian in the 4 bytes at `at` in `data`.
inline void StoreBe32(Bytes& data, size_t at, uint32_t value)
{
    xsflib::StoreBe32(data.data() + at, value);
}

// Stores `value` big-endian in the 8 bytes at `at` in `data`.
inline void StoreBe64(Bytes& data, size_t at, uint64_t value)
{
    xsflib::StoreBe64(data.data() + at, value);
}

// Writes `data` to the file at `path`, and makes the file's folder first. Throws std::runtime_error when the file
// can't be written.
inline void WriteFile(const fs::path& path, const Bytes& data)
{
    fs::create_directories(path.parent_path());
    if (const std::optional<std::string> error = xsflib::WriteFile(path, data))
    {
        throw std::runtime_error(*error);
    }
}

inline Bytes Text(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

// An AppleDouble file with no entries: the magic 0x00051607, version 2, 16 bytes of filler and an entry count of 0
// (RFC 1740).
inline Bytes AppleDouble()
{
    Bytes file(26, 0);
    StoreBe32(file, 0, 0x00051607);
    StoreBe32(file, 4, 0x00020000);

    return file;
}

// Checks that `game` lists the files in `files`, by path, with their sizes, and that each one reads as `files` gives
// it.
inline void CheckFiles(const std::string& name, const ntdlib::disc::GameFiles& game,
                       const std::map<std::string, Bytes>& files)
{
    xsflib::testing::Check(game.files.size() == files.size(), name + ": the number of files");
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        const auto expected = files.find(file.path);
        Bytes data;
        xsflib::testing::Check(expected != files.end() && file.size == expected->second.size(),
                               name + ": listing " + file.path);
        xsflib::testing::Check(game.read_file(file.path, data) && expected != files.end() && data == expected->second,
                               name + ": reading " + file.path);
    }
}

// Checks that LoadGame, with `options`, rejects `path` with a message that contains `words`.
inline void CheckRefused(const std::string& name, const fs::path& path, const std::string& words,
                         const ntdlib::disc::GameOptions& options)
{
    ntdlib::disc::GameFiles game;
    const auto error = ntdlib::disc::LoadGame(path, game, options);
    xsflib::testing::Check(error && error->find(words) != std::string::npos,
                           name + ": rejected with \"" + words + "\", got \"" + error.value_or("success") + "\"");
}

} // namespace disc_files
