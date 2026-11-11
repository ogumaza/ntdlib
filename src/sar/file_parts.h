// SPDX-License-Identifier: MIT

// The parts that NintendoWare's files of the 3DS, the Wii U and the Switch share: the header with its table of blocks,
// the blocks, the alignment of the files, the DSP-ADPCM info of a wave's or a stream's channel, and the sums of each
// console's DSP-ADPCM decoder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar::detail
{

// The boundary that the header and each block end on.
inline constexpr std::size_t kBlockAlignment = 32;

// The header's fields: its size, the file's version and size, the count of blocks and the table of blocks. Then the
// size of an entry of the table and of a block's header.
inline constexpr std::size_t kHeaderSizeField = 0x06;
inline constexpr std::size_t kVersionField = 0x08;
inline constexpr std::size_t kFileSizeField = 0x0C;
inline constexpr std::size_t kBlockCount = 0x10;
inline constexpr std::size_t kBlockTable = 0x14;
inline constexpr std::size_t kBlockEntrySize = 12; // a type, two bytes of padding, an offset and a size
inline constexpr std::size_t kBlockHeaderSize = 8; // a block's magic and size

// The size of a reference: a type, two bytes of padding and an offset.
inline constexpr std::size_t kReferenceSize = 8;

// The size of a channel's DSP-ADPCM info: the coefficients, the context at the start and at the loop, and 2 bytes of
// padding. ReadDspAdpcm and WriteDspAdpcm read and write all but the padding.
inline constexpr std::size_t kDspAdpcmInfoSize = 46;

// The flavor of a file of `kind` (such as WAV or STM), from its magic and its byte order mark, and a reader in its byte
// order. Throws xsflib::FormatError for another file. `name` names the file in the message.
inline std::pair<Flavor, xsflib::Reader> OpenFile(std::span<const uint8_t> file, const char* kind,
                                                  const std::string& name)
{
    if (file.size() < 0x14 || (file[0] != 'C' && file[0] != 'F') || std::memcmp(file.data() + 1, kind, 3) != 0)
    {
        throw xsflib::FormatError("not a " + name + " (C" + kind + " or F" + kind + ")");
    }

    const bool big_endian = file[4] == 0xFE && file[5] == 0xFF;
    if (!big_endian && (file[4] != 0xFF || file[5] != 0xFE))
    {
        throw xsflib::FormatError("a " + name + " without a byte order mark");
    }
    const Flavor flavor = file[0] == 'C' ? Flavor::kCtr : big_endian ? Flavor::kCafe : Flavor::kNx;
    if (flavor == Flavor::kCtr && big_endian)
    {
        throw xsflib::FormatError(std::string("a big-endian C") + kind);
    }

    return {flavor, xsflib::Reader(file, big_endian)};
}

// Where the table of blocks of the file that `r` reads lists the first block of type `type`: the entry's offset in the
// file. An entry holds the type, two bytes of padding, the block's offset and its size. Nothing for a file without one.
inline std::optional<std::size_t> FindBlockEntry(const xsflib::Reader& r, uint16_t type)
{
    for (uint16_t i = 0; i < r.U16(kBlockCount); ++i)
    {
        const std::size_t entry = kBlockTable + std::size_t{i} * kBlockEntrySize;
        if (r.U16(entry) == type)
        {
            return entry;
        }
    }

    return std::nullopt;
}

// The offset and the size of the first block of type `type` that the table of blocks of the file that `r` reads lists.
// Nothing for a file without one.
inline std::optional<std::pair<std::size_t, std::size_t>> FindBlock(const xsflib::Reader& r, uint16_t type)
{
    const std::optional<std::size_t> entry = FindBlockEntry(r, type);
    if (!entry)
    {
        return std::nullopt;
    }

    return std::pair<std::size_t, std::size_t>{r.U32(*entry + 4), r.U32(*entry + 8)};
}

// The header of a file with `blocks` blocks: its magic, its byte order mark, its size, its version and the table of
// blocks, padded. SetBlock fills the table, and the caller sets the file's size.
inline void AppendHeader(Out& out, const char* magic, uint32_t version, uint16_t blocks)
{
    out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(kBlockTable + std::size_t{blocks} * kBlockEntrySize - 4);
    out.Align(kBlockAlignment);
    out.U16(4, 0xFEFF);
    out.U16(kHeaderSizeField, static_cast<uint16_t>(out.Size()));
    out.U32(kVersionField, version);
    out.U16(kBlockCount, blocks);
}

// Appends a block's magic and room for its size. Returns where the block starts.
inline std::size_t StartBlock(Out& out, const char* magic)
{
    const std::size_t start = out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(4);

    return start;
}

// Ends the block that starts at `start` at the end of `out`: its size, and entry `index` of the table of blocks.
inline void SetBlock(Out& out, std::size_t index, uint16_t type, std::size_t start)
{
    const auto size = static_cast<uint32_t>(out.Size() - start);
    const std::size_t entry = kBlockTable + index * kBlockEntrySize;
    out.U32(start + 4, size);
    out.Reference(entry, type, static_cast<uint32_t>(start));
    out.U32(entry + 8, size);
}

// The DSP-ADPCM info at `at`: the coefficients, then the predictor and scale and the history at the start and at the
// loop.
inline DspAdpcmInfo ReadDspAdpcm(const xsflib::Reader& r, std::size_t at)
{
    DspAdpcmInfo adpcm;
    for (std::size_t k = 0; k < adpcm.coefs.size(); ++k)
    {
        adpcm.coefs[k] = r.S16(at + k * 2);
    }
    adpcm.pred_scale = r.U16(at + 32);
    adpcm.yn1 = r.S16(at + 34);
    adpcm.yn2 = r.S16(at + 36);
    adpcm.loop_pred_scale = r.U16(at + 38);
    adpcm.loop_yn1 = r.S16(at + 40);
    adpcm.loop_yn2 = r.S16(at + 42);

    return adpcm;
}

// Writes `adpcm` at `at`, as ReadDspAdpcm reads it.
inline void WriteDspAdpcm(Out& out, std::size_t at, const DspAdpcmInfo& adpcm)
{
    for (std::size_t k = 0; k < adpcm.coefs.size(); ++k)
    {
        out.U16(at + k * 2, static_cast<uint16_t>(adpcm.coefs[k]));
    }
    out.U16(at + 32, adpcm.pred_scale);
    out.U16(at + 34, static_cast<uint16_t>(adpcm.yn1));
    out.U16(at + 36, static_cast<uint16_t>(adpcm.yn2));
    out.U16(at + 38, adpcm.loop_pred_scale);
    out.U16(at + 40, static_cast<uint16_t>(adpcm.loop_yn1));
    out.U16(at + 42, static_cast<uint16_t>(adpcm.loop_yn2));
}

// The boundary that the tools start an archive's files on, and the copies in a group's file, the waves in a wave
// archive and a wave's samples: 64 bytes on the Switch, and 32 on the 3DS and the Wii U.
inline std::size_t FileAlignment(Flavor flavor)
{
    return flavor == Flavor::kNx ? 64 : 32;
}

// How the DSP-ADPCM decoder of `flavor` adds up a sample: in 64 bits on the 3DS, and in 32 bits that wrap on the Wii U
// and the Switch.
inline wave::DspAdpcmSum SumOf(Flavor flavor)
{
    return flavor == Flavor::kCtr ? wave::DspAdpcmSum::kWide : wave::DspAdpcmSum::kWrap;
}

} // namespace ntdlib::sar::detail
