// SPDX-License-Identifier: MIT

// The catalogues of Star Fox Adventures, Tales of Symphonia, Paper Mario: The Thousand-Year Door and the NDDEMO tech
// demo (see ntdlib/musyx/catalogue.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"

namespace ntdlib::musyx::detail
{
namespace
{

// The volumes of Star Fox Adventures' effects, by the effects' IDs (see ReadStarFox). The game starts its effects
// through the rows of audio/data/Sfx.bin, 32 bytes each. A row gives the effect's volume in its byte at 2. Six effect
// IDs follow from 0xA, in big-endian halves. The top four bits of the row's last byte give how many of them the row
// uses. The game picks one of those at random. An ID of 0 picks no effect. An effect plays at the volume that most of
// its rows with a volume above 0 give it. Of volumes that tie, the first one in the file wins. An effect whose rows all
// give 0 plays at 0.
std::map<uint16_t, uint8_t> StarFoxEffectVolumes(std::span<const uint8_t> rows)
{
    constexpr std::size_t kRow = 32, kVolume = 2, kIds = 0xa, kMostIds = 6, kCount = 0x1f;
    std::map<uint16_t, std::vector<uint8_t>> volumes;
    for (std::size_t at = 0; at + kRow <= rows.size(); at += kRow)
    {
        const uint8_t* row = rows.data() + at;
        const std::size_t count = std::min<std::size_t>(row[kCount] >> 4, kMostIds);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint16_t id = xsflib::LoadBe16(row + kIds + i * 2);
            if (id != 0)
            {
                volumes[id].push_back(row[kVolume]);
            }
        }
    }

    std::map<uint16_t, uint8_t> result;
    for (const auto& [id, all] : volumes)
    {
        std::vector<uint8_t> above_0;
        const auto audible = [](uint8_t volume)
        {
            return volume > 0;
        };
        std::copy_if(all.begin(), all.end(), std::back_inserter(above_0), audible);
        result[id] = MostCommon(above_0.empty() ? all : above_0);
    }

