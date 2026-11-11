// SPDX-License-Identifier: MIT

// Tests of the reader of Hudson's sound manager (ntdlib/musyx/msm.h) on small synthetic files: the sound manager's
// file, the game's sound test, and the glyphs of its messages. The expected values follow from the layouts that the
// reader's source describes.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/musyx/msm.h"
#include "ntdlib/testing/big_endian_bytes.h"

namespace musyx = ntdlib::musyx;
using BigEndianBytes = ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

std::vector<uint8_t> TextBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// A sound manager's file with one group, two songs and two effects. The header gives the offsets and sizes of the
// tables of groups and songs. It also gives the offsets of the groups' data, the groups' samples, the songs that no
// group holds and the table of effects. The table of groups' first entry holds no group. Group 7's data starts with the
// offsets of its pool, its project, its sample directory and its songs. Song 0's SNG file is at offset 2 of the group's
// songs and holds the bytes 1 to 4. Song 1's file is at offset 1 of the songs that no group holds and holds the bytes 5
// to 7. The group's samples are at offset 1 of the groups' samples. They're the bytes 0xaa and 0xbb. The settings list
// one base group: the table's group 1. They give the table of effects three entries. The second holds no effect.
void TestSoundManagerFile()
{
    BigEndianBytes b;
    b.PadTo(0x60);
    b.Put32(0x20, 0x60);
    b.Put32(0x24, 0x40);
    b.PadTo(0x80);
    b.U16(7).U16(0).U32(0).U32(0x20).U32(1).U32(2).PadTo(0xa0);
    b.Put32(0x28, 0xa0);
    b.Put32(0x2c, 0x20);
    b.U16(7).U16(3).U32(2).U32(4).U8(0).U8(90).U16(0);
    b.U16(7).U16(4).U32(1).U32(3).U8(0xff).U8(127).U16(0);
    b.Put32(0x38, 0xc0);
    b.U32(0x10).U32(0x14).U32(0x18).U32(0x1a);
    b.U32(0x11111111).U32(0x22222222).U16(0x3333).U16(0).U32(0x01020304);
    b.Put32(0x40, 0xe0);
    b.U8(0).U8(0xaa).U8(0xbb);
    b.Put32(0x48, 0xe3);
    b.U8(0).U8(5).U8(6).U8(7);
    b.Put32(0x10, 0xe7);
    b.PadTo(0xe7 + 0x28).U8(1).U8(1);
    b.Put16(0xe7 + 6, 3);
    b.PadTo(0x120);
    b.Put32(0x30, 0x120);
    b.U16(7).U16(12).U8(100).U8(32).U16(0xfff0).U8(64).U8(20).U8(30).U8(0).U32(0);
    b.U16(7).U16(0xffff).PadTo(0x140);
    b.U16(7).U16(13).U8(127).U8(64).U16(0x10).U8(0).U8(0).U8(0).U8(0).U32(0);
    const std::vector<uint8_t>& file = b.Data();
    const musyx::MsmFile msm(file, "sound/test.msm");

    const musyx::MsmSong in_group = msm.Song(0);
    Check(in_group.group_id == 7 && in_group.song_id == 3 && in_group.volume == 90 &&
              std::vector<uint8_t>(in_group.data.begin(), in_group.data.end()) == std::vector<uint8_t>{1, 2, 3, 4},
          "a song in a group: its group, its ID, its volume and its file in the group's songs");
    const musyx::MsmSong outside = msm.Song(1);
    Check(outside.song_id == 4 && outside.volume == 127 &&
              std::vector<uint8_t>(outside.data.begin(), outside.data.end()) == std::vector<uint8_t>{5, 6, 7},
          "a song that no group holds: its file among those songs");
    Check(ErrorOf([&] { msm.Song(2); }) == "sound/test.msm has no song 2", "a song past the table fails");

    const musyx::MsmGroup group = msm.Group(7);
    Check(group.pool.size() == 0x10 && group.pool[0] == 0x11 && group.project.size() == 0xc &&
              group.project[0] == 0x22 && group.directory.size() == 8 && group.directory[0] == 0x33,
          "a group's parts run from their offsets to the end of the group's data");
    Check(std::vector<uint8_t>(group.samples.begin(), group.samples.end()) == std::vector<uint8_t>{0xaa, 0xbb},
          "a group's samples");
    Check(ErrorOf([&] { msm.Group(8); }) == "sound/test.msm has no group 8", "a group that isn't in the table fails");
    const std::vector<musyx::MsmGroup> base = msm.BaseGroups();
    Check(base.size() == 1 && base[0].pool.data() == group.pool.data(), "the settings' base group");
    const std::vector<musyx::MsmEffect> effects = msm.Effects();
    if (Check(effects.size() == 2, "the table lists two effects"))
    {
        const musyx::MsmEffect& first = effects[0];
        Check(first.number == 0 && first.group_id == 7 && first.id == 12 && first.volume == 100 && first.pan == 32 &&
                  first.pitch == -16 && first.reverb == 20 && first.chorus == 30,
              "the first effect: its number, group, ID, volume, pan, pitch and sends");
        Check(effects[1].number == 2 && effects[1].id == 13 && effects[1].pitch == 16,
              "the entry after an empty one keeps its number");
    }

    std::vector<uint8_t> wrong_base = file;
    wrong_base[0xe7 + 0x29] = 5;
    Check(ErrorOf([&] { musyx::MsmFile(wrong_base, "sound/test.msm").BaseGroups(); }) ==
              "sound/test.msm has no group at 5 in its table",
          "a base group that isn't in the table fails");

    const std::vector<uint8_t> cut(file.begin(), file.begin() + 0x40);
    Check(
        ErrorOf([&] { musyx::MsmFile short_file(cut, "sound/test.msm"); }) == "sound/test.msm is cut short or damaged",
        "a file shorter than the header fails");
}

