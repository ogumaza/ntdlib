// SPDX-License-Identifier: MIT

// A sound archive and its group files, loaded from a set's files: the first step of archive mode when the set holds the
// archive (see xsflib/player/archive_source.h).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// Loads the sound archive at `archive_path` among a set's files, then adds the group files at `group_paths` in order.
// `set.File(path)` gives the set's file at `path`, or null when the set has none. The bytes of the archive and of the
// group files move out of the set. Returns "the set has no file P" for the first path that the set lacks, and then
// leaves `archive` empty. Otherwise `archive` holds the archive with its group files added. Load and AddGroupFile throw
// FormatError for a file that isn't an archive or a group file.
template <typename Set>
std::optional<std::string> LoadSetArchive(Set& set, const std::string& archive_path,
                                          const std::vector<std::string>& group_paths, const Options& options,
                                          std::optional<SoundArchive>& archive)
{
    std::vector<uint8_t>* bytes = set.File(archive_path);
    if (!bytes)
    {
        return "the set has no file " + archive_path;
    }

    SoundArchive loaded = SoundArchive::Load(std::move(*bytes), options);
    for (const std::string& path : group_paths)
    {
        std::vector<uint8_t>* group = set.File(path);
        if (!group)
        {
            return "the set has no file " + path;
        }

        loaded.AddGroupFile(std::move(*group));
    }

    archive.emplace(std::move(loaded));

    return std::nullopt;
}

} // namespace ntdlib::sar
