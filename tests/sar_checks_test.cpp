// SPDX-License-Identifier: MIT

// Tests StartsAtLabel of ntdlib/testing/sar_checks.h on hand-made sequences. The header's bank and wave checks compile
// here. The tests of the programs that read every archive of a game run them.

#include "ntdlib/testing/sar_checks.h"

#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"

using ntdlib::testing::sar_checks::StartsAtLabel;
using xsflib::testing::Check;

namespace
{

namespace sar = ntdlib::sar;

// A sound that starts at `start` with the allocation of tracks `tracks`.
sar::SoundInfo Sound(uint32_t start, uint32_t tracks)
{
    sar::SoundInfo sound;
    sound.start_offset = start;
    sound.allocate_track_flags = tracks;
    return sound;
}

void TestStartsAtLabel()
{
    // A label at 0, and a label at 4 that allocates tracks 1 and 2 (mask 0x0006).
    const std::vector<uint8_t> big = {0xFF, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x06, 0xFF};
    const std::vector<uint8_t> little = {0xFF, 0x00, 0x00, 0x00, 0xFE, 0x06, 0x00, 0xFF};
    for (const bool big_endian : {true, false})
    {
        sar::Sequence sequence;
        sequence.data = big_endian ? big : little;
        sequence.big_endian = big_endian;
        sequence.labels = {{"A", 0}, {"B", 4}};
        const char* order = big_endian ? "big-endian" : "little-endian";

        Check(StartsAtLabel(sequence, Sound(0, 1)), std::string(order) + ": a sound at a label");
        Check(StartsAtLabel(sequence, Sound(4, 1)), std::string(order) + ": a sound at a label that allocates tracks");
        Check(StartsAtLabel(sequence, Sound(7, 7)),
              std::string(order) + ": a sound past the allocation, with track 0 added to the mask");
        Check(!StartsAtLabel(sequence, Sound(7, 6)), std::string(order) + ": a mask without track 0");
        Check(!StartsAtLabel(sequence, Sound(3, 1)), std::string(order) + ": a sound 3 bytes past a plain label");
        Check(!StartsAtLabel(sequence, Sound(5, 1)), std::string(order) + ": a sound between labels");
    }

    // A label too close to the end for an allocation.
    const std::vector<uint8_t> short_data = {0xFE, 0x00};
    sar::Sequence cut;
    cut.data = short_data;
    cut.labels = {{"A", 0}};
    Check(!StartsAtLabel(cut, Sound(3, 1)), "an allocation past the end of the data");

    // Offsets near the ends of the 32-bit range, and a label past the end of the data. None of them may wrap around.
    sar::Sequence far;
    far.data = short_data;
    far.labels = {{"A", 0xFFFFFFFE}};
    Check(!StartsAtLabel(far, Sound(1, 1)), "a label near 2^32 and a start under 3");
    far.labels = {{"A", 0xFFFFFFFC}};
    Check(!StartsAtLabel(far, Sound(0xFFFFFFFF, 1)), "a start near 2^32 past a label beyond the data");
    const std::vector<uint8_t> eight = {0xFE, 0x00, 0x01, 0xFF, 0xFE, 0x00, 0x01, 0xFF};
    sar::Sequence past;
    past.data = eight;
    past.big_endian = true;
    past.labels = {{"A", 100}};
    Check(!StartsAtLabel(past, Sound(103, 1)), "a label past the end of the data");
    past.labels = {{"A", 4}};
    Check(StartsAtLabel(past, Sound(7, 1)), "an allocation that ends at the end of the data");
    const std::vector<uint8_t> four = {0xFF, 0xFF, 0xFE, 0x00};
    past.data = four;
    past.labels = {{"A", 2}};
    Check(!StartsAtLabel(past, Sound(5, 1)), "an allocation that runs past the end of the data");

    // A placeholder: no labels, and a start at 0.
    const sar::Sequence placeholder;
    Check(StartsAtLabel(placeholder, Sound(0, 1)), "a placeholder starts at 0");
    Check(!StartsAtLabel(placeholder, Sound(2, 1)), "a placeholder starts nowhere else");
}

} // namespace

int main()
{
    TestStartsAtLabel();

    return xsflib::testing::Summary("sar_checks");
}
