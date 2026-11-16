// SPDX-License-Identifier: MIT

// The DS's converter on the frame (see archive_input.h).

#include "ntdlib/sequence_export/ntr/archive_input.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "ntdlib/dse/build.h"
#include "ntdlib/dse/files.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/universomo/convert.h"

namespace ntdlib::sequence_export::ntr
{

namespace dse = ntdlib::sequence_export::dse;
namespace ums = ntdlib::sequence_export::universomo;
namespace fs = std::filesystem;

namespace
{

// Whether the file at `input` starts as a sound archive does, or can't be opened.
bool StartsAsArchive(const fs::path& input)
{
    std::ifstream in(input, std::ios::binary);
    char magic[4] = {};
    if (!in.read(magic, sizeof(magic)))
    {
        return !in.is_open();
    }

    return std::string(magic, sizeof(magic)) == "SDAT";
}

// The output folder of the archive in the image's file at `path`: the file's name without its extension, and the
// archive's number in the file after the first.
std::string FolderName(const std::string& path, std::size_t number)
{
    std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
    {
        name.erase(dot);
    }
    if (number > 0)
    {
        name += "_" + std::to_string(number);
    }

    return seqlib::SafeFileName(name);
}

// The sequences of `input` whose labels or names match `only`.
std::vector<Sequence> Selected(const ArchiveInput& input, const std::regex& only)
{
    std::vector<Sequence> selected;
    for (Sequence& sequence : ArchiveSequences(input))
    {
        if (std::regex_search(sequence.name, only))
        {
            selected.push_back(std::move(sequence));
        }
    }

    return selected;
}

// Reads --dse-model earlier or later into `perform.dse_model`. Returns false for any other option.
bool DseModelOption(const std::string& option, OptionReader& reader, PerformOptions& perform)
{
    if (option != "--dse-model")
    {
        return false;
    }

    const std::string value = reader.Value();
    if (value != "earlier" && value != "later")
    {
        throw std::runtime_error("--dse-model takes earlier or later, not " + value);
    }
    perform.dse_model = value == "earlier" ? ntdlib::dse::Build::kEarlier : ntdlib::dse::Build::kLater;

    return true;
}

} // namespace

std::string UsageText(const ArchiveConverter& archive_converter)
{
    const ArchiveConverter& c = archive_converter;
    std::string app = c.program;
    if (!app.empty())
    {
        app[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(app[0])));
    }

    std::string text = c.description;
    text += "\n";
    text += "usage: " + c.program + " [options] <archive.sdat | image.nds> [...]\n";
    text += "\n";
    text += "Each sequence gets <name>.mid, <name>.sf2, and per-track SFZ files in sfz/<name>.\n";
    text += "Shared samples go in sfz/samples. Output is written to <input name>_" + c.program + "\n";
    text += "beside the input. A cartridge image gives each sound archive in its files a\n";
    text += "folder of its own there, and so does each folder of DSE's songs (.smd) and sound\n";
    text += "effects (.sed). A sound effect is named after its file and its slot: se_sys_005.\n";
    text += "The songs of Universomo's driver (.ums, as in Beat City) go in the folder\n";
    text += "universomo, each named after its file: au81.\n";
    text += "You can also drop inputs onto " + c.program + ".exe on Windows or " + app + ".app on\n";
    text += "macOS.\n";
    text += "\n";
    text += "options:\n";
    text += "  --output DIR   write to DIR (one input only)\n";
    text += "  --only REGEX   convert matching sequence names (for example --only BGM)\n";
    text += kLoopsHelp;
    text += "  --seed N       the driver's initial random state (default 0x12345678)\n";
    text += "  --var N=V      set variable N (0-15 player, 16-31 global) to V before each\n";
    text += "                 sequence starts; repeat for more variables\n";
    text += "  --dse-model earlier|later\n";
    text += "                 convert DSE songs and sound effects on the model of Explorers\n";
    text += "                 of Time (earlier) or Explorers of Sky (later) where no model\n";
    text += "                 covers the game's driver build; the result only approximates\n";
    text += "                 the game. DSE files of version 0x402, and packs compressed\n";
    text += "                 other than with LZ77, aren't found, and give no warning\n";
    text += c.extra_options;
    text += kListQuietVersionHelp;

