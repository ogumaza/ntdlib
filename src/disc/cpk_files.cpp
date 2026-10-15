// SPDX-License-Identifier: MIT

// Lists and reads the sound archives inside a game's CPK archives (see cpk_files.h).

#include "disc/cpk_files.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"

#include "disc/file_table.h"
#include "ntdlib/disc/cpk.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/disc/source.h"

namespace ntdlib::disc::detail
{
namespace
{

// A sound archive inside a CPK: the reads of the CPK, and the archive's entry in the CPK's TOC.
struct InnerFile
{
    CpkRead read;
    CpkFile file;
};

} // namespace

void AddCpkSoundArchives(GameFiles& out, const ReadFileRange& read_range)
{
    // A sound archive is listed at the CPK's path, a slash and its path in the CPK. It takes the CPK's index.
    auto inner = std::make_shared<std::map<std::string, InnerFile>>();
    std::vector<GameFile> found;
    for (const GameFile& cpk : out.files)
    {
        if (!xsflib::AsciiLower(cpk.path).ends_with(".cpk"))
        {
            continue;
        }

        CpkRead read = [read_range, path = cpk.path](uint64_t offset, std::size_t size)
        {
            return read_range(path, offset, size);
        };
        std::vector<CpkFile> files;
        try
        {
            files = ListCpk(read, cpk.size);
        }
        catch (const Error&)
        {
            continue;
        }

        for (const CpkFile& file : files)
        {
            const std::string path = cpk.path + "/" + file.path;
            if (xsflib::AsciiLower(file.path).ends_with(".brsar") && inner->emplace(path, InnerFile{read, file}).second)
            {
                found.push_back({path, file.size, cpk.index});
            }
        }
    }

    if (found.empty())
    {
        return;
    }

    out.files.insert(out.files.end(), found.begin(), found.end());
    std::sort(out.files.begin(), out.files.end(), [](const GameFile& a, const GameFile& b) { return a.path < b.path; });
    out.read_file = [inner, read_file = std::move(out.read_file)](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto file = inner->find(path);
        if (file == inner->end())
        {
            return read_file(path, data);
        }

        try
        {
            data = ReadCpkFile(file->second.read, file->second.file, kMaxFileSize);
        }
        catch (const Error& e)
        {
            throw Error(path + ": " + e.what());
        }

        return true;
    };
}

} // namespace ntdlib::disc::detail
