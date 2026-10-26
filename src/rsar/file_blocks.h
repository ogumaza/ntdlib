// SPDX-License-Identifier: MIT

// The headers and blocks of the files of the Wii that NintendoWare for Revolution's tools write: RWAV, RWAR, RBNK,
// RWSD and RSTM. A header takes 32 bytes, and 64 in RSTM. It holds the magic, the byte order mark, the version, the
// file's size, the header's size and the number of blocks, then each block's offset and size. A block starts with its
// magic and its size. Zeros pad each block to 32 bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "sar/file_bytes.h"

namespace ntdlib::rsar::detail
{

inline constexpr std::size_t kHeaderSize = 0x20;
inline constexpr std::size_t kBlockHeader = 8;
inline constexpr std::size_t kBlockAlignment = 32;

// The header of a file of `magic` and `version` with `blocks` blocks. CloseBlock lists each block, and CloseFile gives
// the file's size.
inline sar::detail::Out Header(const char* magic, uint16_t version, uint16_t blocks)
{
    sar::detail::Out out(true);
    out.Zeros(kHeaderSize);
    for (std::size_t i = 0; i < 4; ++i)
    {
        out.U8(i, static_cast<uint8_t>(magic[i]));
    }
    out.U16(4, 0xFEFF);
    out.U16(6, version);
    out.U16(0xC, static_cast<uint16_t>(kHeaderSize));
    out.U16(0xE, blocks);

    return out;
}

// Opens a block of `magic` at the end of `out`, and returns where it starts.
inline std::size_t OpenBlock(sar::detail::Out& out, const char* magic)
{
    const std::size_t at = out.Zeros(kBlockHeader);
    for (std::size_t i = 0; i < 4; ++i)
    {
        out.U8(at + i, static_cast<uint8_t>(magic[i]));
    }

    return at;
}

// Pads the block at `at` to 32 bytes, gives its size, and lists it in slot `slot` of the header.
inline void CloseBlock(sar::detail::Out& out, std::size_t at, std::size_t slot)
{
    out.Align(kBlockAlignment);
    const auto size = static_cast<uint32_t>(out.Size() - at);
    out.U32(at + 4, size);
    out.U32(0x10 + slot * 8, static_cast<uint32_t>(at));
    out.U32(0x14 + slot * 8, size);
}

// Gives the file's size in its header, and returns its bytes.
inline std::vector<uint8_t> CloseFile(sar::detail::Out& out)
{
    out.U32(8, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

// The version of the file of `magic` that `r` holds. Throws xsflib::FormatError for another file.
inline uint16_t VersionOf(const xsflib::Reader& r, const char* magic)
{
    if (r.Size() < kHeaderSize || !r.Magic(0, magic) || r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError(std::string("not an ") + magic + " file");
    }

    return r.U16(6);
}

// The offset and the size of block `slot` of the file of `magic` whose header `r` holds, after a check of the block's
// magic `block`.
inline std::pair<std::size_t, std::size_t> BlockOf(const xsflib::Reader& r, std::size_t slot, const char* block,
                                                   const char* magic)
{
    const std::size_t at = slot < r.U16(0xE) ? r.U32(0x10 + slot * 8) : 0;
    if (at == 0 || r.Size() < at || r.Size() - at < kBlockHeader || !r.Magic(at, block))
    {
        throw xsflib::FormatError(std::string("an ") + magic + " file has no " + block + " block");
    }

    return {at, r.U32(at + 4)};
}

} // namespace ntdlib::rsar::detail
