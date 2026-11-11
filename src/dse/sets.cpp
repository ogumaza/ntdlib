// SPDX-License-Identifier: MIT

#include "ntdlib/dse/sets.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/dse/formats.h"

namespace ntdlib::dse
{

namespace
{

constexpr std::size_t kBankHeaderSize = 0x50;
constexpr uint8_t kMainBank = 1;
constexpr uint8_t kWavesInMainBank = 2;

bool StartsWith(std::span<const uint8_t> bytes, const char* magic)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
}

// The folder of `path`, with its last '/', and its name without the extension.
std::string FolderOf(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

std::string StemOf(const std::string& path)
{
    std::string name = path.substr(FolderOf(path).size());
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
    {
        name.erase(dot);
    }
    return name;
}

// The songs and the wave banks of a game's files, with each song's bank.
class Index
{
public:
    explicit Index(std::span<const NamedFile> files) : files_(files)
    {
        for (std::size_t i = 0; i < files.size(); i++)
        {
            if (StartsWith(files[i].bytes, "smdl"))
            {
                songs_.push_back(i);
            }
            else if (StartsWith(files[i].bytes, "swdl") && files[i].bytes.size() >= kBankHeaderSize)
            {
                banks_.push_back(i);
            }
        }

        // The songs' names in each folder, and the banks by folder and name.
        std::set<std::string> song_names;
        for (const std::size_t i : songs_)
        {
            song_names.insert(FolderOf(files[i].path) + StemOf(files[i].path));
        }
        std::map<std::string, std::size_t> by_name;
        std::map<std::string, std::vector<std::size_t>> lone;
        for (const std::size_t i : banks_)
        {
            const std::string folder = FolderOf(files[i].path);
            const std::string name = folder + StemOf(files[i].path);
            by_name.try_emplace(name, i);
            if (!song_names.contains(name) && files[i].bytes[0x4B] != kMainBank)
            {
                lone[folder].push_back(i);
            }
        }

        for (const std::size_t i : songs_)
        {
            SongSet set{.song = i};
            const std::string folder = FolderOf(files[i].path);
            if (const auto found = by_name.find(folder + StemOf(files[i].path)); found != by_name.end())
            {
                set.bank = found->second;
            }
            else if (const auto others = lone.find(folder); others != lone.end() && others->second.size() == 1)
            {
                set.bank = others->second[0];
            }
            if (set.bank)
            {
                set.main_bank = MainBank(*set.bank);
                songs_banks_.insert(*set.bank);
            }
            song_sets_.push_back(set);
        }
    }

    const std::vector<SongSet>& SongSets() const
    {
        return song_sets_;
    }

    // The main bank of `bank`: a main bank of the id that `bank` gives, the one in its folder, or else the first.
    std::optional<std::size_t> MainBank(std::size_t bank) const
    {
        const std::span<const uint8_t> header = files_[bank].bytes;
        if (header[0x4B] != kWavesInMainBank)
        {
            return std::nullopt;
        }
        const uint16_t id = xsflib::LoadLe16(&header[0x40]);
        std::optional<std::size_t> found;
        for (const std::size_t i : banks_)
        {
            const std::span<const uint8_t> other = files_[i].bytes;
            if (other[0x4B] != kMainBank || xsflib::LoadLe16(&other[0x0E]) != id)
            {
                continue;
            }
            if (FolderOf(files_[i].path) == FolderOf(files_[bank].path))
            {
                return i;
            }
            found = found ? found : i;
        }
        return found;
    }

    // The bank of `id` for the sound effects of file `effects` (FindEffectSets).
    std::optional<std::size_t> EffectBank(std::size_t effects, uint16_t id) const
    {
        char named[8];
        std::snprintf(named, sizeof(named), "%03d", id & 0xFF);
        std::vector<std::size_t> found;
        std::optional<std::size_t> song_bank;
        for (const std::size_t i : banks_)
        {
            if (xsflib::LoadLe16(&(files_[i].bytes)[0x0E]) != id)
            {
                continue;
            }
            if (FolderOf(files_[i].path) == FolderOf(files_[effects].path))
            {
                return i;
            }
            if (!songs_banks_.contains(i))
            {
                found.push_back(i);
            }
            else if (!song_bank)
            {
                song_bank = i;
            }
        }
        for (const std::size_t i : found)
        {
            if (StemOf(files_[i].path) == named)
            {
                return i;
            }
        }
        return found.empty() ? song_bank : std::optional<std::size_t>(found[0]);
    }

private:
    std::span<const NamedFile> files_;
    std::vector<std::size_t> songs_;
    std::vector<std::size_t> banks_;
    std::vector<SongSet> song_sets_;
    std::set<std::size_t> songs_banks_; // the banks that songs play
};

} // namespace

std::vector<SongSet> FindSongSets(std::span<const NamedFile> files)
{
    return Index(files).SongSets();
}

std::vector<EffectSet> FindEffectSets(std::span<const NamedFile> files)
{
    const Index index(files);
    std::vector<EffectSet> sets;
    for (std::size_t i = 0; i < files.size(); i++)
    {
        if (!StartsWith(files[i].bytes, "sedl"))
        {
            continue;
        }

        EffectSet set{.effects = i};
        try
        {
            const SoundEffects effects = SoundEffects::Parse(files[i].bytes);
            for (std::size_t slot = 0; slot < effects.sequences.size(); slot++)
            {
                EffectBanks& banks = set.slots.emplace_back();
                for (const uint16_t id : effects.BankIds(slot))
                {
                    const std::optional<std::size_t> bank = index.EffectBank(i, id);
                    if (!bank)
                    {
                        banks.missing.push_back(id);
                        continue;
                    }
                    banks.banks.push_back(*bank);
                    if (!banks.main_bank)
                    {
                        banks.main_bank = index.MainBank(*bank);
                    }
                }
            }
        }
        catch (const xsflib::FormatError&)
        {
            set.slots.clear();
        }
        sets.push_back(std::move(set));
    }
    return sets;
}

std::optional<SequenceSet> FindSequenceSet(std::span<const NamedFile> files, const SequenceRef& sequence)
{
    if (!sequence.effect)
    {
        for (const SongSet& set : FindSongSets(files))
        {
            if (files[set.song].path == sequence.path)
            {
                SequenceSet found{.file = set.song, .main_bank = set.main_bank};
                if (set.bank)
                {
                    found.banks.push_back(*set.bank);
                }
                return found;
            }
        }
        return std::nullopt;
    }

    for (EffectSet& set : FindEffectSets(files))
    {
        if (files[set.effects].path == sequence.path && *sequence.effect < set.slots.size())
        {
            EffectBanks& slot = set.slots[*sequence.effect];
            return SequenceSet{.file = set.effects, .banks = std::move(slot.banks), .main_bank = slot.main_bank};
        }
    }
    return std::nullopt;
}

} // namespace ntdlib::dse
