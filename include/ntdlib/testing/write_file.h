// SPDX-License-Identifier: MIT

// The writer of a test's files: xsflib's WriteFile, with the file's folder made first and a failure thrown.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/files.h"

namespace ntdlib::testing
{

// Writes `data` to the file at `path`, and makes the file's folder first. Throws std::runtime_error when the file can't
// be written.
inline void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    std::filesystem::create_directories(path.parent_path());
    if (const std::optional<std::string> error = xsflib::WriteFile(path, data))
    {
        throw std::runtime_error(*error);
    }
}

} // namespace ntdlib::testing
