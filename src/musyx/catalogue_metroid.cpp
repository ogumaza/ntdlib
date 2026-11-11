// SPDX-License-Identifier: MIT

// The catalogue of Metroid Prime and Metroid Prime 2: Echoes, from Retro Studios' packs (see ntdlib/musyx/catalogue.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/compression.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/musyx/catalogue.h"

namespace ntdlib::musyx::detail
{
namespace
{

// A resource of a Retro Studios pack: its type, its ID, its name if the pack names it, and its data, decompressed.
struct PakResource
{
    std::string type;
    uint32_t id = 0;
    std::string name;
    std::vector<uint8_t> data;
};

// The resources of a pack whose type is in `types` (Metroid Prime's pack format, version 3.5). The header gives the
// version, then the named resources: each one's type, ID, and the length and bytes of its name. A table follows: the
// number of resources, then for each one whether it's compressed, its type, its ID, its size and its offset. A
// compressed resource holds its size once decompressed, then a zlib stream. Throws FormatError if the pack is damaged.
std::vector<PakResource> ReadPak(std::span<const uint8_t> pak, const std::vector<std::string>& types)
{
    constexpr uint16_t kMajor = 3, kMinor = 5;
    if (pak.size() < 12 || xsflib::LoadBe16(pak.data()) != kMajor || xsflib::LoadBe16(pak.data() + 2) != kMinor)
    {
        throw xsflib::FormatError("a pack isn't of version 3.5");
    }

    std::size_t at = 8;
    const auto word = [&pak, &at]()
    {
        const uint32_t value = xsflib::LoadBe32(Part(pak, at, 4, "a pack's table").data());
        at += 4;

        return value;
    };
    const auto four_cc = [&pak, &at]()
    {
        const std::span<const uint8_t> bytes = Part(pak, at, 4, "a pack's table");
        at += 4;

        return std::string(bytes.begin(), bytes.end());
    };

    std::map<std::pair<std::string, uint32_t>, std::string> names;
    const uint32_t named = word();
    for (uint32_t i = 0; i < named; i++)
    {
        const std::string type = four_cc();
        const uint32_t id = word();
        const uint32_t length = word();
        names[{type, id}] = TextBeforeZero(Part(pak, at, length, "a pack's name"));
        at += length;
    }

    std::vector<PakResource> resources;
    const uint32_t count = word();
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t compressed = word();
        PakResource resource;
        resource.type = four_cc();
        resource.id = word();
        const uint32_t size = word();
        const uint32_t offset = word();
        if (std::find(types.begin(), types.end(), resource.type) == types.end())
        {
            continue;
        }

        const std::span<const uint8_t> stored = Part(pak, offset, size, "a pack's resource");
        if (compressed != 0)
        {
            resource.data.resize(xsflib::LoadBe32(Part(stored, 0, 4, "a compressed resource").data()));
            try
            {
                resource.data.resize(disc::Inflate(stored.subspan(4), resource.data));
            }
            catch (const std::exception&)
            {
                throw xsflib::FormatError("a pack's compressed resource is damaged");
            }
        }
        else
        {
            resource.data.assign(stored.begin(), stored.end());
        }

        if (const auto name = names.find({resource.type, resource.id}); name != names.end())
        {
            resource.name = name->second;
        }

        resources.push_back(std::move(resource));
    }