    return result;
}

// The volumes that Star Fox Adventures' triggers give its songs (see ReadStarFox): the triggers' volumes for each song,
// in the file's order, and the volume of the trigger that plays a song that its caller picks, if the file has one.
struct StarFoxTriggers
{
    std::map<uint16_t, std::vector<uint8_t>> volumes;
    std::optional<uint8_t> picked_volume;
};

// The triggers of audio/data/Music.bin, 16 bytes each. An entry gives its trigger's number and the song's ID in
// big-endian halves at its start, and the song's volume in its byte at 0xC. Trigger 0xEC plays a song that its caller
// picks.
StarFoxTriggers ReadStarFoxTriggers(std::span<const uint8_t> triggers)
{
    constexpr std::size_t kTrigger = 16;
    constexpr uint16_t kPickedSong = 0xec;

    StarFoxTriggers read;
    for (std::size_t at = 0; at + kTrigger <= triggers.size(); at += kTrigger)
    {
        const uint8_t* trigger = triggers.data() + at;
        if (xsflib::LoadBe16(trigger) == kPickedSong)
        {
            read.picked_volume = trigger[0xc];
        }
        else
        {
            read.volumes[xsflib::LoadBe16(trigger + 2)].push_back(trigger[0xc]);
        }
    }

    return read;
}

// The songs of audio/midi.wad, by their IDs: their sizes in 100 big-endian words at the file's start, then the songs
// from 0x1A0, each padded to 32 bytes. Throws FormatError if the table or a song runs past the file's end.
std::vector<std::span<const uint8_t>> WadSongs(std::span<const uint8_t> wad)
{
    constexpr uint32_t kWadSongs = 100;
    constexpr std::size_t kWadHeader = 0x1a0;

    const std::span<const uint8_t> sizes = Part(wad, 0, kWadHeader, "audio/midi.wad's table");
    std::vector<std::span<const uint8_t>> songs;
    std::size_t at = kWadHeader;
    for (uint32_t id = 0; id < kWadSongs; id++)
    {
        const uint32_t size = xsflib::LoadBe32(sizes.data() + id * 4);
        songs.push_back(Part(wad, at, size, "a song of audio/midi.wad"));
        at += (std::size_t{size} + 31) & ~std::size_t{31};
    }

    return songs;
}

// Adds Star Fox Adventures' songs to `catalogue`, in the order of their IDs (see ReadStarFox). Each song pushes
// `groups`.
void AddStarFoxSongs(Catalogue& catalogue, const disc::GameFiles& game, const CatalogueTables& tables,
                     const std::vector<PushedGroup>& groups)
{
    constexpr uint32_t kSongEntry = 16;
    constexpr uint32_t kFadeInMs = 500;

    const Dol dol = Dol::ParseMain(game.dol);
    const StarFoxTriggers triggers = ReadStarFoxTriggers(disc::ReadGameFile(game, "audio/data/Music.bin"));
    const std::vector<uint8_t> wad = disc::ReadGameFile(game, "audio/midi.wad");
    const std::vector<std::span<const uint8_t>> songs = WadSongs(wad);

    struct Song
    {
        uint16_t id = 0;
        uint8_t group = 0;
        std::string name;
    };
    std::vector<Song> table;
    for (uint32_t i = 0; i < tables.song_count; i++)
    {
        const std::span<const uint8_t> row = dol.Bytes(tables.songs + i * kSongEntry, kSongEntry, "the table of songs");
        table.push_back({.id = xsflib::LoadBe16(row.data()),
                         .group = row[2],
                         .name = dol.Text(xsflib::LoadBe32(row.data() + 4), kLongestText, "a song's name")});
    }

    const auto by_id = [](const Song& a, const Song& b)
    {
        return a.id < b.id;
    };
    std::stable_sort(table.begin(), table.end(), by_id);
    for (const Song& song : table)
    {
        if (song.id >= songs.size())
        {
            throw xsflib::FormatError("audio/midi.wad has no song " + xsflib::Hex(song.id, 4));
        }

        const auto found = triggers.volumes.find(song.id);
        Sound sound;
        sound.name = song.name.empty() ? xsflib::Hex(song.id, 4) : song.name;
        sound.volume = found == triggers.volumes.end() ? triggers.picked_volume : MostCommon(found->second);
        sound.id = song.id;
        sound.song_group = song.group;
        sound.groups = groups;
        sound.fade_in_ms = kFadeInMs;
        AddSong(catalogue, std::move(sound), {songs[song.id].begin(), songs[song.id].end()});
    }
}

// Adds Star Fox Adventures' effects to `catalogue`, in the order of their IDs (see ReadStarFox). Each effect pushes
// `groups`.
void AddStarFoxEffects(Catalogue& catalogue, const disc::GameFiles& game, const std::vector<PushedGroup>& groups,
                       std::span<const char* const> set_names)
{
    constexpr uint8_t kCentre = 0x40;

    struct Effect
    {
        uint16_t id = 0;
        std::size_t set = 0;
    };
    std::vector<Effect> effects;
    for (std::size_t set = 0; set < catalogue.sets.size(); set++)
    {
        for (const ProjectEffect& effect : EffectsOf(catalogue.sets[set].project))
        {
            const auto pushed = [&](const PushedGroup& g)
            {
                return g.id == effect.group && g.set == set;
            };
            if (std::any_of(groups.begin(), groups.end(), pushed))
            {
                effects.push_back({.id = effect.row.id, .set = set});
            }
        }
    }

    KeepFirstOfEachId(effects);
    const std::map<uint16_t, uint8_t> volumes = StarFoxEffectVolumes(disc::ReadGameFile(game, "audio/data/Sfx.bin"));
    for (const Effect& effect : effects)
    {
        Sound sound;
        sound.name = EffectName(set_names[effect.set], effect.id);
        sound.kind = SoundKind::kEffect;
        if (const auto found = volumes.find(effect.id); found != volumes.end())
        {
            sound.volume = found->second;
            sound.pan = kCentre;
        }

        sound.id = effect.id;
        sound.groups = groups;
        catalogue.sounds.push_back(std::move(sound));
    }
}

// MusyX's four files from one file of Tales of Symphonia. The file starts with the number of its parts, 4, then gives
// each part's offset and size, a big-endian word each. The parts are the project, the pool, the sample directory and
// the samples.
FileSet SoundFile(std::span<const uint8_t> file)
{
    constexpr uint32_t kParts = 4;
    if (file.size() < 4 + kParts * 8 || xsflib::LoadBe32(file.data()) != kParts)
    {
        throw xsflib::FormatError("a sound file doesn't hold four parts");
    }

    std::array<std::vector<uint8_t>, kParts> parts;
    for (uint32_t i = 0; i < kParts; i++)
    {
        const uint32_t offset = xsflib::LoadBe32(file.data() + 4 + i * 8);
        const uint32_t size = xsflib::LoadBe32(file.data() + 8 + i * 8);
        const std::span<const uint8_t> part = Part(file, offset, size, "a sound file's part");
        parts[i].assign(part.begin(), part.end());
    }

    return {.project = std::move(parts[0]),
            .pool = std::move(parts[1]),
            .sdir = std::move(parts[2]),
            .samples = std::move(parts[3])};
}

// A row of Paper Mario: The Thousand-Year Door's table of sounds (see ReadPaperMario): the sound's name, its kind and
// ID, its volume, its pan, its send to the reverb and the bend of its pitch.
struct PaperMarioRow
{
    std::string name;
    uint32_t sound = 0;
    uint8_t volume = 0, pan = 0, send = 0;
    int16_t bend = 0;
};

// The bits of a row's second word that give the sound's kind, and the kinds of Paper Mario's effects and songs.
constexpr uint32_t kPaperMarioKind = 0xf0000000, kPaperMarioEffect = 0x20000000, kPaperMarioSong = 0x40000000;

// The rows of Paper Mario's table of sounds that play sounds of `kind`: an effect's row by the effect's ID, a song's
// row by the song's record (see AddPaperMarioSongs). Several rows can play one sound. The sound takes the first row
// with a volume above 0, or the first row if none has one.
std::map<uint32_t, PaperMarioRow> PaperMarioRows(const Dol& dol, const CatalogueTables& tables, uint32_t kind)
{
    constexpr uint32_t kRow = 20, kId = 0xffff, kRecord = 0xff;
    std::map<uint32_t, PaperMarioRow> rows;
    for (uint32_t i = 0; i < tables.effect_count; i++)
    {
        const std::span<const uint8_t> row = dol.Bytes(tables.effects + i * kRow, kRow, "the table of sounds");
        const uint32_t sound = xsflib::LoadBe32(row.data() + 4);
        const uint32_t key = sound & (kind == kPaperMarioSong ? kRecord : kId);
        const auto found = rows.find(key);
        if ((sound & kPaperMarioKind) != kind || (found != rows.end() && (found->second.volume != 0 || row[0xc] == 0)))
        {
            continue;
        }

        std::string name;
        if (const uint32_t at = xsflib::LoadBe32(row.data()); at != 0)
        {
            name = dol.Text(at, kLongestText, "a sound's name");
        }

        rows[key] = {.name = std::move(name),
                     .sound = sound,
                     .volume = row[0xc],
                     .pan = row[0xd],
                     .send = row[0xf],
                     .bend = static_cast<int16_t>(xsflib::LoadBe16(row.data() + 0x12))};
    }

    return rows;
}

// Adds Paper Mario's effects to `catalogue`, in the order of their IDs (see ReadPaperMario).
void AddPaperMarioEffects(Catalogue& catalogue, const Dol& dol, const CatalogueTables& tables)
{
    constexpr uint8_t kStartVolume = 0x7f, kVolume = 7, kReverbSend = 91, kPitchWheel = 0x80;
    constexpr int32_t kNoBend = 0x2000;

    const std::map<uint32_t, PaperMarioRow> rows = PaperMarioRows(dol, tables, kPaperMarioEffect);
    std::vector<ProjectEffect> effects = EffectsOf(catalogue.sets[0].project);
    KeepFirstOfEachId(effects);
    for (const ProjectEffect& effect : effects)
    {
        Sound sound;
        sound.name = EffectName("pmario", effect.row.id);
        sound.kind = SoundKind::kEffect;
        sound.id = effect.row.id;
        sound.groups = {{.id = effect.group, .set = 0}};
        if (const auto found = rows.find(effect.row.id); found != rows.end())
        {
            const PaperMarioRow& row = found->second;
            if (!row.name.empty())
            {
                sound.name = row.name;
            }

            sound.volume = kStartVolume;
            sound.pan = row.pan;
            sound.controls = {{.controller = kVolume, .value = row.volume},
                              {.controller = kPitchWheel, .fine = true, .value = kNoBend + row.bend},
                              {.controller = kReverbSend, .value = row.send}};
        }

        catalogue.sounds.push_back(std::move(sound));
    }
}

// Adds Paper Mario's songs to `catalogue`, in the order of sound/proj/pmario.slib (see ReadPaperMario).
void AddPaperMarioSongs(Catalogue& catalogue, const disc::GameFiles& game, const Dol& dol,
                        const CatalogueTables& tables)
{
    constexpr uint32_t kEndOfSongs = 0xffffffff;
    constexpr std::size_t kHeader = 32, kName = 32, kNameGroup = 0x1e, kNameId = 0x1f;

    const std::map<uint32_t, PaperMarioRow> rows = PaperMarioRows(dol, tables, kPaperMarioSong);
    const std::vector<uint8_t> songs = disc::ReadGameFile(game, "sound/proj/pmario.slib");
    const std::vector<uint8_t> names = disc::ReadGameFile(game, "sound/proj/pmario.stbl");
    std::size_t at = 0;
    for (uint32_t record = 0;; record++)
    {
        const uint32_t size = xsflib::LoadBe32(Part(songs, at, 4, "the song library").data());
        if (size == kEndOfSongs)
        {
            break;
        }

        if (size < kHeader)
        {
            throw xsflib::FormatError("a record of the song library is shorter than its header");
        }

        const std::span<const uint8_t> song = Part(songs, at + kHeader, size - kHeader, "the song library");
        const std::span<const uint8_t> name = Part(names, record * kName, kName, "the table of song names");
        at += size;

        Sound sound;
        if (const auto found = rows.find(record); found != rows.end())
        {
            const PaperMarioRow& row = found->second;
            sound.name = row.name;
            sound.id = static_cast<uint16_t>(row.sound >> 8 & 0xff);
            sound.song_group = static_cast<uint16_t>(row.sound >> 16 & 0xff);
            sound.volume = row.volume;
        }
        else
        {
            sound.id = name[kNameId];
            sound.song_group = name[kNameGroup];
        }

        if (sound.name.empty())
        {
            sound.name = TextBeforeZero(name.first(kNameGroup));
        }

        sound.groups = {{.id = sound.song_group, .set = 0}};
        AddSong(catalogue, std::move(sound), {song.begin(), song.end()});
    }
}

// The names of a project's effects, by their IDs, from `header`, the header of C defines that MusyX's tools write for
// the project. A line "#define SFX<name> <ID>" names an effect, with the ID in decimal. Spaces or tabs separate the
// three. The define's name, "SFX" and all, is the effect's name. The defines of groups and songs start with other
// prefixes. Of two names for one ID, the first counts.
std::map<uint16_t, std::string> HeaderEffectNames(std::span<const uint8_t> header)
{
    constexpr std::string_view kDefine = "#define", kPrefix = "SFX", kBlanks = " \t";

    std::map<uint16_t, std::string> names;
    const std::string_view text(reinterpret_cast<const char*>(header.data()), header.size());
    std::size_t line = 0;
    while (line < text.size())
    {
        const std::size_t end = std::min(text.find('\n', line), text.size());
        std::string_view row = text.substr(line, end - line);
        line = end + 1;
        const std::size_t name_start = row.find_first_not_of(kBlanks, kDefine.size());
        if (!row.starts_with(kDefine) || name_start == kDefine.size() || name_start == std::string_view::npos)
        {
            continue;
        }

        // The define's name, then its value.
        row.remove_prefix(name_start);
        const std::size_t name_end = row.find_first_of(kBlanks);
        if (name_end == std::string_view::npos || !row.starts_with(kPrefix))
        {
            continue;
        }

        const std::string_view name = row.substr(0, name_end);
        const std::size_t value = row.find_first_not_of(kBlanks, name_end);
        uint32_t id = 0;
        std::size_t digits = 0;
        while (value != std::string_view::npos && value + digits < row.size() && id <= 0xffff &&
               row[value + digits] >= '0' && row[value + digits] <= '9')
        {
            id = id * 10 + static_cast<uint32_t>(row[value + digits] - '0');
            digits++;
        }

        if (digits != 0 && id <= 0xffff)
        {
            names.try_emplace(static_cast<uint16_t>(id), name);
        }
    }

    return names;
}

} // namespace

