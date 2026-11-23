// SPDX-License-Identifier: MIT

// Checks of a game's sound archive (BFSAR) for the tests that read every archive of a game: where a sequence sound
// starts, whether its files are there, the banks that the sounds use, and the waves that the banks name.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"

namespace ntdlib::testing::sar_checks
{

// Whether `sound` starts at one of the labels of `sequence`, its sequence, or 3 bytes after one (see
// sar::StartsAtLabel). A placeholder's sequence has no labels and starts at 0. Sequence::Parse reads labels only on
// the Wii U and the Switch, where the commands' arguments follow the file's byte order.
inline bool StartsAtLabel(const sar::Sequence& sequence, const sar::SoundInfo& sound)
{
    const auto here = [&](const sar::Sequence::Label& label)
    {
        return sar::StartsAtLabel(sequence.data, sequence.big_endian, label.offset, sound);
    };

    return (sequence.labels.empty() && sound.start_offset == 0) || std::ranges::any_of(sequence.labels, here);
}

// Checks that `sound` starts inside `sequence`, its sequence, at one of its labels or 3 bytes after one
// (StartsAtLabel). `what` names the archive in the checks.
inline void CheckStart(const std::string& what, const sar::SoundInfo& sound, const sar::Sequence& sequence)
{
    using xsflib::testing::Check;

    if (Check(sound.start_offset < sequence.data.size(), what + ": " + sound.name + " starts inside its sequence"))
    {
        Check(StartsAtLabel(sequence, sound), what + ": " + sound.name + " starts at a label");
    }
}

// The banks of an archive that its sequence sounds use, each read once. A bank is missing when its file, or the file of
// one of its waves' archives, isn't in the archive. A wave archive that the archive doesn't list counts as there.
// CheckBank reports it.
class Banks
{
public:
    explicit Banks(const sar::SoundArchive& archive) : archive_(archive)
    {
    }

    // Reads bank `index`, unless it's read or missing already. Returns whether it's here.
    bool Load(uint32_t index)
    {
        if (loaded_.contains(index) || missing_.contains(index))
        {
            return loaded_.contains(index);
        }

        const std::span<const uint8_t> data = archive_.FileData(archive_.Banks()[index].file_id);
        bool here = !data.empty();
        if (here)
        {
            sar::Bank bank = sar::Bank::Parse(data, archive_.ReadOptions());
            for (const sar::WaveId& wave : bank.waves)
            {
                const uint32_t war = sar::ItemNumber(wave.wave_archive_item);
                here = here && (war >= archive_.WaveArchives().size() ||
                                !archive_.FileData(archive_.WaveArchives()[war].file_id).empty());
            }
            if (here)
            {
                loaded_[index] = std::move(bank);
            }
        }

        if (!here)
        {
            missing_.insert(index);
        }

        return here;
    }

    // The banks read, by their indices.
    const std::map<uint32_t, sar::Bank>& Loaded() const
    {
        return loaded_;
    }

private:
    const sar::SoundArchive& archive_;
    std::map<uint32_t, sar::Bank> loaded_;
    std::set<uint32_t> missing_;
};

// Checks that each bank slot of `sound`, a sequence sound of `archive`, names a bank of the archive, and reads the
// banks into `banks`. A sequence picks its banks by slot, and may leave a slot empty. Returns whether the sound's
// sequence file and every bank that it names are here. `what` names the archive in the checks.
inline bool LoadSequenceFiles(const std::string& what, const sar::SoundArchive& archive, const sar::SoundInfo& sound,
                              Banks& banks)
{
    using xsflib::testing::Check;

    bool here = !archive.FileData(sound.file_id).empty();
    for (const uint32_t bank_id : sound.banks)
    {
        const uint32_t index = sar::ItemNumber(bank_id);
        if (bank_id != sar::kNoItem && Check(sar::ItemType(bank_id) == sar::kBankItem && index < archive.Banks().size(),
                                             what + ": " + sound.name + "'s bank"))
        {
            here = banks.Load(index) && here;
        }
    }

    return here;
}

// The wave archives that the banks name, each read once, and the waves that the banks name in them.
struct BankWaves
{
    std::map<uint32_t, sar::WaveArchive> archives;
    std::set<std::pair<uint32_t, uint32_t>> waves; // the wave archive's index and the wave's
};

// Checks bank `index` of `archive`, `bank`: each of its instruments at each key and velocity names one of its waves,
// and each of its waves is in its wave archive. Adds the bank's waves to `waves`. Returns the number of regions found.
// `what` names the archive in the checks.
inline std::size_t CheckBank(const std::string& what, const sar::SoundArchive& archive, uint32_t index,
                             const sar::Bank& bank, BankWaves& waves)
{
    using xsflib::testing::Check;

    std::size_t regions = 0;
    for (const std::optional<sar::Instrument>& instrument : bank.instruments)
    {
        for (int key = 0; instrument && key < 128; ++key)
        {
            for (int velocity = 0; velocity < 128; ++velocity)
            {
                const sar::VelocityRegion* region = instrument->Find(key, velocity);
                if (region)
                {
                    ++regions;
                    Check(region->wave_id_index < bank.waves.size(),
                          what + ": bank " + archive.Banks()[index].name + " names a wave it has");
                }
            }
        }
    }

    for (const sar::WaveId& wave : bank.waves)
    {
        const uint32_t war = sar::ItemNumber(wave.wave_archive_item);
        if (!Check(
                sar::ItemType(wave.wave_archive_item) == sar::kWaveArchiveItem && war < archive.WaveArchives().size(),
                what + ": bank " + archive.Banks()[index].name + "'s wave archive"))
        {
            continue;
        }

        if (!waves.archives.contains(war))
        {
            waves.archives[war] =
                sar::WaveArchive::Parse(archive.FileData(archive.WaveArchives()[war].file_id), archive.ReadOptions());
        }
        Check(wave.index < waves.archives[war].waves.size(), what + ": a bank's wave is in its archive");
        waves.waves.insert({war, wave.index});
    }

    return regions;
}

// Checks each of the banks' waves that its wave archive holds: it has channels, and each DSP-ADPCM channel starts with
// the frame header that its info gives. `archive` is the archive that the banks belong to.
inline void CheckWaves(const std::string& what, const sar::SoundArchive& archive, const BankWaves& waves)
{
    using xsflib::testing::Check;

    for (const auto& [war, index] : waves.waves)
    {
        const sar::WaveArchive& wave_archive = waves.archives.at(war);
        if (index >= wave_archive.waves.size())
        {
            continue;
        }

        const sar::Wave wave = sar::Wave::Parse(wave_archive.waves[index], archive.ReadOptions());
        Check(!wave.channels.empty(), what + ": a wave has channels");
        for (const sar::WaveChannel& channel : wave.channels)
        {
            if (channel.adpcm)
            {
                Check(!channel.data.empty() && channel.data[0] == (channel.adpcm->pred_scale & 0xFF),
                      what + ": a DSP-ADPCM channel starts with its frame header");
            }
        }
    }
}

} // namespace ntdlib::testing::sar_checks
