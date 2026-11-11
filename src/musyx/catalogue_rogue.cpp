// SPDX-License-Identifier: MIT

// The catalogues of Star Wars Rogue Squadron II: Rogue Leader and Star Wars Rogue Squadron III: Rebel Strike, from the
// banks in their packed audio/data (see ntdlib/musyx/catalogue.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"

namespace ntdlib::musyx::detail
{
namespace
{

// Converts a UTF-16 string in big-endian halves to UTF-8. The string ends at a half of 0. A lone surrogate becomes
// U+FFFD.
std::string Utf16Be(std::span<const uint8_t> bytes)
{
    std::string out;
    xsflib::AppendUtf16(out, bytes, {.big_endian = true, .stop_at_nul = true});

    return out;
}

// The bytes of the file `path` in the pack `folder`/data.dat. `folder`/data.dir lists the pack's files in lines of
// text: the file's path, padded with spaces to 64 characters, then its offset in the pack and its size in decimal, and
// two more numbers, all split by tabs. Rebel Strike's list pads the numbers with spaces on the left. Throws FormatError
// if either file can't be read, if the list doesn't give the file, or if the file runs past the pack's end.
std::vector<uint8_t> PackedFile(const disc::GameFiles& game, const std::string& folder, const std::string& path)
{
    constexpr std::size_t kNameWidth = 64;
    const std::string list = folder + "/data.dir";
    const std::vector<uint8_t> dir = disc::ReadGameFile(game, list);
    const std::string text(dir.begin(), dir.end());
    for (std::size_t at = 0; at < text.size();)
    {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.size() <= kNameWidth || line.substr(0, line.find_last_not_of(' ', kNameWidth - 1) + 1) != path)
        {
            continue;
        }

        std::array<std::size_t, 2> numbers{};
        std::size_t field = kNameWidth;
        for (std::size_t& number : numbers)
        {
            while (field < line.size() && line[field] == ' ')
            {
                field++;
            }

            std::size_t digits = 0;
            for (; field < line.size() && line[field] >= '0' && line[field] <= '9'; field++, digits++)
            {
                number = number * 10 + static_cast<std::size_t>(line[field] - '0');
            }

            if (digits == 0 || field >= line.size() || line[field] != '\t')
            {
                throw xsflib::FormatError(list + "'s line for " + path + " is damaged");
            }

            field++;
        }

        const std::vector<uint8_t> pack = disc::ReadGameFile(game, folder + "/data.dat");
        const std::span<const uint8_t> bytes = Part(pack, numbers[0], numbers[1], path.c_str());
        return {bytes.begin(), bytes.end()};
    }

