// SPDX-License-Identifier: MIT

// Tests of a sound's arrangements (arrangements.h) on a listing made up here: the indices and names that
// WithArrangements gives, the labels that let --only pick an arrangement with its sequence, the sound and the
// arrangement that ArrangedSoundAt finds again, and the changes that SetUpArrangement makes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/sequence_export/arrangements.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"

using namespace ntdlib::sequence_export;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// An archive of 4 sounds. Sounds 0 and 2 are sequences. Sound 3 is a piece that plays sounds 3 and 1 together. Sound 0
// has two arrangements. One plays track 0, and the other track 1. The piece has one arrangement, with track 2. Sound 2
// has none.
constexpr std::size_t kSounds = 4;

std::vector<Sequence> TestSequences()
{
    return {{0, "A"}, {2, "C"}, {3, "PIECE", {"D", "B"}}};
}

std::vector<Arrangement> TestArrangements(std::size_t sound)
{
    // An arrangement's changes: the tracks in `mask` play.
    const auto plays = [](uint16_t mask)
    {
        return [mask](PerformOptions& options)
        {
            options.tracks = mask;
        };
    };
    switch (sound)
    {
    case 0:
        return {{"_one", plays(0x0001)}, {"_two", plays(0x0002)}};
    case 3:
        return {{"_three", plays(0x0004)}};
    default:
        return {};
    }
}

void TestListing()
{
    // Each sequence comes first, then its arrangements. Arrangement k of sound s takes the index (k + 1) * 4 + s.
    const std::vector<Sequence> listed = WithArrangements(TestSequences(), kSounds, TestArrangements);

    std::vector<std::size_t> indices;
    std::vector<std::string> names;
    bool labelled = false;
    for (const Sequence& sequence : listed)
    {
        indices.push_back(sequence.index);
        names.push_back(sequence.name);
        labelled = labelled || (sequence.index >= kSounds && !sequence.labels.empty());
    }

    Check(indices == std::vector<std::size_t>{0, 4, 8, 2, 3, 7}, "each sequence, then its arrangements");
    Check(names == std::vector<std::string>{"A", "A_one", "A_two", "C", "PIECE", "PIECE_three"},
          "an arrangement takes its sequence's name and its suffix");
    Check(!labelled, "an arrangement takes no labels by default");
}

void TestPickedWithSequence()
{
    // With picked_with_sequence, an arrangement takes the sequence's name and labels as its labels.
    const std::vector<Sequence> listed = WithArrangements(TestSequences(), kSounds, TestArrangements, true);

    Check(listed.size() == 6 && listed[1].labels == std::vector<std::string>{"A"} &&
              listed[2].labels == std::vector<std::string>{"A"},
          "a sound's arrangements take the sound's name");
    Check(listed[5].labels == std::vector<std::string>{"PIECE", "D", "B"},
          "a piece's arrangement takes the piece's name and labels");
    Check(listed[0].labels.empty() && listed[4].labels == std::vector<std::string>{"D", "B"},
          "the sequences keep their labels");
}

void TestArrangedSoundAt()
{
    // An index below the sound count is the sound itself.
    const ArrangedSound sound = ArrangedSoundAt(2, kSounds);
    const ArrangedSound first = ArrangedSoundAt(4, kSounds);
    const ArrangedSound second = ArrangedSoundAt(8, kSounds);
    const ArrangedSound piece = ArrangedSoundAt(7, kSounds);
    const ArrangedSound empty = ArrangedSoundAt(5, 0);

    Check(sound.sound == 2 && !sound.arrangement, "index 2 is sound 2");
    Check(first.sound == 0 && first.arrangement == 0u, "index 4 is sound 0's first arrangement");
    Check(second.sound == 0 && second.arrangement == 1u, "index 8 is sound 0's second arrangement");
    Check(piece.sound == 3 && piece.arrangement == 0u, "index 7 is the piece's arrangement");
    Check(empty.sound == 5 && !empty.arrangement, "an archive without sounds has no arrangements");
}

void TestSetUpArrangement()
{
    PerformOptions arranged;
    SetUpArrangement(ArrangedSoundAt(8, kSounds), TestArrangements, arranged);
    PerformOptions itself;
    SetUpArrangement(ArrangedSoundAt(0, kSounds), TestArrangements, itself);

    Check(arranged.tracks == 0x0002, "the arrangement sets its tracks");
    Check(itself.tracks == PerformOptions().tracks, "the sound itself keeps its options");

    PerformOptions options;
    const std::string message =
        ErrorOf<std::runtime_error>([&] { SetUpArrangement(ArrangedSoundAt(6, kSounds), TestArrangements, options); });
    Check(message == "sound 2 has no arrangement 0", "an arrangement that the sound lacks throws");
}

} // namespace

int main()
{
    TestListing();
    TestPickedWithSequence();
    TestArrangedSoundAt();
    TestSetUpArrangement();

    return xsflib::testing::Summary("arrangements");
}
