// SPDX-License-Identifier: MIT

#include "ntdlib/nx/nso.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/sha256.h"

#include "ntdlib/nx/lz4.h"

namespace ntdlib::nx
{
namespace
{

constexpr std::size_t kHeaderSize = 0x100;
constexpr std::size_t kFlags = 0xc;
constexpr std::size_t kSegmentEntries = 0x10;
constexpr std::size_t kSegmentEntrySize = 0x10;
constexpr std::size_t kFileSizes = 0x60;
constexpr std::size_t kHashes = 0xa0;
constexpr std::size_t kHashSize = 32;

// A segment's flag of a hash is 3 bits above its flag of compression.
constexpr uint32_t kHashFlags = 3;

constexpr std::array<const char*, 3> kSegmentNames = {"text", "rodata", "data"};

// Every read past the end of the file gives one message.
constexpr xsflib::ReaderOptions kNsoReaderOptions{.bounds_message = xsflib::BoundsMessage::kFixed,
                                                  .fixed_message = "the NSO is damaged"};

} // namespace

Nso ReadNso(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, false, kNsoReaderOptions);
    if (file.size() < 4 || !r.Magic(0, "NSO0"))
    {
        throw xsflib::FormatError("not an NSO");
    }

    r.Sub(0, kHeaderSize);
    const uint32_t flags = r.U32(kFlags);
    Nso nso;
    const std::array<NsoSegment*, 3> segments = {&nso.text, &nso.rodata, &nso.data};
    for (std::size_t i = 0; i < segments.size(); i++)
    {
        // The segment's entry, and its bytes in the file.
        const std::size_t entry = kSegmentEntries + i * kSegmentEntrySize;
        const uint32_t size = r.U32(entry + 8);
        const bool compressed = (flags >> i & 1) != 0;
        const std::span<const uint8_t> stored = r.Sub(r.U32(entry), compressed ? r.U32(kFileSizes + 4 * i) : size);
        const std::string what = std::string("the NSO's ") + kSegmentNames[i] + " segment";

        NsoSegment& segment = *segments[i];
        segment.address = r.U32(entry + 4);
        try
        {
            segment.bytes =
                compressed ? DecompressLz4Block(stored, size) : std::vector<uint8_t>(stored.begin(), stored.end());
        }
        catch (const xsflib::FormatError& e)
        {
            throw xsflib::FormatError(what + ": " + e.what());
        }

        const std::span<const uint8_t> hash = r.Sub(kHashes + i * kHashSize, kHashSize);
        if ((flags >> (i + kHashFlags) & 1) != 0 && !std::ranges::equal(xsflib::Sha256::Of(segment.bytes), hash))
        {
            throw xsflib::FormatError(what + " doesn't match its hash");
        }
    }

    return nso;
}

} // namespace ntdlib::nx
