// SPDX-License-Identifier: MIT

// The command line of a converter of NintendoWare's sound archives (archive_converter.h) that takes JAudio games too.
// An input that starts as a JAudio start-up archive converts as the game that it starts, with the game's other files
// beside it (Game::FromFolder).

#pragma once

#include <string>
#include <vector>

#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export::jaudio
{

// Runs the command line of `archive_converter` with `argc` and `argv` as RunArchiveConverter does, with JAudio games
// among the inputs (ArchiveProgramGames). A game takes the program's options. Of those beyond the frame's, only
// --voices applies to its sequences (ArchiveProgramOptions). Returns the exit status for main() to return.
template <typename Archive>
int RunArchiveConverterWithGames(int argc, char** argv, const ArchiveConverter<Archive>& archive_converter)
{
    const Converter<sequence_export::PerformOptions, Archive, sequence_export::Conversion> archives =
        MakeArchiveConverter(archive_converter);
    const GameConverter games = MakeGameConverter(archives.program, archives.version_line,
                                                  ArchiveProgramOptions(archives.option), archives.usage);
    const auto run = [&archives, &games](const std::vector<std::string>& args)
    {
        return Run(args, archives, ArchiveProgramGames(args, games));
    };

    return RunMain(argc, argv, run);
}

} // namespace ntdlib::sequence_export::jaudio
