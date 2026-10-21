// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/jaudio/game_input.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/jaudio/convert.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sfz.h"
#include "ntdlib/sequence_export/performance.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// Wrap `game` for the frame, and print its warnings after `name`.
std::optional<std::unique_ptr<GameInput>> Opened(Game game, const std::string& name)
{
    auto input = std::make_unique<GameInput>(std::move(game));
    input->PrintWarnings(name);

    return input;
}

} // namespace

bool IsStartUpArchive(std::span<const uint8_t> header)
{
    const bool baa = header.size() >= 4 && header[0] == 'A' && header[1] == 'A' && header[2] == '_' && header[3] == '<';

    return baa || ntdlib::jaudio::IsAaf(header);
}

bool IsStartUpArchiveFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> header(4);
    if (!f || !f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())))
    {
        return false;
    }

    return IsStartUpArchive(header);
}

GameInput::GameInput(Game game)
    : game_(std::move(game)),
      banks_(game_.Archive(), [this](const std::string& name) { return game_.ReadWaveArchive(name); })
{
}

void GameInput::PrintWarnings(const std::string& name) const
{
    for (const std::string& w : game_.Archive().Warnings())
    {
        std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), w.c_str());
    }
    for (const std::string& w : game_.Warnings())
    {
        std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), w.c_str());
    }
}

Conversion GameInput::Convert(std::size_t index, const PerformOptions& perform, const std::string& program)
{
    Conversion c = ConvertSequence(game_.Sequences()[index], banks_, perform, program);
    for (; errors_reported_ < banks_.Errors().size(); errors_reported_++)
    {
        c.warnings.push_back(banks_.Errors()[errors_reported_]);
    }

    return c;
}

GameConverter MakeGameConverter(const std::string& program, const std::string& version_line,
                                OptionHandler<PerformOptions> option, std::function<void()> usage)
{
    GameConverter converter;
    converter.program = program;
    converter.version_line = version_line;
    converter.input_word = "input";
    converter.folder_message = " is a folder: give its .baa or .aaf file";
    converter.samples_folder = kSfzSamplesFolder;
    converter.usage = std::move(usage);
    converter.option = std::move(option);
    converter.open = [program](const std::filesystem::path& input, const std::string& name)
    {
        return Opened(Game::FromFolder(input, program), name);
    };
    converter.sequences = [](const std::unique_ptr<GameInput>& input)
    {
        std::vector<Sequence> sequences;
        for (std::size_t i = 0; i < input->Files().Sequences().size(); i++)
        {
            sequences.push_back({i, input->Files().Sequences()[i].name});
        }

        return sequences;
    };
    converter.convert = [program](std::unique_ptr<GameInput>& input, std::size_t index, const PerformOptions& perform)
    {
        return input->Convert(index, perform, program);
    };

    return converter;
}

OptionHandler<PerformOptions> ArchiveProgramOptions(OptionHandler<sequence_export::PerformOptions> archive_options)
{
    return [archive_handler = std::move(archive_options)](const std::string& option, OptionReader& reader,
                                                          PerformOptions& perform)
    {
        sequence_export::PerformOptions archive;
        if (!archive_handler || !archive_handler(option, reader, archive))
        {
            return false;
        }

        if (option == "--voices")
        {
            perform.voice_limit = archive.voice_limit;
        }

        return true;
    };
}

OtherInput<sequence_export::PerformOptions> ArchiveProgramGames(const std::vector<std::string>& args,
                                                                const GameConverter& games)
{
    const GameConverter* g = &games;

    return [g, options = ParseOptions<PerformOptions>(args, games.option)](
               const std::string& path, const Options<sequence_export::PerformOptions>&, const std::regex& only,
               bool several, std::vector<std::pair<std::filesystem::path, std::string>>& used_folders)
    {
        return IsStartUpArchiveFile(PathFromUtf8(path))
                   ? std::optional<int>(ConvertInput(path, options, only, several, used_folders, *g))
                   : std::nullopt;
    };
}

GameConverter MakeOpenGameConverter(const GameConverter& converter, std::function<std::optional<Game>()> open)
{
    GameConverter opened = converter;
    opened.open = [open_game = std::move(open)](const std::filesystem::path&,
                                                const std::string& name) -> std::optional<std::unique_ptr<GameInput>>
    {
        std::optional<Game> game = open_game();
        if (!game)
        {
            return std::nullopt;
        }

        return Opened(std::move(*game), name);
    };

    return opened;
}

} // namespace ntdlib::sequence_export::jaudio
