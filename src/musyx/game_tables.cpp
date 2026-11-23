// SPDX-License-Identifier: MIT

// The tables of a MusyX game's songs and effects in its main.dol (see ntdlib/musyx/game_tables.h).

#include "ntdlib/musyx/game_tables.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"

namespace ntdlib::musyx
{
namespace
{

// The longest name that a table's entry gives through an address.
constexpr std::size_t kLongestName = 64;

// The words that end a Camelot song list's entry: the file number, the ID and the volume.
constexpr uint32_t kSongWords = 12;

// The word that ends a table of ranges, and the most ranges that a table contains.
constexpr uint32_t kEndOfRanges = 0xffffffff;
constexpr uint32_t kMostRanges = 256;

// The size of an entry of a range table, of a table of narrations and of a table of courts' banks.
constexpr uint32_t kCamelotEntry = 8;

// The size of an entry of a sound room's table, and the offset of the entry's English name.
constexpr uint32_t kSoundRoomEntrySize = 0x20;
constexpr uint32_t kEnglishName = 0xc;

// The bytes with which a sound room's names mark each space.
constexpr std::string_view kSoundRoomSpace = "\x94\xbc";

// The sizes of an entry of the table of songs that play as effects, and of the table of effects that a play function
// starts.
constexpr uint32_t kEffectSongEntrySize = 8;
constexpr uint32_t kPlayedEffectSize = 6;

// Checks that `dol` contains the `count` entries of `size` bytes of the table `what` at `address`.
void CheckTable(const Dol& dol, uint32_t address, uint32_t count, uint32_t size, std::string_view what)
{
    if (count != 0)
    {
        dol.Bytes(address, std::size_t{count} * size, what);
    }
}

} // namespace

std::string CamelotPath(uint32_t file_number)
{
    const uint32_t n = file_number >> 1;
    std::string path;
    path += static_cast<char>('A' + n / (26 * 26) % 26);
    path += '/';
    path += static_cast<char>('A' + n / 26 % 26);
    path += '/';
    path += static_cast<char>('A' + n % 26);

    return path;
}

std::vector<CamelotSong> ReadCamelotSongs(const Dol& dol, const CamelotSongList& list)
{
    constexpr char kList[] = "the song list";
    std::vector<CamelotSong> songs;
    for (uint32_t i = 0; i < list.count; i++)
    {
        const uint32_t entry = list.address + i * list.entry_size;
        const uint32_t words = entry + list.entry_size - kSongWords;
        CamelotSong song;
        song.name = list.name_inline ? dol.Text(entry, words - entry, kList)
                                     : dol.Text(dol.Word(entry, kList), kLongestName, "a song's name");
        song.file_number = dol.Word(words, kList);
        song.id = static_cast<uint16_t>(dol.Word(words + 4, kList));
        song.volume = static_cast<uint8_t>(dol.Word(words + 8, kList) >> 24);
        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<CamelotRange> ReadCamelotRanges(const Dol& dol, uint32_t address)
{
    constexpr char kRanges[] = "the table of effects' ranges";
    std::vector<CamelotRange> ranges;
    for (uint32_t r = 0; r < kMostRanges; r++)
    {
        const uint32_t first = dol.Word(address + r * kCamelotEntry, kRanges);
        if (first == kEndOfRanges)
        {
            break;
        }

        ranges.push_back({first, dol.Word(address + r * kCamelotEntry + 4, kRanges)});
    }

    return ranges;
}

std::vector<VoiceBank> ReadVoiceBanks(const Dol& dol, const VoiceBankTable& table)
{
    constexpr char kBanks[] = "the table of voice banks";
    CheckTable(dol, table.address, table.count, table.entry_size, kBanks);

    // A name's address takes a word.
    const uint32_t name_size = table.name_size != 0 ? table.name_size : 4;
    std::vector<VoiceBank> banks;
    for (uint32_t b = 0; b < table.count; b++)
    {
        const uint32_t entry = table.address + b * table.entry_size;
        VoiceBank bank;
        bank.name = table.name_size != 0 ? dol.Text(entry, table.name_size, kBanks)
                                         : dol.Text(dol.Word(entry, kBanks), kLongestName, "a voice bank's name");
        bank.first_file = dol.Word(entry + name_size, kBanks);
        bank.effect_count = dol.Word(entry + name_size + 4, kBanks);
        bank.first_effect = dol.Word(entry + name_size + 8, kBanks);
        bank.group_id = static_cast<uint16_t>(dol.Word(entry + name_size + 12, kBanks));
        banks.push_back(std::move(bank));
    }

    return banks;
}

std::vector<Narration> ReadNarrations(const Dol& dol, uint32_t address, uint32_t count)
{
    constexpr char kNarrations[] = "the table of narrations";
    CheckTable(dol, address, count, kCamelotEntry, kNarrations);

    std::vector<Narration> narrations;
    for (uint32_t n = 0; n < count; n++)
    {
        const uint32_t entry = address + n * kCamelotEntry;
        const uint32_t ids = dol.Word(entry + 4, kNarrations);
        narrations.push_back({.first_file = dol.Word(entry, kNarrations),
                              .group_id = static_cast<uint16_t>(ids >> 16),
                              .effect_id = static_cast<uint16_t>(ids)});
    }

    return narrations;
}

std::vector<CourtBank> ReadCourtBanks(const Dol& dol, uint32_t address, uint32_t count)
{
    constexpr char kCourts[] = "the table of courts";
    CheckTable(dol, address, count, kCamelotEntry, kCourts);

    std::vector<CourtBank> courts;
    for (uint32_t c = 0; c < count; c++)
    {
        const uint32_t entry = address + c * kCamelotEntry;
        courts.push_back(
            {.first_file = dol.Word(entry, kCourts), .group_id = static_cast<uint16_t>(dol.Word(entry + 4, kCourts))});
    }

    return courts;
}

std::vector<SoundRoomSong> ReadSoundRoom(const Dol& dol, uint32_t address, uint32_t count)
{
    constexpr char kRoom[] = "the sound room";
    std::vector<SoundRoomSong> songs;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t entry = address + i * kSoundRoomEntrySize;
        SoundRoomSong song;
        song.entry_word = dol.Word(entry, kRoom);
        song.number = static_cast<uint16_t>(song.entry_word);
        song.group_id = static_cast<uint16_t>(song.entry_word >> 24);
        song.volume = dol.Bytes(entry + 4, 1, kRoom)[0];
        song.name = dol.Text(dol.Word(entry + kEnglishName, kRoom), kLongestName, "a song's name");
        for (std::size_t at = song.name.find(kSoundRoomSpace); at != std::string::npos;
             at = song.name.find(kSoundRoomSpace, at + 1))
        {
            song.name.replace(at, kSoundRoomSpace.size(), " ");
        }

        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<SoundRoomSong> ReadEffectSongs(const Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<SoundRoomSong> songs;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::span<const uint8_t> entry = dol.Bytes(address + i * kEffectSongEntrySize, kEffectSongEntrySize,
                                                         "the table of songs that play as effects");
        SoundRoomSong song;
        song.number = xsflib::LoadBe16(entry.data() + 2);
        song.group_id = entry[0];
        song.volume = entry[4];
        song.name = EffectName("se", song.number);
        songs.push_back(std::move(song));
    }

    return songs;
}

std::vector<PlayedEffect> ReadPlayedEffects(const Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<PlayedEffect> effects;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::span<const uint8_t> entry =
            dol.Bytes(address + i * kPlayedEffectSize, kPlayedEffectSize, "the table of effects");
        effects.push_back(
            {.number = xsflib::LoadBe16(entry.data()), .volume = entry[2], .reverb = entry[4], .chorus = entry[5]});
    }

    return effects;
}

} // namespace ntdlib::musyx
