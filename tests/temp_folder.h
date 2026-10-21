// SPDX-License-Identifier: MIT

// A folder for a test's files in the system's folder for temporary files. The folder is removed at the end.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
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

} // namespace ntdlib::testing
