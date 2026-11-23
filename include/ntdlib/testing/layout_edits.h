// SPDX-License-Identifier: MIT

// The edits that the tests of ntdlib/sar/layout.h and ntdlib/rsar/layout.h make to a layout, the way an editor makes
// them.

#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::testing::layout_edits
{

// Inserts `count` copies of item `copied` of sequence `sequence` before item `at`, in a sar::Layout or an
// rsar::Layout. Moves the labels and the places at or after it the way an editor moves them.
template <typename Layout>
void Insert(Layout& layout, uint32_t sequence, uint32_t at, uint32_t count, uint32_t copied)
{
    std::vector<commands::Command>& items = layout.sequences[sequence].items;
    const commands::Command command = items[copied];
    items.insert(items.begin() + at, count, command);
    for (commands::Label& label : layout.sequences[sequence].labels)
    {
        label.item += label.item >= at ? count : 0;
    }

    for (sar::Field& field : layout.fields)
    {
        // A place at the archive's end has no piece. Only an item's place names a sequence piece, or a sequence.
        for (sar::Place* place : {&field.from, &field.to})
        {
            if (!place->item)
            {
                continue;
            }

            const auto* piece =
                place->sequence ? nullptr : std::get_if<sar::SequencePiece>(&layout.pieces[place->piece]);
            if ((piece ? piece->sequence : place->piece) == sequence && place->offset >= at)
            {
                place->offset += count;
            }
        }
    }
}

} // namespace ntdlib::testing::layout_edits
