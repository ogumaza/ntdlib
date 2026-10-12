// SPDX-License-Identifier: MIT

// The command line of a converter of NintendoWare's sound archives, on the command-line frame (frame.h). A converter
// for the 3DS, the Wii U or the Switch reads CSAR and FSAR archives with sar. A converter for the Wii reads RSAR
// archives with rsar. Each converter gives its name, its version line, the parts of its help that differ (UsageText)
// and its console's settings (consoles.h). A converter for the Wii also fills the hooks for its --dol option and for
// each archive's settings.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"

namespace ntdlib::sequence_export
{

// A converter of sound archives of type Archive: sar::SoundArchive or rsar::SoundArchive.
template <typename Archive>
struct ArchiveConverter
{
    std::string program;      // the program's name, such as "xyz2midi"
    std::string version_line; // what --version prints, such as "xyz2midi 1.0.2"

    // The parts of the help that differ (UsageText). `description` describes what the program does, in lines that end
    // with a newline. `extension` is that of the archives, such as ".bxsar". The --seed line takes `seed_option`.
    // `extra_options` are the lines for the program's options (`option`), each ending with a newline. They come after
    // the --var lines, and after the --voices lines where the console has a voice count (Settings::voice_limit).
    std::string description;
    std::string extension;
    std::string seed_option = "initial game PRNG state (default 0x12345678)";
    std::string extra_options;

    // The console's settings for the archive reader, the performer and the conversion.
    Settings settings;

    // Whether to add the group files that the game keeps beside an archive, as on the Wii U and the Switch.
    bool group_files = false;

    // Reads the program's options. Every program on the frame takes --var.
    OptionHandler<PerformOptions> option = VarOption<PerformOptions>;

    // Runs once the options are checked, before the first archive, if set.
    std::function<void()> before_inputs;

    // The settings for `archive`, read from `input`, if set. They replace `settings` for the archive's sequences. It
    // runs after the archive's report line.
    std::function<Settings(const std::filesystem::path& input, const Archive& archive,
                           const Options<PerformOptions>& options)>
        archive_settings;
};

// Reads the sound archive at `input`, named `name`, with `settings`, and prints its warnings: a truncated archive, a
// damaged name table, and each group that can't be read. With `group_files`, adds the group files that the game keeps
// beside the archive. Returns nothing if the archive can't be read.
template <typename Archive>
std::optional<Archive> OpenArchive(const std::filesystem::path& input, const std::string& name,
                                   const Settings& settings, bool group_files)
{
    std::optional<std::vector<uint8_t>> bytes = ReadFile(input);
    if (!bytes)
    {
        return std::nullopt;
    }

    const std::size_t file_size = bytes->size();
    Archive archive = [&]
    {
        if constexpr (std::is_same_v<Archive, sar::SoundArchive>)
        {
            return Archive::Load(std::move(*bytes), settings.archive);
        }
        else
        {
            return Archive::Load(std::move(*bytes));
        }
    }();

    // A program's macOS app reports a truncated archive when it finds this warning.
    if (archive.Truncated())
    {
        std::fprintf(stderr,
                     "warning: %s is truncated (%zu bytes): sounds whose data is missing are skipped, and notes whose "
                     "waves are missing are silent\n",
                     name.c_str(), file_size);
    }
    if (archive.NamesDamaged())
    {
        std::fprintf(stderr, "warning: %s has a damaged name table: its sounds are named by number\n", name.c_str());
    }
    if constexpr (std::is_same_v<Archive, sar::SoundArchive>)
    {
        for (const std::string& group_error : archive.GroupErrors())
        {
            std::fprintf(stderr, "warning: %s\n", group_error.c_str());
        }

        if (group_files)
        {
            AddGroupFiles(archive, input, name);
        }
    }

    return archive;
}

// The help of `archive_converter`: its description, the usage line, where the files go, and the options. The macOS
// app's name is the program's name with a capital letter.
template <typename Archive>
std::string UsageText(const ArchiveConverter<Archive>& archive_converter)
{
    const ArchiveConverter<Archive>& c = archive_converter;
    std::string app = c.program;
    if (!app.empty())
    {
        app[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(app[0])));
    }

