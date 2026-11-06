// SPDX-License-Identifier: MIT

// The DS's instruments (see instruments.h).

#include "ntdlib/sequence_export/ntr/instruments.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"
#include "xsflib/binary.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/sequence_export/psg_samples.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

namespace snd = ntdlib::ntr::snd;
using sdat::InstrumentType;

// A frame of the driver in milliseconds.
constexpr double kFrameMs = 1000.0 * snd::kFrameClocks / ntdlib::ntr::kSystemClockHz;

// The rate of the sound unit's timers: half the system clock.
constexpr double kTimerHz = ntdlib::ntr::kSystemClockHz / 2.0;

// The attenuation that an attack starts from, and the driver's unit of it: 1/128 of 0.1 dB.
constexpr int32_t kStartAttenuation = -92544;
constexpr double kAttenuationPerDb = 1280.0;

// The fall of 100 dB that SoundFont's decay and release times give, and the fall of e^-8 that SFZ's give, in the
// driver's unit.
constexpr double kSf2Fall = 100.0 * kAttenuationPerDb;
constexpr double kSfzFall = 8.0 * 20.0 / 2.302585092994046 * kAttenuationPerDb;

// SoundFont's slowest decay or release, 8000 timecents, in milliseconds.
constexpr double kSlowestSf2DecayMs = 101593.6673259648;

// The driver's rate of a decay or a release, in its unit a frame (SND's CalcRelease).
double DecayRate(int value)
{
    value = std::clamp(value, 0, 127);
    if (value == 127)
    {
        return 0xFFFF;
    }
    if (value == 126)
    {
        return 0x3C00;
    }
    if (value < 50)
    {
        return value * 2 + 1;
    }

    return static_cast<double>(0x1E00 / (126 - value));
}

// The level of a sustain in 0.1 dB.
int SustainLevel(int sustain, const snd::Build& build)
{
    sustain = std::clamp(sustain, 0, 127);
    return sustain == 0 ? build.zero_level : snd::tables::kDecibelSquare[sustain];
}

// An attack as SoundFont plays it: the time of its linear ramp, and the time the driver's attack takes to reach full
// level. The ramp is the one whose amplitudes come closest to the driver's, frame by frame, in the least squares.
struct AttackFit
{
    double ramp_ms = 0.0;
    double full_ms = 0.0;
};

AttackFit FitAttack(int attack)
{
    attack = std::clamp(attack, 0, 127);
    const int32_t factor = attack < 109 ? 255 - attack : snd::tables::kFastAttack[127 - attack];

    // Each frame's level, after the frame's step (Driver::StepEnvelope). The decay starts in the frame after the one
    // that reaches full level.
    std::vector<double> levels;
    int32_t attenuation = kStartAttenuation;
    do
    {
        attenuation = -((-attenuation * factor) >> 8);
        levels.push_back(std::pow(10.0, attenuation / kAttenuationPerDb / 20.0));
    } while (attenuation != 0);

    const double full_ms = static_cast<double>(levels.size()) * kFrameMs;
    const auto error = [&](double ramp_ms)
    {
        double sum = 0.0;
        for (std::size_t k = 0; k < levels.size(); k++)
        {
            const double t = (static_cast<double>(k) + 0.5) * kFrameMs;
            const double ramp = ramp_ms > 0.0 ? std::min(1.0, t / ramp_ms) : 1.0;
            sum += (levels[k] - ramp) * (levels[k] - ramp);
        }
        return sum;
    };

    double low = 0.0;
    double high = full_ms;
    for (int i = 0; i < 100; i++)
    {
        const double a = low + (high - low) / 3.0;
        const double b = high - (high - low) / 3.0;
        if (error(a) <= error(b))
        {
            high = b;
        }
        else
        {
            low = a;
        }
    }

    return {(low + high) / 2.0, full_ms};
}

const AttackFit& AttackFitOf(int attack)
{
    static const std::array<AttackFit, 128> kFits = []
    {
        std::array<AttackFit, 128> fits;
        for (int a = 0; a < 128; a++)
        {
            fits[static_cast<std::size_t>(a)] = FitAttack(a);
        }
        return fits;
    }();

    return kFits[static_cast<std::size_t>(std::clamp(attack, 0, 127))];
}

// The time of a decay or release of `value` over `fall`, in milliseconds.
double FallMs(int value, double fall)
{
    return fall / DecayRate(value) * kFrameMs;
}

