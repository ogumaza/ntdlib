// SPDX-License-Identifier: MIT

// A Nintendo 64 image: the cartridge's bytes in the console's byte order, and the hash that names the game.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::sequence_export::n64
{

class Image
{
public:
    // Takes a whole file. A file in the byte order of .v64 or .n64 images comes back in the console's. Bytes that start
    // no image throw xsflib::FormatError.
    explicit Image(std::vector<uint8_t> file);

    std::span<const uint8_t> Bytes() const;

    // The SHA-1 of the bytes in the console's order, as 40 lowercase hex digits.
    const std::string& Sha1() const;

    // The header's name of the game, without its trailing spaces.
    std::string HeaderName() const;

private:
    std::vector<uint8_t> bytes_;
    std::string sha1_;
};

} // namespace ntdlib::sequence_export::n64
