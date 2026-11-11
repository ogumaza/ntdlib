// SPDX-License-Identifier: MIT

// JAudio2's banks as the Super Mario Galaxy games' `main` builds them for AArch64: an instrument bank (JASBasicBank)
// with its instruments (JASBasicInst), drum sets (JASDrumSet) and their effects (JASInstRand and JASInstSense), and
// the wave bank (JASBasicWaveBank) that gives a note its wave. ntdlib reads the bank's file. The model keeps the
// oscillators' tables as the game's parser leaves them: each points into the game's copy of the bank's envelope
// chunk. A table that the game reads past its end then reads the next tables of the chunk, as in the game.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio2/channel.h"
#include "ntdlib/jaudio2/oscillator.h"

namespace ntdlib::jaudio2
{

// An instrument's or a drum's effect on its notes (JASInstRand and JASInstSense), in a bank of either version.
struct BankEffect
{
    bool random = false;
    uint32_t target = 0;
    float base = 1.0f; // JASInstRand
    float width = 0.0f;
    uint8_t trigger = 0; // JASInstSense: 1 velocity, 2 key
    uint8_t center = 60;
    float low = 1.0f;
    float high = 1.0f;
};

// The programs of an instrument bank, as JASBNKParser builds them from a bank of either version: the instruments
// (JASBasicInst) with their key and velocity regions, and the drum sets (JASDrumSet). `Osc` is the oscillators'
// settings, and `Bound` is the type of a region's highest key and velocity.
template <typename Osc, typename Bound>
struct BankPrograms
{
    struct VelocityRegion
    {
        Bound high_velocity = 0;
        uint32_t wave_id = 0;
        float volume = 1.0f;
        float pitch = 1.0f;
    };

    struct KeyRegion
    {
        Bound high_key = 0;
        std::vector<VelocityRegion> velocities;
    };

    struct Instrument
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        std::array<const Osc*, 2> oscillators{};
        std::vector<BankEffect> effects;
        std::vector<KeyRegion> keys;
    };

    struct Drum
    {
        float volume = 1.0f;
        float pitch = 1.0f;
        float pan = 0.5f;
        uint16_t release = 1000;
        std::vector<BankEffect> effects;
        std::vector<VelocityRegion> velocities;
    };

    struct DrumSet
    {
        std::array<std::optional<Drum>, 128> keys;
    };

    using Program = std::variant<std::monostate, Instrument, DrumSet>;
};

// The model's effects of a bank's effects.
std::vector<BankEffect> CopyEffects(const std::vector<ntdlib::jaudio::InstEffect>& from);

// The model's velocity regions of a bank's regions.
template <typename Region>
std::vector<Region> CopyVelocities(const std::vector<ntdlib::jaudio::VelocityRegion>& from)
{
    std::vector<Region> to;
    to.reserve(from.size());
    for (const ntdlib::jaudio::VelocityRegion& v : from)
    {
        to.push_back({v.high_velocity, v.wave_id, v.volume, v.pitch});
    }

    return to;
}

// The programs of ntdlib's reading of `bank`, as the model's `Programs` hold them. A program past the end of
// `programs` is left out. `oscillator` gives the model's settings of the bank's oscillator by its index.
template <typename Programs, std::size_t kCount, typename OscillatorAt>
void CopyPrograms(const ntdlib::jaudio::Bank& bank, std::array<typename Programs::Program, kCount>& programs,
                  OscillatorAt oscillator)
{
    using VelocityRegion = typename Programs::VelocityRegion;
    using Instrument = typename Programs::Instrument;
    using Drum = typename Programs::Drum;
    using DrumSet = typename Programs::DrumSet;

    for (const auto& [number, program] : bank.programs)
    {
        if (number >= programs.size())
        {
            continue;
        }

        if (const auto* inst = std::get_if<ntdlib::jaudio::Instrument>(&program))
        {
            Instrument& to = programs[number].template emplace<Instrument>();
            to.volume = inst->volume;
            to.pitch = inst->pitch;
            for (std::size_t k = 0; k < std::min<std::size_t>(inst->oscillators.size(), 2); k++)
            {
                to.oscillators[k] = oscillator(inst->oscillators[k]);
            }
            to.effects = CopyEffects(inst->effects);
            for (const ntdlib::jaudio::KeyRegion& key : inst->keys)
            {
                to.keys.push_back({key.high_key, CopyVelocities<VelocityRegion>(key.velocities)});
            }
        }
        else
        {
            const auto& drums = std::get<ntdlib::jaudio::DrumSet>(program);
            DrumSet& to = programs[number].template emplace<DrumSet>();
            for (std::size_t key = 0; key < drums.keys.size(); key++)
            {
                if (const auto& drum = drums.keys[key])
                {
                    to.keys[key] = Drum{drum->volume,
                                        drum->pitch,
                                        drum->pan,
                                        drum->release,
                                        CopyEffects(drum->effects),
                                        CopyVelocities<VelocityRegion>(drum->velocities)};
                }
            }
        }
    }
}

