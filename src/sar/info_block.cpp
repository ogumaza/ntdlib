// SPDX-License-Identifier: MIT

#include "ntdlib/sar/info_block.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr std::size_t kBody = 8;          // after the block's magic and size
constexpr std::size_t kReferenceSize = 8; // a type, two bytes of padding and an offset
constexpr std::size_t kTableCount = 7;    // the references to the tables come before the settings'
constexpr uint16_t kSettingsReference = 0x220B;
constexpr std::size_t kTablesStart = kBody + (kTableCount + 1) * kReferenceSize;
constexpr std::size_t kEntryAlignment = 4;
constexpr std::size_t kBlockAlignment = 32;

// A table: the type of the block's reference to it, the type of its references to its entries, and its items' name.
struct TableKind
{
    uint16_t table = 0;
    uint16_t entry = 0;
    const char* item = "";
};

// The tables in the order of the block's references.
constexpr std::array<TableKind, kTableCount> kTables = {{
    {0x2100, 0x2200, "sound"},
    {0x2104, 0x2204, "sound group"},
    {0x2101, 0x2206, "bank"},
    {0x2103, 0x2207, "wave archive"},
    {0x2105, 0x2208, "group"},
    {0x2102, 0x2209, "player"},
    {0x2106, 0x220A, "file"},
}};

using detail::Out;

// The entries of each table of `block`, in the order of kTables.
template <typename Block>
auto EntriesOf(Block& block)
{
    return std::array{&block.sounds, &block.sound_groups, &block.banks, &block.wave_archives,
                      &block.groups, &block.players,      &block.files};
}

// The name of the table at `k` in kTables, or of the settings at kTableCount, in messages.
std::string SectionName(std::size_t k)
{
    return k < kTableCount ? std::string("the ") + kTables[k].item + " table" : "the settings";
}

xsflib::FormatError OtherLayout(const std::string& what)
{
    return xsflib::FormatError("an INFO block laid out another way: " + what);
}

// The offset of the reference at `at` in a block that `r` reads. Throws xsflib::FormatError for a reference of
// another type than `type`, and for one with bytes in its padding. `what` names the reference's target in the message.
uint32_t OffsetOf(const xsflib::Reader& r, std::size_t at, uint16_t type, const std::string& what)
{
    if (r.U16(at) != type || r.U16(at + 2) != 0)
    {
        throw OtherLayout("the reference to " + what + " has another type, or bytes in its padding");
    }

    return r.U32(at + 4);
}

} // namespace

InfoBlock InfoBlock::Read(std::span<const uint8_t> block, Flavor flavor)
{
    const xsflib::Reader r(block, flavor == Flavor::kCafe);
    if (r.Size() < kTablesStart || !r.Magic(0, "INFO"))
    {
        throw xsflib::FormatError("not an INFO block");
    }
    if (r.U32(4) != r.Size())
    {
        throw xsflib::FormatError("an INFO block of " + std::to_string(r.Size()) + " bytes whose size field gives " +
                                  std::to_string(r.U32(4)));
    }

    // Where each table starts, and where the settings start.
    std::array<std::size_t, kTableCount + 1> starts{};
    for (std::size_t k = 0; k <= kTableCount; ++k)
    {
        const uint16_t type = k < kTableCount ? kTables[k].table : kSettingsReference;
        starts[k] = kBody + std::size_t{OffsetOf(r, kBody + k * kReferenceSize, type, SectionName(k))};
    }
    if (starts[0] != kTablesStart)
    {
        throw OtherLayout("the sound table doesn't follow the references");
    }

    InfoBlock result;
    result.flavor = flavor;
    const auto tables = EntriesOf(result);
    for (std::size_t k = 0; k < kTableCount; ++k)
    {
        const std::size_t table = starts[k];
        const std::string item = kTables[k].item;
        const uint32_t count = r.U32(table);
        if (count > (r.Size() - table - 4) / kReferenceSize)
        {
            throw xsflib::FormatError("an INFO block whose " + item + " table runs past its end");
        }

        // Where each entry starts, then where the next table starts. The entries follow the table in order. Each
        // runs up to the next. The last runs up to the next table.
        std::vector<std::size_t> bounds;
        bounds.reserve(std::size_t{count} + 1);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::string what = item + " " + std::to_string(i);
            bounds.push_back(table + OffsetOf(r, table + 4 + i * kReferenceSize, kTables[k].entry, what));
        }
        bounds.push_back(starts[k + 1]);

        std::size_t at = table + 4 + std::size_t{count} * kReferenceSize;
        for (std::size_t i = 0; i < bounds.size(); ++i)
        {
            const std::string what = i < count ? item + " " + std::to_string(i) : SectionName(k + 1);
            if (i == 0 ? bounds[i] != at : bounds[i] <= at)
            {
                const std::string before = i == 0 ? "the " + item + " table" : item + " " + std::to_string(i - 1);
                throw OtherLayout(what + " doesn't follow " + before);
            }
            if (bounds[i] % kEntryAlignment != 0)
            {
                throw OtherLayout(what + " doesn't start on a multiple of 4 bytes");
            }
            if (i > 0)
            {
                const auto bytes = r.Sub(at, bounds[i] - at);
                tables[k]->emplace_back(bytes.begin(), bytes.end());
            }
            at = bounds[i];
        }
    }

    // The settings, then zeros up to the block's end.
    const std::size_t settings = starts[kTableCount];
    const std::size_t end = settings + result.player_settings.size();
    const std::size_t padded = (end + kBlockAlignment - 1) / kBlockAlignment * kBlockAlignment;
    if (padded != r.Size())
    {
        throw OtherLayout("the settings aren't the block's last 20 bytes before the padding");
    }
    std::ranges::copy(r.Sub(settings, result.player_settings.size()), result.player_settings.begin());
    const auto padding = r.Sub(end, padded - end);
    if (std::ranges::any_of(padding, [](uint8_t byte) { return byte != 0; }))
    {
        throw OtherLayout("the block's padding holds bytes other than zeros");
    }

    return result;
}

std::vector<uint8_t> InfoBlock::Write() const
{
    Out out(flavor == Flavor::kCafe);
    out.Append(std::span(reinterpret_cast<const uint8_t*>("INFO"), 4));
    out.Zeros(kTablesStart - 4);

    const auto tables = EntriesOf(*this);
    for (std::size_t k = 0; k < kTableCount; ++k)
    {
        const std::vector<std::vector<uint8_t>>& entries = *tables[k];
        const std::size_t table = out.Zeros(4 + kReferenceSize * entries.size());
        out.Reference(kBody + k * kReferenceSize, kTables[k].table, static_cast<uint32_t>(table - kBody));
        out.U32(table, static_cast<uint32_t>(entries.size()));
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].empty() || entries[i].size() % kEntryAlignment != 0)
            {
                throw std::invalid_argument(std::string(kTables[k].item) + " " + std::to_string(i) + " of " +
                                            std::to_string(entries[i].size()) +
                                            " bytes. An entry takes a multiple of 4 bytes.");
            }

            const std::size_t at = out.Append(entries[i]);
            out.Reference(table + 4 + i * kReferenceSize, kTables[k].entry, static_cast<uint32_t>(at - table));
        }
    }

    const std::size_t settings = out.Append(player_settings);
    out.Reference(kBody + kTableCount * kReferenceSize, kSettingsReference, static_cast<uint32_t>(settings - kBody));
    out.Align(kBlockAlignment);
    out.U32(4, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

} // namespace ntdlib::sar
