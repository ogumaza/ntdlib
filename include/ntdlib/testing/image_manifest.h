// SPDX-License-Identifier: MIT

// A test of a disc's extracted files against their manifest, and of the disc's image too when it's there. The game
// folder holds sys/ and files/ in the layout that LoadGame reads, and MANIFEST.txt. The manifest names the image
// ("source: NAME") and gives a line for each file: its SHA-1, its size and its path in the folder.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/disc/game.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

namespace ntdlib::testing
{

// A game folder's MANIFEST.txt.
struct ImageManifest
{
    std::string source;                        // the image's file name
    std::map<std::string, std::string> hashes; // path in the folder -> SHA-1
};

inline ImageManifest ReadImageManifest(const std::filesystem::path& path)
{
    ImageManifest manifest;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line))
    {
        if (line.starts_with("source: "))
        {
            manifest.source = line.substr(8);
            continue;
        }

        std::istringstream fields(line);
        std::string hash;
        uint64_t size = 0;
        std::string name;
        if (fields >> hash >> size >> name && hash.size() == 40)
        {
            manifest.hashes[name] = hash;
        }
    }

    return manifest;
}

// Checks the game that LoadGame reads from `input` with `options` against the manifest: the system files, a Wii
// partition's TMD, and every file that the manifest lists under files/. Prints a line for the input.
inline void CheckGameAgainstManifest(const std::string& name, const std::filesystem::path& input,
                                     const ImageManifest& manifest, const disc::GameOptions& options)
{
    disc::GameFiles game;
    const auto error = disc::LoadGame(input, game, options);
    xsflib::testing::Check(!error, name + ": " + error.value_or(""));
    if (error)
    {
        return;
    }

    const auto hash = [](const std::vector<uint8_t>& data)
    {
        return xsflib::Sha1::Hex(xsflib::Sha1::Of(data));
    };
    const std::map<std::string, const std::vector<uint8_t>*> system_files = {
        {"tmd.bin", &game.tmd},      {"sys/boot.bin", &game.boot},
        {"sys/bi2.bin", &game.bi2},  {"sys/apploader.img", &game.apploader},
        {"sys/main.dol", &game.dol}, {"sys/fst.bin", &game.fst},
    };
    int files = 0;
    for (const auto& [path, expected] : manifest.hashes)
    {
        if (const auto system = system_files.find(path); system != system_files.end())
        {
            xsflib::testing::Check(hash(*system->second) == expected, name + ": " + path);
            continue;
        }

        std::vector<uint8_t> data;
        xsflib::testing::Check(
            path.starts_with("files/") && game.read_file(path.substr(6), data) && hash(data) == expected,
            name + ": " + path);
        ++files;
    }

    std::printf("%s: %s, the system files and %d file%s match the manifest\n", name.c_str(), game.source.c_str(), files,
                files == 1 ? "" : "s");
}

// An image test's main, with the arguments <game folder> <roms folder>: checks the folder, then the image in the roms
// folder when it's there. Returns the exit code.
inline int RunImageTest(int argc, char** argv, const disc::GameOptions& options)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: image_test <game folder> <roms folder>\n");
        return 2;
    }

    const std::filesystem::path folder = argv[1];
    const ImageManifest manifest = ReadImageManifest(folder / "MANIFEST.txt");
    xsflib::testing::Check(!manifest.hashes.empty(), "the manifest lists files");
    CheckGameAgainstManifest(folder.filename().string(), folder, manifest, options);

    std::error_code error;
    const std::filesystem::path image = std::filesystem::path(argv[2]) / manifest.source;
    if (std::filesystem::is_regular_file(image, error))
    {
        CheckGameAgainstManifest(manifest.source, image, manifest, options);
    }
    else
    {
        std::printf("%s isn't here. Only the folder was checked.\n", manifest.source.c_str());
    }

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    return 0;
}

} // namespace ntdlib::testing
