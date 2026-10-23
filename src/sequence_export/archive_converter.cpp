// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/archive_converter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sequence_export/frame.h"

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
                     "<group>.bfgrp, in a userManagementFiles folder or in a .sarc file)\n",
                     name.c_str(), groups.size());
    }

    return found;
}

} // namespace ntdlib::sequence_export
