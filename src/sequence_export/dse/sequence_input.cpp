// SPDX-License-Identifier: MIT

// DSE's songs and sound effects for a converter (see sequence_input.h).

#include "ntdlib/sequence_export/dse/sequence_input.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/sequence_export/dse/performance.h"

namespace ntdlib::sequence_export::dse
{
namespace
{

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

// Each file read once, by its index. A file that can't be read gives a warning, once.
template <typename T>
class Files
{
public:
    Files(std::span<const ntdlib::dse::NamedFile> files, const char* what, std::vector<std::string>& warnings)
        : files_(files), what_(what), warnings_(warnings)
    {
    }

    std::shared_ptr<const T> Get(std::size_t index)
    {
        if (const auto found = read_.find(index); found != read_.end())
        {
            return found->second;
        }

        std::shared_ptr<const T> file;
        try
        {
            file = std::make_shared<const T>(T::Parse(files_[index].bytes));
        }
        catch (const xsflib::FormatError& e)
        {
            warnings_.push_back(files_[index].path + ": the " + what_ + " can't be read (" + e.what() + ")");
        }
        read_.emplace(index, file);

        return file;
    }

private:
    std::span<const ntdlib::dse::NamedFile> files_;
    const char* what_;
    std::vector<std::string>& warnings_;
    std::map<std::size_t, std::shared_ptr<const T>> read_;
};

} // namespace

SequenceFiles SequenceInput::Files() const
{
    SequenceFiles files;
    files.song = song.get();
    files.effects = effects.get();
    files.effect = effect;
    for (const std::shared_ptr<const ntdlib::dse::WaveBank>& bank : banks)
    {
        files.banks.push_back(bank.get());
    }
    files.main_bank = main_bank.get();

    return files;
}

std::vector<SequenceInput> ReadSequences(std::span<const ntdlib::dse::NamedFile> files,
                                         std::vector<std::string>& warnings)
{
    Files<ntdlib::dse::Song> songs(files, "song", warnings);
    Files<ntdlib::dse::WaveBank> banks(files, "wave bank", warnings);
    Files<ntdlib::dse::SoundEffects> effects(files, "bank of sound effects", warnings);

    std::vector<SequenceInput> inputs;
    for (const ntdlib::dse::SongSet& set : ntdlib::dse::FindSongSets(files))
    {
        const std::string& path = files[set.song].path;
        if (!set.bank)
        {
            warnings.push_back(path + ": no wave bank goes with the song");
            continue;
        }

        SequenceInput input{.folder = FolderOf(path), .name = StemOf(path), .ref = {path}, .song = songs.Get(set.song)};
        const std::shared_ptr<const ntdlib::dse::WaveBank> bank = banks.Get(*set.bank);
        if (!input.song || !bank)
        {
            continue;
        }

        input.banks.push_back(bank);
        if (set.main_bank)
        {
            input.main_bank = banks.Get(*set.main_bank);
        }
        inputs.push_back(std::move(input));
    }

    for (const ntdlib::dse::EffectSet& set : ntdlib::dse::FindEffectSets(files))
    {
        const std::string& path = files[set.effects].path;
        const std::shared_ptr<const ntdlib::dse::SoundEffects> read = effects.Get(set.effects);
        if (!read)
        {
            continue;
        }

        for (std::size_t slot = 0; slot < read->sequences.size() && slot < set.slots.size(); slot++)
        {
            if (!read->sequences[slot])
            {
                continue;
            }

            char number[16];
            std::snprintf(number, sizeof(number), "_%03zu", slot);
            SequenceInput input{.folder = FolderOf(path),
                                .name = StemOf(path) + number,
                                .ref = {path, static_cast<uint16_t>(slot)},
                                .effects = read,
                                .effect = static_cast<uint16_t>(slot)};

            const ntdlib::dse::EffectBanks& slot_banks = set.slots[slot];
            for (const uint16_t id : slot_banks.missing)
            {
                char line[64];
                std::snprintf(line, sizeof(line), ": no file holds bank 0x%04X of sound effect %zu", id, slot);
                warnings.push_back(path + line);
            }

            for (const std::size_t bank : slot_banks.banks)
            {
                if (const std::shared_ptr<const ntdlib::dse::WaveBank> read_bank = banks.Get(bank))
                {
                    input.banks.push_back(read_bank);
                }
            }
            if (slot_banks.main_bank)
            {
                input.main_bank = banks.Get(*slot_banks.main_bank);
            }
            inputs.push_back(std::move(input));
        }
    }

    return inputs;
}

} // namespace ntdlib::sequence_export::dse