// Star Fox Adventures' sounds. The game's code lists its songs at `songs`, in `song_count` entries of 16 bytes. An
// entry gives MusyX's song ID in a big-endian half, then the song's group in a byte. The address of the song's name
// follows at +4. The game fills in the entry's last two words. audio/midi.wad holds the songs by ID (WadSongs). The
// game plays a song through sndSeqPlayEx (0x8027C140), with the song's group and ID. At start-up it pushes group 0 of
// the starfoxm files, then groups 1 to 55 of the starfoxs files, and keeps them all. The songs come in the order of
// their IDs. A song without a name is named after its ID, such as "0x0012".
//
// The game starts its music through triggers (ReadStarFoxTriggers). A song plays at the volume that most of its
// triggers give it. A song that no other trigger names takes the volume of the trigger that plays a song that its
// caller picks. The trigger's code (0x8000B440) starts the song at a volume of 0 (flag 4 of sndSeqPlayEx's parameters,
// with a time and a volume of 0). It then raises the volume to the trigger's over 500 ms (sndSeqVolume at 0x8000B4B4).
//
// The effects follow the songs, in the order of their IDs: every effect of the effects groups that the game pushes.
// The game starts an effect through 0x8000C4D8, which calls sndFXStartEx (0x8027300C) with the volume of a row of
// audio/data/Sfx.bin (StarFoxEffectVolumes), a pan of 0x40 and studio 0. An effect pushes the same groups as a song.
// An effect that no row plays takes the volume and the pan of its row in its group's table of effects. An effect whose
// rows all give a volume of 0 makes no sound. Nothing names the effects. An effect is named after its set and its ID,
// such as "starfoxs-0420".
Catalogue ReadStarFox(const disc::GameFiles& game, const CatalogueTables& tables)
{
    constexpr uint16_t kLastGroup = 55;
    constexpr std::array<const char*, 2> kSetNames = {"starfoxm", "starfoxs"};
    constexpr FileExtensions kExtensions = {.project = ".pro", .pool = ".poo", .sdir = ".sdi", .samples = ".sam"};

    Catalogue catalogue;
    for (const char* name : kSetNames)
    {
        catalogue.sets.push_back(ReadFileSet(game, std::string("audio/") + name, kExtensions));
    }

    std::vector<PushedGroup> groups = {{.id = 0, .set = 0}};
    for (uint16_t group = 1; group <= kLastGroup; group++)
    {
        groups.push_back({.id = group, .set = 1});
    }

    AddStarFoxSongs(catalogue, game, tables, groups);
    AddStarFoxEffects(catalogue, game, groups, kSetNames);

    return catalogue;
}

