// SPDX-License-Identifier: MIT

// The catalogues of Pokémon Channel and Pokémon Colosseum (see ntdlib/musyx/catalogue.h).

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/error.h"
#include "ntdlib/disc/fsys.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"

namespace ntdlib::musyx::detail
{
namespace
{

// The names that Pokémon Channel's sound viewer gives the effects, by the effects' IDs (see ReadPokemonChannel). The
// viewer's script holds its names from `names_at` on. Each name starts and ends with a tab, and a byte 0 follows it.
// The first string that doesn't start with a tab ends the names. From `effects_at` on, the script's code gives the
// names' effects in a run of entries. An entry holds 0x31 0x00 and the name's number in a big-endian word, 0x02 and a
// word, then 0x19 and the value. The value is 0x00 and the effect's ID in a big-endian word, or one byte other than 0.
// Nothing tells which effect such a byte gives. The bytes 0x1F 0x35 follow every entry but the last. An effect that
// several names give takes the first name. Throws FormatError if a name doesn't end.
std::map<uint16_t, std::string> ViewerNames(std::span<const uint8_t> script, std::size_t names_at,
                                            std::size_t effects_at)
{
    constexpr uint8_t kTab = '\t';
    std::vector<std::string> names;
    for (std::size_t at = names_at; at < script.size() && script[at] == kTab;)
    {
        const std::span<const uint8_t> rest = script.subspan(at);
        const auto end = std::find(rest.begin(), rest.end(), uint8_t{0});
        if (end == rest.end())
        {
            throw xsflib::FormatError("a name of the sound viewer doesn't end");
        }

        const std::string text(rest.begin(), end);
        if (text.size() < 2 || text.back() != kTab)
        {
            break;
        }

        names.push_back(text.substr(1, text.size() - 2));
        at += text.size() + 1;
    }

    // An entry's parts: its number at +2, the 0x02 at +6, the 0x19 at +11 and its value from +12.
    constexpr std::size_t kNumber = 2, kJump = 6, kValue = 11, kShortEntry = 13, kLongEntry = 17;
    std::map<uint16_t, std::string> by_effect;
    std::size_t at = effects_at;
    while (at + kShortEntry <= script.size() && script[at] == 0x31 && script[at + 1] == 0 &&
           script[at + kJump] == 0x02 && script[at + kValue] == 0x19)
    {
        const uint32_t number = xsflib::LoadBe32(&script[at + kNumber]);
        std::size_t next = at + kShortEntry;
        if (script[at + kValue + 1] == 0)
        {
            if (at + kLongEntry > script.size())
            {
                break;
            }

            const uint32_t id = xsflib::LoadBe32(&script[at + kValue + 2]);
            if (number < names.size() && id <= 0xffff)
            {
                by_effect.try_emplace(static_cast<uint16_t>(id), names[number]);
            }

            next = at + kLongEntry;
        }

        if (next + 2 > script.size() || script[next] != 0x1f || script[next + 1] != 0x35)
        {
            break;
        }

        at = next + 2;
    }

    return by_effect;
}

// The files of the game's FSYS archive at `path` (disc::ReadFsys). Throws FormatError if the game has no such file or
// the archive is damaged.
std::vector<disc::FsysFile> ReadFsysFile(const disc::GameFiles& game, const std::string& path)
{
    try
    {
        return disc::ReadFsys(disc::ReadGameFile(game, path));
    }
    catch (const disc::Error& e)
    {
        throw xsflib::FormatError(e.what());
    }
}

// The file of an FSYS archive with the name `name`. Throws FormatError if the archive has none.
const disc::FsysFile& FsysFileNamed(const std::vector<disc::FsysFile>& files, const std::string& name,
                                    const char* archive)
{
    const auto named = [&name](const disc::FsysFile& f)
    {
        return f.name == name;
    };
    const auto file = std::find_if(files.begin(), files.end(), named);
    if (file == files.end())
    {
        throw xsflib::FormatError(std::string(archive) + " has no file " + name);
    }

    return *file;
}

// Pokémon Colosseum's sets of MusyX's files, by their names. The first two hold the music's groups, the others the
// effects' groups (see ReadColosseum).
constexpr std::array<const char*, 7> kColosseumSets = {
    "snd_music", "snd_music_atmos", "snd_se", "snd_se_battle", "snd_se_motion", "snd_se_nakigoe", "snd_se_nakigoe2"};

// Pokémon Colosseum's file sets, from the files of common.fsys `common`: the set of each group, by the group's ID, and
// the effects that MusyX starts, by their groups and IDs.
struct ColosseumSets
{
    std::map<uint16_t, std::size_t> set_of_group;
    std::set<std::pair<uint16_t, uint16_t>> playable;
};

// Adds Pokémon Colosseum's file sets to `catalogue`, in the order of kColosseumSets (see ReadColosseum). Each set's
// group is its project's first group.
ColosseumSets AddColosseumSets(Catalogue& catalogue, const disc::GameFiles& game,
                               const std::vector<disc::FsysFile>& common)
{
    ColosseumSets sets;
    for (const char* stem : kColosseumSets)
    {
        const std::string name = stem;
        FileSet set{.project = FsysFileNamed(common, name + "_proj", "common.fsys").data,
                    .pool = FsysFileNamed(common, name + "_pool", "common.fsys").data,
                    .sdir = FsysFileNamed(common, name + "_sdir", "common.fsys").data,
                    .samples = disc::ReadGameFile(game, "sound/" + name + ".samp")};
        for (const ProjectEffect& effect : EffectsOf(set.project))
        {
            sets.playable.insert({effect.group, effect.row.id});
        }

        sets.set_of_group[FirstGroupId(set.project)] = catalogue.sets.size();
        catalogue.sets.push_back(std::move(set));
    }

    return sets;
}

// The settings of aux A's reverb in preset `preset` of the presets at `presets` in `module` (see ReadColosseum).
ReverbSettings ColosseumReverb(std::span<const uint8_t> module, uint32_t presets, uint8_t preset)
{
    constexpr std::size_t kPreset = 24;

    const std::span<const uint8_t> settings = Part(module, presets + preset * kPreset, kPreset, "a reverb's preset");
    const auto setting = [&settings](std::size_t index)
    {
        return std::bit_cast<float>(xsflib::LoadBe32(settings.data() + index * 4));
    };

    return {.coloration = setting(0),
            .mix = setting(1),
            .time = setting(2),
            .damping = setting(3),
            .pre_delay = setting(4),
            .crosstalk = setting(5)};
}

// Adds the songs that Pokémon Colosseum's table `listed` lists to `catalogue`, in the table's order (see
// ReadColosseum).
void AddColosseumSongs(Catalogue& catalogue, const std::vector<TableSound>& listed, const ColosseumSets& sets,
                       const std::vector<disc::FsysFile>& songs, std::span<const uint8_t> module,
                       const CatalogueTables& tables)
{
    constexpr uint8_t kMusicVolume = 0x7f;

    for (const TableSound& listing : listed)
    {
        if (listing.kind != SoundKind::kSong)
        {
            continue;
        }

        const auto is_file = [&listing](const disc::FsysFile& f)
        {
            return f.id == listing.file;
        };
        const auto song = std::find_if(songs.begin(), songs.end(), is_file);
        const auto set = sets.set_of_group.find(listing.group);
        if (song == songs.end() || set == sets.set_of_group.end())
        {
            throw xsflib::FormatError("bgm_archive.fsys and common.fsys have no files for sound " +
                                      std::to_string(listing.sound));
        }

        Sound sound;
        sound.name = song->name;
        sound.volume = kMusicVolume;
        sound.id = listing.id;
        sound.song_group = listing.group;
        sound.groups = {{.id = listing.group, .set = set->second}};
        sound.reverb = ColosseumReverb(module, tables.reverb_presets, listing.reverb);
        AddSong(catalogue, std::move(sound), song->data);
    }
}

// Adds the effects that Pokémon Colosseum's table `listed` lists to `catalogue`, in the table's order (see
// ReadColosseum).
void AddColosseumEffects(Catalogue& catalogue, const std::vector<TableSound>& listed, const ColosseumSets& sets,
                         std::span<const uint8_t> module, const CatalogueTables& tables)
{
    constexpr uint8_t kEffectVolume = 0x7f, kEffectPan = 0x40;

    const ReverbSettings reverb = ColosseumReverb(module, tables.reverb_presets, 0);
    for (const TableSound& listing : listed)
    {
        if (listing.kind != SoundKind::kEffect)
        {
            continue;
        }

        const auto set = sets.set_of_group.find(listing.group);
        if (set == sets.set_of_group.end())
        {
            throw xsflib::FormatError("common.fsys has no group for sound " + std::to_string(listing.sound));
        }

        if (!sets.playable.contains({listing.group, listing.id}))
        {
            continue;
        }

        Sound sound;
        sound.kind = SoundKind::kEffect;
        sound.volume = kEffectVolume;
        sound.pan = kEffectPan;
        sound.id = listing.id;
        sound.name = EffectName(kColosseumSets.at(set->second), sound.id);
        sound.groups = {{.id = listing.group, .set = set->second}};
        sound.reverb = reverb;
        catalogue.sounds.push_back(std::move(sound));
    }
}

} // namespace

// Pokémon Channel's sounds. en/pch/pchsound.proj, .pool, .sdir and .samp hold every MusyX group of the game. The game
// plays its music as MusyX effects: the effect's layer starts two voices that play a stereo recording, panned hard left
// and right. The game's code lists its music at `songs`, in `song_count` rows of 8 bytes: the effect's ID, then the
// number of a sound whose group the game pushes first, a big-endian word each. The game's music function (0x80015398)
// pushes the group that the project gives the effect. It then starts the effect. The music comes first, in the order
// of the table. The project's other effects follow, in the order of their IDs. Each sound pushes its group alone.
//
// The game starts every sound through 0x80015570: sndFXStartEx (0x80119B38) with a volume of 0x7F and the default pan,
// then MIDI controller 7 with the sound's volume, and the pan through controllers 0x83 (the surround pan) and 10. The
// music plays at a volume of 127 and a pan of 0x40 until a scene changes them. A scene's effect takes the volume and
// the pan of an object of the scene. The catalogue gives every sound the music's volume and pan.
//
// The game's sound viewer, pch/scripts/sndviewer.pkc, names most of the sounds (ViewerNames). Its names start at
// `viewer_names` in the script, and its table of effects at `viewer_effects`. A sound that it doesn't name is named
// after the project and its ID, such as "pchsound-1064".
Catalogue ReadPokemonChannel(const disc::GameFiles& game, const CatalogueTables& tables)
{
    constexpr uint32_t kRow = 8;
    constexpr uint8_t kStartVolume = 0x7f, kVolume = 7, kSurroundPan = 0x83, kPan = 10;
    constexpr int32_t kFullVolume = 127, kCentre = 0x40;
    constexpr FileExtensions kExtensions = {.project = ".proj", .pool = ".pool", .sdir = ".sdir", .samples = ".samp"};

    Catalogue catalogue;
    catalogue.sets.push_back(ReadFileSet(game, "en/pch/pchsound", kExtensions));
    std::vector<ProjectEffect> effects = EffectsOf(catalogue.sets[0].project);
    KeepFirstOfEachId(effects);
    const std::map<uint16_t, std::string> names =
        ViewerNames(disc::ReadGameFile(game, "pch/scripts/sndviewer.pkc"), tables.viewer_names, tables.viewer_effects);
    const auto add = [&](const ProjectEffect& effect, bool music)
    {
        Sound sound;
        const auto name = names.find(effect.row.id);
        sound.name = name != names.end() ? name->second : EffectName("pchsound", effect.row.id);
        sound.kind = SoundKind::kEffect;
        sound.volume = kStartVolume;
        sound.id = effect.row.id;
        sound.groups = {{.id = effect.group, .set = 0}};
        sound.controls = {{.controller = kVolume, .value = kFullVolume},
                          {.controller = kSurroundPan, .value = kCentre},
                          {.controller = kPan, .value = kCentre}};
        sound.music = music;
        catalogue.sounds.push_back(std::move(sound));
    };

    const Dol dol = Dol::ParseMain(game.dol);
    std::vector<uint16_t> music;
    for (uint32_t i = 0; i < tables.song_count; i++)
    {
        const uint32_t id = xsflib::LoadBe32(dol.Bytes(tables.songs + i * kRow, kRow, "the table of music").data());
        const auto is_id = [id](const ProjectEffect& effect)
        {
            return effect.row.id == id;
        };
        const auto found = std::find_if(effects.begin(), effects.end(), is_id);
        if (found == effects.end())
        {
            throw xsflib::FormatError("the table of music names an effect that en/pch/pchsound.proj doesn't hold");
        }

        add(*found, true);
        music.push_back(found->row.id);
    }

    for (const ProjectEffect& effect : effects)
    {
        if (std::find(music.begin(), music.end(), effect.row.id) == music.end())
        {
            add(effect, false);
        }
    }

    return catalogue;
}

// Pokémon Colosseum's sounds. The module common_rel in common.fsys holds the game's table of `song_count` sounds at
// `songs`, and the table of its songs' `file_count` files at `file_table` (ReadSoundTable). bgm_archive.fsys holds the
// songs under the files' IDs and names them. The game plays a song through sndSeqPlayEx with the song's group and ID,
// at volume 0x7F. The songs come in the table's order.
//
// The presets at `reverb_presets` in common_rel give the settings of aux A's high-quality reverb, 24 bytes each: six
// floats in the order of ReverbSettings. A song's preset other than 0 sets them. With 0, the reverb keeps its
// settings. The game's set-up writes preset 0's. A song with preset 0 therefore takes preset 0's settings.
//
// The game's play function (0x80166AB8) starts an effect with sndFXStartEx at volume 0x7F, pan 0x40 and studio 0. It
// then sets the effect's MIDI volume to 0x7F with sndFXCtrl. MusyX starts an effect's controllers at that value. The
// catalogue sets no controller. An effect plays with preset 0's reverb. The effects follow the songs in the table's
// order. An effect is left out if its group's project limits its voices to 0 (ProjectEffects). MusyX never starts such
// an effect. Nothing names the effects. An effect is named after its set and its ID, such as "snd_se_battle-0201".
//
// The music has two groups: group 0 in the files of snd_music, and group 1 in those of snd_music_atmos. The effects
// have five, one in each of the sets snd_se, snd_se_battle, snd_se_motion, snd_se_nakigoe and snd_se_nakigoe2.
// common.fsys holds a group's project, pool and sample directory under the files' name with _proj, _pool and _sdir. The
// samples are in sound/, with .samp.
Catalogue ReadColosseum(const disc::GameFiles& game, const CatalogueTables& tables)
{
    const std::vector<disc::FsysFile> common = ReadFsysFile(game, "common.fsys");
    const std::vector<disc::FsysFile> songs = ReadFsysFile(game, "bgm_archive.fsys");
    const std::vector<uint8_t>& module = FsysFileNamed(common, "common_rel", "common.fsys").data;

    Catalogue catalogue;
    const ColosseumSets sets = AddColosseumSets(catalogue, game, common);
    const std::vector<TableSound> listed =
        ReadSoundTable(module, "common_rel", tables.songs, tables.song_count, tables.file_table, tables.file_count);
    AddColosseumSongs(catalogue, listed, sets, songs, module, tables);
    AddColosseumEffects(catalogue, listed, sets, module, tables);

    return catalogue;
}

} // namespace ntdlib::musyx::detail
