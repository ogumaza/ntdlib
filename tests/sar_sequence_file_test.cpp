// SPDX-License-Identifier: MIT

// Tests of the sequence files' reader and writer (ntdlib/sar/sequence_file.h), on files built in each flavor's format:
// the formats of the 3DS, the Wii U and the Switch.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
using ntdlib::sar::Flavor;
using ntdlib::sar::SequenceFile;
using xsflib::testing::ErrorOf;

void TestFlavor(Flavor flavor)
{
    // Labels whose names pad to 4 bytes in three ways.
    const std::vector<uint8_t> kBytecode = {0x88, 0x01, 0x08, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xC1, 0x64, 0xFF};
    const std::vector<std::pair<std::string, uint32_t>> kLabels = {{"Song", 0}, {"Track_1", 8}, {"Another_label", 5}};
    const std::vector<uint8_t> file = sar_files::Sequence(flavor, kBytecode, kLabels);

    const SequenceFile s = SequenceFile::Read(file);
    XSFLIB_CHECK(s.flavor == flavor);
    XSFLIB_CHECK(s.version == (flavor == Flavor::kCtr ? 0x01000000u : 0x00020000u));
    XSFLIB_CHECK(s.data.size() == 24 && std::equal(kBytecode.begin(), kBytecode.end(), s.data.begin()));
    XSFLIB_CHECK(s.labels.size() == 3 && s.labels[1].name == "Track_1" && s.labels[1].offset == 8);
    XSFLIB_CHECK(s.labels[2].name == "Another_label" && s.labels[2].offset == 5);
    XSFLIB_CHECK(s.Write() == file);

    // Without its padding, the bytecode gives the same file.
    SequenceFile made = s;
    made.data = kBytecode;
    XSFLIB_CHECK(made.Write() == file);

    // No bytecode and no labels.
    const std::vector<uint8_t> empty = sar_files::Sequence(flavor, {}, {});
    const SequenceFile none = SequenceFile::Read(empty);
    XSFLIB_CHECK(none.data.size() == 24 && none.labels.empty() && none.Write() == empty);
}

bool Refuses(const std::vector<uint8_t>& file)
{
    return !ErrorOf<xsflib::FormatError>([&] { SequenceFile::Read(file); }).empty();
}

void TestRefusals()
{
    std::vector<uint8_t> file = sar_files::Sequence(Flavor::kNx, {0xFF}, {{"Song", 0}});
    XSFLIB_CHECK(!Refuses(file));

    std::vector<uint8_t> bank = file; // another kind of file
    bank[1] = 'B';
    bank[2] = 'N';
    bank[3] = 'K';
    XSFLIB_CHECK(Refuses(bank));

    std::vector<uint8_t> unmarked = file; // no byte order mark
    unmarked[4] = 0;
    XSFLIB_CHECK(Refuses(unmarked));

    std::vector<uint8_t> unlabelled = file; // no LABL block
    unlabelled[0x21] = 0x52;
    XSFLIB_CHECK(Refuses(unlabelled));

    file.resize(0x50); // cut short
    XSFLIB_CHECK(Refuses(file));
}

} // namespace

int main()
{
    TestFlavor(Flavor::kCtr);
    TestFlavor(Flavor::kCafe);
    TestFlavor(Flavor::kNx);
    TestRefusals();

    return xsflib::testing::Summary("Sequence file");
}