// Tales of Symphonia's sounds. Both discs hold the same main.dol. The game's code lists its songs at `songs`, 8 bytes
// each: the address of the file's name, then MusyX's song ID and the song's kind, a big-endian half each. A song ID of
// 0xFFFF ends the list. The game plays a song through sndSeqPlayEx, with the song ID and the group of S/inst.snd. The
// song plays at the music's volume in the game's options: 127 by default (0x80081D80), its most. The game's disc
// library matches paths in any case. The code names its files in lower case. The disc's folder is "S". The byte at
// `song_reverbs` plus the song ID picks aux A's reverb: 2 picks the game's other reverb.
//
// The effects follow the songs. Each of the other .snd files in S/ holds one group of effects: se.snd the game's
// common effects, se_ev00.snd to se_ev07.snd the effects of events, and v_01_llo.snd to v_70_bh18.snd the voices in
// battle, in the order of the files' names. The effects' IDs rise from file to file. The game's start-up (0x800708C8)
// pushes the music's group and then the common effects' group. The game keeps both. The common effects take some
// samples and tables from the music's group. The effects of events take samples, macros and tables from the common
// effects' group. An effect therefore pushes those two groups first, then its group. A file without a pool holds no
// macros. MusyX can't push its group. Its effects are left out. One of the voice files has no pool. The game starts an
// effect through sndFXStartEx with a volume of 127, a pan of 64 and studio 0. Every row of the groups' tables of
// effects gives the same volume and pan. Nothing names the effects. An effect is named after its file and its ID, such
// as "se-0012".
Catalogue ReadTales(const disc::GameFiles& game, const CatalogueTables& tables)
{
    constexpr uint8_t kMusicVolume = 0x7f, kEffectVolume = 127, kCentre = 64, kOtherReverb = 2;
    constexpr uint32_t kEntry = 8;
    constexpr uint16_t kEnd = 0xffff;
    constexpr std::string_view kFolder = "s/", kExtension = ".snd", kCommon = "s/se.snd", kBank = "s/inst.snd";

    const Dol dol = Dol::ParseMain(game.dol);
    const AnyCaseFiles files(game);
    Catalogue catalogue;
    catalogue.sets.push_back(SoundFile(files.Read(std::string(kBank))));
    const uint16_t group = FirstGroupId(catalogue.sets[0].project);
    for (uint32_t at = tables.songs;; at += kEntry)
    {
        const std::span<const uint8_t> row = dol.Bytes(at, kEntry, "the table of songs");
        const uint16_t id = xsflib::LoadBe16(row.data() + 4);
        if (id == kEnd)
        {
            break;
        }

        const std::string path = dol.Text(xsflib::LoadBe32(row.data()), kLongestText, "a song's file");
        Sound sound;
        sound.name = Stem(path);
        sound.volume = kMusicVolume;
        sound.id = id;
        sound.song_group = group;
        sound.groups = {{.id = group, .set = 0}};
        sound.other_reverb =
            tables.song_reverbs != 0 && dol.Bytes(tables.song_reverbs + id, 1, "the songs' reverbs")[0] == kOtherReverb;
        AddSong(catalogue, std::move(sound), files.Read(path));
    }

    const std::size_t common = catalogue.sets.size();
    catalogue.sets.push_back(SoundFile(files.Read(std::string(kCommon))));
    const std::vector<PushedGroup> start_groups = {{.id = group, .set = 0},
                                                   {.id = FirstGroupId(catalogue.sets[common].project), .set = common}};
    for (const auto& [lower, path] : files.FilesIn(kFolder, kExtension))
    {
        if (lower == kBank)
        {
            continue;
        }

        std::size_t set = common;
        if (lower != kCommon)
        {
            FileSet file = SoundFile(files.Read(path));
            if (file.pool.empty())
            {
                continue;
            }

            set = catalogue.sets.size();
            catalogue.sets.push_back(std::move(file));
        }

        const std::string name = path.substr(kFolder.size(), path.size() - kFolder.size() - kExtension.size());
        for (const ProjectEffect& effect : EffectsOf(catalogue.sets[set].project))
        {
            Sound sound;
            sound.name = EffectName(name, effect.row.id);
            sound.kind = SoundKind::kEffect;
            sound.volume = kEffectVolume;
            sound.pan = kCentre;
            sound.id = effect.row.id;
            sound.groups = start_groups;
            if (set != common)
            {
                sound.groups.push_back({.id = effect.group, .set = set});
            }

            catalogue.sounds.push_back(std::move(sound));
        }
    }

    return catalogue;
}

