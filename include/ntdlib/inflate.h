// SPDX-License-Identifier: MIT

// Minimal zlib/DEFLATE decoder (RFC 1950/1951), for the readers of GSF files and N64 ROMs, without an external zlib.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib
{

// Decodes the raw DEFLATE stream at the start of `in` into `out`. Sets `used` to the number of bytes of `in` that the
// stream uses. Returns false and sets `error` if the stream is corrupt or cut short.
bool DecodeDeflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, std::size_t& used, std::string& error);

// Decompresses a zlib stream (2-byte header, DEFLATE data, Adler-32 trailer). Returns false and sets `error` on
// malformed input.
bool ZlibDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);

} // namespace ntdlib
