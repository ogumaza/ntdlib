// SPDX-License-Identifier: MIT

// Checks LoadSetArchive (ntdlib/sar/set_archive.h) in both byte orders: an archive with a group file that the set holds
// beside it, a set without the archive, and a set without the group file. Also a 3DS archive and its group file from a
// const set.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/set_archive.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

using ntdlib::sar::LoadSetArchive;
using ntdlib::sar::SoundArchive;
using xsflib::testing::Check;

namespace sar_files = ntdlib::testing::sar_files;

// A set's files by path, through the File function that LoadSetArchive calls.
struct Set
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto it = files.find(path);
        return it == files.end() ? nullptr : &it->second;
    }

    std::map<std::string, std::vector<uint8_t>> files;
};

// A set that a player reads without changing it: File gives its files as const.
struct ConstSet
{
    const std::vector<uint8_t>* File(const std::string& path) const
    {
        const auto it = files.find(path);
        return it == files.end() ? nullptr : &it->second;
    }

    std::map<std::string, std::vector<uint8_t>> files;
};

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool StartsWith(std::span<const uint8_t> data, const std::vector<uint8_t>& start)
{
    return data.size() >= start.size() && std::equal(start.begin(), start.end(), data.begin());
}

// An archive whose second file only a group holds. The game keeps the group outside the archive.
Set GroupedSet(bool big_endian)
{
    sar_files::ArchiveSpec spec;
    spec.files = {{Bytes("a file the archive holds"), "", false}, {{}, "", true}};
    spec.groups = {{"GROUP_TEST", 0xFFFFFFFF}};

    Set set;
    set.files["Sound.bfsar"] = sar_files::Archive(big_endian, spec);
    set.files["group/GROUP_TEST.bfgrp"] = sar_files::Group(big_endian, {{1, Bytes("a file only the group holds")}});

    return set;
}

void TestLoad(bool big_endian)
{
    const std::string order = big_endian ? "big-endian: " : "little-endian: ";
    ntdlib::sar::Options options;
    options.flavor = big_endian ? ntdlib::sar::Flavor::kCafe : ntdlib::sar::Flavor::kNx;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;
    const std::vector<std::string> kGroups = {"group/GROUP_TEST.bfgrp"};

    // The archive and its group file. Their bytes move out of the set.
    Set set = GroupedSet(big_endian);
    std::optional<SoundArchive> archive;
    const std::optional<std::string> loaded = LoadSetArchive(set, "Sound.bfsar", kGroups, options, archive);
    Check(!loaded && archive && StartsWith(archive->FileData(0), Bytes("a file the archive holds")) &&
              StartsWith(archive->FileData(1), Bytes("a file only the group holds")),
          order + "the archive with its group file");
    Check(set.files["Sound.bfsar"].empty() && set.files["group/GROUP_TEST.bfgrp"].empty(),
          order + "the files' bytes move out of the set");

    // A set without the archive.
    Set no_archive = GroupedSet(big_endian);
    no_archive.files.erase("Sound.bfsar");
    std::optional<SoundArchive> none;
    Check(
        LoadSetArchive(no_archive, "Sound.bfsar", kGroups, options, none) == "the set has no file Sound.bfsar" && !none,
        order + "a set without the archive");

    // A set without the group file. The archive's bytes have moved out by then, and the archive stays empty.
    Set no_group = GroupedSet(big_endian);
    no_group.files.erase("group/GROUP_TEST.bfgrp");
    std::optional<SoundArchive> partial;
    Check(LoadSetArchive(no_group, "Sound.bfsar", kGroups, options, partial) ==
                  "the set has no file group/GROUP_TEST.bfgrp" &&
              !partial && no_group.files["Sound.bfsar"].empty(),
          order + "a set without the group file");
}

// A const set keeps its files' bytes, and the archive takes copies of them.
void TestLoadFromConstSet()
{
    namespace sar = ntdlib::sar;
    sar_files::ArchiveSpec spec;
    spec.files = {{Bytes("a file the archive holds"), "", false}, {{}, "", true}};
    spec.groups = {{"GROUP_TEST", sar::kExternalGroupFile}};
    const std::vector<uint8_t> archive_bytes = sar_files::Archive(sar::Flavor::kCtr, spec);
    const std::vector<uint8_t> group_bytes =
        sar_files::Group(sar::Flavor::kCtr, {{1, Bytes("a file only the group holds")}});
    const ConstSet set{{{"Sound.bcsar", archive_bytes}, {"GROUP_TEST.bcgrp", group_bytes}}};
    const sar::Options kOptions{sar::Flavor::kCtr, {}, false, sar::Rules::kStopAtFirstProblem};

    std::optional<SoundArchive> archive;
    const std::optional<std::string> loaded =
        LoadSetArchive(set, "Sound.bcsar", {"GROUP_TEST.bcgrp"}, kOptions, archive);
    Check(!loaded && archive && StartsWith(archive->FileData(0), Bytes("a file the archive holds")) &&
              StartsWith(archive->FileData(1), Bytes("a file only the group holds")),
          "3DS: the archive with its group file, from a const set");
    Check(set.files.at("Sound.bcsar") == archive_bytes && set.files.at("GROUP_TEST.bcgrp") == group_bytes,
          "3DS: a const set keeps its files' bytes");
}

} // namespace

int main()
{
    TestLoad(true);
    TestLoad(false);
    TestLoadFromConstSet();

    return xsflib::testing::Summary("sar_set_archive");
}
