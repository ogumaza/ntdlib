// SPDX-License-Identifier: MIT

// The command line of a converter of the DS's sound archives (SDAT), on the command-line frame (frame.h). Its inputs
// are sound archives and cartridge images of the DS and the DSi. An image gives each sound archive that a file of its
// file system holds, whatever the file's name, each into a folder of its own in the image's output folder. The
// image's ARM7 binary gives the driver's build (ntdlib::ntr::snd::BuildOf). A loose archive plays on the default
// build. A converter gives its name, its version line, its description and its options.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/ntr/performance.h"

namespace ntdlib::sequence_export::ntr
{

struct ArchiveInput;

// A state of a game that stays set while a sequence plays and changes what it plays, such as a variable that the game's
// code writes before it starts the sequence. A converter gives such a state a separate conversion beside the
// sequence's: an arrangement. Its label is the sequence's, followed by `suffix`.
struct Arrangement
{
    std::string suffix;

    // The variables' values, in place of those that the game's settings give (0-15 the player's, 16-31 the global
    // variables).
    std::map<uint8_t, int16_t> variables;

    // The driver's commands that the game's code sends once it has started the sequence (PerformOptions::commands).
    // They follow the commands of the game's settings. Where both set a value, the arrangement's takes effect.
    ntdlib::ntr::snd::CommandList commands;
};

// A conversion of an archive: a sequence of ArchiveInput::sequences as the game's settings start it, or in one of its
// arrangements.
struct ArchiveEntry
{
    std::size_t sequence = 0;
    std::optional<Arrangement> arrangement = {};
};

// A converter of the DS's sound archives.
struct ArchiveConverter
{
    std::string program;      // the program's name, such as "xyz2midi"
    std::string version_line; // what --version prints, such as "xyz2midi 1.0.0"

    // What the program does, in lines that end with a newline, at the start of the help.
    std::string description;

    // The lines of the help for the program's options (`option`), each ending with a newline. They come after the
    // --var lines.
    std::string extra_options;

    // Reads the program's options. Every program on the frame takes --var.
    OptionHandler<PerformOptions> option = VarOption<PerformOptions>;

    // Sets what a game's code sets before it starts `sequence` of `input` (PerformOptions' variables, player_channels,
    // track_channels and reverb) and the commands that it sends once it has started it, before the sequence's
    // conversion. A program with games' profiles gives one. Without one, each sequence plays as the archive gives it. A
    // variable that `perform` holds already keeps its value: the command line's --var, then an arrangement's. An
    // arrangement's commands follow those that the function gives.
    std::function<void(const ArchiveInput& input, const ntdlib::ntr::SequenceRef& sequence, PerformOptions& perform)>
        game_settings;

    // The arrangements of `sequence` of `input`, in their order. A program with games' profiles gives them. Without
    // the function, no sequence has any.
    std::function<std::vector<Arrangement>(const ArchiveInput& input, const ntdlib::ntr::SequenceRef& sequence)>
        arrangements;
};

// A sound archive to convert, the bytes that it points into, and the build of the driver that plays it. An archive
// from a cartridge image gives the image's game code, the path of the image's file that holds the archive, and the
// archive's place among that file's archives (sdat::FindArchives). A loose archive leaves them empty and 0.
struct ArchiveInput
{
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    sdat::Archive archive;
    ntdlib::ntr::snd::Build build;
    std::vector<ntdlib::ntr::SequenceRef> sequences; // ntdlib::ntr::Sequences(archive)
    std::vector<ArchiveEntry> entries;               // ArchiveEntries
    std::string game_code;
    std::string file_path;
    std::size_t index_in_file = 0;
};

// The help of `archive_converter`: its description, the usage line, where the files go, and the options.
std::string UsageText(const ArchiveConverter& archive_converter);

// The conversions of `input`: each of its sequences, followed by the arrangements that `archive_converter` gives it.
std::vector<ArchiveEntry> ArchiveEntries(const ArchiveInput& input, const ArchiveConverter& archive_converter);

// The archive's entries, each by its label: the sequence's label (ntdlib::ntr::Label), followed by an arrangement's
// suffix. An entry's index is its place in ArchiveInput::entries.
std::vector<Sequence> ArchiveSequences(const ArchiveInput& input);

// The frame's converter of loose sound archives. `archive_converter` has to outlive the converter.
Converter<PerformOptions, ArchiveInput, Conversion> MakeArchiveConverter(const ArchiveConverter& archive_converter);

// Converts or lists the sound archives of the cartridge image at `path`, with the options that Run parsed (the frame's
// OtherInput). `converter` is MakeArchiveConverter's for `archive_converter`. Returns nothing for an input that starts
// as a sound archive does, or that can't be opened: the frame then reads it with `converter`. Throws std::runtime_error
// for a file that is neither an archive nor an image.
std::optional<int> ConvertImage(const std::string& path, const Options<PerformOptions>& options, const std::regex& only,
                                bool several, std::vector<std::pair<std::filesystem::path, std::string>>& used_folders,
                                const Converter<PerformOptions, ArchiveInput, Conversion>& converter,
                                const ArchiveConverter& archive_converter);

// Runs the command line of `archive_converter` with `argc` and `argv`. Returns the exit status for main() to return.
int RunArchiveConverter(int argc, char** argv, const ArchiveConverter& archive_converter);

// Runs `archive_converter` on `args`, a command line in UTF-8 with the program's name first. Returns the exit status.
// The entry point above runs it on the program's command line.
int RunArchiveConverter(const std::vector<std::string>& args, const ArchiveConverter& archive_converter);

} // namespace ntdlib::sequence_export::ntr