// A sound test of four entries: a song, an unused entry, a stream and a sound effect. The song and the stream are
// listed. The song's name is message 1 of bank 47, and the stream's is message 0. The message file's table gives bank
// 47 slot 1. The word at 8 then gives that bank's offset. The message's control codes set a colour, make spaces and
// move to the next tab stop. Bytes 0x5c and 0xc2 draw an apostrophe and '!'.
void TestSoundTest()
{
    BigEndianBytes messages;
    messages.U32(1).U32(0x10).U32(0x20).U32(0).U16(47).U16(1).PadTo(0x20);
    messages.U32(2).U32(0xc).U32(0xd).U8(0);
    messages.U8(0x1e).U8(5).Append(TextBytes("Boo")).U8(0x5c).Append(TextBytes("s")).U8(0x10);
    messages.Append(TextBytes("Haunted")).U8(0x0a).Append(TextBytes("Bash")).U8(0xc2).U8(0x0c).U8(0);

    BigEndianBytes module;
    module.U32(0);
    module.U32(1).U32(47u << 16 | 1).U16(3).U16(80).U32(45);
    module.U32(0).U32(47u << 16 | 1).U16(3).U16(80).U32(46);
    module.U32(1).U32(47u << 16).U16(1).U16(0).U32(12);
    module.U32(1).U32(47u << 16).U16(28).U16(0).U32(100);
    BigEndianBytes missing;
    missing.U32(1).U32(47u << 16 | 2).U16(3).U16(80).U32(45);

    const std::vector<musyx::SoundTestEntry> entries =
        musyx::ReadSoundTest(module.Data(), "dll/test.rel", 4, 4, messages.Data(), "mess/test.dat", false);
    if (Check(entries.size() == 2, "the sound test lists a song and a stream"))
    {
        const musyx::SoundTestEntry& song = entries[0];
        Check(song.kind == 3 && song.group_set == 80 && song.number == 45 && song.name == "Boo's Haunted Bash!",
              "the song: its kind, its group set, its number and its name from the messages");
        Check(entries[1].kind == musyx::kSoundTestStream && entries[1].number == 12 && entries[1].name.empty(),
              "the stream, named by an empty message");
    }

    const auto read_past_end = [&]
    {
        musyx::ReadSoundTest(module.Data(), "dll/test.rel", 4, 5, messages.Data(), "mess/test.dat", false);
    };
    Check(ErrorOf(read_past_end) == "dll/test.rel's sound test runs past the file's end",
          "an entry past the module's end fails");

    // A list of voices in the same form names effects by their numbers. Of two entries for one effect, the later
    // counts. An unused entry names nothing.
    BigEndianBytes voices;
    voices.U32(1).U32(47u << 16).U16(28).U16(0).U32(100);
    voices.U32(1).U32(47u << 16 | 1).U16(28).U16(0).U32(100);
    voices.U32(0).U32(47u << 16 | 1).U16(28).U16(0).U32(101);
    voices.U32(1).U32(47u << 16 | 1).U16(3).U16(80).U32(102);
    const std::map<uint32_t, std::string> named =
        musyx::ReadSoundTestEffects(voices.Data(), "dll/test.rel", 0, 4, messages.Data(), "mess/test.dat", false);
    Check(named.size() == 1 && named.contains(100) && named.at(100) == "Boo's Haunted Bash!",
          "the list names effect 100 by its later entry");
    const auto read_missing = [&]
    {
        musyx::ReadSoundTest(missing.Data(), "dll/test.rel", 0, 1, messages.Data(), "mess/test.dat", false);
    };
    Check(ErrorOf(read_missing) == "mess/test.dat's message 47-2 is missing", "a name that isn't in the bank fails");
}

