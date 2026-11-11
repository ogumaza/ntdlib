// SPDX-License-Identifier: MIT

// Checks the lookups of the rhythm modes: an archive that the table doesn't hold has no pieces, and a piece is found by
// its label.

#include <array>
#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/bex/rhythm_modes.h"

namespace
{

namespace bex = ntdlib::nx::bex;

using xsflib::testing::Check;

void TestUnknownArchive()
{
    const std::vector<uint8_t> archive(1'729'416, 0);

    const bex::RhythmMode mode = bex::RhythmModeOf(archive);

    Check(mode.pieces.empty() && mode.controller.label.empty(), "an archive of a known size and another SHA-1");
}

void TestPieceOf()
{
    const std::array<bex::RhythmPiece, 2> pieces = {{{"FIRST", 1, 120}, {"SECOND", 50, 180}}};

    const bex::RhythmPiece* second = bex::RhythmPieceOf(pieces, "SECOND");
    const bex::RhythmPiece* none = bex::RhythmPieceOf(pieces, "THIRD");

    Check(second != nullptr && second->number == 50 && second->bpm == 180, "a piece is found by its label");
    Check(none == nullptr, "a label that no piece has");
}

} // namespace

int main()
{
    TestUnknownArchive();
    TestPieceOf();

    return xsflib::testing::Summary("nx_bex_rhythm_modes");
}
