// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/barc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "jaudio/barc_parse.h"
#include "ntdlib/jaudio/rarc.h"

namespace ntdlib::jaudio
{
namespace
{

// An entry: the sequence's name in up to 14 bytes, the sequence whose data it shares, two words, then the sequence's
// offset and size.
constexpr std::size_t kEntrySize = 0x20;
constexpr std::size_t kNameSize = 14;
constexpr std::size_t kShares = 0xe;
constexpr std::size_t kOffset = 0x18;
constexpr std::size_t kSize = 0x1c;
constexpr uint32_t kMaxEntries = 0x10000;

// An entry that shares no other sequence's data.
constexpr uint16_t kSharesNone = 0xffff;

// Every read past the end of the index gives one message.
constexpr xsflib::ReaderOptions kBarcReaderOptions{.bounds_message = xsflib::BoundsMessage::kFixed,
                                                   .fixed_message = "the sequence index is damaged"};

// The layout of an index in the byte order that `big_endian` gives.
detail::BarcLayout LayoutOf(bool big_endian)
{
    return big_endian ? detail::kBarc : detail::kWideBarc;
}

} // namespace

std::vector<ArchiveFile> detail::ParseBarc(const xsflib::Reader& r, std::size_t at, BarcLayout layout,
                                           std::span<const uint8_t> archive, const std::string& what)
{
    r.Sub(at, layout.entries_at);
    const uint32_t count = r.U32(at + layout.count_at);
    if (!r.Magic(at, "BARC") || count > kMaxEntries)
    {
        throw xsflib::FormatError(what + " is damaged");
    }

    std::vector<ArchiveFile> files;
    for (uint32_t number = 0; number < count; number++)
    {
        const std::size_t entry = at + layout.entries_at + std::size_t{number} * kEntrySize;
        r.Sub(entry, kEntrySize);
        const uint32_t offset = r.U32(entry + kOffset);
        const uint32_t size = r.U32(entry + kSize);
        if (offset > archive.size() || size > archive.size() - offset)
        {
            throw xsflib::FormatError(what + " names a sequence outside the sequence archive");
        }

        files.push_back(
            {static_cast<uint16_t>(number), r.CString(entry, kNameSize), archive.subspan(offset, size), std::nullopt});
    }

    // Each sequence's chain of shares, to the sequence that shares no other sequence's data. A chain ends at a sequence
    // whose end is already known, and the sequences on the chain take that end.
    constexpr uint32_t kUnknown = 0xffffffff;
    std::vector<uint32_t> sources(count, kUnknown);
    std::vector<bool> on_chain(count);
    std::vector<uint32_t> chain;
    for (uint32_t number = 0; number < count; number++)
    {
        chain.clear();
        uint32_t current = number;
        while (sources[current] == kUnknown)
        {
            if (on_chain[current])
            {
                throw xsflib::FormatError(what + " shares data in a loop");
            }

            on_chain[current] = true;
            chain.push_back(current);

            const uint16_t shares = r.U16(at + layout.entries_at + std::size_t{current} * kEntrySize + kShares);
            if (shares == kSharesNone)
            {
                sources[current] = current;
                break;
            }
            if (shares >= count)
            {
                throw xsflib::FormatError(what + " shares the data of a sequence past its end");
            }

            current = shares;
        }

        for (const uint32_t link : chain)
        {
            sources[link] = sources[current];
            on_chain[link] = false;
        }

        files[number].data_id = static_cast<uint16_t>(sources[number]);
    }

    return files;
}

std::vector<ArchiveFile> ReadBarcSequenceFiles(std::span<const uint8_t> index, std::span<const uint8_t> archive,
                                               bool big_endian)
{
    const xsflib::Reader r(index, big_endian, kBarcReaderOptions);

    return detail::ParseBarc(r, 0, LayoutOf(big_endian), archive, "the sequence index");
}

std::optional<std::size_t> FindBarc(std::span<const uint8_t> data, bool big_endian)
{
    const detail::BarcLayout layout = LayoutOf(big_endian);
    for (std::size_t at = 0; at + layout.entries_at <= data.size(); at += 4)
    {
        if (std::memcmp(data.data() + at, "BARC", 4) != 0)
        {
            continue;
        }

        const xsflib::Reader r(data, big_endian);
        const uint32_t count = r.U32(at + layout.count_at);
        if (count <= kMaxEntries && std::size_t{count} * kEntrySize <= data.size() - at - layout.entries_at)
        {
            return at;
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::jaudio