// The European messages draw accented letters from 0x91 and 0xD1. The Japanese font draws kana at those codes. Its
// voiced and semi-voiced marks (0x80 and 0x81) join the kana before them. Message 0 of bank 47 is "Seaside Soirée" in
// the English font, and message 1 is "がんばれ マリオパーティ4" in the Japanese font.
void TestSoundTestGlyphs()
{
    BigEndianBytes messages;
    messages.U32(1).U32(0x10).U32(0x20).U32(0).U16(47).U16(1).PadTo(0x20);
    messages.U32(2).U32(0xc).U32(0x1b);
    messages.Append(TextBytes("Seaside")).U8(0x10).Append(TextBytes("Soir")).U8(0xd6).Append(TextBytes("e")).U8(0);
    messages.Append({0x96, 0x80, 0xbd, 0xaa, 0x80, 0xba, 0x10, 0xef, 0xf8, 0xd5, 0xea, 0x81, 0x84, 0xe3, 0xc8});
    messages.Append(TextBytes("4")).U8(0);

    BigEndianBytes module;
    module.U32(1).U32(47u << 16).U16(3).U16(80).U32(45);
    module.U32(1).U32(47u << 16 | 1).U16(3).U16(80).U32(46);
    const std::vector<musyx::SoundTestEntry> english =
        musyx::ReadSoundTest(module.Data(), "dll/test.rel", 0, 1, messages.Data(), "mess/test.dat", false);
    Check(english.size() == 1 && english[0].name == "Seaside Soirée", "the English font draws an accented letter");
    const std::vector<musyx::SoundTestEntry> japanese =
        musyx::ReadSoundTest(module.Data(), "dll/test.rel", 16, 1, messages.Data(), "mess/test.dat", true);
    Check(japanese.size() == 1 && japanese[0].name == "がんばれ マリオパーティ4",
          "the Japanese font draws kana and joins the marks to them");
}

} // namespace

int main()
{
    TestSoundManagerFile();
    TestSoundTest();
    TestSoundTestGlyphs();

    return xsflib::testing::Summary("musyx_msm");
}
