// SPDX-License-Identifier: MIT

// A folder for a test's files in the system's folder for temporary files. The folder is removed at the end.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

namespace ntdlib::testing
{

class TempFolder
{
public:
    // A new folder, named after `test` and the time.
    explicit TempFolder(const std::string& test)
        : path_(std::filesystem::temp_directory_path() /
                (test + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TempFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& Path() const
    {
        return path_;
    }

    // Writes `data` to the file `name` in the folder. Returns the file's path.
    std::filesystem::path Write(const std::string& name, const std::vector<uint8_t>& data) const
    {
        const std::filesystem::path path = path_ / name;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return path;
    }

private:
    std::filesystem::path path_;
};

} // namespace ntdlib::testing
