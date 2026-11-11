// SPDX-License-Identifier: MIT

// A folder for a test's files in the system's folder for temporary files. The folder is removed at the end.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/testing/temp_folder.h"

namespace ntdlib::testing
{

// xsflib's folder for a test's files, with a writer of files in it.
class TempFolder : public xsflib::testing::TempFolder
{
public:
    // A new folder, named after `test` and the time.
    using xsflib::testing::TempFolder::TempFolder;

    // Writes `data` to the file `name` in the folder. Returns the file's path. Throws std::runtime_error when the file
    // can't be written.
    std::filesystem::path Write(const std::string& name, const std::vector<uint8_t>& data) const
    {
        const std::filesystem::path path = Path() / name;
        if (const std::optional<std::string> error = xsflib::WriteFile(path, data))
        {
            throw std::runtime_error(*error);
        }

        return path;
    }
};

// Empty folders that the test's process can't open. Each folder takes no permissions, and gets its owner's back at the
// end, before its temporary folder goes. A system that keeps a folder open to its owner, such as Windows, leaves them
// readable.
class LockedFolders
{
public:
    explicit LockedFolders(std::vector<std::filesystem::path> folders) : folders_(std::move(folders))
    {
        for (const std::filesystem::path& folder : folders_)
        {
            std::filesystem::create_directories(folder);
            std::error_code ignored;
            std::filesystem::permissions(folder, std::filesystem::perms::none, ignored);
        }
    }

    LockedFolders(const LockedFolders&) = delete;
    LockedFolders& operator=(const LockedFolders&) = delete;

    ~LockedFolders()
    {
        for (const std::filesystem::path& folder : folders_)
        {
            std::error_code ignored;
            std::filesystem::permissions(folder, std::filesystem::perms::owner_all, ignored);
        }
    }

private:
    std::vector<std::filesystem::path> folders_;
};

} // namespace ntdlib::testing
