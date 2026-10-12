// SPDX-License-Identifier: MIT

// Tests of the readers of JAudio1's start-up archive (jaudio/aaf.h), with a made-up archive.

#include "ntdlib/jaudio/aaf.h"

#include <cstddef>
#include <cstdint>
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
    TestWaveSystems();

    return xsflib::testing::Summary("aaf");
}