    throw xsflib::FormatError(list + " doesn't list " + path);
}

// The big-endian word at `at` in audio/data. Throws FormatError if it runs past the end of audio/data.
uint32_t AudioWord(std::span<const uint8_t> audio, std::size_t at)
{
    return xsflib::LoadBe32(Part(audio, at, 4, "audio/data").data());
}

// A song's row in a bank's table of songs (see ReadRogueBanks), with the number of its bank.
struct RogueSong
{
    uint32_t offset = 0, size = 0;
    uint16_t group = 0, id = 0;
    std::size_t bank = 0;
};

// The banks of audio/data: each bank's MusyX files and groups, and the songs of every bank. Bank n is file set n.
struct RogueBanks
{
    std::vector<FileSet> files;
    std::vector<std::vector<PushedGroup>> groups;
    std::vector<RogueSong> songs;
};

// MusyX's four files of the bank whose entry is at `entry` in audio/data (see ReadRogueBanks). Throws FormatError if
// the entry or a file runs past the end of audio/data.
FileSet RogueBankFiles(std::span<const uint8_t> audio, uint32_t entry)
{
    constexpr std::size_t kBankEntry = 13 * 4;
    Part(audio, entry, kBankEntry, "a bank's entry");
    const auto file = [&](std::size_t n)
    {
        const std::span<const uint8_t> bytes =
            Part(audio, AudioWord(audio, entry + n * 8), AudioWord(audio, entry + n * 8 + 4), "a bank");
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };

    return {.project = file(0), .pool = file(1), .sdir = file(2), .samples = file(3)};
}

// The banks of audio/data, in the order of its table of banks, and their songs in the order of each bank's table.
// audio/data starts with the size of its index and the offset of its table of banks. The table gives the number of
// banks, then the offset of each bank's entry. An entry gives the offsets and sizes of MusyX's four files (the project,
// the pool, the sample directory and the samples), the offset and size of a block before the songs, the number of
// songs, the offset of their table and the offset of the bank's list of groups. A song's row in the table gives the
// song's offset and size, then its group and its ID in big-endian halves. The list of groups holds big-endian halves
// and ends with 0xFFFF. Every word is big-endian. Every offset counts from the start of audio/data. Throws FormatError
// if the table or a part of a bank runs past the end of audio/data.
RogueBanks ReadRogueBanks(std::span<const uint8_t> audio)
{
    constexpr std::size_t kBankTable = 4, kSongRow = 12;
    constexpr uint16_t kEndOfGroups = 0xffff;

    RogueBanks banks;
    const uint32_t table = AudioWord(audio, kBankTable);
    const uint32_t count = AudioWord(audio, table);
    for (uint32_t b = 0; b < count; b++)
    {
        const uint32_t entry = AudioWord(audio, table + 4 + b * 4);
        banks.files.push_back(RogueBankFiles(audio, entry));

        std::vector<PushedGroup> groups;
        for (uint32_t at = AudioWord(audio, entry + 48);; at += 2)
        {
            const uint16_t group = xsflib::LoadBe16(Part(audio, at, 2, "a bank's groups").data());
            if (group == kEndOfGroups)
            {
                break;
            }

            groups.push_back({.id = group, .set = b});
        }

        banks.groups.push_back(std::move(groups));
        const uint32_t songs = AudioWord(audio, entry + 40), song_table = AudioWord(audio, entry + 44);
        for (uint32_t s = 0; s < songs; s++)
        {
            const std::span<const uint8_t> row = Part(audio, song_table + s * kSongRow, kSongRow, "a bank's songs");
            const uint32_t key = xsflib::LoadBe32(row.data() + 8);
            banks.songs.push_back({.offset = xsflib::LoadBe32(row.data()),
                                   .size = xsflib::LoadBe32(row.data() + 4),
                                   .group = static_cast<uint16_t>(key >> 16),
                                   .id = static_cast<uint16_t>(key),
                                   .bank = b});
        }
    }

    return banks;
}

// Adds Rogue Leader's songs to `catalogue` (see ReadRogueLeader). `game_dol` is the game's DOL inside main.dol.
void AddRogueLeaderSongs(Catalogue& catalogue, const RogueBanks& banks, std::span<const uint8_t> audio, const Dol& dol,
                         const Dol& game_dol, const CatalogueTables& tables)
{
    constexpr uint32_t kHallEntry = 16;

    // The bank of the hall's first song holds the samples that the songs of the other banks play too.
    std::optional<std::size_t> base;
    const auto add = [&](const RogueSong& row, std::string name, std::optional<uint8_t> volume)
    {
        base = base.value_or(row.bank);
        const std::span<const uint8_t> song = Part(audio, row.offset, row.size, "a song");
        Sound sound;
        sound.name = std::move(name);
        sound.volume = volume;
        sound.id = row.id;
        sound.song_group = row.group;
        if (row.bank != *base)
        {
            sound.groups = banks.groups[*base];
        }

        sound.groups.insert(sound.groups.end(), banks.groups[row.bank].begin(), banks.groups[row.bank].end());
        AddSong(catalogue, std::move(sound), std::vector<uint8_t>(song.begin(), song.end()));
    };

    std::vector<bool> listed(banks.songs.size(), false);
    for (uint32_t i = 0; i < tables.song_count; i++)
    {
        const std::span<const uint8_t> hall = dol.Bytes(tables.songs + i * kHallEntry, kHallEntry, "the music hall");
        const uint16_t id = xsflib::LoadBe16(hall.data()), group = xsflib::LoadBe16(hall.data() + 2);
        const auto is_song = [&](const RogueSong& r)
        {
            return r.group == group && r.id == id;
        };
        const auto row = std::find_if(banks.songs.begin(), banks.songs.end(), is_song);
        if (row == banks.songs.end())
        {
            throw xsflib::FormatError("audio/data has no song " + xsflib::Hex(id, 4) + " in group " +
                                      xsflib::Hex(group, 4));
        }

        listed[static_cast<std::size_t>(row - banks.songs.begin())] = true;
        const uint32_t name = xsflib::LoadBe32(hall.data() + 4);
        add(*row, Utf16Be(game_dol.From(name, "a song's name")), hall[0xa]);
    }

    for (std::size_t r = 0; r < banks.songs.size(); r++)
    {
        if (listed[r])
        {
            continue;
        }

        const RogueSong& row = banks.songs[r];
        const uint32_t key = uint32_t{row.group} << 16 | row.id;
        std::optional<uint8_t> volume;
        for (const auto& [song, given] : tables.song_volumes)
        {
            if (song == key)
            {
                volume = given;
            }
        }

        add(row, "Song " + xsflib::Hex(row.group, 2, true) + "-" + xsflib::Hex(row.id, 2, true), volume);
    }
}

// Adds Rogue Leader's effects to `catalogue`, bank by bank (see ReadRogueLeader). `game_dol` holds the table of sounds.
void AddRogueLeaderEffects(Catalogue& catalogue, const RogueBanks& banks, const Dol& game_dol,
                           const CatalogueTables& tables)
{
    constexpr uint32_t kSoundRow = 12;
    constexpr uint16_t kNoEffect = 0xffff;
    constexpr uint8_t kCentre = 0x40;

    std::map<uint16_t, std::vector<uint8_t>> volumes;
    for (uint32_t i = 0; i < tables.effect_count; i++)
    {
        const std::span<const uint8_t> row =
            game_dol.Bytes(tables.effects + i * kSoundRow, kSoundRow, "the table of sounds");
        if (const uint16_t id = xsflib::LoadBe16(row.data()); id != kNoEffect)
        {
            volumes[id].push_back(row[3]);
        }
    }

    for (std::size_t b = 0; b < catalogue.sets.size(); b++)
    {
        for (const ProjectEffect& effect : EffectsOf(catalogue.sets[b].project))
        {
            Sound sound;
            sound.name = EffectName("RogueLeader", effect.row.id);
            sound.kind = SoundKind::kEffect;
            sound.id = effect.row.id;
            if (const auto found = volumes.find(effect.row.id); found != volumes.end())
            {
                sound.volume = MostCommon(found->second);
                sound.pan = kCentre;
            }

            if (b != 0)
            {
                sound.groups = banks.groups[0];
            }

            sound.groups.insert(sound.groups.end(), banks.groups[b].begin(), banks.groups[b].end());
            catalogue.sounds.push_back(std::move(sound));
        }
    }
}

} // namespace

