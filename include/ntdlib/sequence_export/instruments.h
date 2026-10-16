// SPDX-License-Identifier: MIT

// The banks, instruments and decoded waves a sound uses.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

// Effective key and velocity ranges for one velocity region. nw::snd selects the first matching key region, then the
// first matching velocity region within it. Earlier entries can therefore mask part of a later region.
struct PlayedRanges
{
    const sar::KeyRegion* key_region = nullptr;
    const sar::VelocityRegion* region = nullptr;
    std::vector<std::pair<int, int>> keys, velocities;
};

// Returns effective ranges for `inst` in bank order, without the velocity entries that have no region.
std::vector<PlayedRanges> RegionRanges(const sar::Instrument& inst);

// A sequence's banks, with waves decoded on demand. Read failures are collected in Errors() and silence only the
// affected notes.
class BankSet
{
public:
    // Reads the banks of `sound` with the archive options of `settings`.
    BankSet(const sar::SoundArchive& archive, const sar::SoundInfo& sound, const Settings& settings);

    // Reads bank `bank` of the archive's banks into slot 0, with the archive options of `settings`. The other slots
    // stay empty.
    BankSet(const sar::SoundArchive& archive, std::size_t bank, const Settings& settings);

    // Returns the bank in `slot`, or nullptr. Only four slots exist. nw::snd accepts higher slots and reads past its
    // bank table. Those slots are silent here.
    const sar::Bank* GetBank(int slot) const;

    // Returns the bank name, BANK_<index> for unnamed banks, or an empty string for an empty slot.
    const std::string& BankName(int slot) const;

    // An instrument of the bank in a slot, or nullptr.
    const sar::Instrument* GetInstrument(int slot, uint32_t program) const;

    // The velocity region that plays `key` at `velocity` on an instrument, or nullptr.
    const sar::VelocityRegion* FindRegion(int slot, uint32_t program, int key, int velocity) const;

    // Decodes and caches a bank wave entry, or returns nullptr. Invalid references are added to Errors(); intentional
    // no-wave entries are not (see NoWave).
    const sar::Pcm* GetWave(int slot, uint32_t wave_id_index);

    // Identifies a shared wave across banks by (wave archive index, wave index).
    std::optional<std::pair<uint32_t, uint32_t>> WaveKey(int slot, uint32_t wave_id_index) const;

    // True for the no-wave sentinel, wave index 0xFFFFFFFF. Such regions are silent in the game too.
    bool NoWave(int slot, uint32_t wave_id_index) const;

    // Read failures for banks, wave archives and waves.
    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    // Reads bank `index` of the archive's banks into `slot`. A bank that can't be read is added to Errors().
    void ReadBank(std::size_t slot, std::size_t index);

    // Reports an invalid wave reference that WaveKey could not resolve. Each entry is reported only once.
    void ReportBrokenWaveId(int slot, uint32_t wave_id_index);

    const sar::SoundArchive& archive_;
    sar::Options options_;
    std::array<std::optional<sar::Bank>, 4> banks_;
    std::array<std::string, 4> bank_names_;
    std::map<uint32_t, std::optional<sar::WaveArchive>> wave_archives_; // by wave archive index
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<sar::Pcm>> waves_;
    std::set<std::pair<int, uint32_t>> broken_wave_ids_; // bank slot and wave table index
    std::vector<std::string> errors_;
};

} // namespace ntdlib::sequence_export
