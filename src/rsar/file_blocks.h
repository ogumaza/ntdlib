// SPDX-License-Identifier: MIT

// The headers and blocks of the files of the Wii that NintendoWare for Revolution's tools write: RWAV, RWAR, RBNK,
// RWSD, RSEQ and RSTM. A header takes 32 bytes, and 64 in RSTM. It holds the magic, the byte order mark, the version,
// the file's size, the header's size and the number of blocks, then each block's offset and size. A block starts with
// its magic and its size. Zeros pad each block to 32 bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar::detail
{

inline constexpr std::size_t kHeaderSize = 0x20;
inline constexpr std::size_t kBlockHeader = 8;
inline constexpr std::size_t kBlockAlignment = 32;

// The highest rate that the 24 bits of a wave's or a stream's rate hold, and the most channels and tracks that the
// 8-bit counts of a stream hold.
inline constexpr uint32_t kMostRate = 0xFFFFFF;
inline constexpr std::size_t kMostStreamCount = 255;

// The 24-bit rate of a wave or a stream at `at`: its high byte, then its low 16 bits.
inline uint32_t ReadRate(const xsflib::Reader& r, std::size_t at)
{
    return uint32_t{r.U8(at)} << 16 | r.U16(at + 1);
}

// Writes `rate` at `at`, in the layout that ReadRate reads.
inline void WriteRate(sar::detail::Out& out, std::size_t at, uint32_t rate)
{
    out.U8(at, static_cast<uint8_t>(rate >> 16));
    out.U16(at + 1, static_cast<uint16_t>(rate));
}

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

// The DSP-ADPCM info at `at`: the coefficients, the gain, then the predictor and scale and the history at the start
// and at the loop.
inline DspAdpcmInfo ReadDspAdpcm(const xsflib::Reader& r, std::size_t at)
{
    DspAdpcmInfo info;
    for (std::size_t i = 0; i < info.coefs.size(); ++i)
    {
        info.coefs[i] = r.S16(at + i * 2);
    }
    info.gain = r.U16(at + 0x20);
    info.pred_scale = r.U16(at + 0x22);
    info.yn1 = r.S16(at + 0x24);
    info.yn2 = r.S16(at + 0x26);
    info.loop_pred_scale = r.U16(at + 0x28);
    info.loop_yn1 = r.S16(at + 0x2A);
    info.loop_yn2 = r.S16(at + 0x2C);

    return info;
}

// Writes `info` at `at`, in the layout that ReadDspAdpcm reads.
inline void WriteDspAdpcm(sar::detail::Out& out, std::size_t at, const DspAdpcmInfo& info)
{
    for (std::size_t i = 0; i < info.coefs.size(); ++i)
    {
        out.U16(at + i * 2, static_cast<uint16_t>(info.coefs[i]));
    }
    out.U16(at + 0x20, info.gain);
    out.U16(at + 0x22, info.pred_scale);
    out.U16(at + 0x24, static_cast<uint16_t>(info.yn1));
    out.U16(at + 0x26, static_cast<uint16_t>(info.yn2));
    out.U16(at + 0x28, info.loop_pred_scale);
    out.U16(at + 0x2A, static_cast<uint16_t>(info.loop_yn1));
    out.U16(at + 0x2C, static_cast<uint16_t>(info.loop_yn2));
}

// The labels of the LABL block at `block` of the RSEQ file that `r` holds. The block holds the number of labels and an
// offset to each from the count. Each label holds its offset in the bytecode, its name's length and its name.
inline std::vector<Sequence::Label> ReadLabels(const xsflib::Reader& r, std::size_t block)
{
    std::vector<Sequence::Label> labels;
    const std::size_t table = block + kBlockHeader;
    const uint32_t count = r.U32(table);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t label = table + r.U32(table + 4 + std::size_t{i} * 4);
        const std::span<const uint8_t> name = r.Sub(label + 8, r.U32(label + 4));
        labels.push_back({std::string(name.begin(), name.end()), r.U32(label)});
    }

    return labels;
}

} // namespace ntdlib::rsar::detail
