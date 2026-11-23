// SPDX-License-Identifier: MIT

// The command line of a converter of NintendoWare's sound archives (archive_converter.h) that takes JAudio games too.
// An input that starts as a JAudio start-up archive converts as the game that it starts, with the game's other files
// beside it (Game::FromFolder).

#pragma once

#include <string>
#include <vector>

#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export::jaudio
{

// The converter of the JAudio games among the inputs of the archive converter `archives`. It takes the name, the
// version line and the usage of `archives`. A game takes the program's options. Of those beyond the frame's, only
// --voices applies to its sequences (ArchiveProgramOptions). `game_options` says which sounds of each game to list, and
// whether the games run on the Wii U.
template <typename Archive>
GameConverter MakeGameConverterFor(
    const Converter<sequence_export::PerformOptions, Archive, sequence_export::Conversion>& archives,
    const GameOptions& game_options = {})
{
    return MakeGameConverter(archives.program, archives.version_line, ArchiveProgramOptions(archives.option),
                             archives.usage, game_options);
}

// Runs the command line `argc` and `argv` through the archive converter `archives`, as RunMain and Run do, with the
// JAudio games among the inputs (ArchiveProgramGames). The games convert with MakeGameConverterFor. Returns the exit
// status for main() to return.
template <typename Archive>
int RunConverterWithGames(
    int argc, char** argv,
    const Converter<sequence_export::PerformOptions, Archive, sequence_export::Conversion>& archives,
    const GameOptions& game_options = {})
{
    const GameConverter games = MakeGameConverterFor(archives, game_options);
    const auto run = [&archives, &games](const std::vector<std::string>& args)
    {
        return Run(args, archives, ArchiveProgramGames(args, games));
    };

    return RunMain(argc, argv, run);
}

} // namespace ntdlib::sequence_export::jaudio
