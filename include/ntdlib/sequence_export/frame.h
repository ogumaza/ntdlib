// SPDX-License-Identifier: MIT

// The command-line frame of a converter: UTF-8 arguments and paths, files, the console on Windows, the options, the
// batch of inputs with their output folders, and the lines that report a conversion. A program's macOS app reads those
// lines. A converter supplies its name, its words and messages, its extra options, and how it reads an input and
// converts a sequence (Converter).

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/text.h"

#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export
{

// UTF-8 paths and whole numbers (xsflib's files.h and text.h).
using xsflib::ParseInteger;
using xsflib::PathFromUtf8;
using xsflib::Utf8;

// Reads a file. Returns nothing if it can't be opened.
std::optional<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path);

// Writes `data` to a file. Checks for errors after closing, when the final buffered bytes are flushed, and names the
// file in any std::runtime_error.
void WriteFile(const std::filesystem::path& path, std::span<const uint8_t> data);

// Writes `text` verbatim. Throws std::runtime_error if the write fails.
void WriteFile(const std::filesystem::path& path, const std::string& text);

// Lowercases the ASCII letters in a name to detect file name collisions on Windows and macOS (xsflib's AsciiLower).
using xsflib::AsciiLower;

// Seconds as minutes and seconds, like 1:05.3.
std::string Duration(double seconds);

// Prints the sequence's duration, loop, channel/preset/sample counts, approximations and warnings. `c` is a program's
// Conversion: its performance, channels, samples and warnings.
template <typename Conversion>
void PrintConversion(const std::string& name, const Conversion& c)
{
    const auto& p = c.performance;
    std::string loop;
    if (p.loop)
    {
        loop = p.loops > 1 ? ", looped " + std::to_string(p.loops) + " times" : ", loops";
    }
    else if (p.holds)
    {
        loop = ", holds until stopped";
    }
    else if (p.truncated)
    {
        loop = ", cut at the time limit";
    }

    std::printf("  %-28s %8s%s, %d channels, %zu presets, %zu samples\n", name.c_str(), Duration(p.seconds).c_str(),
                loop.c_str(), c.channels, p.presets.size(), c.samples);
    for (const auto& [what, n] : p.approximations)
    {
        std::printf("      approximated: %s (%u)\n", what.c_str(), n);
    }
    for (const std::string& w : c.warnings)
    {
        std::printf("      warning: %s\n", w.c_str());
    }
}

// The command-line arguments as UTF-8 (xsflib's console.h).
using xsflib::console::Arguments;

// The body of a program's main(), on xsflib's RunTool. RunMain runs `run` on the command-line arguments as UTF-8 and
// returns its exit status. RunMain reports an exception that escapes `run` as "error: <what>" and returns 1. A console
// window that Windows opened for the program alone stays open until the user presses Enter.
int RunMain(int argc, char** argv, const std::function<int(const std::vector<std::string>&)>& run);

// Command-line options. `perform` holds the program's options for playing a sequence. --loops and --seed set its
// `loops` and `seed`.
template <typename Perform>
struct Options
{
    std::vector<std::string> archives; // the inputs
    std::string output;                // override the output directory beside the archive
    std::string only;
    bool list = false;
    bool quiet = false;
    bool help = false;
    bool version = false;
    Perform perform;
};

// Reads the value of an option from the argument after it.
class OptionReader
{
public:
    // Reads the value of `args[i]`. Each read moves `i` on to the argument it read.
    OptionReader(const std::vector<std::string>& args, std::size_t& i);

    // The option's value. Throws std::runtime_error if no argument follows the option.
    std::string Value();

    // Parses an integer no greater than `max`, in decimal or hexadecimal with a 0x prefix.
    uint32_t Number(uint32_t max);

    // Parses a count from 1 to `max` the way Number does, or the word "off" as 0.
    uint32_t CountOrOff(uint32_t max);

private:
    // Parses `value` as Number does, from `min` to `max`. A value that isn't a number names `expected`.
    uint32_t Parse(const std::string& value, uint32_t min, uint32_t max, const std::string& expected) const;

    const std::vector<std::string>& args_;
    std::size_t& i_;
    std::string option_;
};

// Parses the value N=V of `option`: an index N from 0 to `max_index` and a number V from `min` to `max`. Throws
// std::runtime_error, naming the option, for another form or a number out of range.
std::pair<uint8_t, int64_t> ParseAssignment(const std::string& option, const std::string& value, int64_t max_index,
                                            int64_t min, int64_t max);

