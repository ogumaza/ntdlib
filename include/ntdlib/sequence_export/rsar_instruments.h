// SPDX-License-Identifier: MIT

// The bank, instruments and decoded waves a sound uses.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

// The keys and velocities with which one region plays. NW4R SND selects a key's entry in an instrument's key table,
// then a velocity's entry in that entry's velocity table. A region's notes are then a set of keys that select the same
// entry, times a set of velocities.
struct RsarPlayedRanges
{
    const rsar::Region* region = nullptr;
    std::vector<std::pair<int, int>> keys, velocities;
};

// Returns the regions of `program` in `bank` with the keys and velocities that play them, in key order. Keys and
// velocities that play nothing are left out.
std::vector<RsarPlayedRanges> RegionRanges(const rsar::Bank& bank, uint32_t program);

// A sequence's bank, with its waves decoded on demand. Read failures are collected in Errors() and silence only the
// affected notes.
class RsarBankSet
{
public:
    // Reads the bank of `sound` with rsar's default options. `settings` adds nothing yet.
    RsarBankSet(const rsar::SoundArchive& archive, const rsar::SoundInfo& sound, const Settings& settings);

    // Returns the bank, or nullptr if it can't be read.
    const rsar::Bank* GetBank() const
    {
        return bank_ ? &*bank_ : nullptr;
    }

    // Returns the bank name, BANK_<index> for unnamed banks, or an empty string if the sound names none.
    const std::string& BankName() const
    {
        return bank_name_;
    }

    // The velocity region that plays `key` at `velocity` on `program`, or nullptr.
    const rsar::Region* FindRegion(uint32_t program, int key, int velocity) const;

    // True if the bank has an instrument for `program`. The instrument may play nothing.
    bool HasInstrument(uint32_t program) const;

    // Decodes and caches bank wave `wave_index`, or returns nullptr. Errors are added to Errors().
    const rsar::Pcm* GetWave(uint32_t wave_index);

    // Identifies a wave across the archive by (bank index, wave index).
    std::pair<uint32_t, uint32_t> WaveKey(uint32_t wave_index) const
    {
        return {bank_index_, wave_index};
    }

    // Read failures for the bank and its waves.
    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    // Loads the bank's waves on first use: the RWAR of a later bank, or nothing for a version 1.1 bank. The WAVE block
    // of a version 1.1 bank describes its waves. Reports what's missing.
    void LoadWaves();

    const rsar::SoundArchive& archive_;
    uint32_t bank_index_ = 0xffffffff;
    std::string bank_name_;
    std::optional<rsar::Bank> bank_;
    std::span<const uint8_t> wave_data_;
    bool waves_loaded_ = false;
    std::optional<rsar::WaveArchive> wave_archive_;
    std::map<uint32_t, std::unique_ptr<rsar::Pcm>> waves_;
    std::vector<std::string> errors_;
};

// The envelope of a Wii region. Builds before March 2008 have no hold.
EnvelopeValues RegionEnvelope(const rsar::Region& region, bool since_march_2008);

} // namespace ntdlib::sequence_export