    std::string text = c.description;
    text += "\n";
    text += "usage: " + c.program + " [options] <archive" + c.extension + "> [<archive" + c.extension + "> ...]\n";
    text += "\n";
    text += "Each sequence gets <name>.mid, <name>.sf2, and per-track SFZ files in sfz/<name>.\n";
    text += "Shared samples go in sfz/samples. Output is written to <archive name>_" + c.program + "\n";
    text += "beside the archive. You can also drop archives onto " + c.program + ".exe on Windows\n";
    text += "or " + app + ".app on macOS.\n";
    text += "\n";
    text += "options:\n";
    text += "  --output DIR   write to DIR (one archive only)\n";
    text += "  --only REGEX   convert matching sequence names (for example --only BGM)\n";
    text += "  --loops N      repeat the main loop N times (default 1)\n";
    text += "                 MIDI loop markers are included for players that support them\n";
    text += "  --seed N       " + c.seed_option + "\n";
    text += "  --var N=V      set variable N (0-15 player, 16-31 global) to V before each\n";
    text += "                 sequence starts; repeat for more variables\n";
    if (c.settings.voice_limit > 0)
    {
        text += "  --voices N     drop the notes that the game drops for lack of a voice,\n";
        text += "                 with N voices (the game has " + std::to_string(c.settings.voice_limit) +
                " unless it sets another\n";
        text += "                 count). The result only approximates the game.\n";
        text += "  --voices off   keep every note (default)\n";
    }
    text += c.extra_options;
    text += "  --list         list sequences without converting\n";
    text += "  --quiet        print errors only\n";
    text += "  --version      show the version\n";

    return text;
}

// The options of `archive_converter` beyond those of the frame: --voices where the console has a voice count
// (Settings::voice_limit), then the program's options. `archive_converter` has to outlive the handler.
template <typename Archive>
OptionHandler<PerformOptions> ArchiveOptions(const ArchiveConverter<Archive>& archive_converter)
{
    const ArchiveConverter<Archive>* c = &archive_converter;

    return [c](const std::string& option, OptionReader& reader, PerformOptions& perform)
    {
        if (c->settings.voice_limit > 0 && VoicesOption(option, reader, perform))
        {
            return true;
        }

        return c->option && c->option(option, reader, perform);
    };
}

// The archive's sequences, in the order of its sounds.
template <typename Archive>
std::vector<Sequence> ArchiveSequences(const Archive& archive)
{
    std::vector<Sequence> sequences;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const auto& s = archive.Sounds()[i];
        if (s.type == decltype(s.type)::kSequence)
        {
            sequences.push_back({i, s.name});
        }
    }

    return sequences;
}

// Runs the command line of `archive_converter` with `argc` and `argv`. Returns the exit status for main() to return.
template <typename Archive>
int RunArchiveConverter(int argc, char** argv, const ArchiveConverter<Archive>& archive_converter)
{
    const ArchiveConverter<Archive>& c = archive_converter;
    Converter<PerformOptions, Archive, Conversion> converter;
    converter.program = c.program;
    converter.version_line = c.version_line;
    converter.input_word = "archive";
    converter.folder_message = " is a folder, not a sound archive";
    converter.samples_folder = kSfzSamplesFolder;
    converter.usage = [&c]
    {
        std::puts(UsageText(c).c_str());
    };
    converter.option = ArchiveOptions(c);
    converter.before_inputs = c.before_inputs;
    converter.open = [&c](const std::filesystem::path& input, const std::string& name)
    {
        return OpenArchive<Archive>(input, name, c.settings, c.group_files);
    };
    converter.sequences = ArchiveSequences<Archive>;

    // An archive's sequences convert with the console's settings, or with those that archive_settings gives.
    Settings settings = c.settings;
    if (c.archive_settings)
    {
        converter.prepare = [&c, &settings](const std::filesystem::path& input, const Archive& archive,
                                            const Options<PerformOptions>& options, PerformOptions&)
        {
            settings = c.archive_settings(input, archive, options);
        };
    }

    converter.convert = [&c, &settings](Archive& archive, std::size_t index, const PerformOptions& perform)
    {
        return ConvertSequence(archive, static_cast<uint32_t>(index), perform, settings, c.program);
    };

    const auto run = [&converter](const std::vector<std::string>& args)
    {
        return Run(args, converter);
    };

    return RunMain(argc, argv, run);
}

} // namespace ntdlib::sequence_export
