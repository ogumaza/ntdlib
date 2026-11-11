// SPDX-License-Identifier: MIT

// The catalogues of Resident Evil and Resident Evil Zero (see ntdlib/musyx/catalogue.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"

namespace ntdlib::musyx::detail
{
namespace
{

// A part of one of Resident Evil Zero's archives: its type and its bytes.
struct ArchivePart
{
    std::string type;
    std::span<const uint8_t> data;
};

// The parts of one of Resident Evil Zero's archives (.arc). The archive starts with a word 0, the number of its parts
// and the size of its header. Each part then has 0x20 bytes: its offset and size, its name in 8 bytes and its type in
// 16. Every word is big-endian. Throws FormatError if a part runs past the archive's end.
std::vector<ArchivePart> ArchiveParts(std::span<const uint8_t> archive)
{
    constexpr std::size_t kPart = 0x20, kType = 0x10, kTypeSize = 16;

    const std::span<const uint8_t> header = Part(archive, 0, 12, "an archive's header");
    const uint32_t count = xsflib::LoadBe32(header.data() + 4);
    const uint32_t first = xsflib::LoadBe32(header.data() + 8);
    std::vector<ArchivePart> parts;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::span<const uint8_t> row = Part(archive, first + std::size_t{i} * kPart, kPart, "an archive's parts");
        parts.push_back({.type = TextBeforeZero(row.subspan(kType, kTypeSize)),
                         .data = Part(archive, xsflib::LoadBe32(row.data()), xsflib::LoadBe32(row.data() + 4),
                                      "an archive's part")});
    }

    return parts;
}

// The first of `parts` of the type `type`, or null if none has the type.
const ArchivePart* PartOfType(const std::vector<ArchivePart>& parts, std::string_view type)
{
    const auto is_type = [type](const ArchivePart& p)
    {
        return p.type == type;
    };
    const auto found = std::find_if(parts.begin(), parts.end(), is_type);

    return found == parts.end() ? nullptr : &*found;
}

// The bytes of `data` as a vector.
std::vector<uint8_t> Bytes(std::span<const uint8_t> data)
{
    return {data.begin(), data.end()};
}

// The contents of one of Resident Evil Zero's compressed files (.alz). The file's bits come from each byte's lowest bit
// up. The header gives the mode in 8 bits and the size of the contents in 32. Mode 0 stores the contents after the
// header. In modes 1 and 2, a 1 bit starts a literal byte in the next 8 bits. A 0 bit starts a copy of earlier
// contents: its distance less 1, then its length. Mode 1 gives the distance in 10 bits. Mode 2 gives it as a class and
// a value. A class is the number of 0 bits before a 1 bit. Classes 0 to 3 give the value in 2, 4, 6 and 10 bits. The
// length is a class and a value in both modes. The game copies the bytes as memmove does (0x80007360). Throws
// FormatError if the file ends early, if a copy reaches outside the contents, or if the contents would be larger than
// the console's memory.
std::vector<uint8_t> Unalz(std::span<const uint8_t> file)
{
    constexpr uint32_t kMaxSize = 24 << 20;
    constexpr std::array<uint32_t, 4> kWidths = {2, 4, 6, 10};
    constexpr std::size_t kHeader = 5;

    std::size_t bit = 0;
    const auto read = [&](uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; i++, bit++)
        {
            if (bit / 8 >= file.size())
            {
                throw xsflib::FormatError("a compressed file ends early");
            }

            value |= uint32_t{(file[bit / 8] >> (bit % 8)) & 1u} << i;
        }

        return value;
    };
    const auto width = [&]
    {
        std::size_t zeros = 0;
        while (read(1) == 0)
        {
            if (++zeros >= kWidths.size())
            {
                throw xsflib::FormatError("a compressed file is damaged");
            }
        }

        return kWidths[zeros];
    };

    const uint32_t mode = read(8);
    const uint32_t size = read(32);
    if (size > kMaxSize)
    {
        throw xsflib::FormatError("a compressed file is too large");
    }

