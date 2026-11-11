// SPDX-License-Identifier: MIT

// Checks of a game's sound archive (BFSAR) for the tests that read every archive of a game: where a sequence sound
// starts, the banks that the sounds use, and the waves that the banks name.

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

// Whether `sound` starts at one of the labels of `sequence`, its sequence, or 3 bytes after one. A label can start with
// an allocation of tracks: 0xFE and a 16-bit mask in the file's byte order. SoundMaker moves the allocation into the
// sound's info and adds track 0 to the mask. A placeholder's sequence has no labels and starts at 0.
inline bool StartsAtLabel(const sar::Sequence& sequence, const sar::SoundInfo& sound)
{
    const std::span<const uint8_t> data = sequence.data;
    const auto here = [&](const sar::Sequence::Label& label)
    {
        const std::size_t at = label.offset;
        if (at == sound.start_offset)
        {
            return true;
        }
        if (at + 3 != sound.start_offset || at + 3 > data.size() || data[at] != 0xFE)
        {
            return false;
        }

        const uint32_t mask =
            sequence.big_endian ? (data[at + 1] << 8 | data[at + 2]) : (data[at + 1] | data[at + 2] << 8);

        return (mask | 1) == sound.allocate_track_flags;
    };

    return (sequence.labels.empty() && sound.start_offset == 0) ||
           std::find_if(sequence.labels.begin(), sequence.labels.end(), here) != sequence.labels.end();
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