// Reads --var N=V into `perform.variables`: player variables 0-15 and global variables 16-31, each from -32768 to
// 32767. Returns false for any other option. `perform` is sequence_export's PerformOptions or a program's struct with
// the same member.
template <typename Perform>
bool VarOption(const std::string& option, OptionReader& reader, Perform& perform)
{
    if (option != "--var")
    {
        return false;
    }

    const auto [index, number] = ParseAssignment(option, reader.Value(), 31, std::numeric_limits<int16_t>::min(),
                                                 std::numeric_limits<int16_t>::max());
    perform.variables[index] = static_cast<int16_t>(number);

    return true;
}

// Reads --voices N, from 1 to kMaxVoiceLimit (performance.h), or --voices off into `perform.voice_limit`, with off as
// 0. Returns false for any other option. `perform` is sequence_export's PerformOptions or a program's struct with the
// same member.
template <typename Perform>
bool VoicesOption(const std::string& option, OptionReader& reader, Perform& perform)
{
    if (option != "--voices")
    {
        return false;
    }

    perform.voice_limit = static_cast<int>(reader.CountOrOff(kMaxVoiceLimit));

    return true;
}

// Reads one of a program's extra options into `perform`. Returns false for any other option.
template <typename Perform>
using OptionHandler = std::function<bool(const std::string& option, OptionReader& reader, Perform& perform)>;

// Parses arguments up to --help or --version. `extra` reads the program's extra options. Throws std::runtime_error for
// unknown options, missing values or invalid values.
template <typename Perform>
Options<Perform> ParseOptions(const std::vector<std::string>& args, const OptionHandler<Perform>& extra)
{
    Options<Perform> o;
    for (std::size_t i = 1; i < args.size(); i++)
    {
        const std::string& a = args[i];
        OptionReader reader(args, i);
        if (a == "--output" || a == "-o")
        {
            o.output = reader.Value();
        }
        else if (a == "--only")
        {
            o.only = reader.Value();
        }
        else if (a == "--loops")
        {
            o.perform.loops = std::max(1, static_cast<int>(reader.Number(std::numeric_limits<int>::max())));
        }
        else if (a == "--seed")
        {
            o.perform.seed = reader.Number(std::numeric_limits<uint32_t>::max());
        }
        else if (extra && extra(a, reader, o.perform))
        {
            // One of the program's extra options.
        }
        else if (a == "--list")
        {
            o.list = true;
        }
        else if (a == "--quiet" || a == "-q")
        {
            o.quiet = true;
        }
        else if (a == "--help" || a == "-h")
        {
            o.help = true;
            return o;
        }
        else if (a == "--version")
        {
            o.version = true;
            return o;
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            throw std::runtime_error("unknown option " + a);
        }
        else
        {
            o.archives.push_back(a);
        }
    }

    return o;
}

// A sequence of an input: its index there, and its name. A piece of several sequences that play together also gives
// their labels (ConvertTogether in convert.h). --only picks the piece by its name or by any of its labels.
struct Sequence
{
    std::size_t index = 0;
    std::string name;
    std::vector<std::string> labels = {};
};

