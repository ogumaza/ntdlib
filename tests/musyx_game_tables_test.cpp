// SPDX-License-Identifier: MIT

// Tests of the readers of the tables of songs and effects in a MusyX game's main.dol (ntdlib/musyx/game_tables.h), on
// small synthetic DOLs. The expected values follow from the layouts that the header describes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dol.h"
#include "ntdlib/musyx/game_tables.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "ntdlib/testing/dol_files.h"

namespace musyx = ntdlib::musyx;
using BigEndianBytes = ntdlib::testing::BigEndianBytes;
using ntdlib::testing::DolWithData;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// The load address of the tests' data.
constexpr uint32_t kBase = 0x80200000;

// The bytes of `text`, without a terminating zero.
std::vector<uint8_t> TextBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// File number 2 × (26² × 1 + 26 × 2 + 3) is the file B/C/D. The low bit doesn't count.
void TestCamelotPath()
{
    constexpr uint32_t kIndex = 26 * 26 + 26 * 2 + 3;

    const std::string first = musyx::CamelotPath(0);
    const std::string path = musyx::CamelotPath(kIndex * 2);
    const std::string flagged = musyx::CamelotPath(kIndex * 2 + 1);

    Check(first == "A/A/A", "file 0 is A/A/A: " + first);
    Check(path == "B/C/D", "the index in base 26 gives B/C/D: " + path);
    Check(flagged == "B/C/D", "the low bit leaves the path alone: " + flagged);
}

// Two song lists: one of 16-byte entries that give the name's address, one of 0x14-byte entries with the name inline.
void TestCamelotSongs()
{
    BigEndianBytes b;
    b.U32(kBase + 0x40).U32(4650).U32(3).U32(0x7f000000);                    // at 0x00: a name's address
    b.Append(TextBytes("title")).PadTo(0x18).U32(12).U32(9).U32(0x50000000); // at 0x10: an inline name of 8 bytes
    b.PadTo(0x40).Append(TextBytes("golf.sng")).PadTo(0x50);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::CamelotSong> named =
        musyx::ReadCamelotSongs(*dol, {.address = kBase, .count = 1, .entry_size = 0x10});
    const std::vector<musyx::CamelotSong> inline_names =
        musyx::ReadCamelotSongs(*dol, {.address = kBase + 0x10, .count = 1, .entry_size = 0x14, .name_inline = true});
    const musyx::CamelotSongList past_the_data{.address = kBase + 0x50, .count = 1, .entry_size = 0x10};
    const std::string past = ErrorOf([&] { musyx::ReadCamelotSongs(*dol, past_the_data); });

    if (Check(named.size() == 1, "the first list has one song"))
    {
        Check(named[0].name == "golf.sng" && named[0].file_number == 4650 && named[0].id == 3 && named[0].volume == 127,
              "the song through its name's address: its name as given, its file, its ID and its volume");
    }
    if (Check(inline_names.size() == 1, "the second list has one song"))
    {
        Check(inline_names[0].name == "title" && inline_names[0].file_number == 12 && inline_names[0].id == 9 &&
                  inline_names[0].volume == 0x50,
              "the song with its name inline");
    }
    Check(past == "main.dol has no data at the song list", "a list past the DOL's data fails: " + past);
}

