// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/frame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/cli/tool.h"
#include "xsflib/files.h"
#include "xsflib/text.h"

namespace ntdlib::sequence_export
{

namespace fs = std::filesystem;

std::optional<std::vector<uint8_t>> ReadFile(const fs::path& path)
{
    std::vector<uint8_t> data;
    if (!xsflib::ReadFile(path, data))
    {
        return std::nullopt;
    }

    return data;
}

void WriteFile(const fs::path& path, std::span<const uint8_t> data)
{
    if (const std::optional<std::string> error = xsflib::WriteFile(path, data))
    {
        throw std::runtime_error(*error);
    }
}

void WriteFile(const fs::path& path, const std::string& text)
{
    WriteFile(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::string Duration(double seconds)
{
    int m = static_cast<int>(seconds / 60.0);
    double s = seconds - m * 60.0;
    if (s >= 59.95) // %04.1f would round it up to 60.0
    {
        m++;
        s = 0.0;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%04.1f", m, s);

    return buf;
}

int RunMain(int argc, char** argv, const std::function<int(const std::vector<std::string>&)>& run)
{
    const xsflib::cli::ToolConsole console{.restore_code_page = false,
                                           .line_buffered = true,
                                           .prompt_stream = stdout,
                                           .prompt = "\nPress Enter to close.\n"};
    return xsflib::cli::RunTool([&] { return run(Arguments(argc, argv)); }, console);
}

OptionReader::OptionReader(const std::vector<std::string>& args, std::size_t& i) : args_(args), i_(i), option_(args[i])
{
}

std::string OptionReader::Value()
{
    if (i_ + 1 >= args_.size())
    {
        throw std::runtime_error(option_ + " requires a value");
    }

    return args_[++i_];
}

uint32_t OptionReader::Number(uint32_t max)
{
    return Parse(Value(), 0, max, "a number");
}

uint32_t OptionReader::CountOrOff(uint32_t max)
{
    const std::string v = Value();
    if (v == "off")
    {
        return 0;
    }

    return Parse(v, 1, max, "a number or off");
}

uint32_t OptionReader::Parse(const std::string& value, uint32_t min, uint32_t max, const std::string& expected) const
{
    // Decimal, or hex after a 0x prefix, with no sign.
    xsflib::IntegerError error = xsflib::IntegerError::kNotNumber;
    const std::optional<int64_t> n = ParseInteger(value, {.minus = false}, &error);
    if (!n && error == xsflib::IntegerError::kNotNumber)
    {
        throw std::runtime_error(option_ + " requires " + expected + ", not " + value);
    }
    if (!n || *n < min || *n > max)
    {
        throw std::runtime_error(option_ + " " + value + " is out of range");
    }

    return static_cast<uint32_t>(*n);
}

std::pair<uint8_t, int64_t> ParseAssignment(const std::string& option, const std::string& value, int64_t max_index,
                                            int64_t min, int64_t max)
{
    const std::optional<xsflib::Assignment> assignment = xsflib::ParseAssignment(value);
    if (!assignment)
    {
        throw std::runtime_error(option + " requires N=V, not " + value);
    }
    if (assignment->index < 0 || assignment->index > max_index || assignment->value < min || assignment->value > max)
    {
        throw std::runtime_error(option + " " + value + " is out of range");
    }

    return {static_cast<uint8_t>(assignment->index), assignment->value};
}

std::vector<Sequence> MatchingSequences(std::vector<Sequence> sequences, const std::regex& only)
{
    std::vector<Sequence> selected;
    const auto matches = [&only](const std::string& text)
    {
        return std::regex_search(text, only);
    };
    for (Sequence& sequence : sequences)
    {
        if (matches(sequence.name) || std::ranges::any_of(sequence.labels, matches))
        {
            selected.push_back(std::move(sequence));
        }
    }

    return selected;
}

void ListSequences(const std::string& name, const std::vector<Sequence>& selected, bool several)
{
    if (several)
    {
        std::printf("%s:\n", name.c_str());
    }
    for (const Sequence& sequence : selected)
    {
        std::printf("%s%s\n", several ? "  " : "", sequence.name.c_str());
    }
}

fs::path FolderPath(fs::path input)
{
    // A folder's path may end in a separator, "." or "..". Its name and its output folder come from the folder
    // itself. canonical() names the folder that the system opens. macOS resolves a ".." after a symbolic link from
    // the link's target. lexically_normal() would give the folder that contains the link. Windows removes a ".."
    // and the name before it from the path. So does lexically_normal(). Some file systems on Windows can't give
    // canonical() a path. lexically_normal() then names the same folder.
    if (!input.has_filename())
    {
        input = input.parent_path();
    }
    if (input.filename() == "." || input.filename() == "..")
    {
        std::error_code error;
        std::filesystem::path resolved = std::filesystem::canonical(input, error);
        if (error)
        {
            resolved = std::filesystem::absolute(input, error).lexically_normal();
        }
        input = std::move(resolved);
        if (!input.has_filename())
        {
            input = input.parent_path();
        }
    }

    return input;
}

void CheckOutputFolder(const fs::path& out_dir, const std::vector<std::pair<fs::path, std::string>>& used_folders)
{
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
}

} // namespace ntdlib::sequence_export