    if (mode == 0)
    {
        return Bytes(Part(file, kHeader, size, "a stored file"));
    }

    std::vector<uint8_t> contents(size);
    for (std::size_t at = 0; at < size;)
    {
        if (read(1) != 0)
        {
            contents[at++] = static_cast<uint8_t>(read(8));
            continue;
        }

        const uint32_t distance = read(mode == 1 ? 10 : width()) + 1;
        const uint32_t length = read(width());
        if (distance > at || length > size - at)
        {
            throw xsflib::FormatError("a compressed file copies from outside its contents");
        }

        std::memmove(contents.data() + at, contents.data() + at - distance, length);
        at += length;
    }

    return contents;
}

// A song of one of Resident Evil Zero's cutscenes: the files of its group, and the song.
struct CutsceneSong
{
    FileSet set;
    std::vector<uint8_t> song;
};

// The sound effects of one of Resident Evil Zero's cutscenes, which the game plays as songs: the cutscene's name, the
// ID of the effects' group, the group's files, and the songs in the order of their parts.
struct CutsceneEffects
{
    std::string name;
    uint16_t group = 0;
    FileSet set;
    std::vector<std::vector<uint8_t>> songs;
};

// The sounds of Resident Evil Zero's cutscenes: the songs of their music, by the ID of each song's group, and their
// sound effects, in the order of the cutscenes' paths in lower case.
struct Cutscenes
{
    std::map<uint16_t, CutsceneSong> songs;
    std::vector<CutsceneEffects> effects;
};

// The sounds of Resident Evil Zero's cutscenes (see ReadResidentEvilZero). The game loads a cutscene's compressed
// archive, bio0/demo/<name>.alz (0x80169280). Part 7 of the archive holds the music: an archive of MusyX's pool,
// project and sample directory, then the song. Part 1 of bio0/demo/<name>sam.arc holds the music's samples
// (0x8016A3C0). The ID of the project's first group is the group in the song's row of the game's list. The music's
// files come from the first cutscene that holds the group, in the order of the paths in lower case. Part 6 of the
// archive holds the cutscene's sound effects, if it has any: the pool, the project and the sample directory of their
// group, then an archive of songs. Part 0 of the archive of samples holds their samples. Throws FormatError if a
// cutscene's archives can't be read, are damaged or lack a part of the music or of the effects.
Cutscenes ReadCutscenes(const AnyCaseFiles& files)
{
    constexpr std::string_view kCutscenes = "bio0/demo/", kCompressed = ".alz";
    // A cutscene's parts: its effects and its music in its archive, and their samples in its archive of samples.
    constexpr std::size_t kEffectsPart = 6, kMusicPart = 7, kEffectSamples = 0, kMusicSamples = 1;

    // A part of the archive that holds MusyX's pool, project and sample directory, and then the songs.
    const auto group_files = [](const ArchivePart& part, const std::string& path)
    {
        std::vector<ArchivePart> group = ArchiveParts(part.data);
        if (group.size() < 4)
        {
            throw xsflib::FormatError(path + " lacks a part of its sounds");
        }

        return group;
    };

    Cutscenes cutscenes;
    for (const auto& [lower, path] : files.FilesIn(kCutscenes, kCompressed))
    {
        const std::string stem = lower.substr(0, lower.size() - kCompressed.size());
        const std::string* samples = files.Find(stem + "sam.arc");
        if (samples == nullptr)
        {
            continue;
        }

        const std::vector<uint8_t> archive = Unalz(files.Read(path));
        const std::vector<ArchivePart> parts = ArchiveParts(archive);
        const auto holds = [&parts](std::size_t part)
        {
            return parts.size() > part && !parts[part].data.empty();
        };
        if (!holds(kEffectsPart) && !holds(kMusicPart))
        {
            continue;
        }

        const std::vector<uint8_t> sample_archive = files.Read(*samples);
        const std::vector<ArchivePart> sample_parts = ArchiveParts(sample_archive);
        if (sample_parts.size() <= kMusicSamples)
        {
            throw xsflib::FormatError(*samples + " lacks a part of its samples");
        }

        if (holds(kMusicPart))
        {
            const std::vector<ArchivePart> music = group_files(parts[kMusicPart], path);
            cutscenes.songs.try_emplace(FirstGroupId(music[1].data),
                                        CutsceneSong{.set = {.project = Bytes(music[1].data),
                                                             .pool = Bytes(music[0].data),
                                                             .sdir = Bytes(music[2].data),
                                                             .samples = Bytes(sample_parts[kMusicSamples].data)},
                                                     .song = Bytes(music[3].data)});
        }

        if (holds(kEffectsPart))
        {
            const std::vector<ArchivePart> effects = group_files(parts[kEffectsPart], path);
            CutsceneEffects sounds{
                .name = path.substr(kCutscenes.size(), path.size() - kCutscenes.size() - kCompressed.size()),
                .group = FirstGroupId(effects[1].data),
                .set = {.project = Bytes(effects[1].data),
                        .pool = Bytes(effects[0].data),
                        .sdir = Bytes(effects[2].data),
                        .samples = Bytes(sample_parts[kEffectSamples].data)}};
            for (const ArchivePart& song : ArchiveParts(effects[3].data))
            {
                sounds.songs.push_back(Bytes(song.data));
            }

            cutscenes.effects.push_back(std::move(sounds));
        }
    }

    return cutscenes;
}

