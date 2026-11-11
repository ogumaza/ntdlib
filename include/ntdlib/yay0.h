// SPDX-License-Identifier: MIT

// Yay0, a compression format of Nintendo 64 and GameCube games. A Yay0 block keeps three streams: a stream of bits that
// say which comes next, a stream of literal bytes and a stream of references back into the output. A 16-byte header
// gives "Yay0", the decoded size, and the offsets of the references and the literals from the block's start, all
// big-endian. The bits start after the header, from the top bit of each byte. A set bit takes the next literal.
//
// A clear bit takes the next reference: two bytes, with the distance back less 1 in the low 12 bits and the length
// less 2 in the top 4. A length of 0 there means that the next literal gives the length less 18.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib
{

// The decoded bytes of the Yay0 block at the start of `data`. Throws xsflib::FormatError for data that isn't Yay0, for
// a block that is damaged, or for one that decodes to more than 64 MiB.
std::vector<uint8_t> DecodeYay0(std::span<const uint8_t> data);

} // namespace ntdlib
