// SPDX-License-Identifier: MIT

// The decompressors that the image formats require: zlib for GCZ, bzip2, LZMA and LZMA2 for WIA and RVZ, and Zstandard
// for RVZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::disc
{

// The compression methods of WIA and RVZ, with the numbers that their headers use. PURGE is a run-length scheme that
// only WIA uses. The image reader decodes it.
enum class Method : uint32_t
{
    kNone = 0,
    kPurge = 1,
    kBzip2 = 2,
    kLzma = 3,
    kLzma2 = 4,
    kZstd = 5,
};

// Inflates zlib-wrapped deflate data, as in a GCZ block, into `out`. Returns the number of bytes written (at most
// out.size()). Throws Error if the data is damaged.
size_t Inflate(std::span<const uint8_t> in, std::span<uint8_t> out);

// The Adler-32 checksum of `data`. GCZ stores one for each block.
uint32_t Adler32(std::span<const uint8_t> data);

// Decompresses one block of a WIA or RVZ file. `method` gives the compression (bzip2, LZMA, LZMA2 or Zstandard).
// `props` holds the compressor data from the header. For LZMA, these are the 7-Zip SDK's 5-byte properties. For LZMA2,
// they are the SDK's dictionary-size byte. Decompression stops at the end of the stream, at the end of the input or at
// `max_size` bytes. Throws Error if the data is damaged.
std::vector<uint8_t> Decompress(Method method, std::span<const uint8_t> props, std::span<const uint8_t> in,
                                size_t max_size);

} // namespace ntdlib::disc