// The track's value where it has one, otherwise the region's.
uint8_t Pick(uint8_t track, uint8_t region)
{
    return track != 0xFF ? track : region;
}

// The zone of `region` over keys `low` to `high`, with the track's envelope from `key`, or nothing for a region that
// plays nothing. A region's release of 0xFF holds its notes until the track releases them, at the release of 0.
std::optional<PresetZone> Zone(int low, int high, const sdat::Region& region, const PresetKey& key)
{
    if (region.type != InstrumentType::kPcm && region.type != InstrumentType::kPsg &&
        region.type != InstrumentType::kNoise)
    {
        return std::nullopt;
    }

    PresetZone zone;
    zone.low_key = static_cast<uint8_t>(low);
    zone.high_key = static_cast<uint8_t>(high);
    zone.region = region;
    zone.envelope.attack = Pick(key.attack, region.attack);
    zone.envelope.decay = Pick(key.decay, region.decay);
    zone.envelope.sustain = Pick(key.sustain, region.sustain);
    zone.envelope.release = Pick(key.release, region.release == 0xFF ? 0 : region.release);

    return zone;
}

} // namespace

std::vector<PresetZone> PresetZones(const sdat::Bank& bank, const PresetKey& key)
{
    if (key.program >= bank.instruments.size())
    {
        return {};
    }

    std::vector<PresetZone> zones;
    const auto add = [&](int low, int high, const sdat::Region& region)
    {
        if (std::optional<PresetZone> zone = Zone(low, high, region, key))
        {
            zones.push_back(*zone);
        }
    };

    // A drum set plays a region for each key from its first to its last. A key split plays the first region whose last
    // key is at or above the note's key (Driver::ReadInstrument).
    const sdat::Instrument& instrument = bank.instruments[key.program];
    switch (instrument.type)
    {
    case InstrumentType::kPcm:
    case InstrumentType::kPsg:
    case InstrumentType::kNoise:
        add(0, 127, instrument.region);
        break;
    case InstrumentType::kDrumSet:
        for (std::size_t i = 0; i < instrument.regions.size() && instrument.min_key + i <= instrument.max_key; i++)
        {
            const int note = static_cast<int>(instrument.min_key + i);
            if (note <= 127)
            {
                add(note, note, instrument.regions[i]);
            }
        }
        break;
    case InstrumentType::kKeySplit:
        {
            int below = -1;
            for (std::size_t i = 0; i < instrument.regions.size() && i < instrument.split_keys.size(); i++)
            {
                const int last = std::min<int>(instrument.split_keys[i], 127);
                if (last > below)
                {
                    add(below + 1, last, instrument.regions[i]);
                    below = last;
                }
            }
            break;
        }
    default:
        break;
    }

    return zones;
}

Sf2Envelope ConvertEnvelope(const Envelope& e, const snd::Build& build)
{
    using seqlib::sf2::ToTimecents;
    const AttackFit& attack = AttackFitOf(e.attack);
    const int sustain = std::clamp(-SustainLevel(e.sustain, build), 0, 1440);

    // A decay slower than SoundFont allows gets a longer hold. sequence_export's envelope.cpp gives such a decay the
    // same hold.
    const double decay_ms = FallMs(e.decay, kSf2Fall);
    double hold_ms = attack.full_ms - attack.ramp_ms;
    if (decay_ms > kSlowestSf2DecayMs)
    {
        hold_ms += sustain / 1000.0 * (decay_ms - kSlowestSf2DecayMs) / 2.0;
    }

    Sf2Envelope out;
    out.attack = ToTimecents(attack.ramp_ms, -12000, 8000);
    out.hold = ToTimecents(hold_ms, -12000, 5000);
    out.decay = ToTimecents(decay_ms, -12000, 8000);
    out.sustain = sustain;
    out.release = ToTimecents(FallMs(e.release, kSf2Fall), -12000, 8000);

    return out;
}

SfzEnvelope ConvertEnvelopeForSfz(const Envelope& e, const snd::Build& build)
{
    const AttackFit& attack = AttackFitOf(e.attack);
    SfzEnvelope out;
    out.attack = attack.ramp_ms / 1000.0;
    out.hold = (attack.full_ms - attack.ramp_ms) / 1000.0;
    out.decay = FallMs(e.decay, kSfzFall) / 1000.0;
    out.sustain = 100.0 * std::pow(10.0, SustainLevel(e.sustain, build) / 200.0);
    out.release = FallMs(e.release, kSfzFall) / 1000.0;

    return out;
}

