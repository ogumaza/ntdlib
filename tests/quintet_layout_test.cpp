// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a ROM of Quintet's driver (ntdlib/quintet/layout.h). The ROMs are built by hand: a
// length table and a music file of two songs and a sound effect.

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/layout.h"

using xsflib::testing::Check;

namespace
{

namespace quintet = ntdlib::quintet;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The places in a test ROM.
constexpr uint32_t kLengthTable = 0x100;
constexpr uint32_t kFile = 0x200;      // the music file's first word: the size of its songs
constexpr uint32_t kSongs = kFile + 4; // two songs of 26 bytes, a length of 0 and 8 zeros
constexpr uint32_t kSongsSize = 64;
constexpr uint32_t kEffects = kSongs + kSongsSize + 4; // a sound effect, and a length of 0
constexpr uint32_t kEffectsSize = 28;
constexpr uint32_t kSongBytes = 26;

// A song of 6 channels, each a quarter note of C and an end.
std::vector<uint8_t> Song()
{
    std::vector<uint8_t> song = {kSongBytes, 0};
    for (uint8_t c = 0; c < 6; c++)
    {
        song.push_back(uint8_t(14 + 2 * c));
        song.push_back(0);
    }
    for (int c = 0; c < 6; c++)
    {
        song.push_back(0x20);
        song.push_back(0xFF);
    }

    return song;
}

std::vector<uint8_t> TestBytes()
{
    std::vector<uint8_t> bytes(0x400, 0xEE);
    const uint8_t lengths[13] = {1, 2, 4, 8, 16, 32, 64, 3, 6, 12, 24, 48, 4};
    for (uint32_t i = 0; i < 13; i++)
    {
        bytes[kLengthTable + i] = lengths[i];
    }

    const std::vector<uint8_t> song = Song();
    xsflib::StoreLe32(&bytes[kFile], kSongsSize);
    std::copy(song.begin(), song.end(), bytes.begin() + kSongs);
    std::copy(song.begin(), song.end(), bytes.begin() + kSongs + kSongBytes);
    std::fill(bytes.begin() + kSongs + 2 * kSongBytes, bytes.begin() + kSongs + kSongsSize, uint8_t(0));
    xsflib::StoreLe32(&bytes[kEffects - 4], kEffectsSize);
    std::copy(song.begin(), song.end(), bytes.begin() + kEffects);
    bytes[kEffects + kSongBytes] = 0;
    bytes[kEffects + kSongBytes + 1] = 0;

    return bytes;
}

quintet::DriverInfo InfoOf()
{
    quintet::DriverInfo info;
    info.revision = quintet::Revision::kA;
    info.length_table = kRomBase + kLengthTable;
    info.music_files = {kRomBase + kFile};
    info.music_file = kRomBase + kSongs;
    info.effects_file = kRomBase + kEffects;
    info.song_addresses = {kRomBase + kSongs, kRomBase + kSongs + kSongBytes};
    info.effect_addresses = {kRomBase + kEffects};
    return info;
}

Rom RomOf(const std::vector<uint8_t>& bytes)
{
    Rom rom;
    rom.Assign(bytes);
    return rom;
}

// Inserts `item` at the start of the first channel of sequence `sequence`, and moves the fields that name the items
// after it: the other channels' starts and the song's end.
void InsertFirst(quintet::Layout& layout, uint32_t sequence, const Command& item)
{
    std::vector<Command>& items = layout.sequences[sequence].items;
    items.insert(items.begin(), item);
    for (ntdlib::sar::Field& field : layout.fields)
    {
        const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&layout.pieces[field.to.piece]);
        const bool first_channel = field.at.offset == 2;
        if (field.to.item && piece && piece->sequence == sequence && !first_channel)
        {
            field.to.offset++;
        }
    }
}

void TestRoundTrip()
{
    const std::vector<uint8_t> bytes = TestBytes();
    const Rom rom = RomOf(bytes);
    const std::vector<quintet::DriverInfo> files = {InfoOf()};
    const quintet::Layout layout = quintet::Cut(rom, files);
    Check(layout.sequences.size() == 3, "each song and the sound effect is a sequence");
    Check(layout.fields.size() == 21, "a length and 6 offsets for each");
    Check(layout.rooms.size() == 2, "a room for each list");
    Check(layout.rooms[0].end == kSongs + kSongsSize && layout.rooms[1].end == kEffects + kEffectsSize,
          "the rooms end at the parts' ends");
    Check(quintet::Join(layout) == bytes, "the pieces join to the ROM");
}

// A song that grows moves the songs after it, up to its list's room.
void TestGrow()
{
    const std::vector<uint8_t> bytes = TestBytes();
    const Rom rom = RomOf(bytes);
    const std::vector<quintet::DriverInfo> files = {InfoOf()};
    quintet::Layout layout = quintet::Cut(rom, files);

    Command tempo;
    tempo.kind = Kind::kTempo;
    tempo.value.value = 90 * kOne;
    InsertFirst(layout, 0, tempo);
    const std::vector<uint8_t> grown = quintet::Join(layout);
    const Rom grown_rom = RomOf(grown);
    Check(grown.size() == bytes.size(), "the ROM keeps its size");
    Check(grown_rom.U16(kRomBase + kSongs) == kSongBytes + 2, "the first song's length grows");
    Check(grown_rom.U16(kRomBase + kSongs + 2) == 14 && grown_rom.U16(kRomBase + kSongs + 4) == 18,
          "its channels start after the tempo");
    Check(grown_rom.U8(kRomBase + kSongs + 14) == 0x7F, "the tempo comes first");
    Check(grown_rom.U16(kRomBase + kSongs + kSongBytes + 2) == kSongBytes, "the second song follows it");
    Check(grown_rom.U16(kRomBase + kSongs + 2 * kSongBytes + 2) == 0, "and the length of 0 after it");
    Check(grown_rom.U32(kRomBase + kEffects - 4) == kEffectsSize, "the next part keeps its place");

    quintet::DriverInfo moved = InfoOf();
    moved.song_addresses[1] += 2;
    const std::vector<quintet::DriverInfo> moved_files = {moved};
    Check(quintet::Join(quintet::Cut(grown_rom, moved_files)) == grown, "the grown ROM cuts and joins");

    // Five tempos more outgrow the room.
    for (int i = 0; i < 5; i++)
    {
        InsertFirst(layout, 0, tempo);
    }
    bool threw = false;
    try
    {
        quintet::Join(layout);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "a list that outgrows its room");
}

// A song whose length has its lowest bit set stays in the bytes.
void TestOddLength()
{
    std::vector<uint8_t> bytes = TestBytes();
    bytes[kSongs + kSongBytes] = kSongBytes + 1;
    const std::vector<quintet::DriverInfo> files = {InfoOf()};
    const quintet::Layout layout = quintet::Cut(RomOf(bytes), files);
    Check(layout.sequences.size() == 2, "the song with an odd length stays in the bytes");
    Check(quintet::Join(layout) == bytes, "the pieces join to the ROM");
}

} // namespace

int main()
{
    TestRoundTrip();
    TestGrow();
    TestOddLength();

    return xsflib::testing::Summary("Quintet's layout");
}