// The folder of Resident Evil Zero's archives, and the archives' extension.
constexpr std::string_view kSounds = "bio0/sound/", kArchive = ".arc";

// The file sets of the archives in bio0/sound/ of Resident Evil Zero, by the archives' names. The first use of an
// archive adds the archive's set to the catalogue. The archive's parts of the types "proj", "pool" and "sdir" are
// MusyX's project, pool and sample directory. The archive's samples are in a file of the same name with the extension
// .sam.
class ArchiveSets
{
public:
    ArchiveSets(Catalogue& catalogue, const AnyCaseFiles& files) : catalogue_(catalogue), files_(files)
    {
    }

    // The set of the archive `archive`. Throws FormatError if the archive or its samples can't be read, or if the
    // archive is damaged or has no part of a type.
    std::size_t Of(const std::string& archive)
    {
        if (const auto found = sets_.find(archive); found != sets_.end())
        {
            return found->second;
        }

        const std::vector<uint8_t> file = files_.Read(std::string(kSounds) + archive + std::string(kArchive));
        return Of(archive, ArchiveParts(file));
    }

    // The set of the archive `archive`, whose parts are `parts`.
    std::size_t Of(const std::string& archive, const std::vector<ArchivePart>& parts)
    {
        const auto [set, added] = sets_.try_emplace(archive, catalogue_.sets.size());
        if (!added)
        {
            return set->second;
        }

        const std::string stem = std::string(kSounds) + archive;
        const auto part = [&](const char* type)
        {
            const ArchivePart* found = PartOfType(parts, type);
            if (found == nullptr)
            {
                throw xsflib::FormatError(stem + ".arc has no part of the type " + type);
            }

            return Bytes(found->data);
        };
        catalogue_.sets.push_back({.project = part("proj"),
                                   .pool = part("pool"),
                                   .sdir = part("sdir"),
                                   .samples = files_.Read(stem + ".sam")});

        return set->second;
    }

private:
    Catalogue& catalogue_;
    const AnyCaseFiles& files_;
    std::map<std::string, std::size_t> sets_;
};

// The volume that Resident Evil Zero's sound options give a song at start-up, and the time over which a cutscene
// raises its song to that volume, in ms.
constexpr uint8_t kOptionVolume = 0x7f;
constexpr uint32_t kCutsceneFadeInMs = 133;

