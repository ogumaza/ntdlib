// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/frame.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sarc.h"
#include "xsflib/binary.h"
#include "xsflib/cli/tool.h"

namespace ntdlib::sequence_export
{

namespace fs = std::filesystem;

namespace
{

// Adds `archive`'s group files in the .sarc files in `folder` and returns how many it added. The SARCs are read in the
// order of their paths' UTF-8 bytes. A copy of a group file that the archive already has changes nothing.
int AddSarcGroupFiles(sar::SoundArchive& archive, const fs::path& folder)
{
    std::vector<std::string> sarcs;
    std::error_code error;
    for (fs::directory_iterator entry(folder.empty() ? fs::path(".") : folder, error);
         !error && entry != fs::directory_iterator(); entry.increment(error))
    {
        if (entry->path().extension() == ".sarc")
        {
            sarcs.push_back(Utf8(entry->path()));
        }
    }

    std::sort(sarcs.begin(), sarcs.end());
    int added = 0;
    for (const std::string& path : sarcs)
    {
        const std::optional<std::vector<uint8_t>> sarc = ReadFile(PathFromUtf8(path));
        if (!sarc)
        {
            continue;
        }

        for (const sar::SarcFile& file : sar::SarcFiles(*sarc))
        {
            if (archive.MatchesGroupFile(file.data))
            {
                archive.AddGroupFile(std::vector<uint8_t>(file.data.begin(), file.data.end()));
                added++;
            }
        }
    }

    return added;
}

} // namespace

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

int AddGroupFiles(sar::SoundArchive& archive, const fs::path& input, const std::string& name)
{
    const std::vector<std::string>& groups = archive.ExternalGroups();
    if (groups.empty())
    {
        return 0;
    }

    int found = 0;
    for (const std::string& group : groups)
    {
        for (const fs::path& folder : {input.parent_path(), input.parent_path() / "userManagementFiles"})
        {
            const fs::path path = folder / PathFromUtf8(sar::GroupFileName(seqlib::SafeFileName(group)));
            std::optional<std::vector<uint8_t>> bytes = ReadFile(path);
            if (!bytes)
            {
                continue;
            }

            try
            {
                archive.AddGroupFile(std::move(*bytes));
                found++;
            }
            catch (const xsflib::FormatError& e)
            {
                std::fprintf(stderr, "warning: %s isn't a group file (%s)\n", Utf8(path).c_str(), e.what());
            }
            break;
        }
    }

    // A game can also keep its group files in SARC archives beside the archive, under other names.
    if (static_cast<std::size_t>(found) < groups.size())
    {
        found += AddSarcGroupFiles(archive, input.parent_path());
    }

    if (found == 0)
    {
        std::fprintf(stderr,
                     "warning: %s keeps %zu groups in files of their own, and none of them is beside it (as "
                     "<group>.bfgrp, or in a userManagementFiles folder)\n",
                     name.c_str(), groups.size());
    }

    return found;
}

} // namespace ntdlib::sequence_export
