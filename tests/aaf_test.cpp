// SPDX-License-Identifier: MIT

// Tests of the readers of JAudio1's start-up archive (jaudio/aaf.h), with a made-up archive.

#include "ntdlib/jaudio/aaf.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/testing/jaudio_files.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

namespace jaudio = ntdlib::jaudio;
namespace files = ntdlib::testing::jaudio_files;

namespace
{

// The effects sequence (the table's first sequence) in file 7, two pieces of music in files 3 and 9, and effects in
// categories 0 and 5.
const std::vector<files::AafSoundSpec> kSounds = {
    {16, 127, 7}, {16, 100, 3}, {16, 90, 9}, {0, 80}, {0, 81}, {5, 82},
};

void TestSounds()
{
    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadAafSounds(files::AafFile(kSounds, {"a_0.aw"}));

    XSFLIB_CHECK(sounds.size() == 5);
    if (sounds.size() != 5)
    {
        return;
    }

    // The effects, by category and index, then the music without the effects sequence. An effect's ID has bit 11. The
    // sound library then plays the effect to its end.
    XSFLIB_CHECK(sounds[0].Id() == 0x0800 && sounds[0].volume == 80 && sounds[0].type == 0);
    XSFLIB_CHECK(sounds[1].Id() == 0x0801 && sounds[1].volume == 81);
    XSFLIB_CHECK(sounds[2].Id() == 0x5800 && jaudio::AafCategory(sounds[2].Id()) == 5);
    XSFLIB_CHECK(sounds[0].source == jaudio::SoundEntry::Source::kCollection && sounds[0].resource == 0);
    XSFLIB_CHECK(sounds[3].Id() == 0x80000001 && sounds[3].volume == 100 && sounds[3].type == 0x80);
    XSFLIB_CHECK(sounds[3].source == jaudio::SoundEntry::Source::kArchive && sounds[3].resource == 3);
    XSFLIB_CHECK(sounds[4].Id() == 0x80000002 && sounds[4].resource == 9);
    XSFLIB_CHECK(sounds[0].name.empty() && sounds[4].name.empty());

    // An entry's offset: the table at 0x100, its entries from +0x50. The effects of category 0 follow the three
    // sequences.
    XSFLIB_CHECK(sounds[0].offset == 0x100 + 0x50 + 3 * 0x10);
}

// The message with which ReadAafSounds rejects the archive, or an empty one.
std::string Refusal(std::span<const uint8_t> aaf)
{
    try
    {
        jaudio::ReadAafSounds(aaf);
    }
    catch (const xsflib::FormatError& error)
    {
        return error.what();
    }

    return {};
}

void TestDamaged()
{
    // An archive cut short, an archive without a sound table, and a table without sounds are rejected.
    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {});
    XSFLIB_CHECK(Refusal(aaf).empty());
    XSFLIB_CHECK(Refusal(std::span(aaf).first(0x180)) == "the AAF archive is damaged");

    std::vector<uint8_t> no_table = aaf;
    no_table[3] = 4; // the sound table's chunk becomes a chunk of another type
    XSFLIB_CHECK(Refusal(no_table) == "the AAF archive has no sound table");
    XSFLIB_CHECK(Refusal(files::AafFile({{16, 127, 7}}, {})) == "the AAF archive's sound table lists no sounds");
}

void TestEffectsSequence()
{
    XSFLIB_CHECK(jaudio::AafEffectsSequenceFile(files::AafFile(kSounds, {})) == 7);
}

void TestSequenceFormat()
{
    // Without bit 0 of the sequences' format, a sequence's file is its index, whatever its entry holds there. The
    // effects sequence is index 0.
    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {}, 0);
    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadAafSounds(aaf);

    XSFLIB_CHECK(sounds.size() == 5);
    if (sounds.size() == 5)
    {
        XSFLIB_CHECK(sounds[3].Id() == 0x80000001 && sounds[3].resource == 1);
        XSFLIB_CHECK(sounds[4].Id() == 0x80000002 && sounds[4].resource == 2);
        XSFLIB_CHECK(sounds[0].resource == 0 && sounds[0].source == jaudio::SoundEntry::Source::kCollection);
    }

    XSFLIB_CHECK(jaudio::AafEffectsSequenceFile(aaf) == 0);

    // Any format with bit 0 gives the entry's file.
    XSFLIB_CHECK(jaudio::AafEffectsSequenceFile(files::AafFile(kSounds, {}, 3)) == 7);
}

