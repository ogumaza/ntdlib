// SPDX-License-Identifier: MIT

// JAudio1's banks as the Switch release of Pikmin 2 builds them for AArch64: an instrument bank (JASBasicBank) of
// version 0 with its instruments (JASBasicInst), drum sets (JASDrumSet) and their effects (JASInstRand and
// JASInstSense). ntdlib reads the bank's file. The game's parser copies each oscillator's two tables separately. Their
// pointers then differ whenever both exist, and the model keeps them apart too.
//
// JASInstRand's generator (JMath's TRandom_fast_) serves every bank. The game's parser steps it as it reads a bank:
// for an oscillator that an earlier instrument of the bank also plays, it asks that instrument for a note of key 60 at
// velocity 127, and each random effect of that instrument takes a number.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio1/calc.h"
#include "ntdlib/jaudio1/oscillator.h"
#include "ntdlib/jaudio2/instruments.h"

namespace ntdlib::jaudio
{
struct Bank;
} // namespace ntdlib::jaudio

namespace ntdlib::jaudio1
{

using jaudio2::BankWave;
using jaudio2::WaveBank;

// What an instrument or a drum gives a note (JASInstParam). The defaults are JASBankMgr::noteOn's.
struct InstParam
{
    uint8_t wave_format = 0; // 0 a wave, 2 the DSP's oscillator
    uint32_t wave_id = 0;
    std::array<const OscData*, 2> oscillators{};
    uint32_t oscillator_count = 0;
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    bool percussion = false; // a drum's wave plays at its rate whatever the key
    uint16_t release = 0;    // a direct release for the first oscillator, or 0 for none
};

// JASInstSense's curve (the C JAudio's Bank_SenseToOfs): the factor at key or velocity `x` of a curve from `low` at 0,
// through 1 at `center`, to `high` at 127. A center of 0 or 127 gives a straight line from `low` to `high`.
float SenseCurve(float low, float high, uint32_t center, uint32_t x);

// An instrument bank (JASBasicBank) of version 0, as JASBNKParser::createBasicBank builds it.
class InstrumentBank
{
public:
    // The bank as ntdlib has read it into `bank`. The parser's draws step `random`. Throws xsflib::FormatError for a
    // bank of version 1.
    InstrumentBank(const ntdlib::jaudio::Bank& bank, RandomFast& random);

    InstrumentBank(const InstrumentBank&) = delete;
    InstrumentBank& operator=(const InstrumentBank&) = delete;
    InstrumentBank(InstrumentBank&&) = default;
    InstrumentBank& operator=(InstrumentBank&&) = default;

    // The number that the sequences give the bank by (the virtual number in its header).
    uint32_t Number() const
    {
        return number_;
    }

    void SetWaveBank(const WaveBank* waves)
    {
        waves_ = waves;
    }

    const WaveBank* Waves() const
    {
        return waves_;
    }

    // JASBasicInst::getParam or JASDrumSet::getParam for a note of `key` and `velocity` (each 0 to 255) on program
    // `program`. The effects step `random`, whether a region matches or not. Returns false where the bank has no such
    // program, or the program has no region for the key and velocity.
    bool GetInstParam(uint32_t program, uint32_t key, uint32_t velocity, InstParam& param, RandomFast& random) const;

    // Whether the bank has program `program` (JASBasicBank::getInst).
    bool HasProgram(uint32_t program) const;

private:
    using Programs = jaudio2::BankPrograms<OscData, int32_t>;
    using Effect = jaudio2::BankEffect;
    using VelocityRegion = Programs::VelocityRegion;
    using KeyRegion = Programs::KeyRegion;
    using Instrument = Programs::Instrument;
    using Drum = Programs::Drum;
    using DrumSet = Programs::DrumSet;
    using Program = Programs::Program;

    // The effect's value for the note, and its change to the note's levels.
    static void ApplyEffect(const Effect& effect, uint32_t key, uint32_t velocity, InstParam& param,
                            RandomFast& random);

    uint32_t number_ = 0;
    std::vector<std::unique_ptr<std::vector<OscPoint>>> tables_;
    std::vector<std::unique_ptr<OscData>> oscillators_;
    std::array<Program, 0x100> programs_{};
    const WaveBank* waves_ = nullptr;
};

} // namespace ntdlib::jaudio1
