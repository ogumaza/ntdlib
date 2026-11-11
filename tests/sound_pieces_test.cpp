// SPDX-License-Identifier: MIT

// Checks sound_pieces.h on an archive made up here: the pieces that a set of channel suffixes finds, the sequences that
// they need, and the lookup by a piece's first sound.

#include "ntdlib/sound_pieces.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

namespace
{

using xsflib::testing::Check;

// A stand-in for a sound archive: the sounds that FindSoundPieces reads.
struct FakeArchive
{
    enum class SoundType
    {
        kSequence,
        kStream,
    };

    struct Sound
    {
        SoundType type = SoundType::kSequence;
        std::string name;
    };

    const std::vector<Sound>& Sounds() const
    {
        return sounds;
    }

    std::vector<Sound> sounds;
};

constexpr std::array<std::string_view, 2> kChannels = {"_A", "_B"};

void TestPieces()
{
    // ONE has both channels, though its second comes first. TWO has a stream for its second channel. THREE has only
    // its first. A second ONE_A doesn't start a piece, and neither does _A, a channel's suffix with no name before it.
    const FakeArchive archive{{{.name = "ONE_B"},
                               {.name = "ONE_A"},
                               {.name = "TWO_A"},
                               {.type = FakeArchive::SoundType::kStream, .name = "TWO_B"},
                               {.name = "THREE_A"},
                               {.name = "ONE_A"},
                               {.name = "_A"}}};

    const std::vector<ntdlib::SoundPiece> pieces = ntdlib::FindSoundPieces(archive, kChannels);

    Check(pieces.size() == 1, "only a piece with every channel's sequence counts");
    Check(!pieces.empty() && pieces[0].name == "ONE" && pieces[0].sounds == std::vector<uint32_t>{1, 0},
          "a piece's sounds come in the order of the suffixes");
}

void TestNoSuffixes()
{
    const FakeArchive archive{{{.name = "ONE_A"}}};

    Check(ntdlib::FindSoundPieces(archive, std::span<const std::string_view>()).empty(), "no suffixes find no piece");
}

void TestPieceStartingAt()
{
    const std::vector<ntdlib::SoundPiece> pieces = {{"ONE", {3, 4}}, {"TWO", {7, 2}}};

    const ntdlib::SoundPiece* two = ntdlib::PieceStartingAt(pieces, 7);
    const ntdlib::SoundPiece* none = ntdlib::PieceStartingAt(pieces, 4);

    Check(two != nullptr && two->name == "TWO", "a piece is found by its first sound");
    Check(none == nullptr, "a piece's later sound starts no piece");
}

} // namespace

int main()
{
    TestPieces();
    TestNoSuffixes();
    TestPieceStartingAt();

    return xsflib::testing::Summary("sound_pieces");
}
