// SPDX-License-Identifier: MIT

// Tests of the tables that the readers of the MusyX catalogues share (ntdlib/musyx/catalogue.h), on small synthetic
// data: a module's table of sounds and a DOL's table of songs' paths. The expected values follow from the layouts that
// the header describes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"
#include "ntdlib/testing/big_endian_bytes.h"
#include "ntdlib/testing/dol_files.h"

namespace musyx = ntdlib::musyx;
using BigEndianBytes = ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// The bytes of `text`, without a terminating zero.
std::vector<uint8_t> TextBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// A DOL whose first data section holds `data` at load address `address`.
std::vector<uint8_t> DolWithData(uint32_t address, const std::vector<uint8_t>& data)
{
    constexpr std::size_t kHeader = 0x100;
    std::vector<uint8_t> dol(kHeader);
    ntdlib::testing::SetDolSection(dol, 7, static_cast<uint32_t>(kHeader), address, static_cast<uint32_t>(data.size()));
    dol.insert(dol.end(), data.begin(), data.end());

    return dol;
}

// A module's table of sounds lists songs and sound effects in entries of 12 bytes. A song's entry gives its song group,
// its reverb, its file's number in the table of files and its song ID. The table of files gives each file's FSYS ID.
// An effect's entry gives its effects group and its effect ID.
void TestSoundTable()
{
    constexpr uint32_t kSounds = 0x10;
    constexpr uint32_t kFiles = 0x50;
    BigEndianBytes b;
    b.PadTo(kSounds);
    b.U8(0x00).U8(0).U8(3).U8(1).U16(0x0102).U16(0).U32(0); // a song
    b.U8(0x80).U8(2).U8(0).U8(9).U16(0x0007).U16(0).U32(0); // a sound effect, with a file past the table
    b.U8(0x00).U8(0).U8(1).U8(0).U16(0x0003).U16(0).U32(0); // a song whose file has the ID 0
    b.U8(0x00).U8(1).U8(5).U8(2).U16(0x004d).U16(0).U32(0); // a song of the second group
    b.PadTo(kFiles).U32(0).U32(0x6d4).U32(0x01a40800).U32(0x6bc).U32(0x00100800).U32(0x6a4);
    const std::vector<uint8_t> module = b.Data();

    const std::vector<musyx::TableSound> sounds = musyx::ReadSoundTable(module, "test_rel", kSounds, 4, kFiles, 3);
    if (Check(sounds.size() == 3, "the table lists two songs and the effect, and no song without a file"))
    {
        Check(sounds[0].sound == 0 && sounds[0].kind == musyx::SoundKind::kSong && sounds[0].group == 0 &&
                  sounds[0].id == 0x0102 && sounds[0].reverb == 3 && sounds[0].file == 0x01a40800,
              "the first song: its entry, its group, its song ID, its reverb and its file's FSYS ID");
        Check(sounds[1].sound == 1 && sounds[1].kind == musyx::SoundKind::kEffect && sounds[1].group == 2 &&
                  sounds[1].id == 0x0007,
              "the effect: its entry, its group and its effect ID, whatever file its entry names");
        Check(sounds[2].sound == 3 && sounds[2].kind == musyx::SoundKind::kSong && sounds[2].group == 1 &&
                  sounds[2].id == 0x004d && sounds[2].reverb == 5 && sounds[2].file == 0x00100800,
              "the song of the second group");
    }

    std::vector<uint8_t> past_the_files = module;
    past_the_files[kSounds + 3] = 3;
    const std::string no_file =
        ErrorOf([&] { musyx::ReadSoundTable(past_the_files, "test_rel", kSounds, 4, kFiles, 3); });
    Check(no_file == "test_rel's sound 0 names no file", "a song's file past the table of files fails: " + no_file);
    const std::string long_sounds = ErrorOf([&] { musyx::ReadSoundTable(module, "test_rel", kSounds, 8, kFiles, 3); });
    Check(long_sounds == "test_rel's table of sounds runs past the file's end",
          "a table of sounds past the module's end fails: " + long_sounds);
    const std::string long_files = ErrorOf([&] { musyx::ReadSoundTable(module, "test_rel", kSounds, 4, kFiles, 4); });
    Check(long_files == "test_rel's table of files runs past the file's end",
          "a table of files past the module's end fails: " + long_files);
}

// A table of two words, each the address of a song's path. The paths follow the table.
void TestSongPaths()
{
    constexpr uint32_t kBase = 0x80220000;
    BigEndianBytes b;
    b.U32(kBase + 0x10).U32(kBase + 0x20).PadTo(0x10);
    b.Append(TextBytes("audio/title.sng")).PadTo(0x20);
    b.Append(TextBytes("audio/staff.sng")).PadTo(0x30);
    const std::vector<uint8_t> file = DolWithData(kBase, b.Data());
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(file);
    if (!Check(dol.has_value(), "the test's DOL reads"))
    {
        return;
    }

    const std::vector<std::string> paths = musyx::ReadSongPaths(*dol, kBase, 2);
    const std::string past_the_data = ErrorOf([&] { musyx::ReadSongPaths(*dol, kBase + 0x30, 1); });

    Check(paths == std::vector<std::string>{"audio/title.sng", "audio/staff.sng"}, "the table gives both paths");
    Check(past_the_data == "main.dol has no data at the table of songs' paths",
          "a table past the DOL's data fails: " + past_the_data);
}

} // namespace

int main()
{
    TestSoundTable();
    TestSongPaths();

    return xsflib::testing::Summary("musyx_catalogue");
}