BankSet::BankSet(const sdat::Archive& archive, uint16_t bank) : archive_(archive), number_(bank)
{
    if (bank >= archive.banks.size() || !archive.banks[bank])
    {
        warnings_.push_back("the archive has no bank " + std::to_string(bank));
        return;
    }

    const sdat::BankInfo& info = *archive.banks[bank];
    wave_archives_ = info.wave_archives;
    try
    {
        bank_ = sdat::Bank::Parse(archive.File(info.file_id));
    }
    catch (const xsflib::FormatError& e)
    {
        warnings_.push_back("bank " + std::to_string(bank) + " can't be read: " + e.what());
    }
}

std::optional<uint16_t> BankSet::WaveArchiveOf(uint16_t slot) const
{
    if (slot >= wave_archives_.size() || wave_archives_[slot] == 0xFFFF)
    {
        return std::nullopt;
    }

    return wave_archives_[slot];
}

std::string BankSet::WaveName(const sdat::Region& region) const
{
    switch (region.type)
    {
    case InstrumentType::kPsg:
        return "psg" + std::to_string(region.wave & 7);
    case InstrumentType::kNoise:
        return "noise";
    default:
        {
            const std::optional<uint16_t> archive = WaveArchiveOf(region.wave_archive);
            return archive ? "w" + std::to_string(*archive) + "_" + std::to_string(region.wave) : "";
        }
    }
}

const Pcm* BankSet::Wave(const sdat::Region& region)
{
    const std::string name = WaveName(region);
    if (const auto it = waves_.find(name); it != waves_.end())
    {
        return it->second ? &*it->second : nullptr;
    }

    std::optional<Pcm> pcm;
    if (region.type == InstrumentType::kPsg)
    {
        pcm.emplace().channels.push_back(ntdlib::sequence_export::PsgSamples(region.wave & 7));
    }
    else if (region.type == InstrumentType::kNoise)
    {
        pcm.emplace().channels.push_back(ntdlib::sequence_export::NoiseSamples());
    }
    else if (name.empty())
    {
        warnings_.push_back("bank " + std::to_string(number_) + " has no wave archive in its slot " +
                            std::to_string(region.wave_archive));
    }
    else
    {
        const uint16_t number = *WaveArchiveOf(region.wave_archive);
        std::optional<sdat::WaveArchive>& parsed = parsed_[number];
        try
        {
            if (!parsed)
            {
                if (number >= archive_.wave_archives.size() || !archive_.wave_archives[number])
                {
                    throw xsflib::FormatError("the archive has no wave archive " + std::to_string(number));
                }
                parsed = sdat::WaveArchive::Parse(archive_.File(archive_.wave_archives[number]->file_id & 0xFFFFFF));
            }
            if (region.wave >= parsed->waves.size())
            {
                throw xsflib::FormatError("wave archive " + std::to_string(number) + " has no wave " +
                                          std::to_string(region.wave));
            }

            const sdat::Wave& wave = parsed->waves[region.wave];
            Pcm& decoded = pcm.emplace();
            decoded.channels.push_back(sdat::DecodeWave(wave));
            decoded.sample_rate =
                wave.timer != 0 ? static_cast<uint32_t>(std::lround(kTimerHz / wave.timer)) : wave.sample_rate;
            const auto length = static_cast<uint32_t>(decoded.channels[0].size());
            if (wave.loop == 1 && wave.LoopStartSample() < length)
            {
                decoded.loop = true;
                decoded.loop_start = wave.LoopStartSample();
                decoded.loop_end = length;
            }
        }
        catch (const xsflib::FormatError& e)
        {
            pcm.reset();
            warnings_.push_back(name + " can't be read: " + e.what());
        }
    }

    if (pcm && (region.type == InstrumentType::kPsg || region.type == InstrumentType::kNoise))
    {
        pcm->sample_rate = kPsgSampleRate;
        pcm->loop = true;
        pcm->loop_start = 0;
        pcm->loop_end = static_cast<uint32_t>(pcm->channels[0].size());
    }

    const std::optional<Pcm>& added = waves_.emplace(name, std::move(pcm)).first->second;
    return added ? &*added : nullptr;
}

} // namespace ntdlib::sequence_export::ntr
