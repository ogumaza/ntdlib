// SPDX-License-Identifier: MIT

// The part that the consoles' encoders of bytecode share: the layout of a sequence's items as bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"

namespace ntdlib::commands::detail
{

// Lays out `items`, whose values are the console's, as bytecode from address `base`. `append(bytes, item, target)`
// appends the bytes of `item`, whose target lies at address `target`. `reads_back(bytes, start, item, target)` gives
// whether `reader` reads the bytes from `start` to the end of `bytes` as `item`. Gives the bytes, and each item's
// offset and the end's. Throws std::invalid_argument for a target past the items, or for an item whose bytes read as
// other commands.
template <typename Encoded, typename Append, typename ReadsBack>
Encoded Encode(const std::vector<Command>& items, uint32_t base, std::string_view reader, Append append,
               ReadsBack reads_back)
{
    // Each item's size doesn't depend on its target's address. The sizes give the offsets.
    Encoded encoded;
    encoded.offsets.reserve(items.size() + 1);
    std::vector<uint8_t> scratch;
    uint32_t at = 0;
    for (const Command& item : items)
    {
        encoded.offsets.push_back(at);
        scratch.clear();
        append(scratch, item, 0);
        at += static_cast<uint32_t>(scratch.size());
    }
    encoded.offsets.push_back(at);

    // Then the bytes, with each target's address. The reader has to read each command's bytes back as the command.
    encoded.bytes.reserve(at);
    for (const Command& item : items)
    {
        uint32_t target = item.target;
        if (HasTarget(item.kind) && !item.target_outside)
        {
            if (item.target >= encoded.offsets.size())
            {
                throw std::invalid_argument("a target past the items");
            }

            target = base + encoded.offsets[item.target];
        }

        const std::size_t start = encoded.bytes.size();
        append(encoded.bytes, item, target);
        if (item.kind != Kind::kData && !reads_back(encoded.bytes, start, item, target))
        {
            throw std::invalid_argument("the " + std::string(reader) + " would read other commands from the bytes of " +
                                        std::string(Name(item.kind)));
        }
    }

    return encoded;
}

} // namespace ntdlib::commands::detail
