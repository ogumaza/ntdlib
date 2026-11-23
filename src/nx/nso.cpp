// SPDX-License-Identifier: MIT

#include "ntdlib/nx/nso.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
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

// The module's header (MOD0): the place of its address in the text segment, its tag read little-endian, and the place
// of the dynamic section's offset in it. The dynamic section's entries, and the tag of the last.
constexpr uint64_t kMod0At = 4;
constexpr uint32_t kMod0 = 0x30444f4d;
constexpr uint64_t kDynamicOffsetAt = 4;
constexpr std::size_t kDynamicEntry = 16;
constexpr int64_t kDtNull = 0;

// The `size` bytes at `address` in the segment of `nso` that contains them all, or an empty span if no segment does.
std::span<const uint8_t> SegmentBytes(const Nso& nso, uint64_t address, std::size_t size)
{
    for (const NsoSegment* segment : {&nso.text, &nso.rodata, &nso.data})
    {
        if (address >= segment->address && address - segment->address <= segment->bytes.size() &&
            size <= segment->bytes.size() - (address - segment->address))
        {
            return std::span<const uint8_t>(segment->bytes)
                .subspan(static_cast<std::size_t>(address - segment->address), size);
        }
    }

    return {};
}

// The 32-bit word at `address` of `nso`, or nothing if no segment contains it.
std::optional<uint32_t> WordAt(const Nso& nso, uint64_t address)
{
    const std::span<const uint8_t> bytes = SegmentBytes(nso, address, 4);
    if (bytes.empty())
    {
        return std::nullopt;
    }

    return xsflib::LoadLe32(bytes.data());
}

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

std::map<int64_t, uint64_t> DynamicEntries(const Nso& nso)
{
    const std::optional<uint32_t> mod0 = WordAt(nso, kMod0At);
    const std::optional<uint32_t> tag = mod0 ? WordAt(nso, *mod0) : std::nullopt;
    const std::optional<uint32_t> offset = mod0 ? WordAt(nso, *mod0 + kDynamicOffsetAt) : std::nullopt;
    if (tag != kMod0 || !offset)
    {
        throw xsflib::FormatError("the module has no MOD0 header");
    }

    // The offset is signed. The section can come before the header.
    std::map<int64_t, uint64_t> entries;
    for (uint64_t at = *mod0 + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(*offset)));;
         at += kDynamicEntry)
    {
        const std::span<const uint8_t> entry = SegmentBytes(nso, at, kDynamicEntry);
        if (entry.empty())
        {
            throw xsflib::FormatError("the module's dynamic section runs past the end of a segment");
        }

        const auto entry_tag = static_cast<int64_t>(xsflib::LoadLe64(entry.data()));
        if (entry_tag == kDtNull)
        {
            break;
        }

        entries.emplace(entry_tag, xsflib::LoadLe64(entry.data() + 8));
    }

    return entries;
}

} // namespace ntdlib::nx
