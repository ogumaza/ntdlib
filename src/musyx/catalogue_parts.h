// SPDX-License-Identifier: MIT

// The parts that the readers of the MusyX catalogues share (ntdlib/musyx/catalogue.h), and each layout's reader.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/catalogue.h"
#include "ntdlib/musyx/project.h"

namespace ntdlib::musyx::detail
{

// `size` bytes at `offset` of `data`. Throws FormatError, naming `what`, if they run past its end.
std::span<const uint8_t> Part(std::span<const uint8_t> data, std::size_t offset, std::size_t size, const char* what);

// The longest name that the readers take from main.dol (Dol::Text).
constexpr std::size_t kLongestText = 64;

// The text that starts at `bytes`, up to its zero byte or the end of `bytes`.
std::string TextBeforeZero(std::span<const uint8_t> bytes);

// The file name of `path` without its folders and its extension: "title" for "audio/title.sng".
std::string Stem(const std::string& path);

// The ID of the first group in `project`, a MusyX project file. A group's header starts with the offset of the next
// group, then the group's ID in a big-endian half. Throws FormatError if the file is too short.
uint16_t FirstGroupId(std::span<const uint8_t> project);

// The value that most of `values` give. Of values that tie, the one that comes first wins.
uint8_t MostCommon(const std::vector<uint8_t>& values);

// The ID of an effect of a project's tables of effects, and of any other effect.
inline uint16_t EffectId(const ProjectEffect& effect)
{
    return effect.row.id;
}

template <typename Effect>
uint16_t EffectId(const Effect& effect)
{
    return effect.id;
}

// Sorts `effects` by their IDs (EffectId) and keeps the first effect of each ID. The sort keeps the order of effects
// that share an ID.
template <typename Effect>
void KeepFirstOfEachId(std::vector<Effect>& effects)
{
    const auto by_id = [](const Effect& a, const Effect& b)
    {
        return EffectId(a) < EffectId(b);
    };
    std::stable_sort(effects.begin(), effects.end(), by_id);

    const auto same_id = [](const Effect& a, const Effect& b)
    {
        return EffectId(a) == EffectId(b);
    };
    effects.erase(std::unique(effects.begin(), effects.end(), same_id), effects.end());
}

// Adds a song's sound and the song's SNG file to `catalogue`. The sound's `song` then names the file.
void AddSong(Catalogue& catalogue, Sound sound, std::vector<uint8_t> song);

// The disc's files, found by their paths in any case. The disc libraries of some games match paths in any case.
class AnyCaseFiles
{
public:
    explicit AnyCaseFiles(const disc::GameFiles& game);

    // The disc's paths, by their paths in lower case. Of two paths that differ only in case, the first on the disc
    // stands for both.
    const std::map<std::string, std::string>& Paths() const
    {
        return paths_;
    }

    // The disc's path of the file whose path matches `path` in any case, or null if the disc has none.
    const std::string* Find(const std::string& path) const;

    // The file whose path matches `path` in any case. Throws FormatError if the disc has none, or if it can't be read.
    std::vector<uint8_t> Read(const std::string& path) const;

    // The files of `folder` (in lower case, ending with a slash) whose names end with `extension`, outside its
    // subfolders, in the order of their paths in lower case: each one's path in lower case and on the disc.
    std::vector<std::pair<std::string, std::string>> FilesIn(std::string_view folder, std::string_view extension) const;

private:
    const disc::GameFiles& game_;
    std::map<std::string, std::string> paths_;
};

// The extensions of MusyX's four files when they share a name.
struct FileExtensions
{
    const char* project;
    const char* pool;
    const char* sdir;
    const char* samples;
};

// MusyX's four files `stem` with `extensions`. Throws FormatError if one of them can't be read.
FileSet ReadFileSet(const disc::GameFiles& game, const std::string& stem, const FileExtensions& extensions);

// The effects of the effects groups in `project`, a MusyX project file, in the file's order, without the effects whose
// rows allow no voices (ProjectEffects).
std::vector<ProjectEffect> EffectsOf(std::span<const uint8_t> project);

// Each layout's reader (see ReadCatalogue).
Catalogue ReadRetro(const disc::GameFiles& game);
Catalogue ReadTales(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadStarFox(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadRogueLeader(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadRebelStrike(const disc::GameFiles& game);
Catalogue ReadColosseum(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadResidentEvil(const disc::GameFiles& game);
Catalogue ReadResidentEvilZero(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadPaperMario(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadPokemonChannel(const disc::GameFiles& game, const CatalogueTables& tables);
Catalogue ReadPuzzleCollection(const disc::GameFiles& game);
Catalogue ReadNdDemo(const disc::GameFiles& game, const CatalogueTables& tables);

} // namespace ntdlib::musyx::detail
