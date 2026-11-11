// SPDX-License-Identifier: MIT

// The items that the GBA drivers' decoders keep as data: bytes that no command reaches, and the sequences that a
// decoder can't read safely.

#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

namespace ntdlib::agb
{

// A kData item of `bytes`.
inline commands::Command DataItem(std::span<const uint8_t> bytes)
{
    commands::Command item;
    item.kind = commands::Kind::kData;
    item.bytes.assign(bytes.begin(), bytes.end());

    return item;
}

// Appends `data` to `items` as kData items, split at offset 0 and at each of `splits`. Each split has to be inside
// `data`. Returns the item at each split.
inline std::map<uint32_t, uint32_t> AppendDataItems(std::span<const uint8_t> data, std::set<uint32_t> splits,
                                                    std::vector<commands::Command>& items)
{
    splits.insert(0);
    std::map<uint32_t, uint32_t> at;
    for (auto it = splits.begin(); it != splits.end(); ++it)
    {
        const auto next = std::next(it);
        const uint32_t end = next == splits.end() ? uint32_t(data.size()) : *next;
        at[*it] = uint32_t(items.size());
        items.push_back(DataItem(data.subspan(*it, end - *it)));
    }

    return at;
}

} // namespace ntdlib::agb
