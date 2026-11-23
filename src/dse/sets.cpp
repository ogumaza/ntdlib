// SPDX-License-Identifier: MIT

#include "ntdlib/dse/sets.h"

#include <algorithm>
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

#include "common/magic.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/file_paths.h"

namespace ntdlib::dse
{

namespace
{

constexpr std::size_t kBankHeaderSize = 0x50;
constexpr uint8_t kMainBank = 1;
constexpr uint8_t kWavesInMainBank = 2;

using ntdlib::detail::FolderOf;
using ntdlib::detail::HasMagic;
using ntdlib::detail::StemOf;

// The songs and the wave banks of a game's files, with each song's bank.
class Index
{
public:
    explicit Index(std::span<const NamedFile> files) : files_(files)
    {
        for (std::size_t i = 0; i < files.size(); i++)
        {
            if (HasMagic(files[i].bytes, "smdl"))
            {
                songs_.push_back(i);
            }
            else if (HasMagic(files[i].bytes, "swdl") && files[i].bytes.size() >= kBankHeaderSize)
            {
                banks_.push_back(i);
            }
        }

        // The songs' names in each folder, the folders that hold banks, and the banks by folder and name, by name alone
        // and by id. A main bank is no song's bank.
        std::set<std::string> song_names;
        for (const std::size_t i : songs_)
        {
            song_names.insert(FolderOf(files[i].path) + StemOf(files[i].path));
        }
        std::map<std::string, std::size_t> by_name;
        std::map<std::string, std::vector<std::size_t>> lone;
        std::map<std::string, std::vector<std::size_t>> by_stem;
        std::map<std::optional<uint16_t>, std::vector<std::size_t>> by_id;
        std::set<std::string> bank_folders;
        for (const std::size_t i : banks_)
        {
            const std::string folder = FolderOf(files[i].path);
            const std::string name = folder + StemOf(files[i].path);
            by_name.try_emplace(name, i);
            bank_folders.insert(folder);
            if (files[i].bytes[0x4B] == kMainBank)
            {
                continue;
            }

            if (!song_names.contains(name))
            {
                lone[folder].push_back(i);
            }
            by_stem[StemOf(files[i].path)].push_back(i);
            by_id[*IdOf(i)].push_back(i);
        }

        for (const std::size_t i : songs_)
        {
            SongSet set{.song = i};
            const std::string folder = FolderOf(files[i].path);
            const bool apart = !bank_folders.contains(folder);
            if (const auto found = by_name.find(folder + StemOf(files[i].path)); found != by_name.end())
            {
                set.bank = found->second;
            }
            else if (const auto others = lone.find(folder); others != lone.end() && others->second.size() == 1)
            {
                set.bank = others->second[0];
            }
            else if (const auto named = by_stem.find(StemOf(files[i].path));
                     apart && named != by_stem.end() && named->second.size() == 1)
            {
                set.bank = named->second[0];
            }
            else if (const std::optional<std::size_t> next = NextBank(i))
            {
                set.bank = next;
            }
            else if (const auto same_id = by_id.find(IdOf(i));
                     apart && same_id != by_id.end() && same_id->second.size() == 1)
            {
                set.bank = same_id->second[0];
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

    // The id at 0x0E of file `index`: a bank's id, or a song's. A song's id is often its bank's. Nothing for a file
    // too short to hold one.
    std::optional<uint16_t> IdOf(std::size_t index) const
    {
        const std::span<const uint8_t> bytes = files_[index].bytes;
        if (bytes.size() < 0x10)
        {
            return std::nullopt;
        }

        return xsflib::LoadLe16(&bytes[0x0E]);
    }

    // The file after song `song`, when it is a wave bank in the song's folder, of the song's id, and no main bank.
    std::optional<std::size_t> NextBank(std::size_t song) const
    {
        const std::size_t next = song + 1;
        if (next >= files_.size() || !HasMagic(files_[next].bytes, "swdl") ||
            files_[next].bytes.size() < kBankHeaderSize || files_[next].bytes[0x4B] == kMainBank ||
            FolderOf(files_[next].path) != FolderOf(files_[song].path) || IdOf(next) != IdOf(song))
        {
            return std::nullopt;
        }

        return next;
    }

    // The main bank of `bank`: a main bank of the id that `bank` gives, the one in its folder, or else the first of
    // those whose paths start with the longest part of the bank's path.
    std::optional<std::size_t> MainBank(std::size_t bank) const
    {
        const std::span<const uint8_t> header = files_[bank].bytes;
        if (header[0x4B] != kWavesInMainBank)
        {
            return std::nullopt;
        }
        const uint16_t id = xsflib::LoadLe16(&header[0x40]);
        const std::string& path = files_[bank].path;
        std::optional<std::size_t> found;
        std::size_t found_shares = 0;
        for (const std::size_t i : banks_)
        {
            const std::span<const uint8_t> other = files_[i].bytes;
            if (other[0x4B] != kMainBank || xsflib::LoadLe16(&other[0x0E]) != id)
            {
                continue;
            }
            if (FolderOf(files_[i].path) == FolderOf(path))
            {
                return i;
            }
            const std::size_t shares =
                static_cast<std::size_t>(std::ranges::mismatch(files_[i].path, path).in1 - files_[i].path.begin());
            if (!found || shares > found_shares)
            {
                found = i;
                found_shares = shares;
            }
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
        if (!HasMagic(files[i].bytes, "sedl"))
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