// Star Wars Rogue Squadron II: Rogue Leader's sounds. main.dol is a loader. It holds the OS, MusyX and the game, a
// second DOL at `embedded_dol`. Rogue2/data.dat packs audio/data (PackedFile), which holds the sound in banks
// (ReadRogueBanks).
//
// The game's music hall lists its songs at `songs` in main.dol, in `song_count` entries of 16 bytes: the song's ID and
// group in big-endian halves, then the address of its name, in UTF-16 in the game's DOL. The byte at 0xA gives the
// volume at which the hall plays the song. The game loads a bank whole and pushes each of its groups. The songs of most
// banks play samples of the bank that holds the hall's first song. A song pushes that bank's groups, then the groups of
// its bank, each bank's in its order. The songs that the hall doesn't list follow, at the volume that `song_volumes`
// gives them, or else at MusyX's volume. Such a song is named after its group and its ID, such as "Song 0x6E-0x65".
//
// Banks 0 to 19 hold the game's effects, in the order of the banks and of each bank's project. Some effects of banks 1
// to 19 play samples of bank 0. Bank 0 shares no ID of a macro, a table or a sample with those banks. An effect pushes
// the groups of bank 0, then those of its bank, each bank's in its order. The game's table of sounds is at `effects` in
// the game's DOL, in `effect_count` rows of 12 bytes. A row gives an effect's ID in a big-endian half and the effect's
// volume in the byte at 3. An ID of 0xFFFF marks an empty row. The game plays a row through sndFXStartEx, with a pan
// from its caller and studio 0. An effect plays at the most common volume of its rows, in the centre. An effect that no
// row names takes the volume and the pan of its row in its group's table of effects. An effect is named after its ID,
// such as "RogueLeader-0048".
Catalogue ReadRogueLeader(const disc::GameFiles& game, const CatalogueTables& tables)
{
    const Dol dol = Dol::ParseMain(game.dol);
    const std::optional<Dol> game_dol = Dol::Parse(dol.At(tables.embedded_dol));
    if (!game_dol)
    {
        throw xsflib::FormatError("main.dol holds no second DOL at " + xsflib::Hex(tables.embedded_dol, 8));
    }

    const std::vector<uint8_t> audio = PackedFile(game, "Rogue2", "audio/data");
    RogueBanks banks = ReadRogueBanks(audio);
    Catalogue catalogue;
    catalogue.sets = std::move(banks.files);
    AddRogueLeaderSongs(catalogue, banks, audio, dol, *game_dol, tables);
    AddRogueLeaderEffects(catalogue, banks, *game_dol, tables);

    return catalogue;
}

