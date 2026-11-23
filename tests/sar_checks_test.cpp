// SPDX-License-Identifier: MIT

// Tests StartsAtLabel of ntdlib/testing/sar_checks.h on hand-made sequences, and LoadSequenceFiles and Banks on a
// hand-made archive, with CheckStart. The header's bank and wave checks compile here. The tests of the programs that
// read every archive of a game run them.

#include "ntdlib/testing/sar_checks.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

using ntdlib::testing::sar_checks::Banks;
using ntdlib::testing::sar_checks::CheckStart;
using ntdlib::testing::sar_checks::LoadSequenceFiles;
using ntdlib::testing::sar_checks::StartsAtLabel;
using xsflib::testing::Check;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

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

// An archive of four sequence sounds. Bank 0 names wave archive 0, and the archive contains it. Bank 1 names wave
// archive 1, and the archive doesn't contain it. "here" uses bank 0 and leaves its second slot empty. "no waves" uses
// bank 1. "no sequence" has its sequence outside the archive. "bank 0 again" uses bank 0 once more.
sar::SoundArchive BanksArchive(bool big_endian)
{
    const sar::Flavor flavor = sar_files::FlavorOf(big_endian);
    sar_files::BankSpec outside = sar_files::OneRegionBank();
    outside.waves = {{sar::ItemId(sar::kWaveArchiveItem, 1), 0}};

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK_HERE", 0}, {"BANK_OUTSIDE", 1}};
    spec.wave_archives = {{"WARC_HERE", 2}, {"WARC_OUTSIDE", 3}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {
        {sar_files::Bank(flavor, sar_files::OneRegionBank()), "", false},
        {sar_files::Bank(flavor, outside), "", false},
        {sar_files::WaveArchive(flavor, {sar_files::Wave(flavor, sar_files::ConstantWave(big_endian))}), "", false},
        {{}, "outside.bcwar", false},
        {sar_files::Sequence(flavor, {0xFF}, {{"Start", 0}}), "", false},
        {{}, "outside.bcseq", false},
    };

    const auto sequence = [](const std::string& name, uint32_t file, std::vector<uint32_t> banks)
    {
        sar_files::SoundSpec sound;
        sound.name = name;
        sound.file = file;
        sound.banks = std::move(banks);
        return sound;
    };
    spec.sounds = {sequence("here", 4, {0, 0xFFFFFFFF}), sequence("no waves", 4, {1}), sequence("no sequence", 5, {0}),
                   sequence("bank 0 again", 4, {0})};

    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return sar::SoundArchive::Load(sar_files::Archive(flavor, spec), options);
}

void TestLoadSequenceFiles()
{
    for (const bool big_endian : {true, false})
    {
        const sar::SoundArchive archive = BanksArchive(big_endian);
        Banks banks(archive);
        std::vector<bool> here;
        for (const sar::SoundInfo& sound : archive.Sounds())
        {
            here.push_back(LoadSequenceFiles("archive", archive, sound, banks));
        }

        const std::string order = big_endian ? "big-endian" : "little-endian";
        Check(here == std::vector<bool>{true, false, false, true},
              order + ": the files of a sound are here only with its sequence, its banks and their wave archives");
        Check(banks.Loaded().size() == 1 && banks.Loaded().contains(0),
              order + ": the banks read are the one whose wave archive is here");
        Check(banks.Load(0) && !banks.Load(1), order + ": a bank keeps whether it's here");

        // The sequence of "here" starts at its label.
        const sar::SoundInfo& here_sound = archive.Sounds()[0];
        CheckStart("archive", here_sound,
                   sar::Sequence::Parse(archive.FileData(here_sound.file_id), archive.ReadOptions()));
    }
}

} // namespace

int main()
{
    TestStartsAtLabel();
    TestLoadSequenceFiles();

    return xsflib::testing::Summary("sar_checks");
}