// Adds Resident Evil Zero's songs to `catalogue`, in the order of the game's list (see ReadResidentEvilZero).
void AddResidentEvilZeroSongs(Catalogue& catalogue, ArchiveSets& sets, const AnyCaseFiles& files, const Dol& dol,
                              const CatalogueTables& tables, const std::map<uint16_t, CutsceneSong>& cutscene_songs)
{
    // The song that the extras play, and its volume there.
    constexpr uint16_t kExtrasSong = 0x90;
    constexpr uint8_t kExtrasVolume = 100;
    constexpr uint32_t kEntry = 12;
    constexpr uint16_t kEnd = 0xffff;

    std::map<uint16_t, std::size_t> cutscene_sets; // the file set of each cutscene's song, by the song's group
    for (uint32_t at = tables.songs;; at += kEntry)
    {
        const std::span<const uint8_t> row = dol.Bytes(at, kEntry, "the table of songs");
        const uint16_t id = xsflib::LoadBe16(row.data());
        if (id == kEnd)
        {
            break;
        }

        Sound sound;
        sound.name = dol.Text(xsflib::LoadBe32(row.data() + 8), kLongestText, "a song's name");
        sound.volume = id == kExtrasSong ? kExtrasVolume : kOptionVolume;
        sound.id = id;
        sound.song_group = xsflib::LoadBe16(row.data() + 2);
        const std::string song_path = std::string(kSounds) + sound.name + ".son";
        std::vector<uint8_t> song;
        if (files.Find(song_path) != nullptr)
        {
            const std::string archive = dol.Text(xsflib::LoadBe32(row.data() + 4), kLongestText, "an archive's name");
            sound.groups = {{.id = sound.song_group, .set = sets.Of(archive)}};
            song = files.Read(song_path);
        }
        else if (const auto cutscene = cutscene_songs.find(sound.song_group); cutscene != cutscene_songs.end())
        {
            // A song that bio0/sound/ lacks is a cutscene's.
            const auto [set, added] = cutscene_sets.try_emplace(sound.song_group, catalogue.sets.size());
            if (added)
            {
                catalogue.sets.push_back(cutscene->second.set);
            }

            sound.groups = {{.id = sound.song_group, .set = set->second}};
            sound.fade_in_ms = kCutsceneFadeInMs;
            song = cutscene->second.song;
        }
        else
        {
            continue;
        }

        AddSong(catalogue, std::move(sound), std::move(song));
    }
}

// Adds the effects of Resident Evil Zero's archives to `catalogue`, in the order of the archives' names (see
// ReadResidentEvilZero). Reads each archive once.
void AddArchiveEffects(Catalogue& catalogue, ArchiveSets& sets, const AnyCaseFiles& files)
{
    const std::size_t common = sets.Of("all");
    const PushedGroup common_group = {.id = FirstGroupId(catalogue.sets[common].project), .set = common};
    for (const auto& [lower, path] : files.FilesIn(kSounds, kArchive))
    {
        const std::vector<uint8_t> file = files.Read(path);
        const std::vector<ArchivePart> parts = ArchiveParts(file);
        const ArchivePart* project = PartOfType(parts, "proj");
        if (project == nullptr)
        {
            continue;
        }

        const std::vector<ProjectEffect> effects = EffectsOf(project->data);
        if (effects.empty())
        {
            continue;
        }

        const std::string archive = path.substr(kSounds.size(), path.size() - kSounds.size() - kArchive.size());
        const std::size_t set = sets.Of(archive, parts);
        for (const ProjectEffect& effect : effects)
        {
            if (effect.row.volume == 0)
            {
                continue;
            }

            Sound sound;
            sound.name = EffectName(archive, effect.row.id);
            sound.kind = SoundKind::kEffect;
            sound.id = effect.row.id;
            sound.groups = {common_group};
            if (set != common)
            {
                sound.groups.push_back({.id = effect.group, .set = set});
            }

            catalogue.sounds.push_back(std::move(sound));
        }
    }
}

