// SPDX-License-Identifier: MIT

// A JAudio game as the frame converts it (frame.h): its start-up archive, the banks that its sequences play, and the
// frame's converter. A program that converts another sound library's archives can take JAudio games with these too.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/jaudio/convert.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/instruments.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export::jaudio
{

// Whether `header`, the first bytes of a file, starts a JAudio start-up archive: JAudio2's BAA ("AA_<") or JAudio1's
// AAF (ntdlib::jaudio::IsAaf).
bool IsStartUpArchive(std::span<const uint8_t> header);

// Whether the file at `path` starts a JAudio start-up archive. A file that can't be read doesn't.
bool IsStartUpArchiveFile(const std::filesystem::path& path);

// A JAudio game being converted, and the banks that its sequences play. It counts the banks' errors that the
// conversions have reported. The banks read the game's files in place. The input therefore can't be copied or moved.
class GameInput
{
public:
    explicit GameInput(Game game);
    GameInput(const GameInput&) = delete;
    GameInput& operator=(const GameInput&) = delete;

    const Game& Files() const
    {
        return game_;
    }

    // Print the warnings of the game's start-up archive and of its other files, each after `name`.
    void PrintWarnings(const std::string& name) const;

    // Convert the game's sequence `index` as `program`, with the banks' errors since the last conversion as warnings.
    Conversion Convert(std::size_t index, const PerformOptions& perform, const std::string& program);

private:
    Game game_;
    BankSet banks_;
    std::size_t errors_reported_ = 0;
};

using GameConverter = Converter<PerformOptions, std::unique_ptr<GameInput>, Conversion>;

// The frame's converter of JAudio games for `program`, whose --version prints `version_line`. An input is a start-up
// archive with its game's other files beside it (Game::FromFolder). `option` reads the program's options beyond the
// frame's. `usage` prints the program's help.
GameConverter MakeGameConverter(const std::string& program, const std::string& version_line,
                                OptionHandler<PerformOptions> option, std::function<void()> usage);

// The options for JAudio games of a program that converts another sound library's archives. `archive_options` reads
// the program's options beyond the frame's for its archives. A JAudio game takes the same options. Of those, only
// --voices applies to its sequences.
OptionHandler<PerformOptions> ArchiveProgramOptions(OptionHandler<sequence_export::PerformOptions> archive_options);

// `converter` with another way to open its input. Each input, whatever its path, is the game that `open` returns.
GameConverter MakeOpenGameConverter(const GameConverter& converter, std::function<std::optional<Game>()> open);

} // namespace ntdlib::sequence_export::jaudio
