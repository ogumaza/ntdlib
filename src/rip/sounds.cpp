// SPDX-License-Identifier: MIT

// The archive mode's choice of the sounds to rip (see archive.h): the sounds that a rip selects, the data that they
// require, and the groups outside an archive.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"

#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/names.h"
#include "ntdlib/rip/player_names.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::rip
{

std::vector<std::string> ExternalGroups(const sar::SoundArchive& archive)
{
    std::vector<std::string> names;
    for (const sar::GroupInfo& group : archive.Groups())
    {
        if (group.file_id == sar::kExternalGroupFile && !group.name.empty())
        {
            names.push_back(group.name);
        }
    }

    return names;
}

std::vector<std::string> MissingData(const sar::SoundArchive& archive, uint32_t index, const sar::Options& options)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const sar::SoundInfo& sound = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (archive.FileData(sound.file_id).empty())
    {
        missing.push_back("its sequence data");
    }

    // Each bank, and the wave archives its waves are in. An empty slot requires nothing.
    std::set<uint32_t> wave_archives;
    for (const uint32_t item : sound.banks)
    {
        if (item == sar::kNoItem)
        {
            continue;
        }

        const std::optional<std::size_t> bank = ItemIndex(item, kBankItem, archive.Banks().size());
        if (!bank)
        {
            missing.push_back("a bank the archive doesn't list");
            continue;
        }

        const sar::BankInfo& info = archive.Banks()[*bank];
        const std::span<const uint8_t> file = archive.FileData(info.file_id);
        const std::string name = NameOrNumber(info.name, *bank);
        if (file.empty())
        {
            missing.push_back("bank " + name);
            continue;
        }

        try
        {
            for (const sar::WaveId& wave : sar::Bank::Parse(file, options).waves)
            {
                wave_archives.insert(wave.wave_archive_item);
            }
        }
        catch (const std::exception&)
        {
            missing.push_back("bank " + name + " (can't be read)");
        }
    }

    for (const uint32_t item : wave_archives)
    {
        const std::optional<std::size_t> wave_archive =
            ItemIndex(item, kWaveArchiveItem, archive.WaveArchives().size());
        if (!wave_archive)
        {
            missing.push_back("a wave archive the archive doesn't list");
        }
        else if (archive.FileData(archive.WaveArchives()[*wave_archive].file_id).empty())
        {
            missing.push_back("wave archive " +
                              NameOrNumber(archive.WaveArchives()[*wave_archive].name, *wave_archive));
        }
    }

    return missing;
}

bool IsMusic(const sar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size() || archive.Sounds()[index].type != sar::SoundType::kSequence)
    {
        return false;
    }

    const sar::SoundInfo& sound = archive.Sounds()[index];
    if (xsflib::AsciiLower(sound.name).find("bgm") != std::string::npos)
    {
        return true;
    }

    const std::optional<std::size_t> player = ItemIndex(sound.player_id, kPlayerItem, archive.Players().size());
    if (!player)
    {
        return false;
    }

    return IsMusicPlayerName(archive.Players()[*player].name);
}

std::vector<Job> SelectSounds(const sar::SoundArchive& archive, const std::regex& only, bool bgm)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope. Wave sounds aren't ripped yet.
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type == sar::SoundType::kSequence && std::regex_search(sounds[i].name, only) &&
            (!bgm || IsMusic(archive, index)))
        {
            jobs.push_back({.index = index, .label = sounds[i].name});
        }
    }

    return jobs;
}

std::optional<sar::SoundArchive> LoadArchiveToRip(const std::vector<uint8_t>& bytes, const std::string& path,
                                                  const sar::Options& options)
{
    std::optional<sar::SoundArchive> archive;
    try
    {
        archive = sar::SoundArchive::Load(bytes, options);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
        return std::nullopt;
    }

    if (archive->Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated: sounds whose data lies past its end are skipped\n",
                     path.c_str());
    }

    return archive;
}

std::vector<Job> SkipIncomplete(const sar::SoundArchive& archive, std::vector<Job> jobs, const sar::Options& options,
                                Skipped& skipped)
{
    std::vector<Job> complete;
    for (Job& job : jobs)
    {
        const std::vector<std::string> missing = MissingData(archive, job.index, options);
        if (!missing.empty())
        {
            skipped.missing_data++;
            std::string what;
            for (std::size_t i = 0; i < missing.size(); i++)
            {
                what += (i ? ", " : "") + missing[i];
            }
            std::fprintf(stderr, "skipped %s: missing %s\n", job.label.c_str(), what.c_str());
        }
        else if (archive.Sounds()[job.index].volume == 0)
        {
            skipped.silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());
        }
        else
        {
            complete.push_back(std::move(job));
        }
    }

    return complete;
}

} // namespace ntdlib::rip