// A table of ranges with two ranges and its end, a table of voice banks with one bank of each kind, two narrations and
// two courts' banks.
void TestCamelotEffects()
{
    BigEndianBytes b;
    b.U32(0x10).U32(0x12).U32(0x40).U32(0x40).U32(0xffffffff);         // 0x00: the ranges
    b.PadTo(0x20).U32(kBase + 0x80).U32(100).U32(2).U32(0x200).U32(7); // 0x20: a bank named through its address
    b.Append(TextBytes("mario")).PadTo(0x44).U32(102).U32(1).U32(0x300).U32(8); // 0x34: a bank named inline
    b.PadTo(0x60).U32(200).U32(0x00050011).U32(0xffffffff).U32(0x00010002);     // 0x60: two narrations
    b.U32(300).U32(0x0000000c).U32(0xffffffff).U32(0);                          // 0x70: two courts' banks
    b.Append(TextBytes("voice")).PadTo(0x90);                                   // 0x80: the first bank's name
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::CamelotRange> ranges = musyx::ReadCamelotRanges(*dol, kBase);
    const std::vector<musyx::VoiceBank> pointed =
        musyx::ReadVoiceBanks(*dol, {.address = kBase + 0x20, .count = 1, .entry_size = 0x14});
    const std::vector<musyx::VoiceBank> named =
        musyx::ReadVoiceBanks(*dol, {.address = kBase + 0x34, .count = 1, .entry_size = 0x20, .name_size = 16});
    const std::vector<musyx::Narration> narrations = musyx::ReadNarrations(*dol, kBase + 0x60, 2);
    const std::vector<musyx::CourtBank> courts = musyx::ReadCourtBanks(*dol, kBase + 0x70, 2);
    const std::string long_courts = ErrorOf([&] { musyx::ReadCourtBanks(*dol, kBase + 0x70, 5); });

    Check(ranges.size() == 2 && ranges[0].first == 0x10 && ranges[0].last == 0x12 && ranges[1].first == 0x40 &&
              ranges[1].last == 0x40,
          "the two ranges up to the end marker");
    Check(pointed.size() == 1 && pointed[0].name == "voice" && pointed[0].first_file == 100 &&
              pointed[0].effect_count == 2 && pointed[0].first_effect == 0x200 && pointed[0].group_id == 7,
          "the bank named through its address");
    Check(named.size() == 1 && named[0].name == "mario" && named[0].first_file == 102 && named[0].effect_count == 1 &&
              named[0].first_effect == 0x300 && named[0].group_id == 8,
          "the bank named inline");
    Check(narrations.size() == 2 && narrations[0].first_file == 200 && narrations[0].group_id == 5 &&
              narrations[0].effect_id == 0x11 && narrations[1].first_file == musyx::kCamelotNoFiles &&
              narrations[1].group_id == 1 && narrations[1].effect_id == 2,
          "the narrations: a group's files and the group that the game keeps");
    Check(courts.size() == 2 && courts[0].first_file == 300 && courts[0].group_id == 0x0c &&
              courts[1].first_file == musyx::kCamelotNoFiles,
          "the courts' banks, the second without files");
    Check(long_courts == "main.dol has no data at the table of courts",
          "a table of courts past the DOL's data fails: " + long_courts);
}

// A sound room of one song, a table of one song that plays as an effect, and a table of one effect.
void TestSoundRoom()
{
    BigEndianBytes b;
    b.U32(0x03000105).U8(100).PadTo(0x0c).U32(kBase + 0x40).PadTo(0x20); // 0x00: the sound room
    b.U8(0x02).U8(0).U16(0x0013).U8(90).PadTo(0x28);                     // 0x20: a song that plays as an effect
    b.U16(1700).U8(80).U8(0).U8(30).U8(40).PadTo(0x40);                  // 0x28: an effect
    b.Append(TextBytes("Main\x94\xbcTheme\x94\xbcOne")).PadTo(0x60);     // 0x40: the song's name
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<musyx::SoundRoomSong> room = musyx::ReadSoundRoom(*dol, kBase, 1);
    const std::vector<musyx::SoundRoomSong> effect_songs = musyx::ReadEffectSongs(*dol, kBase + 0x20, 1);
    const std::vector<musyx::PlayedEffect> effects = musyx::ReadPlayedEffects(*dol, kBase + 0x28, 1);
    const std::string past = ErrorOf([&] { musyx::ReadPlayedEffects(*dol, kBase + 0x5c, 1); });

    Check(room.size() == 1 && room[0].name == "Main Theme One" && room[0].number == 0x0105 && room[0].group_id == 3 &&
              room[0].volume == 100 && room[0].entry_word == 0x03000105,
          "the sound room's song: its name with spaces, its number, its group, its volume and its word");
    Check(effect_songs.size() == 1 && effect_songs[0].name == "se-0019" && effect_songs[0].number == 0x13 &&
              effect_songs[0].group_id == 2 && effect_songs[0].volume == 90,
          "the song that plays as an effect, named after its number");
    Check(effects.size() == 1 && effects[0].number == 1700 && effects[0].volume == 80 && effects[0].reverb == 30 &&
              effects[0].chorus == 40,
          "the effect: its number, its volume and its sends");
    Check(past == "main.dol has no data at the table of effects", "a table past the DOL's data fails: " + past);
}

} // namespace

int main()
{
    TestCamelotPath();
    TestCamelotSongs();
    TestCamelotEffects();
    TestSoundRoom();

    return xsflib::testing::Summary("musyx_game_tables");
}
