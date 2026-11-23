// SPDX-License-Identifier: MIT

// Helpers that the tests of the rip tools' archive mode share (rip_archive_test.cpp and rip_archive_library_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rip_archive_tests
{

// A file as a rip keeps it: its path and its data.
struct Chunk
{
    std::string path;
    std::vector<uint8_t> data;
};

// The paths of `chunks`, in their order.
inline std::vector<std::string> Paths(const std::vector<Chunk>& chunks)
{
    std::vector<std::string> paths;
    for (const Chunk& chunk : chunks)
    {
        paths.push_back(chunk.path);
    }

    return paths;
}

// The bytes of `text`.
inline std::vector<uint8_t> Bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

} // namespace rip_archive_tests
