// SPDX-License-Identifier: MIT

// The parts of a path in a game's file system, where '/' separates the folders.

#pragma once

#include <cstddef>
#include <string>

namespace ntdlib::detail
{

// The folder of `path`, with its last '/', or an empty string for a path without a folder.
inline std::string FolderOf(const std::string& path)
{
    const std::size_t slash = path.rfind('/');

    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// The file name of `path`: the part after its last '/', or the whole path for a path without a folder.
inline std::string FileNameOf(const std::string& path)
{
    return path.substr(FolderOf(path).size());
}

// The file name of `path` without its folder and its extension. A name that starts with its only dot keeps it.
inline std::string StemOf(const std::string& path)
{
    std::string name = FileNameOf(path);
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
    {
        name.erase(dot);
    }

    return name;
}

} // namespace ntdlib::detail