// Adds the effects of Resident Evil Zero's cutscenes that the game plays as songs to `catalogue` (see
// ReadResidentEvilZero).
void AddCutsceneEffects(Catalogue& catalogue, const Dol& dol, const CatalogueTables& tables,
                        const std::vector<CutsceneEffects>& cutscene_effects)
{
    constexpr uint32_t kCutsceneEntry = 4;

    std::map<uint16_t, uint16_t> first_songs; // the song ID of the first song of each group of cutscene effects
    for (uint32_t i = 0; i < tables.effect_count; i++)
    {
        const std::span<const uint8_t> row =
            dol.Bytes(tables.effects + i * kCutsceneEntry, kCutsceneEntry, "the table of the cutscenes' effects");
        const uint16_t group = xsflib::LoadBe16(row.data());
        if (group != 0)
        {
            first_songs.try_emplace(group, xsflib::LoadBe16(row.data() + 2));
        }
    }

    for (const CutsceneEffects& effects : cutscene_effects)
    {
        const auto first = first_songs.find(effects.group);
        if (first == first_songs.end())
        {
            continue;
        }

        const std::size_t set = catalogue.sets.size();
        catalogue.sets.push_back(effects.set);
        for (std::size_t n = 0; n < effects.songs.size(); n++)
        {
            Sound sound;
            sound.id = static_cast<uint16_t>(first->second + n);
            sound.name = EffectName(effects.name, sound.id);
            sound.volume = kOptionVolume;
            sound.song_group = effects.group;
            sound.groups = {{.id = effects.group, .set = set}};
            sound.fade_in_ms = kCutsceneFadeInMs;
            sound.sound_effect = true;
            AddSong(catalogue, std::move(sound), effects.songs[n]);
        }
    }
}

// A level that a room's code gives a channel of one of Resident Evil's songs: the MIDI channel, its volume (CC7) and
// its pan (CC10). A pan of -1 leaves the channel's pan as the song sets it.
struct ResidentEvilLevel
{
    uint8_t channel = 0;
    uint8_t volume = 0;
    int pan = -1;
};

// A mix of a song's channels that a room's code sets: the mix's name, MusyX's song ID and the levels.
struct ResidentEvilMix
{
    const char* name;
    uint16_t song;
    std::vector<ResidentEvilLevel> levels;
};

// The mixes of the ambience songs that Resident Evil's rooms set (from a reading of the game's code). A room's code
// calls 0x80061E6C with a song, a mask of channels, a volume and a pan. That function finds the slot that plays the
// song and sends the driver's command 55 for each channel in the mask. Command 55 (0x80064524) calls sndSeqCntrl
// (0x80155BB8) on the slot's sequence: CC7 for the volume and CC10 for the pan. A volume or a pan of -1 leaves that
// controller alone.
//
// sg74's first pattern gives channels 0 to 2 a volume of 80. 0x800AFB68 sets all three to 80: the mix of the file,
// which the sound sg74 plays. The other rooms that give constant levels play one of the three alone at 80 and the
// other two at 0: channel 0 in 0x800DB360, channel 1 in 0x800CC938 and channel 2 in 0x800DAFE8. sg7a's first pattern
// gives its seven channels a volume of 0. 0x801002A0 gives channels 0 to 3 and 5 a volume of 80 and channels 4 and 6 a
// volume of 0, each at a pan of 64 (0x801002F4 to 0x80100330). 0x800FBF0C plays channel 5 alone at 80 (0x800FC1B8
// and 0x800FC1CC). 0x8010D640 plays channel 4 alone at 80 (0x8010D7A8 to 0x8010D820). Both set every pan to 64. Other
// rooms give channel 5 of sg7a other volumes and pans.
const std::vector<ResidentEvilMix>& ResidentEvilMixes()
{
    static const std::vector<ResidentEvilMix> kMixes = {
        {"sg74 ch0", 0x74, {{0, 80}, {1, 0}, {2, 0}}},
        {"sg74 ch1", 0x74, {{0, 0}, {1, 80}, {2, 0}}},
        {"sg74 ch2", 0x74, {{0, 0}, {1, 0}, {2, 80}}},
        {"sg7a ch0-3+5",
         0x7a,
         {{0, 80, 64}, {1, 80, 64}, {2, 80, 64}, {3, 80, 64}, {4, 0, 64}, {5, 80, 64}, {6, 0, 64}}},
        {"sg7a ch4", 0x7a, {{0, 0, 64}, {1, 0, 64}, {2, 0, 64}, {3, 0, 64}, {4, 80, 64}, {5, 0, 64}, {6, 0, 64}}},
        {"sg7a ch5", 0x7a, {{0, 0, 64}, {1, 0, 64}, {2, 0, 64}, {3, 0, 64}, {4, 0, 64}, {5, 80, 64}, {6, 0, 64}}},
    };

    return kMixes;
}

