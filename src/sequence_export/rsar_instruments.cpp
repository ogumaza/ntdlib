// SPDX-License-Identifier: MIT

// Bank and wave lookup (see rsar_instruments.h).

#include "ntdlib/sequence_export/rsar_instruments.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "sequence_export/runs.h"
#include "xsflib/binary.h"

namespace ntdlib::sequence_export
{

using rsar::Bank;
using rsar::BankInfo;
using rsar::Pcm;
using rsar::Region;
using rsar::SoundArchive;
using rsar::SoundInfo;
using rsar::Wave;
using rsar::WaveArchive;

namespace
{

// Describes why file `file_id` has no data. Either the file extends past the end of `archive`, or `archive` doesn't
// store the file.
std::string NoData(const SoundArchive& archive, uint32_t file_id)
{
    if (!archive.FileMissing(file_id))
    {
        return " is not stored in the archive";
    }

    return archive.Truncated() ? " extends past the truncated archive" : " extends past the archive";
}

} // namespace

std::vector<RsarPlayedRanges> RegionRanges(const Bank& bank, uint32_t program)
{
    // The region each key plays at each velocity.
    using Column = std::array<const Region*, 128>;
    std::array<Column, 128> grid;
    for (int k = 0; k < 128; k++)
    {
        for (int v = 0; v < 128; v++)
        {
            grid[k][v] = bank.Find(program, k, v);
        }
    }

    // Keys whose velocities play the same regions select the same entry of the key table.
    std::array<int, 128> first_key{};
    std::vector<int> columns;
    for (int k = 0; k < 128; k++)
    {
        first_key[k] = k;
        for (const int c : columns)
        {
            if (grid[c] == grid[k])
            {
                first_key[k] = c;
                break;
            }
        }
        if (first_key[k] == k)
        {
            columns.push_back(k);
        }
    }

    std::vector<RsarPlayedRanges> out;
    for (const int c : columns)
    {
        auto same_column = [&](int k)
        {
            return first_key[k] == c;
        };
        const auto keys = Runs(same_column);

        std::vector<const Region*> listed;
        for (int v = 0; v < 128; v++)
        {
            const Region* region = grid[c][v];
            if (!region || std::find(listed.begin(), listed.end(), region) != listed.end())
            {
                continue; // nothing plays, or the region was listed at a lower velocity
            }

            listed.push_back(region);
            auto plays_region = [&](int u)
            {
                return grid[c][u] == region;
            };
            out.push_back({region, keys, Runs(plays_region)});
        }
    }

    return out;
}

RsarBankSet::RsarBankSet(const SoundArchive& archive, const SoundInfo& sound, const Settings&)
    : archive_(archive), bank_index_(sound.bank)
{
    if (sound.bank >= archive_.Banks().size())
    {
        errors_.push_back("bank " + std::to_string(sound.bank) + " doesn't exist (the archive has " +
                          std::to_string(archive_.Banks().size()) + ")");
        return;
    }

    const BankInfo& info = archive_.Banks()[sound.bank];
    bank_name_ = info.name;
    const auto data = archive_.FileData(info.file_id);
    if (data.empty())
    {
        errors_.push_back("bank " + bank_name_ + NoData(archive_, info.file_id));
        return;
    }

    try
    {
        bank_ = Bank::Parse(data);
    }
    catch (const xsflib::FormatError& e)
    {
        errors_.push_back("bank " + bank_name_ + ": " + e.what());
    }
}

const Region* RsarBankSet::FindRegion(uint32_t program, int key, int velocity) const
{
    return bank_ ? bank_->Find(program, key, velocity) : nullptr;
}

bool RsarBankSet::HasInstrument(uint32_t program) const
{
    return bank_ && program < bank_->InstrumentCount();
}

void RsarBankSet::LoadWaves()
{
    if (waves_loaded_ || !bank_)
    {
        return;
    }

    waves_loaded_ = true;
    const uint32_t file_id = archive_.Banks()[bank_index_].file_id;
    wave_data_ = archive_.WaveData(file_id);
    if (archive_.WaveDataMissing(file_id))
    {
        errors_.push_back("bank " + bank_name_ + "'s waves extend past the " +
                          (archive_.Truncated() ? "truncated archive" : "archive"));
        return;
    }
    if (bank_->HasWaveBlock())
    {
        return; // the WAVE block describes the waves, and their samples are the wave data
    }
    if (wave_data_.empty())
    {
        errors_.push_back("bank " + bank_name_ + " has no waves");
        return;
    }

    try
    {
        wave_archive_ = WaveArchive::Parse(wave_data_);
    }
    catch (const xsflib::FormatError& e)
    {
        errors_.push_back("bank " + bank_name_ + "'s wave archive: " + e.what());
    }
}

const Pcm* RsarBankSet::GetWave(uint32_t wave_index)
{
    if (auto it = waves_.find(wave_index); it != waves_.end())
    {
        return it->second.get();
    }

    // Cache failures too. Each error is then reported once.
    LoadWaves();
    std::unique_ptr<Pcm> pcm;
    const std::string what = "bank " + bank_name_ + "'s wave " + std::to_string(wave_index);
    try
    {
        if (bank_ && bank_->HasWaveBlock() && !wave_data_.empty())
        {
            pcm = std::make_unique<Pcm>(rsar::DecodeWave(bank_->ReadWave(wave_index, wave_data_)));
        }
        else if (wave_archive_ && wave_index < wave_archive_->waves.size())
        {
            pcm = std::make_unique<Pcm>(rsar::DecodeWave(Wave::ParseFile(wave_archive_->waves[wave_index])));
        }
        else if (wave_archive_)
        {
            errors_.push_back("bank " + bank_name_ + "'s wave archive has no wave " + std::to_string(wave_index) +
                              " (it has " + std::to_string(wave_archive_->waves.size()) + ")");
        }
    }
    catch (const xsflib::FormatError& e)
    {
        errors_.push_back(what + ": " + e.what());
    }

    return waves_.emplace(wave_index, std::move(pcm)).first->second.get();
}

EnvelopeValues RegionEnvelope(const Region& region, bool since_2008)
{
    return {region.attack, region.decay, region.sustain, since_2008 ? region.hold : 0, region.release};
}

} // namespace ntdlib::sequence_export