// Converts the selected sequences to `out_dir` and reports each result unless `quiet` is set. `convert` converts a
// sequence by its index. The SFZ files' shared samples go in sfz/<samples_folder>. Returns the failure count.
template <typename Conversion>
int ConvertAll(const std::vector<Sequence>& selected, const std::filesystem::path& out_dir, bool quiet,
               const std::string& samples_folder, const std::function<Conversion(std::size_t index)>& convert)
{
    int failed = 0;
    std::set<std::string> used_names = {samples_folder}; // lowercase names for collision checks on Windows and macOS
    std::set<std::string> samples_written;
    const std::filesystem::path sfz_dir = out_dir / "sfz";
    const auto start = std::chrono::steady_clock::now();
    for (const Sequence& sequence : selected)
    {
        const std::string& name = sequence.name;
        try
        {
            const Conversion c = convert(sequence.index);

            // Disambiguate names that differ only in case, and the reserved SFZ samples directory name.
            std::string base = seqlib::SafeFileName(name);
            while (!used_names.insert(xsflib::AsciiLower(base)).second)
            {
                base += "_" + std::to_string(sequence.index);
            }

            WriteFile(out_dir / PathFromUtf8(base + ".mid"), c.midi);
            WriteFile(out_dir / PathFromUtf8(base + ".sf2"), c.sf2);

            // Write SFZ files to each sequence's directory and samples to the shared directory. Sequences with no
            // playing tracks produce no SFZ files.
            const std::filesystem::path sequence_dir = sfz_dir / PathFromUtf8(base);
            const std::filesystem::path samples_dir = sfz_dir / PathFromUtf8(samples_folder);
            for (const auto& f : c.sfz.files)
            {
                std::filesystem::create_directories(sequence_dir);
                WriteFile(sequence_dir / PathFromUtf8(f.name), f.text);
            }

            for (const auto& sample : c.sfz.samples)
            {
                if (samples_written.contains(sample.name))
                {
                    continue; // already written for another sequence
                }

                std::filesystem::create_directories(samples_dir);
                WriteFile(samples_dir / PathFromUtf8(sample.name), sample.wav);
                samples_written.insert(sample.name);
            }

            if (!quiet)
            {
                PrintConversion(name, c);
            }
        }
        catch (const std::exception& e)
        {
            failed++;
            std::fprintf(stderr, "  %s: %s\n", name.c_str(), e.what());
        }
    }

    if (!quiet)
    {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("done: %zu converted, %d failed, %.1f s\n", selected.size() - failed, failed, secs);
    }

    return failed;
}

// What a converter gives the frame. `Source` is what it reads from an input, such as a sound archive, and `Conversion`
// is a sequence's files and report (see ConvertAll and PrintConversion).
template <typename Perform, typename Source, typename Conversion>
struct Converter
{
    std::string program;        // the program's name. An input's output folder is <input>_<program> beside it.
    std::string version_line;   // what --version prints, such as "xyz2midi 1.0.0"
    std::string input_word;     // "archive" or "input", in the message for --output with several inputs
    std::string folder_message; // follows an input's path when the input is a folder
    std::string samples_folder; // the SFZ files' shared folder of samples, in the sfz folder

    // Prints the usage and options.
    std::function<void()> usage;

    // Reads the program's extra options, if it has any.
    OptionHandler<Perform> option;

    // Runs once the options are checked, before the first input, if set.
    std::function<void()> before_inputs;

    // Reads `input`, named `name`, and prints its warnings. Returns nothing if it can't be read: the frame then reports
    // the failure. Throws for an input that isn't one the program reads.
    std::function<std::optional<Source>(const std::filesystem::path& input, const std::string& name)> open;

    // The sequences of `source` that the program can convert, in order.
    std::function<std::vector<Sequence>(const Source& source)> sequences;

    // Sets the performance options for the input's sequences, if set. It runs after the input's report line.
    std::function<void(const std::filesystem::path& input, const Source& source, const Options<Perform>& options,
                       Perform& perform)>
        prepare;

    // Converts sequence `index` of `source`.
    std::function<Conversion(Source& source, std::size_t index, const Perform& perform)> convert;
};