    return text;
}

std::vector<ArchiveEntry> ArchiveEntries(const ArchiveInput& input, const ArchiveConverter& archive_converter)
{
    std::vector<ArchiveEntry> entries;
    for (std::size_t i = 0; i < input.sequences.size(); i++)
    {
        entries.push_back({i});
        if (archive_converter.arrangements)
        {
            for (Arrangement& arrangement : archive_converter.arrangements(input, input.sequences[i]))
            {
                entries.push_back({i, std::move(arrangement)});
            }
        }
    }

    return entries;
}

std::vector<Sequence> ArchiveSequences(const ArchiveInput& input)
{
    std::vector<Sequence> sequences;
    for (std::size_t i = 0; i < input.entries.size(); i++)
    {
        const ArchiveEntry& entry = input.entries[i];
        sequences.push_back({i, ntdlib::ntr::Label(input.archive, input.sequences.at(entry.sequence)) +
                                    (entry.arrangement ? entry.arrangement->suffix : "")});
    }

    return sequences;
}

Converter<PerformOptions, ArchiveInput, Conversion> MakeArchiveConverter(const ArchiveConverter& archive_converter)
{
    const ArchiveConverter* c = &archive_converter;
    Converter<PerformOptions, ArchiveInput, Conversion> converter;
    converter.program = c->program;
    converter.version_line = c->version_line;
    converter.input_word = "input";
    converter.folder_message = " is a folder, not a sound archive or a cartridge image";
    converter.samples_folder = seqlib::sfz::kSamplesFolder;
    converter.usage = [c]
    {
        std::puts(UsageText(*c).c_str());
    };
    converter.option = [c](const std::string& option, OptionReader& reader, PerformOptions& perform)
    {
        return DseModelOption(option, reader, perform) || (c->option && c->option(option, reader, perform));
    };
    converter.open = [c](const fs::path& input, const std::string&) -> std::optional<ArchiveInput>
    {
        std::optional<std::vector<uint8_t>> bytes = ReadFile(input);
        if (!bytes)
        {
            return std::nullopt;
        }

        ArchiveInput opened;
        opened.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(*bytes));
        opened.archive = sdat::Archive::Parse(*opened.bytes);
        opened.sequences = ntdlib::ntr::Sequences(opened.archive);
        opened.entries = ArchiveEntries(opened, *c);
        return opened;
    };
    converter.sequences = ArchiveSequences;
    converter.prepare =
        [](const fs::path&, const ArchiveInput& input, const Options<PerformOptions>&, PerformOptions& perform)
    {
        perform.build = input.build;
    };
    converter.convert = [c](ArchiveInput& input, std::size_t index, const PerformOptions& perform)
    {
        const ArchiveEntry& entry = input.entries.at(index);
        const ntdlib::ntr::SequenceRef& sequence = input.sequences.at(entry.sequence);
        PerformOptions options = perform;
        std::string label = ntdlib::ntr::Label(input.archive, sequence);
        if (entry.arrangement)
        {
            for (const auto& [variable, value] : entry.arrangement->variables)
            {
                options.variables.try_emplace(variable, value);
            }
            label += entry.arrangement->suffix;
        }
        if (c->game_settings)
        {
            c->game_settings(input, sequence, options);
        }
        if (entry.arrangement)
        {
            const ntdlib::ntr::snd::CommandList& commands = entry.arrangement->commands;
            options.commands.insert(options.commands.end(), commands.begin(), commands.end());
        }
        return ConvertSequence(input.archive, sequence, label, options, c->program);
    };

    return converter;
}