// Paper Mario: The Thousand-Year Door's sounds. sound/proj/pmario.proj, .pool, .sdir and .samp hold every MusyX group
// of the game. The sound set-up (0x800E239C) pushes groups 0 to 11 and keeps them all. The game streams its music. Its
// jingles and some of its effects are MusyX songs. The catalogue holds the effects of the project's effects groups, in
// the order of their IDs, then the songs (AddPaperMarioSongs). Each sound pushes its group alone.
//
// The game's code lists its sounds at `effects`, in `effect_count` rows of 20 bytes. A row starts with the address of
// the sound's name. The word after it gives the sound's kind in its top bits and its ID in its low 16 bits. 0x20000000
// marks an effect of MusyX, 0x40000000 a song and 0x80000000 a stream. A row of 0 is unused. The bytes at 0xC, 0xD and
// 0xF give an effect's volume, its pan and its send to the reverb. The big-endian half at 0x12 bends its pitch. The
// game starts the effect with sndFXStartEx, a volume of 0x7F and the row's pan (0x800E0E4C). It then sets MIDI
// controller 7 to the row's volume, the pitch wheel to 0x2000 plus the bend, and controller 91 to the send. The game
// adds its room's reverb to a send above 0. Most of the game's sounds play at a place in the world. Their volumes and
// pans then follow the listener. The catalogue gives an effect the settings of its row, through the controllers. An
// effect takes its row's name (PaperMarioRows). One that no row plays is named after the project and its ID, such as
// "pmario-0051", and plays at MusyX's defaults. An effect whose rows all give a volume of 0 makes no sound.
//
// The songs come in the order of sound/proj/pmario.slib. The game reads the file at start-up. The file holds the
// songs, one record after another. A record starts with its size in a big-endian word, its 32 bytes of header
// included. The song follows the header. A size of 0xFFFFFFFF ends the file. A song's row gives the song's group in
// bits 16 to 23 of its second word, MusyX's song ID in bits 8 to 15, and the song's record in bits 0 to 7. The game
// plays the song with sndSeqPlayEx (0x80287FE0), the group, the ID and the record's song (0x800E124C). It sets the
// song's volume to 0x7F and mutes tracks 32 to 63 through sndSeqMute. No song has a track there. The game then sets
// the song's volume to the row's volume. The row's pan and send play no part. A song takes its row's name.
// pmario.stbl names the records: 32 bytes for each, the song's name, ended by a byte 0, then the song's group at 0x1E
// and its ID at 0x1F. A song that no row plays takes its name, its group and its ID from there, and plays at MusyX's
// default volume.
Catalogue ReadPaperMario(const disc::GameFiles& game, const CatalogueTables& tables)
{
    const Dol dol = Dol::ParseMain(game.dol);
    constexpr FileExtensions kExtensions = {.project = ".proj", .pool = ".pool", .sdir = ".sdir", .samples = ".samp"};

    Catalogue catalogue;
    catalogue.sets.push_back(ReadFileSet(game, "sound/proj/pmario", kExtensions));
    AddPaperMarioEffects(catalogue, dol, tables);
    AddPaperMarioSongs(catalogue, game, dol, tables);

    return catalogue;
}

