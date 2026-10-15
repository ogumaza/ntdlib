// SPDX-License-Identifier: MIT

#include "ntdlib/musyx/pool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::musyx
{
namespace
{

// A pool's header: the offsets of its macros, tables, keymaps and layers. Each list is a run of records, and a record
// whose size is 0xffffffff ends it. A record starts with its size, its ID and two bytes, then its data.
constexpr std::size_t kHeader = 16;
constexpr std::size_t kRecordHeader = 8;
constexpr uint32_t kEndOfRecords = 0xffffffff;

// Sizes of a step, a keymap's key, a layer's entry, and the two forms of envelope.
constexpr std::size_t kStepSize = 8;
constexpr std::size_t kKeymapEntrySize = 8;
constexpr std::size_t kKeys = 128;
constexpr std::size_t kLayerEntrySize = 12;
constexpr std::size_t kLinearAdsrSize = 8;
constexpr std::size_t kDlsAdsrSize = 20;

// Calls `read` with the ID and the data of each record in the list at `offset`. An offset of 0 means no list.
void ForEachRecord(const xsflib::Reader& r, uint32_t offset,
                   const std::function<void(uint16_t id, std::span<const uint8_t> data)>& read)
{
    if (offset == 0)
    {
        return;
    }

    for (std::size_t at = offset; r.U32(at) != kEndOfRecords;)
    {
        const uint32_t size = r.U32(at);
        if (size < kRecordHeader)
        {
            throw xsflib::FormatError("pool file has a record of " + std::to_string(size) + " bytes");
        }

        read(r.U16(at + 4), r.Sub(at + kRecordHeader, size - kRecordHeader));
        at += size;
    }
}

} // namespace

MacroOp MacroStep::Op() const
{
    return static_cast<MacroOp>(word0 & 0x7f);
}

uint8_t MacroStep::Byte(int index) const
{
    const uint32_t word = index < 4 ? word0 : word1;
    return static_cast<uint8_t>(word >> (8 * (index % 4)));
}

uint16_t MacroStep::Half(int index) const
{
    return static_cast<uint16_t>(Byte(index) | Byte(index + 1) << 8);
}

std::optional<LinearAdsr> ReadLinearAdsr(std::span<const uint8_t> data)
{
    if (data.size() < kLinearAdsrSize)
    {
        return std::nullopt;
    }

    const xsflib::Reader r(data, false);
    return LinearAdsr{r.U16(0), r.U16(2), r.U16(4), r.U16(6)};
}

std::optional<DlsAdsr> ReadDlsAdsr(std::span<const uint8_t> data)
{
    if (data.size() < kDlsAdsrSize)
    {
        return std::nullopt;
    }

    const xsflib::Reader r(data, false);
    return DlsAdsr{r.S32(0), r.S32(4), r.U16(8), r.U16(10), r.S32(12), r.S32(16)};
}

Pool ReadPool(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    r.Sub(0, kHeader);
    Pool pool;

    // A macro's data is its steps, each a pair of words.
    const auto read_macro = [&pool](uint16_t id, std::span<const uint8_t> data)
    {
        const xsflib::Reader steps(data, true);
        std::vector<MacroStep>& macro = pool.macros[id];
        for (std::size_t at = 0; at + kStepSize <= data.size(); at += kStepSize)
        {
            macro.push_back({steps.U32(at), steps.U32(at + 4)});
        }
    };
    ForEachRecord(r, r.U32(0), read_macro);

    // A table's data stays as it is: its form depends on the step that reads it.
    const auto read_table = [&pool](uint16_t id, std::span<const uint8_t> data)
    {
        pool.tables[id].assign(data.begin(), data.end());
    };
    ForEachRecord(r, r.U32(4), read_table);

    // A keymap gives each of the 128 keys an object, a transposition, a pan and a priority offset.
    const auto read_keymap = [&pool](uint16_t id, std::span<const uint8_t> data)
    {
        const xsflib::Reader keys(data, true);
        std::array<KeymapEntry, kKeys>& keymap = pool.keymaps[id];
        for (std::size_t k = 0; k < kKeys; k++)
        {
            const std::size_t at = k * kKeymapEntrySize;
            keymap[k] = {keys.U16(at), static_cast<int8_t>(keys.U8(at + 2)), keys.U8(at + 3), keys.S16(at + 4)};
        }
    };
    ForEachRecord(r, r.U32(8), read_keymap);

    // A layer gives its count of entries, then the entries.
    const auto read_layer = [&pool](uint16_t id, std::span<const uint8_t> data)
    {
        const xsflib::Reader entries(data, true);
        const uint32_t count = entries.U32(0);
        entries.Sub(4, std::size_t{count} * kLayerEntrySize);
        std::vector<LayerEntry>& layer = pool.layers[id];
        for (std::size_t i = 0; i < count; i++)
        {
            const std::size_t at = 4 + i * kLayerEntrySize;
            layer.push_back({entries.U16(at), entries.U8(at + 2), entries.U8(at + 3),
                             static_cast<int8_t>(entries.U8(at + 4)), entries.U8(at + 5), entries.S16(at + 6),
                             entries.U8(at + 8)});
        }
    };
    ForEachRecord(r, r.U32(12), read_layer);

    return pool;
}

} // namespace ntdlib::musyx
