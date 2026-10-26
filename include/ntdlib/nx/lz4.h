// SPDX-License-Identifier: MIT

// LZ4's block format, as the LZ4 project documents it: a run of sequences. A sequence starts with a token. The token's
// high 4 bits give the number of literals, and its low 4 bits give the length of a copy, less 4. A field of 15
// continues in further bytes: each adds its value, up to a byte below 255. The bytes that continue the number of
// literals follow the token, and the literals follow them. Next come the copy's offset, in 2 little-endian bytes, and
// the bytes that continue the copy's length. The copy repeats the output from that many bytes back. It may overlap the
// bytes that it writes. The last sequence ends after its literals.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::nx
{

// The `size` bytes that the LZ4 block `block` holds. Throws FormatError if the block is damaged, or if it holds more or
// fewer than `size` bytes.
std::vector<uint8_t> DecompressLz4Block(std::span<const uint8_t> block, std::size_t size);

} // namespace ntdlib::nx
