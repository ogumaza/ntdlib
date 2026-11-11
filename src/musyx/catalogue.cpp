// SPDX-License-Identifier: MIT

// The parts that the readers of the MusyX catalogues share, and the tables of sounds and of songs' paths (see
// ntdlib/musyx/catalogue.h).

#include "ntdlib/musyx/catalogue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "musyx/catalogue_parts.h"
#include "ntdlib/disc/error.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/musyx/project.h"

namespace ntdlib::musyx
{
namespace detail
{

std::span<const uint8_t> Part(std::span<const uint8_t> data, std::size_t offset, std::size_t size, const char* what)
{
    if (offset > data.size() || size > data.size() - offset)
    {
        throw xsflib::FormatError(std::string(what) + " runs past the end of its file");
    }

    return data.subspan(offset, size);
}

std::string TextBeforeZero(std::span<const uint8_t> bytes)
{
    return std::string(bytes.begin(), std::find(bytes.begin(), bytes.end(), uint8_t{0}));
}

std::string Stem(const std::string& path)
{
    const std::string file = path.substr(path.find_last_of('/') + 1);
    return file.substr(0, file.find_last_of('.'));
}

uint16_t FirstGroupId(std::span<const uint8_t> project)
{
    return xsflib::LoadBe16(Part(project, 4, 2, "a project").data());
}

uint8_t MostCommon(const std::vector<uint8_t>& values)
{
    uint8_t best = values.front();
    std::ptrdiff_t best_count = 0;
    for (const uint8_t value : values)
    {
        const std::ptrdiff_t count = std::count(values.begin(), values.end(), value);
        if (count > best_count)
        {
            best = value;
            best_count = count;
        }
    }

    return best;
}

void AddSong(Catalogue& catalogue, Sound sound, std::vector<uint8_t> song)
{
    sound.song = catalogue.songs.size();
    catalogue.songs.push_back(std::move(song));
    catalogue.sounds.push_back(std::move(sound));
}

AnyCaseFiles::AnyCaseFiles(const disc::GameFiles& game) : game_(game)
{
    for (const disc::GameFile& file : game.files)
    {
        paths_.emplace(xsflib::AsciiLower(file.path), file.path);
    }
}

const std::string* AnyCaseFiles::Find(const std::string& path) const
{
    const auto found = paths_.find(xsflib::AsciiLower(path));
    return found == paths_.end() ? nullptr : &found->second;
}

std::vector<uint8_t> AnyCaseFiles::Read(const std::string& path) const
{
    const std::string* found = Find(path);
    if (found == nullptr)
    {
        throw xsflib::FormatError("the disc has no " + path);
    }

    return disc::ReadGameFile(game_, *found);
}

std::vector<std::pair<std::string, std::string>> AnyCaseFiles::FilesIn(std::string_view folder,
                                                                       std::string_view extension) const
{
    std::vector<std::pair<std::string, std::string>> found;
    for (const auto& [lower, path] : paths_)
    {
        if (lower.starts_with(folder) && lower.ends_with(extension) &&
            lower.find('/', folder.size()) == std::string::npos)
        {
            found.emplace_back(lower, path);
        }
    }

    return found;
}

FileSet ReadFileSet(const disc::GameFiles& game, const std::string& stem, const FileExtensions& extensions)
{
    return {.project = disc::ReadGameFile(game, stem + extensions.project),
            .pool = disc::ReadGameFile(game, stem + extensions.pool),
            .sdir = disc::ReadGameFile(game, stem + extensions.sdir),
            .samples = disc::ReadGameFile(game, stem + extensions.samples)};
}

std::vector<ProjectEffect> EffectsOf(std::span<const uint8_t> project)
{
    return ProjectEffects(ReadProject(project));
}

} // namespace detail

Catalogue ReadCatalogue(CatalogueLayout layout, const disc::GameFiles& game, const CatalogueTables& tables)
{
    switch (layout)
    {
    case CatalogueLayout::kRetro:
        return detail::ReadRetro(game);
    case CatalogueLayout::kTales:
        return detail::ReadTales(game, tables);
    case CatalogueLayout::kStarFox:
        return detail::ReadStarFox(game, tables);
    case CatalogueLayout::kRogueLeader:
        return detail::ReadRogueLeader(game, tables);
    case CatalogueLayout::kRebelStrike:
        return detail::ReadRebelStrike(game);
    case CatalogueLayout::kColosseum:
        return detail::ReadColosseum(game, tables);
    case CatalogueLayout::kResidentEvil:
        return detail::ReadResidentEvil(game);
    case CatalogueLayout::kResidentEvilZero:
        return detail::ReadResidentEvilZero(game, tables);
    case CatalogueLayout::kPaperMario:
        return detail::ReadPaperMario(game, tables);
    case CatalogueLayout::kPokemonChannel:
        return detail::ReadPokemonChannel(game, tables);
    case CatalogueLayout::kPuzzleCollection:
        return detail::ReadPuzzleCollection(game);
    case CatalogueLayout::kNdDemo:
        return detail::ReadNdDemo(game, tables);
    }

    throw xsflib::FormatError("no catalogue has that layout");
}

std::string EffectName(const std::string& prefix, uint16_t id)
{
    constexpr std::size_t kDigits = 4;
    const std::string number = std::to_string(id);

    return prefix + "-" + std::string(kDigits - std::min(number.size(), kDigits), '0') + number;
}

std::vector<TableSound> ReadSoundTable(std::span<const uint8_t> module, const std::string& name, uint32_t sounds,
                                       uint32_t sound_count, uint32_t files, uint32_t file_count)
{
    constexpr std::size_t kSoundEntry = 12, kFileEntry = 8;
    constexpr uint8_t kEffect = 0x80;

    // Both tables are inside the module's file.
    const auto inside = [&module](uint32_t offset, std::size_t size)
    {
        return offset <= module.size() && size <= module.size() - offset;
    };
    if (!inside(sounds, std::size_t{sound_count} * kSoundEntry))
    {
        throw xsflib::FormatError(name + "'s table of sounds runs past the file's end");
    }
    if (!inside(files, std::size_t{file_count} * kFileEntry))
    {
        throw xsflib::FormatError(name + "'s table of files runs past the file's end");
    }

    std::vector<TableSound> listed;
    for (uint32_t i = 0; i < sound_count; i++)
    {
        const uint8_t* entry = module.data() + sounds + std::size_t{i} * kSoundEntry;
        TableSound sound{.sound = i, .group = entry[1], .id = xsflib::LoadBe16(entry + 4)};
        if ((entry[0] & kEffect) != 0)
        {
            sound.kind = SoundKind::kEffect;
            listed.push_back(sound);
            continue;
        }

        const uint8_t file = entry[3];
        if (file >= file_count)
        {
            throw xsflib::FormatError(name + "'s sound " + std::to_string(i) + " names no file");
        }

        sound.reverb = entry[2];
        sound.file = xsflib::LoadBe32(module.data() + files + std::size_t{file} * kFileEntry);
        if (sound.file != 0)
        {
            listed.push_back(sound);
        }
    }

    return listed;
}

std::vector<std::string> ReadSongPaths(const Dol& dol, uint32_t address, uint32_t count)
{
    std::vector<std::string> paths;
    for (uint32_t i = 0; i < count; i++)
    {
        paths.push_back(
            dol.Text(dol.Word(address + i * 4, "the table of songs' paths"), detail::kLongestText, "a song's path"));
    }

    return paths;
}

} // namespace ntdlib::musyx