// The sequences of TestSequenceIndex: their names and where they lie in a 0x100-byte archive.
const std::vector<files::AafSequenceSpec> kIndex = {
    {"se.bms", 0, 0x40}, {"Mansion.com", 0x40, 0x80}, {"VeryLongNameOfAPiece.com", 0xc0, 0x40}};

// The message with which ReadAafSequenceFiles rejects the archive, or an empty one.
std::string IndexRefusal(std::span<const uint8_t> aaf, std::span<const uint8_t> archive)
{
    try
    {
        jaudio::ReadAafSequenceFiles(aaf, archive);
    }
    catch (const xsflib::FormatError& error)
    {
        return error.what();
    }

    return {};
}

void TestSequenceIndex()
{
    // The index's sequences, by their numbers. A name keeps its first 13 characters. The data lies in the archive.
    std::vector<uint8_t> archive(0x100);
    for (std::size_t i = 0; i < archive.size(); i++)
    {
        archive[i] = static_cast<uint8_t>(i);
    }

    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {"a_0.aw"}, 0, kIndex);
    const std::optional<std::vector<jaudio::ArchiveFile>> sequences = jaudio::ReadAafSequenceFiles(aaf, archive);

    XSFLIB_CHECK(sequences && sequences->size() == 3);
    if (sequences && sequences->size() == 3)
    {
        const auto& s = *sequences;
        XSFLIB_CHECK(s[0].id == 0 && s[0].name == "se.bms" && s[0].data.data() == archive.data());
        XSFLIB_CHECK(s[0].data.size() == 0x40);
        XSFLIB_CHECK(s[1].id == 1 && s[1].name == "Mansion.com" && s[1].data.size() == 0x80 && s[1].data[0] == 0x40);
        XSFLIB_CHECK(s[2].id == 2 && s[2].name == "VeryLongNameO" && s[2].data.size() == 0x40 && s[2].data[0] == 0xc0);
    }

    // The other readers skip the index.
    XSFLIB_CHECK(jaudio::ReadAafSounds(aaf).size() == 5 && jaudio::ReadAafWaveSystems(aaf).size() == 1);

    // An archive without an index gives nothing.
    XSFLIB_CHECK(!jaudio::ReadAafSequenceFiles(files::AafFile(kSounds, {}), archive));

    // A sequence that ends past the archive, an index that isn't one, and an index cut short are rejected.
    XSFLIB_CHECK(IndexRefusal(aaf, archive).empty());
    XSFLIB_CHECK(IndexRefusal(aaf, std::span(archive).first(0xff)) ==
                 "the AAF archive's sequence index names a sequence outside the sequence archive");

    std::vector<uint8_t> not_index = aaf;
    const std::size_t index_at = 0x800 + 0x200;
    not_index[index_at] = 'X';
    XSFLIB_CHECK(IndexRefusal(not_index, archive) == "the AAF archive's sequence index is damaged");
    XSFLIB_CHECK(IndexRefusal(std::span(aaf).first(index_at + 0x20 + 0x30), archive) == "the AAF archive is damaged");
}

void TestWaveSystems()
{
    // Each system is numbered by its place in the list. Two systems with the same archive name it once. Systems 0 and 2
    // load at start-up, and system 1 for a scene.
    const std::vector<uint8_t> aaf = files::AafFile(kSounds, {"a_0.aw", "b_0.aw", "a_0.aw"});

    const std::vector<jaudio::WaveSystem> systems = jaudio::ReadAafWaveSystems(aaf);

    XSFLIB_CHECK(systems.size() == 3);
    for (std::size_t s = 0; s < systems.size(); s++)
    {
        XSFLIB_CHECK(systems[s].number == s && systems[s].groups.size() == 1);
        XSFLIB_CHECK(!systems[s].groups.empty() && systems[s].groups[0].waves == std::vector<uint16_t>{0});
    }

    XSFLIB_CHECK(jaudio::AafWaveArchiveNames(aaf) == (std::vector<std::string>{"a_0.aw", "b_0.aw"}));
    XSFLIB_CHECK(jaudio::AafStayingWaveArchiveNames(aaf) == (std::vector<std::string>{"a_0.aw"}));
}

} // namespace

int main()
{
    TestSounds();
    TestDamaged();
    TestEffectsSequence();
    TestSequenceFormat();
    TestSequenceIndex();
    TestWaveSystems();

    return xsflib::testing::Summary("aaf");
}