    return resources;
}

// An audio group set (AGSC) as MusyX's four files, and the set's name.
struct AudioGroupSet
{
    std::string name;
    FileSet files;
};

// The text from `at` in `bytes` up to its zero byte. Moves `at` past the zero byte. Throws FormatError with `error` if
// the text doesn't end.
std::string TextAt(std::span<const uint8_t> bytes, std::size_t& at, const char* error)
{
    const std::size_t start = std::min(at, bytes.size());
    const std::span<const uint8_t> rest = bytes.subspan(start);
    const auto end = std::find(rest.begin(), rest.end(), uint8_t{0});
    if (end == rest.end())
    {
        throw xsflib::FormatError(error);
    }

    std::string text(rest.begin(), end);
    at = start + text.size() + 1;

    return text;
}

// An audio group set in either of its layouts. Metroid Prime's set starts with two strings: a folder and the set's
// name. The pool, the project, the samples and the sample directory follow. A word before each gives its size. Metroid
// Prime 2: Echoes' set starts with a word 1, the set's name, and its group's ID in a big-endian half. Four words give
// the sizes of the pool, the project, the sample directory and the samples. The four parts follow in that order. A
// folder's name never starts with a byte 0. A set that starts with the word 1 therefore has the second layout. Throws
// FormatError if the set is damaged.
AudioGroupSet ReadAudioGroupSet(std::span<const uint8_t> agsc)
{
    constexpr uint32_t kEchoesLayout = 1;
    constexpr char kNoName[] = "an audio group set has no name";

    AudioGroupSet set;
    std::size_t at = 0;
    if (xsflib::LoadBe32(Part(agsc, 0, 4, "an audio group set").data()) == kEchoesLayout)
    {
        at = 4;
        set.name = TextAt(agsc, at, kNoName);
        at += 2;
        const std::span<const uint8_t> sizes = Part(agsc, at, 16, "an audio group set");
        at += 16;
        const std::array<std::vector<uint8_t>*, 4> parts = {&set.files.pool, &set.files.project, &set.files.sdir,
                                                            &set.files.samples};
        for (std::size_t i = 0; i < parts.size(); i++)
        {
            const std::span<const uint8_t> bytes =
                Part(agsc, at, xsflib::LoadBe32(sizes.data() + i * 4), "an audio group set");
            parts[i]->assign(bytes.begin(), bytes.end());
            at += bytes.size();
        }

        return set;
    }

    TextAt(agsc, at, kNoName);
    set.name = TextAt(agsc, at, kNoName);
    const auto part = [&agsc, &at]()
    {
        const uint32_t size = xsflib::LoadBe32(Part(agsc, at, 4, "an audio group set").data());
        const std::span<const uint8_t> bytes = Part(agsc, at + 4, size, "an audio group set");
        at += 4 + size;

        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    };

    set.files.pool = part();
    set.files.project = part();
    set.files.samples = part();
    set.files.sdir = part();

    return set;
}

// The names in a string table (STLC): the number of strings in a big-endian word, then the strings. A byte 0 ends each
// string. Metroid Prime 2: Echoes' table audio_name_lookup names its effects by their IDs: string n names effect n.
// Throws FormatError if the table is damaged.
std::vector<std::string> StringTable(std::span<const uint8_t> table)
{
    const uint32_t count = xsflib::LoadBe32(Part(table, 0, 4, "a string table").data());
    std::vector<std::string> names;
    std::size_t at = 4;
    for (uint32_t i = 0; i < count; i++)
    {
        names.push_back(TextAt(table, at, "a string table's string doesn't end"));
    }

    return names;
}

// What the packs of the game hold: the audio group sets by ID, the effects' names and the songs.
struct Packs
{
    std::map<uint32_t, AudioGroupSet> sets;
    std::vector<std::string> names;
    std::vector<PakResource> songs;
};

// The audio group sets of every pack, the names in the string table audio_name_lookup, and the songs (CSNG) of
// MidiData.pak, in the order of the packs' paths. Of two sets with one ID, the first pack's counts.
Packs ReadPacks(const disc::GameFiles& game)
{
    Packs packs;
    for (const disc::GameFile& file : game.files)
    {
        const std::string lower = xsflib::AsciiLower(file.path);
        if (!lower.ends_with(".pak"))
        {
            continue;
        }

        for (PakResource& resource : ReadPak(disc::ReadGameFile(game, file.path), {"AGSC", "STLC", "CSNG"}))
        {
            if (resource.type == "STLC" && resource.name.starts_with("audio_name_lookup"))
            {
                packs.names = StringTable(resource.data);
            }
            else if (resource.type == "AGSC" && !packs.sets.contains(resource.id))
            {
                packs.sets.emplace(resource.id, ReadAudioGroupSet(resource.data));
            }
            else if (resource.type == "CSNG" && lower == "mididata.pak")
            {
                packs.songs.push_back(std::move(resource));
            }
        }
    }

    return packs;
}

} // namespace

