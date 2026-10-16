// SPDX-License-Identifier: MIT

// Bank and wave lookup (see instruments.h).

#include "ntdlib/sequence_export/instruments.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "sequence_export/runs.h"
#include "xsflib/binary.h"
#include "xsflib/text.h"

namespace ntdlib::sequence_export
{

using sar::Bank;
using sar::BankInfo;
using sar::Instrument;
using sar::KeyRegion;
using sar::kNoItem;
using sar::Pcm;
using sar::SoundArchive;
using sar::SoundInfo;
using sar::VelocityRegion;
using sar::WaveArchive;
using sar::WaveArchiveInfo;
using sar::WaveId;

namespace
{

// Describes why file `file_id` has no data. Either the file extends past the end of `archive`, or neither `archive` nor
// any group file beside the archive stores the file. The 3DS's message doesn't mention group files.
std::string NoData(const SoundArchive& archive, uint32_t file_id, sar::Flavor flavor)
{
    if (archive.FileMissing(file_id))
    {
        return archive.Truncated() ? " extends past the truncated archive" : " extends past the archive";
    }

    return flavor == sar::Flavor::kCtr || archive.ExternalGroups().empty()
               ? " is not stored in the archive"
               : " is not stored in the archive or a group file beside it";
}

} // namespace

std::vector<PlayedRanges> RegionRanges(const Instrument& inst)
{
    std::vector<PlayedRanges> out;
    const std::size_t nkeys = inst.keys.size();
    for (std::size_t ki = 0; ki < nkeys; ki++)
    {
        const KeyRegion& kr = inst.keys[ki];
        auto owns_key = [&](int k)
        {
            for (std::size_t j = 0; j < nkeys; j++)
            {
                if (k >= inst.keys[j].lo && k <= inst.keys[j].hi)
                {
                    return j == ki;
                }
            }

            return false;
        };
        const auto key_runs = Runs(owns_key);

        for (std::size_t vi = 0; vi < kr.velocities.size(); vi++)
        {
            const auto& vr = kr.velocities[vi];
            if (!vr.region)
            {
                continue;
            }

            auto owns_velocity = [&](int v)
            {
                for (std::size_t j = 0; j < kr.velocities.size(); j++)
                {
                    if (v >= kr.velocities[j].lo && v <= kr.velocities[j].hi)
                    {
                        return j == vi;
                    }
                }

                return false;
            };
            out.push_back({&kr, &*vr.region, key_runs, Runs(owns_velocity)});
        }
    }

    return out;
}

BankSet::BankSet(const SoundArchive& archive, const SoundInfo& sound, const Settings& settings)
    : archive_(archive), options_(settings.archive)
{
    for (std::size_t slot = 0; slot < sound.banks.size() && slot < 4; slot++)
    {
        const uint32_t item = sound.banks[slot];
        if (item == kNoItem)
        {
            continue;
        }

        const std::optional<std::size_t> index = sar::ItemIndex(item, sar::kBankItem, archive_.Banks().size());
        if (!index)
        {
            errors_.push_back("bank slot " + std::to_string(slot) + " has " + xsflib::Hex(item, 8) +
                              ": invalid bank ID");
            continue;
        }

        ReadBank(slot, *index);
    }
}

BankSet::BankSet(const SoundArchive& archive, std::size_t bank, const Settings& settings)
    : archive_(archive), options_(settings.archive)
{
    ReadBank(0, bank);
}

void BankSet::ReadBank(std::size_t slot, std::size_t index)
{
    const BankInfo& info = archive_.Banks().at(index);
    bank_names_[slot] = info.name;
    const auto data = archive_.FileData(info.file_id);
    if (data.empty())
    {
        errors_.push_back("bank " + bank_names_[slot] + NoData(archive_, info.file_id, options_.flavor));
        return;
    }

    try
    {
        banks_[slot] = Bank::Parse(data, options_);
    }
    catch (const xsflib::FormatError& e)
    {
        errors_.push_back("bank " + bank_names_[slot] + ": " + e.what());
    }
}

const Bank* BankSet::GetBank(int slot) const
{
    return slot >= 0 && slot < 4 && banks_[slot] ? &*banks_[slot] : nullptr;
}

const std::string& BankSet::BankName(int slot) const
{
    static const std::string kNone;
    return slot >= 0 && slot < 4 ? bank_names_[slot] : kNone;
}

const Instrument* BankSet::GetInstrument(int slot, uint32_t program) const
{
    const Bank* bank = GetBank(slot);
    if (!bank || program >= bank->instruments.size() || !bank->instruments[program])
    {
        return nullptr;
    }

    return &*bank->instruments[program];
}

const VelocityRegion* BankSet::FindRegion(int slot, uint32_t program, int key, int velocity) const
{
    const Instrument* inst = GetInstrument(slot, program);
    return inst ? inst->Find(key, velocity) : nullptr;
}

std::optional<std::pair<uint32_t, uint32_t>> BankSet::WaveKey(int slot, uint32_t wave_id_index) const
{
    const Bank* bank = GetBank(slot);
    if (!bank || wave_id_index >= bank->waves.size())
    {
        return std::nullopt;
    }

    const WaveId& id = bank->waves[wave_id_index];
    if (sar::ItemType(id.wave_archive_item) != sar::kWaveArchiveItem || id.index == kNoItem)
    {
        return std::nullopt;
    }

    return std::make_pair(sar::ItemNumber(id.wave_archive_item), id.index);
}

bool BankSet::NoWave(int slot, uint32_t wave_id_index) const
{
    const Bank* bank = GetBank(slot);
    return bank && wave_id_index < bank->waves.size() && bank->waves[wave_id_index].index == kNoItem;
}

const Pcm* BankSet::GetWave(int slot, uint32_t wave_id_index)
{
    const auto key = WaveKey(slot, wave_id_index);
    if (!key)
    {
        ReportBrokenWaveId(slot, wave_id_index);
        return nullptr;
    }

    auto it = waves_.find(*key);
    if (it != waves_.end())
    {
        return it->second.get();
    }

    // Load wave archives on first use. Cache failures too. Each error is then reported once.
    const auto [war_index, wave_index] = *key;
    auto war_it = wave_archives_.find(war_index);
    if (war_it == wave_archives_.end())
    {
        std::optional<WaveArchive> war;
        if (war_index < archive_.WaveArchives().size())
        {
            const WaveArchiveInfo& info = archive_.WaveArchives()[war_index];
            const auto data = archive_.FileData(info.file_id);
            if (!data.empty())
            {
                try
                {
                    war = WaveArchive::Parse(data, options_);
                }
                catch (const xsflib::FormatError& e)
                {
                    errors_.push_back("wave archive " + info.name + ": " + e.what());
                }
            }
            else
            {
                errors_.push_back("wave archive " + info.name + NoData(archive_, info.file_id, options_.flavor));
            }
        }
        else
        {
            errors_.push_back("wave archive " + std::to_string(war_index) + " doesn't exist (the archive has " +
                              std::to_string(archive_.WaveArchives().size()) + ")");
        }

        war_it = wave_archives_.emplace(war_index, std::move(war)).first;
    }

    std::unique_ptr<Pcm> pcm;
    if (war_it->second && wave_index < war_it->second->waves.size())
    {
        try
        {
            pcm = std::make_unique<Pcm>(sar::DecodeWave(war_it->second->waves[wave_index], options_));
        }
        catch (const xsflib::FormatError& e)
        {
            errors_.push_back("wave " + std::to_string(war_index) + "/" + std::to_string(wave_index) + ": " + e.what());
        }
    }
    else if (war_it->second)
    {
        errors_.push_back("wave archive " + archive_.WaveArchives()[war_index].name + " has no wave " +
                          std::to_string(wave_index));
    }

    return waves_.emplace(*key, std::move(pcm)).first->second.get();
}

void BankSet::ReportBrokenWaveId(int slot, uint32_t wave_id_index)
{
    const Bank* bank = GetBank(slot);
    if (!bank || !broken_wave_ids_.emplace(slot, wave_id_index).second)
    {
        return; // unreadable banks and previously checked wave IDs have already been reported
    }

    if (wave_id_index >= bank->waves.size())
    {
        errors_.push_back("bank " + bank_names_[slot] + " has no wave " + std::to_string(wave_id_index));
    }
    else if (!NoWave(slot, wave_id_index))
    {
        errors_.push_back("bank " + bank_names_[slot] + "'s wave " + std::to_string(wave_id_index) + " is in " +
                          xsflib::Hex(bank->waves[wave_id_index].wave_archive_item, 8) + ": invalid wave archive ID");
    }
}

} // namespace ntdlib::sequence_export