// The NDDEMO tech demo's sounds. The disc never went on sale. The demo's code holds the addresses of the songs' paths
// at `songs`: `song_count` words, in the order of the songs' IDs (ReadSongPaths). A song takes its file's name. One set
// of MusyX's files, audio/nddemo, holds the demo's two groups: group 0 for the songs and group 1 for the effects. The
// set-up (0x80026B00) pushes both, in that order, and keeps them. The song start (0x80027500) plays a song through
// sndSeqPlayEx (0x800505D8) with group 0 and the song's ID. It then sets the song's volume to 127 at once with
// sndSeqVolume (0x80047CEC).
//
// The effects follow the songs, in the order of the effects group's table. The demo starts each of its four effects
// through sndFXStartEx (0x80047DD4) with the effect's ID, the volume and the pan that the group's table gives it, and
// studio 0. The disc's header audio/nddemo.h names the effects (HeaderEffectNames). An effect that it leaves out is
// named after the project and its ID.
Catalogue ReadNdDemo(const disc::GameFiles& game, const CatalogueTables& tables)
{
    constexpr uint8_t kSongVolume = 0x7f;
    constexpr uint16_t kSongGroup = 0, kEffectsGroup = 1;
    constexpr FileExtensions kExtensions = {.project = ".prj", .pool = ".pol", .sdir = ".sdr", .samples = ".smp"};

    const Dol dol = Dol::ParseMain(game.dol);
    Catalogue catalogue;
    catalogue.sets.push_back(ReadFileSet(game, "audio/nddemo", kExtensions));
    const std::vector<PushedGroup> groups = {{.id = kSongGroup, .set = 0}, {.id = kEffectsGroup, .set = 0}};
    const std::vector<std::string> paths = ReadSongPaths(dol, tables.songs, tables.song_count);
    for (std::size_t id = 0; id < paths.size(); id++)
    {
        Sound sound;
        sound.name = Stem(paths[id]);
        sound.volume = kSongVolume;
        sound.id = static_cast<uint16_t>(id);
        sound.song_group = kSongGroup;
        sound.groups = groups;
        AddSong(catalogue, std::move(sound), disc::ReadGameFile(game, paths[id]));
    }

    const std::map<uint16_t, std::string> names = HeaderEffectNames(disc::ReadGameFile(game, "audio/nddemo.h"));
    for (const ProjectEffect& effect : EffectsOf(catalogue.sets[0].project))
    {
        Sound sound;
        const auto name = names.find(effect.row.id);
        sound.name = name != names.end() ? name->second : EffectName("nddemo", effect.row.id);
        sound.kind = SoundKind::kEffect;
        sound.id = effect.row.id;
        sound.groups = groups;
        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

} // namespace ntdlib::musyx::detail
