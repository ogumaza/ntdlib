// SPDX-License-Identifier: MIT

#include "ntdlib/dse/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/sets.h"

namespace ntdlib::dse::rip
{

namespace
{

constexpr std::size_t kBankHeaderSize = 0x50;
constexpr uint8_t kWavesInMainBank = 2;
constexpr uint32_t kIdBits = 0xFFFF;

std::string StemOf(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
    {
        name.erase(dot);
    }
    return name;
}

// Whether the slot `slot` of the bank of sound effects `file` holds a sequence. A bank that can't be read holds none.
bool HoldsEffect(std::span<const uint8_t> file, std::size_t slot)
{
    try
    {
        const SoundEffects effects = SoundEffects::Parse(file);
        return slot < effects.sequences.size() && effects.sequences[slot].has_value();
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }
}

// The paths of `files` in sorted order.
std::vector<std::string> SortedPaths(std::span<const NamedFile> files)
{
    std::vector<std::string> paths;
    for (const NamedFile& file : files)
    {
        paths.push_back(file.path);
    }
    std::ranges::sort(paths);
    return paths;
}

// The paths of the files that `set` names.
std::vector<std::string> PathsOf(std::span<const NamedFile> files, const SequenceSet& set)
{
    std::vector<std::string> paths = {files[set.file].path};
    for (const std::size_t bank : set.banks)
    {
        paths.push_back(files[bank].path);
    }
    paths.push_back(set.main_bank ? files[*set.main_bank].path : "");
    return paths;
}

} // namespace

std::string Label(const SequenceRef& sequence)
{
    std::string label = StemOf(sequence.path);
    if (sequence.effect)
    {
        char number[16];
        std::snprintf(number, sizeof(number), "_%03u", static_cast<unsigned>(*sequence.effect));
        label += number;
    }
    return label;
}

bool IsMusic(const SequenceRef& sequence)
{
    return !sequence.effect;
}

std::vector<Job> SelectSequences(std::span<const NamedFile> files, const std::regex& only, bool bgm)
{
    std::vector<SequenceRef> sequences;
    for (const SongSet& set : FindSongSets(files))
    {
        sequences.push_back({files[set.song].path, std::nullopt});
    }
    if (!bgm)
    {
        for (const EffectSet& set : FindEffectSets(files))
        {
            std::optional<SoundEffects> effects;
            try
            {
                effects = SoundEffects::Parse(files[set.effects].bytes);
            }
            catch (const xsflib::FormatError&)
            {
                continue;
            }
            for (std::size_t slot = 0; slot < effects->sequences.size() && slot < kIdBits; slot++)
            {
                if (effects->sequences[slot])
                {
                    sequences.push_back({files[set.effects].path, static_cast<uint16_t>(slot)});
                }
            }
        }
    }

    std::vector<Job> jobs;
    for (SequenceRef& sequence : sequences)
    {
        std::string label = Label(sequence);
        if (std::regex_search(label, only))
        {
            jobs.push_back({std::move(sequence), std::move(label), std::nullopt});
        }
    }
    return jobs;
}

std::vector<std::string> MissingData(std::span<const NamedFile> files, const SequenceRef& sequence)
{
    if (std::ranges::none_of(files, [&](const NamedFile& file) { return file.path == sequence.path; }))
    {
        return {"the file " + sequence.path};
    }
    const std::optional<SequenceSet> set = FindSequenceSet(files, sequence);
    if (!set || (sequence.effect && !HoldsEffect(files[set->file].bytes, *sequence.effect)))
    {
        return {sequence.effect ? "sound effect " + std::to_string(*sequence.effect) : "the song"};
    }

    std::vector<std::string> missing;
    if (!sequence.effect && set->banks.empty())
    {
        missing.push_back("the song's wave bank");
    }
    if (sequence.effect)
    {
        for (const EffectSet& effects : FindEffectSets(files))
        {
            if (effects.effects == set->file)
            {
                for (const uint16_t id : effects.slots[*sequence.effect].missing)
                {
                    missing.push_back("wave bank " + xsflib::Hex(id, 4, true));
                }
            }
        }
    }
    for (const std::size_t bank : set->banks)
    {
        const std::span<const uint8_t> header = files[bank].bytes;
        if (!set->main_bank && header.size() >= kBankHeaderSize && header[0x4B] == kWavesInMainBank)
        {
            missing.push_back("the main bank " + xsflib::Hex(xsflib::LoadLe16(&header[0x40]), 4, true));
            break;
        }
    }
    return missing;
}

std::optional<std::vector<std::size_t>> KeepSequenceData(std::span<const NamedFile> files,
                                                         std::span<const SequenceRef> sequences)
{
    std::set<std::size_t> keep;
    std::vector<SequenceSet> sets;
    for (const SequenceRef& sequence : sequences)
    {
        if (!MissingData(files, sequence).empty())
        {
            return std::nullopt;
        }
        const SequenceSet& set = sets.emplace_back(*FindSequenceSet(files, sequence));
        keep.insert(set.file);
        keep.insert(set.banks.begin(), set.banks.end());
        if (set.main_bank)
        {
            keep.insert(*set.main_bank);
        }
    }

    // Each sequence must find the same files among the kept ones.
    const std::vector<std::size_t> kept(keep.begin(), keep.end());
    std::vector<NamedFile> library;
    for (const std::size_t index : kept)
    {
        library.push_back(files[index]);
    }
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        const std::optional<SequenceSet> found = FindSequenceSet(library, sequences[i]);
        if (!found || PathsOf(library, *found) != PathsOf(files, sets[i]))
        {
            return std::nullopt;
        }
    }
    return kept;
}

uint32_t SoundId(std::span<const NamedFile> files, const SequenceRef& sequence)
{
    const std::vector<std::string> paths = SortedPaths(files);
    const auto found = std::ranges::lower_bound(paths, sequence.path);
    if (found == paths.end() || *found != sequence.path)
    {
        throw std::out_of_range("the files lack " + sequence.path);
    }
    const auto place = static_cast<uint32_t>(found - paths.begin());
    if (place > kIdBits || (sequence.effect && *sequence.effect >= kIdBits))
    {
        throw std::out_of_range("a sound's number can't hold " + Label(sequence));
    }
    return place << 16 | (sequence.effect ? *sequence.effect + 1u : 0u);
}

SequenceRef SequenceOf(std::span<const NamedFile> files, uint32_t sound_id)
{
    const std::vector<std::string> paths = SortedPaths(files);
    const uint32_t place = sound_id >> 16;
    if (place >= paths.size())
    {
        throw std::out_of_range("the files have no file " + std::to_string(place));
    }
    const uint32_t slot = sound_id & kIdBits;
    return {paths[place], slot == 0 ? std::nullopt : std::optional<uint16_t>(static_cast<uint16_t>(slot - 1))};
}

} // namespace ntdlib::dse::rip