// Converts or lists the selected sequences in `path`. Returns 1 on any failure, otherwise 0. If `several` is set, the
// listings start with the input's name. `used_folders` maps output directories to inputs already processed. Throws for
// corrupt data, directory creation failure or a conflicting output directory.
template <typename Perform, typename Source, typename Conversion>
int ConvertInput(const std::string& path, const Options<Perform>& options, const std::regex& only, bool several,
                 std::vector<std::pair<std::filesystem::path, std::string>>& used_folders,
                 const Converter<Perform, Source, Conversion>& converter)
{
    const std::filesystem::path input = PathFromUtf8(path);

    // Some systems allow opening directories as empty files. Report those explicitly.
    std::error_code error;
    if (std::filesystem::is_directory(input, error))
    {
        std::fprintf(stderr, "%s%s\n", path.c_str(), converter.folder_message.c_str());
        return 1;
    }

    const std::string name = Utf8(input.filename());
    std::optional<Source> source = converter.open(input, name);
    if (!source)
    {
        std::fprintf(stderr, "can't read %s\n", path.c_str());
        return 1;
    }

    std::vector<Sequence> selected;
    const auto matches = [&only](const std::string& text)
    {
        return std::regex_search(text, only);
    };
    for (Sequence& sequence : converter.sequences(*source))
    {
        if (matches(sequence.name) || std::ranges::any_of(sequence.labels, matches))
        {
            selected.push_back(std::move(sequence));
        }
    }

    if (options.list)
    {
        if (several)
        {
            std::printf("%s:\n", name.c_str());
        }
        for (const Sequence& sequence : selected)
        {
            std::printf("%s%s\n", several ? "  " : "", sequence.name.c_str());
        }
        return 0;
    }

    if (selected.empty())
    {
        std::fprintf(stderr, "%s: no sequences%s\n", name.c_str(), options.only.empty() ? "" : " match");
        return 1;
    }

    const std::filesystem::path out_dir =
        options.output.empty()
            ? input.parent_path() / PathFromUtf8(seqlib::SafeFileName(Utf8(input.stem()) + "_" + converter.program))
            : PathFromUtf8(options.output);

    // Input names differing only in extension produce the same output directory. Whether case differences collide too
    // depends on the filesystem. Before writing anything, use the filesystem to check whether the directory already
    // belongs to an earlier input.
    for (const auto& [folder, earlier] : used_folders)
    {
        std::error_code ignored; // no existing directory to compare
        if (std::filesystem::equivalent(out_dir, folder, ignored))
        {
            throw std::runtime_error("not converted: output would overwrite files from " + earlier + " in " +
                                     Utf8(out_dir));
        }
    }

    std::filesystem::create_directories(out_dir);
    used_folders.emplace_back(out_dir, name);
    if (!options.quiet)
    {
        std::printf("%s: %zu sequences -> %s\n", name.c_str(), selected.size(), Utf8(out_dir).c_str());
    }

    Perform perform = options.perform;
    if (converter.prepare)
    {
        converter.prepare(input, *source, options, perform);
    }

    auto convert = [&](std::size_t index)
    {
        return converter.convert(*source, index, perform);
    };

    return ConvertAll<Conversion>(selected, out_dir, options.quiet, converter.samples_folder, convert) > 0 ? 1 : 0;
}

// Converts an input that another of the program's converters reads, as ConvertInput does, with the options that Run
// parsed. Returns nothing for an input of Run's converter.
template <typename Perform>
using OtherInput = std::function<std::optional<int>(
    const std::string& path, const Options<Perform>& options, const std::regex& only, bool several,
    std::vector<std::pair<std::filesystem::path, std::string>>& used_folders)>;

// Processes the inputs named on the command line and returns the exit status. `other`, if set, converts the inputs
// that another of the program's converters reads.
template <typename Perform, typename Source, typename Conversion>
int Run(const std::vector<std::string>& args, const Converter<Perform, Source, Conversion>& converter,
        const std::type_identity_t<OtherInput<Perform>>& other = {})
{
    const Options<Perform> options = ParseOptions<Perform>(args, converter.option);
    if (options.help)
    {
        converter.usage();
        return 0;
    }
    if (options.version)
    {
        std::puts(converter.version_line.c_str());
        return 0;
    }
    if (options.archives.empty())
    {
        converter.usage();
        return 2;
    }
    if (!options.output.empty() && options.archives.size() > 1)
    {
        throw std::runtime_error("--output requires a single " + converter.input_word +
                                 ". Otherwise, output goes in a folder beside each " + converter.input_word + ".");
    }
    if (converter.before_inputs)
    {
        converter.before_inputs();
    }

    const std::regex only(options.only.empty() ? std::string(".*") : options.only);
    const bool several = options.archives.size() > 1;
    std::vector<std::pair<std::filesystem::path, std::string>> used_folders;
    int rc = 0;
    for (std::size_t i = 0; i < options.archives.size(); i++)
    {
        // Separate input reports with a blank line; the macOS app uses this to split them.
        if (i > 0 && (options.list || !options.quiet))
        {
            std::printf("\n");
        }

        // Continue with the remaining inputs after an error.
        const std::string& path = options.archives[i];
        try
        {
            const std::optional<int> other_rc =
                other ? other(path, options, only, several, used_folders) : std::nullopt;
            rc = std::max(rc,
                          other_rc ? *other_rc : ConvertInput(path, options, only, several, used_folders, converter));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", Utf8(PathFromUtf8(path).filename()).c_str(), e.what());
            rc = 1;
        }
    }

    return rc;
}

} // namespace ntdlib::sequence_export
