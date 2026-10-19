// SPDX-License-Identifier: MIT

// Checks the length analysis's cases (ntdlib/testing/length_cases.h): the archive reads in both byte orders, every case
// is a sequence sound with its settings and its bytecode, and the two byte orders differ only in the commands' 16- and
// 24-bit arguments. A rip tool's test checks the timings on its model.

#include "ntdlib/testing/length_cases.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "xsflib/testing/check.h"

namespace
{

using ntdlib::testing::length_cases::Bytecode;
using ntdlib::testing::length_cases::Cases;
using xsflib::testing::Check;

// Reads the archive in one byte order. Each case must be a sequence sound with the case's settings, in a file whose
// data starts with the case's bytecode, with the start label at 0. The data block's padding follows.
void CheckArchive(bool big_endian)
{
    const std::string order = big_endian ? "big-endian: " : "little-endian: ";
    ntdlib::sar::Options options;
    options.flavor = big_endian ? ntdlib::sar::Flavor::kCafe : ntdlib::sar::Flavor::kNx;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;
    const ntdlib::sar::SoundArchive archive =
        ntdlib::sar::SoundArchive::Load(ntdlib::testing::length_cases::Archive(big_endian), options);
    if (!Check(archive.Sounds().size() == Cases().size(), order + "a sound for each case"))
    {
        return;
    }

    for (std::size_t i = 0; i < Cases().size(); ++i)
    {
        const ntdlib::testing::length_cases::Case& item = Cases()[i];
        const ntdlib::sar::SoundInfo& sound = archive.Sounds()[i];
        const ntdlib::sar::Sequence sequence = ntdlib::sar::Sequence::Parse(archive.FileData(sound.file_id), options);
        const std::vector<uint8_t> bytecode = Bytecode(i, big_endian);
        const bool settings = sound.name == item.name && sound.type == ntdlib::sar::SoundType::kSequence &&
                              sound.volume == item.volume && sound.allocate_track_flags == item.tracks &&
                              sound.start_offset == item.start;
        const bool data = sequence.big_endian == big_endian && sequence.labels.size() == 1 &&
                          sequence.labels[0].offset == 0 && sequence.data.size() >= bytecode.size() &&
                          std::equal(bytecode.begin(), bytecode.end(), sequence.data.begin());
        Check(settings && data, order + item.name + " is its sequence");
    }
}

// The two byte orders differ only in the multi-byte arguments. Each case then holds the same bytes in both orders, and
// its start and every offset in it are the same in both.
void CheckByteOrders()
{
    std::set<std::string> names;
    for (std::size_t i = 0; i < Cases().size(); ++i)
    {
        const std::vector<uint8_t> big = Bytecode(i, true);
        const std::vector<uint8_t> little = Bytecode(i, false);
        std::vector<uint8_t> sorted_big = big;
        std::vector<uint8_t> sorted_little = little;
        std::sort(sorted_big.begin(), sorted_big.end());
        std::sort(sorted_little.begin(), sorted_little.end());
        Check(big.size() == little.size() && sorted_big == sorted_little,
              std::string(Cases()[i].name) + " has the same bytes in both orders");
        names.insert(Cases()[i].name);
    }

    Check(names.size() == Cases().size(), "the cases' names differ");
}

} // namespace

int main()
{
    CheckArchive(true);
    CheckArchive(false);
    CheckByteOrders();

    return xsflib::testing::Summary("length_cases");
}