// Adds the mixes of the song of the last sound of `catalogue` to `catalogue` (ResidentEvilMixes). Each mix plays the
// song with the mix's controllers.
void AddResidentEvilMixes(Catalogue& catalogue)
{
    constexpr uint8_t kVolume = 7, kPan = 10;

    const Sound song = catalogue.sounds.back();
    for (const ResidentEvilMix& mix : ResidentEvilMixes())
    {
        if (mix.song != song.id)
        {
            continue;
        }

        Sound mixed = song;
        mixed.name = mix.name;
        for (const ResidentEvilLevel& level : mix.levels)
        {
            mixed.controls.push_back({.controller = kVolume, .value = level.volume, .channel = level.channel});
            if (level.pan >= 0)
            {
                mixed.controls.push_back({.controller = kPan, .value = level.pan, .channel = level.channel});
            }
        }

        catalogue.sounds.push_back(std::move(mixed));
    }
}

} // namespace

// Resident Evil Zero's sounds. Both discs hold the same main.dol. The game's code lists its songs at `songs`, 12 bytes
// each: MusyX's song ID and the song's group in big-endian halves, then the addresses of two names. The first names
// the archive that holds the group, and the second the song's file. A song ID of 0xFFFF ends the list. The game reads
// the files from bio0/sound/: the archive (.arc), its samples (.sam) and the song (.son). The archive's parts of the
// types "proj", "pool" and "sdir" are MusyX's project, pool and sample directory. The game's disc library matches names
// in any case. The list names the songs of both discs, and songs that neither disc holds. Some of the songs that
// bio0/sound/ lacks are the music of a cutscene (ReadCutscenes). The songs that the disc holds come in the list's
// order.
//
// The game pushes a song's group and plays the song through sndSeqPlayEx. A room or a cutscene starts a song at the
// volume of one of the game's sound options. Each option starts at 127, MusyX's full volume (0x80148F8C). The game's
// two functions that start music (0x8014A9C0 and 0x8014AAB8) start a song at a volume of 0. Each then calls
// sndSeqVolume (0x801CF0DC) with the option's volume and its caller's time. The rooms give a time of 0 (0x800183BC and
// 0x80018478). The cutscenes give 133 ms (0x8016C154 and 0x8016C254). MusyX then raises the music to the option's
// volume over that time. No room plays bgm_52. The extras play it at 100/127 of the music's option, at once
// (0x8017B970).
//
// The effects follow the songs. Each archive of bio0/sound/ whose project lists effects holds one group of them, in
// the order of the archives' names. The game's start-up pushes the group of all.arc and those of ten archives of
// weapons, and keeps them. The effects of each archive start only macros and samples of that archive. An effect pushes
// the group of all.arc, then its group. It plays at the volume and the pan of its row in its group's table of effects.
// A row of volume 0 makes no sound at that volume. Such rows look like the tables' unused rows. Their effects are left
// out. An effect is named after its archive and its ID, such as "sh00-1467".
//
// Some cutscenes play sound effects as songs. 0x8016C06C plays song N of a cutscene's effects (ReadCutscenes) through
// 0x8014AAB8, with the 133 ms fade-in of the cutscenes' music. The game's table of the cutscenes' effects, at
// `effects`, gives each of `effect_count` cutscenes 4 bytes: the ID of the effects' group and the song ID of their
// first song, in big-endian halves. Song N takes the ID that follows the first by N. A group of 0 means that the
// cutscene plays no effects as songs. The effects of each cutscene whose group the table names follow the other
// effects, in the order of the cutscenes' paths in lower case. Each is named after the cutscene and its song ID, such
// as "dd01-0413".
Catalogue ReadResidentEvilZero(const disc::GameFiles& game, const CatalogueTables& tables)
{
    const Dol dol = Dol::ParseMain(game.dol);
    const AnyCaseFiles files(game);
    const Cutscenes cutscenes = ReadCutscenes(files);

    Catalogue catalogue;
    ArchiveSets sets(catalogue, files);
    AddResidentEvilZeroSongs(catalogue, sets, files, dol, tables, cutscenes.songs);
    AddArchiveEffects(catalogue, sets, files);
    AddCutsceneEffects(catalogue, dol, tables, cutscenes.effects);

    return catalogue;
}