// The first of `regions` whose highest velocity reaches `velocity`, or null. JASBasicInst, JASDrumSet and the C
// JAudio's GetInstVmap search the same way. The comparison takes both sides to `Compare`: JAudio2's model compares in
// int32_t, JAudio1's and the Jac's in int64_t. The two agree for every velocity below 2^31.
template <typename Compare, typename Regions>
const typename Regions::value_type* FindVelocity(const Regions& regions, uint32_t velocity)
{
    for (const auto& region : regions)
    {
        if (static_cast<Compare>(region.high_velocity) >= static_cast<Compare>(velocity))
        {
            return &region;
        }
    }

    return nullptr;
}

// JASBasicInst::getParam's search of the key regions: the first whose highest key reaches `key`, or null.
template <typename Regions>
const typename Regions::value_type* FindKeyRegion(const Regions& keys, uint32_t key)
{
    for (const auto& region : keys)
    {
        if (static_cast<int32_t>(region.high_key) >= static_cast<int32_t>(key))
        {
            return &region;
        }
    }

    return nullptr;
}

// What an instrument or a drum gives a note (JASInstParam). The defaults are JASBank::noteOn's.
struct InstParam
{
    float volume = 1.0f;
    float fxmix = 0.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float dolby = 0.0f;
    uint32_t type = 0;
    uint32_t wave_id = 0;
    std::array<const OscData*, 2> oscillators{};
    uint32_t oscillator_count = 0;
    bool fixed_key = false; // a drum's wave plays at its rate whatever the key
    uint16_t direct_release = 0;
};

// A wave as a wave bank gives it: its settings and the address of its samples in the DSP's memory.
struct BankWave
{
    WaveInfo info;
    uint32_t address = 0;
};

// A wave bank (JASBasicWaveBank) by wave id. A wave whose AW file isn't loaded isn't in it. The game's handle of such
// a wave gives no address, and a note on it doesn't start.
class WaveBank
{
public:
    void Add(uint32_t id, const BankWave& wave)
    {
        waves_[id] = wave;
    }

    // getWaveHandle, then the handle's getWaveInfo and getWavePtr. Null if the bank has no such wave.
    const BankWave* Find(uint32_t id) const
    {
        const auto wave = waves_.find(id);

        return wave != waves_.end() ? &wave->second : nullptr;
    }

private:
    std::map<uint32_t, BankWave> waves_;
};

// An instrument bank (JASBasicBank) of version 1, as JASBNKParser builds it.
class InstrumentBank
{
public:
    // The bank in `file`, as ntdlib has read it into `bank`. Its oscillators' tables come from `file`'s envelope chunk.
    // Throws xsflib::FormatError for a bank of version 0: the model has no layout for one yet.
    InstrumentBank(const ntdlib::jaudio::Bank& bank, std::span<const uint8_t> file);

    InstrumentBank(const InstrumentBank&) = delete;
    InstrumentBank& operator=(const InstrumentBank&) = delete;
    InstrumentBank(InstrumentBank&&) = default;
    InstrumentBank& operator=(InstrumentBank&&) = default;

    uint32_t Number() const
    {
        return number_;
    }

    // The wave bank that the bank plays (JASBank::assignWaveBank), or null for none.
    void SetWaveBank(const WaveBank* waves)
    {
        waves_ = waves;
    }

    const WaveBank* Waves() const
    {
        return waves_;
    }

    // JASBasicBank::getInstParam: the instrument's or the drum's parameters for a note of `key` and `velocity` (each
    // 0 to 255). `random` is JASInstRand's generator. Its effects step it. Returns false where the bank has no
    // instrument for the program, no region for the key and velocity, or no drum for the key.
    bool GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param, uint32_t& random) const;

private:
    using Programs = BankPrograms<OscData, uint32_t>;
    using Effect = BankEffect;
    using VelocityRegion = Programs::VelocityRegion;
    using KeyRegion = Programs::KeyRegion;
    using Instrument = Programs::Instrument;
    using Drum = Programs::Drum;
    using DrumSet = Programs::DrumSet;
    using Program = Programs::Program;

    // The constructor's first step: the oscillators and their tables from the bank's file. The programs come from
    // ntdlib's reading of the bank (CopyPrograms).
    void ReadOscillators(std::span<const uint8_t> file);

    static void ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param, uint32_t& random);

    uint32_t number_ = 0;
    std::vector<std::vector<OscPoint>> tables_; // each table from its start to the envelope chunk's end
    std::vector<OscData> oscillators_;
    std::array<Program, 0xf0> programs_{};
    const WaveBank* waves_ = nullptr;
};

} // namespace ntdlib::jaudio2