// Metroid Prime's and Metroid Prime 2: Echoes' sounds. MidiData.pak holds the songs as CSNG resources: a word 2,
// MusyX's song ID (its MIDI set-up), the song's group, the audio group set that holds the group, the song's size and
// the song. The game plays a song with sndSeqPlayEx, the group and the ID. The audio group sets are AGSC resources of
// the packs. Metroid Prime 2 keeps Metroid Prime's four songs. None of its packs holds the set that they name. The
// songs can't play. The catalogue leaves them out. A set becomes a file set when a sound pushes it.
//
// The effects follow the songs, in the order of their IDs: every effect of each set's effects group. Of two sets that
// give one ID, the set with the lower ID counts. The game plays an effect with sndFXStartEx and the effect's ID once it
// has pushed the group, with a volume and a pan of 0xFF. The effect's row gives both. A pack's string table
// audio_name_lookup names the effects by their IDs (StringTable). Metroid Prime 2's AudioGrp.pak holds one. An effect
// without a name there is named after its set and its ID, such as "Scarab-0721".
Catalogue ReadRetro(const disc::GameFiles& game)
{
    const Packs packs = ReadPacks(game);
    Catalogue catalogue;
    std::map<uint32_t, std::size_t> set_numbers;
    const auto set_number = [&](uint32_t id)
    {
        const auto [number, added] = set_numbers.try_emplace(id, catalogue.sets.size());
        if (added)
        {
            catalogue.sets.push_back(packs.sets.at(id).files);
        }

        return number->second;
    };

    for (const PakResource& song : packs.songs)
    {
        constexpr std::size_t kHeader = 20;
        const std::span<const uint8_t> head = Part(song.data, 0, kHeader, "a song");
        const uint32_t set = xsflib::LoadBe32(head.data() + 12);
        if (!packs.sets.contains(set))
        {
            continue;
        }

        const std::span<const uint8_t> data = Part(song.data, kHeader, xsflib::LoadBe32(head.data() + 16), "a song");
        Sound sound;
        sound.name = song.name.empty() ? xsflib::Hex(song.id, 8) : song.name;
        sound.id = static_cast<uint16_t>(xsflib::LoadBe32(head.data() + 4));
        sound.song_group = static_cast<uint16_t>(xsflib::LoadBe32(head.data() + 8));
        sound.groups = {{.id = sound.song_group, .set = set_number(set)}};
        AddSong(catalogue, std::move(sound), std::vector<uint8_t>(data.begin(), data.end()));
    }

    // The effects of every set, by ID. The sets come in the order of their IDs.
    struct Effect
    {
        uint16_t id = 0;
        uint16_t group = 0;
        uint32_t set = 0;
    };
    std::vector<Effect> effects;
    for (const auto& [id, set] : packs.sets)
    {
        if (set.files.project.empty())
        {
            continue;
        }

        for (const ProjectEffect& effect : EffectsOf(set.files.project))
        {
            effects.push_back({.id = effect.row.id, .group = effect.group, .set = id});
        }
    }

    KeepFirstOfEachId(effects);
    for (const Effect& effect : effects)
    {
        Sound sound;
        if (effect.id < packs.names.size() && !packs.names[effect.id].empty())
        {
            sound.name = packs.names[effect.id];
        }
        else
        {
            sound.name = EffectName(packs.sets.at(effect.set).name, effect.id);
        }

        sound.kind = SoundKind::kEffect;
        sound.id = effect.id;
        sound.groups = {{.id = effect.group, .set = set_number(effect.set)}};
        catalogue.sounds.push_back(std::move(sound));
    }

    return catalogue;
}

} // namespace ntdlib::musyx::detail
