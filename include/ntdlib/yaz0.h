// SPDX-License-Identifier: MIT

// Yaz0, a compression format of Nintendo's games from the Nintendo 64 to the Switch. Ocarina of Time compresses the
// files of its file table with it, and JSystem's games compress archives with it (Twilight Princess, for example).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib
{

// Whether `data` is compressed with Yaz0: whether it starts with "Yaz0".
bool IsYaz0(std::span<const uint8_t> data);

// Decodes data compressed with Yaz0. Throws FormatError if it's damaged, or if it decodes to more than 64 MiB.
std::vector<uint8_t> DecodeYaz0(std::span<const uint8_t> data);

// `data` as a game reads it: `data` itself, or `data` decoded into `decoded` if it's compressed with Yaz0. The result
// points into one of the two. Throws FormatError as DecodeYaz0 does.
std::span<const uint8_t> DecodeIfYaz0(std::span<const uint8_t> data, std::vector<uint8_t>& decoded);

} // namespace ntdlib