std::optional<int> ConvertImage(const std::string& path, const Options<PerformOptions>& options, const std::regex& only,
                                bool several, std::vector<std::pair<std::filesystem::path, std::string>>& used_folders,
                                const Converter<PerformOptions, ArchiveInput, Conversion>& converter,
                                const ArchiveConverter& archive_converter)
{
    const fs::path input = PathFromUtf8(path);
    std::error_code error;
    if (fs::is_directory(input, error) || StartsAsArchive(input))
    {
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> read = ReadFile(input);
    if (!read)
    {
        return std::nullopt;
    }

    const auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(*read));
    const std::string name = Utf8(input.filename());
    std::optional<ntdlib::ntr::Rom> rom;
    try
    {
        rom = ntdlib::ntr::Rom::Parse(*bytes);
    }
    catch (const xsflib::FormatError& e)
    {
        throw std::runtime_error(std::string("not a sound archive or a DS cartridge image (") + e.what() + ")");
    }

    ntdlib::ntr::snd::Build build = ntdlib::ntr::snd::kDefaultBuild;
    try
    {
        build = ntdlib::ntr::snd::BuildOf(rom->Arm7());
    }
    catch (const std::runtime_error&)
    {
        std::fprintf(stderr,
                     "warning: %s: the ARM7 binary doesn't hold the sound driver's tables; its sequences play "
                     "on the default build\n",
                     name.c_str());
    }

    // Each archive in each file, with its folder.
    struct Found
    {
        std::string label; // the image's name and the file's path
        std::string folder;
        ArchiveInput input;
        std::vector<Sequence> selected;
    };
    std::vector<Found> found;
    std::set<std::string> folders = {xsflib::AsciiLower(converter.samples_folder)};
    for (const ntdlib::ntr::RomFile& file : rom->Files())
    {
        std::vector<sdat::Archive> archives = sdat::FindArchives(rom->FileData(file.id));
        const std::string file_path = file.path.empty() ? "file " + std::to_string(file.id) : file.path;
        for (std::size_t i = 0; i < archives.size(); i++)
        {
            std::string folder = FolderName(file_path, i);
            for (int n = 2; !folders.insert(xsflib::AsciiLower(folder)).second; n++)
            {
                folder = FolderName(file_path, i) + "_" + std::to_string(n);
            }

            Found item{name + "/" + file_path,
                       folder,
                       {bytes, std::move(archives[i]), build, {}, {}, rom->Header().game_code, file.path, i},
                       {}};
            item.input.sequences = ntdlib::ntr::Sequences(item.input.archive);
            item.input.entries = ArchiveEntries(item.input, archive_converter);
            item.selected = Selected(item.input, only);
            found.push_back(std::move(item));
        }
    }

    // DSE's songs and sound effects, loose or in packs, each folder of them with an output folder. They play on the
    // model of the image's build of DSE's driver, or on the model that --dse-model names. Without either, the image's
    // songs and sound effects are left out.
    std::vector<ntdlib::dse::NamedFile> named;
    for (const ntdlib::ntr::RomFile& file : rom->Files())
    {
        named.push_back({file.path.empty() ? "file " + std::to_string(file.id) : file.path, rom->FileData(file.id)});
    }
    std::vector<std::vector<uint8_t>> decompressed;
    std::vector<ntdlib::dse::NamedFile> dse_files =
        ntdlib::dse::FindFiles(ntdlib::dse::Decompressed(named, decompressed));
    std::optional<ntdlib::dse::Build> dse_build;
    if (!dse_files.empty())
    {
        dse_build = ntdlib::dse::BuildOf(*rom);
        if (!dse_build)
        {
            dse_build = options.perform.dse_model;
        }
        if (!dse_build)
        {
            const std::size_t songs = ntdlib::dse::FindSongSets(dse_files).size();
            const std::size_t effects = ntdlib::dse::FindEffectSets(dse_files).size();
            if (songs + effects > 0)
            {
                std::fprintf(stderr,
                             "warning: %s: DSE's driver is a build that no model covers; %zu songs and %zu banks of "
                             "sound effects aren't converted (--dse-model converts them on another build's model)\n",
                             name.c_str(), songs, effects);
            }
            dse_files.clear();
        }
    }
    std::vector<std::string> dse_warnings;
    const std::vector<dse::SequenceInput> dse_inputs = dse::ReadSequences(dse_files, dse_warnings);
    struct FoundDse
    {
        std::string label;
        std::string folder;
        std::vector<std::size_t> inputs; // indices into `dse_inputs`
        std::vector<Sequence> selected;
    };
    std::vector<FoundDse> found_dse;
    for (std::size_t i = 0; i < dse_inputs.size(); i++)
    {
        if (found_dse.empty() || dse_inputs[i].folder != dse_inputs[found_dse.back().inputs[0]].folder)
        {
            const std::string dse_path =
                dse_inputs[i].folder.empty() ? "dse" : dse_inputs[i].folder.substr(0, dse_inputs[i].folder.size() - 1);
            const std::string base = seqlib::SafeFileName(dse_path.substr(dse_path.rfind('/') + 1));
            std::string folder = base;
            for (int n = 2; !folders.insert(xsflib::AsciiLower(folder)).second; n++)
            {
                folder = base + "_" + std::to_string(n);
            }
            found_dse.push_back({name + "/" + dse_path, folder, {}, {}});
        }
        FoundDse& group = found_dse.back();
        if (std::regex_search(dse_inputs[i].name, only))
        {
            group.selected.push_back({group.inputs.size(), dse_inputs[i].name});
        }
        group.inputs.push_back(i);
    }
    if (!options.quiet)
    {
        for (const std::string& warning : dse_warnings)
        {
            std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), warning.c_str());
        }
    }

    // Universomo's songs, each folder of them with an output folder.
    std::vector<std::string> ums_warnings;
    const std::vector<ums::SongSet> ums_sets = ums::FindSongs(*rom, ums_warnings);
    struct FoundUms
    {
        std::string label;
        std::string folder;
        std::vector<Sequence> selected;
    };
    std::vector<FoundUms> found_ums;
    for (const ums::SongSet& set : ums_sets)
    {
        std::string folder = "universomo";
        for (int n = 2; !folders.insert(xsflib::AsciiLower(folder)).second; n++)
        {
            folder = "universomo_" + std::to_string(n);
        }

        FoundUms group{
            name + "/" + (set.folder.empty() ? "universomo" : set.folder.substr(0, set.folder.size() - 1)), folder, {}};
        for (std::size_t i = 0; i < set.songs.size(); i++)
        {
            if (std::regex_search(set.songs[i].name, only))
            {
                group.selected.push_back({i, set.songs[i].name});
            }
        }
        found_ums.push_back(std::move(group));
    }
    if (!options.quiet)
    {
        for (const std::string& warning : ums_warnings)
        {
            std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), warning.c_str());
        }
    }

    if (options.list)
    {
        if (several)
        {
            std::printf("%s:\n", name.c_str());
        }
        for (const Found& item : found)
        {
            std::printf("%s%s:\n", several ? "  " : "", item.label.c_str());
            for (const Sequence& sequence : item.selected)
            {
                std::printf("%s  %s\n", several ? "  " : "", sequence.name.c_str());
            }
        }
        for (const FoundDse& group : found_dse)
        {
            std::printf("%s%s:\n", several ? "  " : "", group.label.c_str());
            for (const Sequence& sequence : group.selected)
            {
                std::printf("%s  %s\n", several ? "  " : "", sequence.name.c_str());
            }
        }
        for (const FoundUms& group : found_ums)
        {
            std::printf("%s%s:\n", several ? "  " : "", group.label.c_str());
            for (const Sequence& sequence : group.selected)
            {
                std::printf("%s  %s\n", several ? "  " : "", sequence.name.c_str());
            }
        }
        return 0;
    }

    if (std::ranges::none_of(found, [](const Found& item) { return !item.selected.empty(); }) &&
        std::ranges::none_of(found_dse, [](const FoundDse& group) { return !group.selected.empty(); }) &&
        std::ranges::none_of(found_ums, [](const FoundUms& group) { return !group.selected.empty(); }))
    {
        std::fprintf(stderr, "%s: no sequences%s\n", name.c_str(), options.only.empty() ? "" : " match");
        return 1;
    }

    const fs::path out_dir =
        options.output.empty()
            ? input.parent_path() / PathFromUtf8(seqlib::SafeFileName(Utf8(input.stem()) + "_" + converter.program))
            : PathFromUtf8(options.output);
    for (const auto& [folder, earlier] : used_folders)
    {
        std::error_code ignored; // no existing directory to compare
        if (fs::equivalent(out_dir, folder, ignored))
        {
            throw std::runtime_error("not converted: output would overwrite files from " + earlier + " in " +
                                     Utf8(out_dir));
        }
    }
    fs::create_directories(out_dir);
    used_folders.emplace_back(out_dir, name);

    int failed = 0;
    for (Found& item : found)
    {
        if (item.selected.empty())
        {
            continue;
        }

        const fs::path folder = out_dir / PathFromUtf8(item.folder);
        fs::create_directories(folder);
        if (!options.quiet)
        {
            std::printf("%s: %zu sequences -> %s\n", item.label.c_str(), item.selected.size(), Utf8(folder).c_str());
        }

        PerformOptions perform = options.perform;
        perform.build = build;
        const auto convert = [&](std::size_t index)
        {
            return converter.convert(item.input, index, perform);
        };
        failed += ConvertAll<Conversion>(item.selected, folder, options.quiet, converter.samples_folder, convert);
    }

    for (const FoundDse& group : found_dse)
    {
        if (group.selected.empty())
        {
            continue;
        }

        const fs::path folder = out_dir / PathFromUtf8(group.folder);
        fs::create_directories(folder);
        if (!options.quiet)
        {
            std::printf("%s: %zu sequences -> %s\n", group.label.c_str(), group.selected.size(), Utf8(folder).c_str());
        }

        const dse::PerformOptions perform{.loops = options.perform.loops, .build = *dse_build};
        const auto convert = [&](std::size_t index)
        {
            const dse::SequenceInput& sequence = dse_inputs[group.inputs.at(index)];
            return dse::ConvertSequence(sequence.Files(), sequence.name, perform, converter.program);
        };
        failed += ConvertAll<dse::Conversion>(group.selected, folder, options.quiet, converter.samples_folder, convert);
    }

    for (std::size_t g = 0; g < found_ums.size(); g++)
    {
        const FoundUms& group = found_ums[g];
        if (group.selected.empty())
        {
            continue;
        }

        const fs::path folder = out_dir / PathFromUtf8(group.folder);
        fs::create_directories(folder);
        if (!options.quiet)
        {
            std::printf("%s: %zu sequences -> %s\n", group.label.c_str(), group.selected.size(), Utf8(folder).c_str());
        }

        const ums::SongSet& set = ums_sets[g];
        const auto convert = [&](std::size_t index)
        {
            const ums::SongInput& song = set.songs.at(index);
            const ums::PerformOptions perform{.loops = options.perform.loops, .loop = song.loop};
            return ums::ConvertSequence(song.song, set.bank, song.name, perform, converter.program);
        };
        failed += ConvertAll<ums::Conversion>(group.selected, folder, options.quiet, converter.samples_folder, convert);
    }

    return failed > 0 ? 1 : 0;
}

int RunArchiveConverter(int argc, char** argv, const ArchiveConverter& archive_converter)
{
    const auto run = [&archive_converter](const std::vector<std::string>& args)
    {
        return RunArchiveConverter(args, archive_converter);
    };

    return RunMain(argc, argv, run);
}

int RunArchiveConverter(const std::vector<std::string>& args, const ArchiveConverter& archive_converter)
{
    const Converter<PerformOptions, ArchiveInput, Conversion> converter = MakeArchiveConverter(archive_converter);
    const OtherInput<PerformOptions> image =
        [&converter, &archive_converter](const std::string& path, const Options<PerformOptions>& options,
                                         const std::regex& only, bool several,
                                         std::vector<std::pair<fs::path, std::string>>& used)
    {
        return ConvertImage(path, options, only, several, used, converter, archive_converter);
    };

    return Run(args, converter, image);
}

} // namespace ntdlib::sequence_export::ntr