// Resident Evil's songs. The game loads a song from bgm/sgNN.snd, with NN its number in hexadecimal, 00 to 7f
// (0x80061BF4). The file starts with 16 big-endian words: the offsets and the sizes of the pool, the project, the
// sample directory, the game's table of effects, the song and the samples, then the group's ID and the song's ID. Each
// file holds one song and one group. The game pushes the group and starts the song through sndSeqPlayEx, without
// parameters (0x80062364). The songs come in the order of the files' names. A file with the song and samples of an
// earlier file plays the same sound and is left out.
//
// A room's table names up to four songs that play at once (0x80061B1C). The code of a room or an event changes the
// levels of channels through 0x80061E6C, above all in the ambience of songs 0x6E to 0x7C. Each file plays alone at the
// levels of its song. Song 0x7A starts with all seven channels at level 0. Its file alone stays silent. The mixes that
// rooms give songs 0x74 and 0x7A follow their songs (ResidentEvilMixes). Each mix sets its levels once the song's first
// pattern has set the song's.
//
// The game's se/ holds the effects, in files of the same form. The catalogue leaves them out. The game plays each one
// through a table in its file. The table gives the effect's level and sends.
Catalogue ReadResidentEvil(const disc::GameFiles& game)
{
    constexpr std::size_t kHeader = 0x40;
    // The words of the header that give each part's offset, then those of the group's ID and the song's ID. The word
    // after each offset gives the part's size.
    constexpr std::size_t kPool = 0, kProject = 2, kSdir = 4, kSong = 8, kSamples = 10, kGroup = 12, kSongId = 13;
    constexpr std::string_view kFolder = "bgm/", kPrefix = "sg", kSuffix = ".snd";

    const AnyCaseFiles files(game);
    Catalogue catalogue;
    std::set<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> seen;
    for (const auto& [lower, path] : files.FilesIn(kFolder, kSuffix))
    {
        const std::string name = lower.substr(kFolder.size(), lower.size() - kFolder.size() - kSuffix.size());
        if (!name.starts_with(kPrefix))
        {
            continue;
        }

        const std::vector<uint8_t> file = files.Read(path);
        const std::span<const uint8_t> header = Part(file, 0, kHeader, "a sound file's header");
        const auto word = [&](std::size_t index)
        {
            return xsflib::LoadBe32(header.data() + index * 4);
        };
        const auto part = [&](std::size_t index, const char* what)
        {
            return Bytes(Part(file, word(index), word(index + 1), what));
        };

        std::vector<uint8_t> song = part(kSong, "a sound file's song");
        std::vector<uint8_t> samples = part(kSamples, "a sound file's samples");
        if (!seen.emplace(song, samples).second)
        {
            continue;
        }

        const std::size_t set = catalogue.sets.size();
        const auto group = static_cast<uint16_t>(word(kGroup));
        catalogue.sets.push_back({.project = part(kProject, "a sound file's project"),
                                  .pool = part(kPool, "a sound file's pool"),
                                  .sdir = part(kSdir, "a sound file's sample directory"),
                                  .samples = std::move(samples)});

        Sound sound;
        sound.name = name;
        sound.id = static_cast<uint16_t>(word(kSongId));
        sound.song_group = group;
        sound.groups = {{.id = group, .set = set}};
        AddSong(catalogue, std::move(sound), std::move(song));
        AddResidentEvilMixes(catalogue);
    }

    return catalogue;
}

} // namespace ntdlib::musyx::detail