// Star Wars Rogue Squadron III: Rebel Strike's effects. main.dol is a loader. The game's code and data, MusyX among
// them, are a second DOL that main.dol holds packed. Rogue3/data.dat packs the game's files as Rogue Leader's
// Rogue2/data.dat does (PackedFile). The banks in its audio/data have the same layout too (ReadRogueBanks). The game
// streams its music. Many banks hold copies of the same groups. Each effect ID belongs to one group. An effect plays
// from the first bank whose project lists it. It pushes the effect's group alone. Only the banks that the effects take
// become file sets. The game passes a volume and a pan of 0xFF to sndFXStartEx: an effect plays at the volume and the
// pan of its row in its group's table of effects. An effect is named after its ID, such as "RebelStrike-0048".
Catalogue ReadRebelStrike(const disc::GameFiles& game)
{
    const std::vector<uint8_t> audio = PackedFile(game, "Rogue3", "audio/data");
    RogueBanks banks = ReadRogueBanks(audio);

    struct Effect
    {
        uint16_t id = 0;
        uint16_t group = 0;
        std::size_t bank = 0;
    };
    std::vector<Effect> effects;
    for (std::size_t b = 0; b < banks.files.size(); b++)
    {
        for (const ProjectEffect& effect : EffectsOf(banks.files[b].project))
        {
            effects.push_back({.id = effect.row.id, .group = effect.group, .bank = b});
        }
    }

    KeepFirstOfEachId(effects);

    Catalogue catalogue;
    std::map<std::size_t, std::size_t> sets;
    for (const Effect& effect : effects)
    {
        const auto [set, added] = sets.try_emplace(effect.bank, catalogue.sets.size());
        if (added)
        {
            catalogue.sets.push_back(std::move(banks.files[effect.bank]));
        }

        Sound sound;
        sound.name = EffectName("RebelStrike", effect.id);
        sound.kind = SoundKind::kEffect;
        sound.id = effect.id;
        sound.groups = {{.id = effect.group, .set = set->second}};
        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

} // namespace ntdlib::musyx::detail
