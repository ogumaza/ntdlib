// SPDX-License-Identifier: MIT

// DSE's instruments (see instruments.h).

#include "ntdlib/sequence_export/dse/instruments.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"
#include "xsflib/bytes.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/psg_samples.h"

namespace ntdlib::sequence_export::dse
{
namespace
{

using ntdlib::dse::EnvelopeValues;
using ntdlib::dse::SplitView;

// A tick of the driver in milliseconds.
constexpr double kTickMs = 1000.0 * ntdlib::dse::Player::kTickClocks / ntdlib::ntr::kSystemClockHz;

// The ticks of an envelope that the fits read: 200 s. SoundFont's slowest decay takes about 102 s.
constexpr int kEnvelopeTicks = 20000;

// The fall of 100 dB that SoundFont's decay and release times give, and SFZ's fall of e^-8, in dB.
constexpr double kSf2FallDb = 100.0;
constexpr double kSfzFallDb = 8.0 * 20.0 / 2.302585092994046;

// SoundFont's slowest decay or release, 8000 timecents, in milliseconds.
constexpr double kSlowestSf2DecayMs = 101593.6673259648;

// The most samples that a fit sums. A longer stretch of levels takes every nth.
constexpr std::size_t kFitSamples = 2000;

// The formats of a wave's byte 0x13 that play the PSG and the noise.
constexpr uint8_t kPsgFormat = 3;
constexpr uint8_t kNoiseFormat = 4;

// The minimum of `error` between `low` and `high`, by a scan and a search of golden sections.
double Minimum(const std::function<double(double)>& error, double low, double high)
{
    // A fit's error can stay flat over part of the range, such as a fall that ends before the first tick. A scan finds
    // the stretch of the lowest error, and a golden-section search narrows it.
    constexpr int kScan = 64;
    const double step = (high - low) / kScan;
    int best = 0;
    double best_error = error(low);
    for (int i = 1; i <= kScan; i++)
    {
        if (const double e = error(low + step * i); e < best_error)
        {
            best = i;
            best_error = e;
        }
    }

    const double full_low = low;
    const double full_high = high;
    low = std::max(full_low, full_low + step * (best - 1));
    high = std::min(full_high, full_low + step * (best + 1));

    constexpr double kRatio = 0.6180339887498949;
    double a = high - kRatio * (high - low);
    double b = low + kRatio * (high - low);
    double error_a = error(a);
    double error_b = error(b);
    for (int i = 0; i < 80; i++)
    {
        if (error_a <= error_b)
        {
            high = b;
            b = a;
            error_b = error_a;
            a = high - kRatio * (high - low);
            error_a = error(a);
        }
        else
        {
            low = a;
            a = b;
            error_a = error_b;
            b = low + kRatio * (high - low);
            error_b = error(b);
        }
    }

    return (low + high) / 2.0;
}

// The amplitude of a level of the driver's envelope.
double Amplitude(int level)
{
    const double v = level / 127.0;
    return v * v;
}

// The attenuation of an amplitude in centibels, up to 1440.
double Centibels(double amplitude)
{
    return amplitude > 0.0 ? std::min(-200.0 * std::log10(amplitude), 1440.0) : 1440.0;
}

// An envelope in milliseconds, before SoundFont's and SFZ's limits: an attack's linear ramp, a hold, a decay's time of
// a fall of 100 dB, its sustain in centibels of attenuation, and a release's time of a fall of 100 dB.
struct EnvelopeFit
{
    double ramp_ms = 0.0;
    double hold_ms = 0.0;
    double decay_ms = 0.0;
    double sustain_cb = 0.0;
    double release_ms = 0.0;
};

// The time of SoundFont's linear ramp whose amplitudes come closest to `levels`, tick by tick, in the least squares.
// The ramp ends within the levels.
double FitRamp(std::span<const uint8_t> levels)
{
    const double full_ms = static_cast<double>(levels.size()) * kTickMs;
    const auto error = [&](double ramp_ms)
    {
        double sum = 0.0;
        for (std::size_t k = 0; k < levels.size(); k++)
        {
            const double t = (static_cast<double>(k) + 0.5) * kTickMs;
            const double d = Amplitude(levels[k]) - std::min(1.0, t / ramp_ms);
            sum += d * d;
        }

        return sum;
    };
    return Minimum(error, 0.001, full_ms);
}

// The time of a fall of 100 dB that comes closest to `levels`, tick by tick, in amplitude in the least squares. The
// fall starts from full level and stops at `sustain_cb`.
double FitFall(std::span<const uint8_t> levels, double sustain_cb)
{
    const std::size_t step = std::max<std::size_t>(levels.size() / kFitSamples, 1);
    const double floor_db = -sustain_cb / 10.0;
    const auto error = [&](double log_ms)
    {
        const double fall_ms = std::exp(log_ms);
        double sum = 0.0;
        for (std::size_t k = 0; k < levels.size(); k += step)
        {
            const double t = (static_cast<double>(k) + 0.5) * kTickMs;
            const double db = std::max(-kSf2FallDb * t / fall_ms, floor_db);
            const double d = Amplitude(levels[k]) - std::pow(10.0, db / 20.0);
            sum += d * d;
        }

        return sum;
    };
    return std::exp(Minimum(error, std::log(0.001), std::log(kSlowestSf2DecayMs)));
}

// The release's time of a fall of 100 dB for a release that takes `ticks`. The driver's release falls linearly in level
// from where it starts to 0, in the same time from any level. Its shape relative to its start is therefore the same
// from any level, and so is its fit.
double FitRelease(int32_t ticks)
{
    static const double kRatio = []
    {
        // A release from full level over 1,000 ticks.
        std::vector<uint8_t> levels(2000, 0);
        for (std::size_t k = 0; k < 1000; k++)
        {
            levels[k] = static_cast<uint8_t>(std::lround(127.0 * (1.0 - (static_cast<double>(k) + 1.0) / 1000.0)));
        }

        return FitFall(levels, 1440.0) / (1000.0 * kTickMs);
    }();

    return kRatio * ticks * kTickMs;
}

// Fits the driver's envelope of `e`. The levels before the release start at the attack's start level, rise to their
// peak, hold it, then fall to a level that they keep or to 0. The ramp fits the rise. The hold takes the rest of the
// rise and the ticks at the peak. The decay fits the fall to a sustain at the level that the envelope keeps, or to
// silence. A fall can start in the first tick or in the tick that ends the attack. The peak then lies a step below full
// level. An envelope that stays at a peak below full level keeps it as its sustain.
EnvelopeFit FitEnvelope(const EnvelopeValues& e)
{
    const std::vector<uint8_t> levels = ntdlib::dse::EnvelopeLevels(e, kEnvelopeTicks);
    EnvelopeFit fit;
    if (const std::optional<int32_t> release = ntdlib::dse::EnvelopeTicks(e.multiplier, e.release_time))
    {
        fit.release_ms = FitRelease(std::max(*release, 1));
    }
    else
    {
        fit.release_ms = kSlowestSf2DecayMs;
    }

    const auto peak_at = std::ranges::max_element(levels);
    const int peak = *peak_at;
    const auto fall = std::find_if(peak_at, levels.end(), [peak](uint8_t level) { return level != peak; });
    if (peak < 127 && fall == levels.end())
    {
        fit.sustain_cb = Centibels(Amplitude(levels.back()));
        return fit;
    }

    const auto rise = static_cast<std::size_t>(peak_at - levels.begin());
    if (rise > 0)
    {
        fit.ramp_ms = FitRamp(std::span(levels).first(rise + 1));
    }
    fit.hold_ms = static_cast<double>(fall - levels.begin()) * kTickMs - fit.ramp_ms;

    if (fall == levels.end())
    {
        return fit;
    }

    // The level that the envelope keeps: its last, where it holds it for the last tenth of the ticks.
    const uint8_t last = levels.back();
    const bool keeps =
        std::all_of(levels.end() - kEnvelopeTicks / 10, levels.end(), [last](uint8_t level) { return level == last; });
    fit.sustain_cb = keeps ? Centibels(Amplitude(last)) : 1440.0;

    // The fit reads the fall and as many ticks again after it settles, or the rest of the ticks.
    auto end = levels.end();
    if (keeps)
    {
        const auto settled =
            std::find_if(levels.rbegin(), levels.rend(), [last](uint8_t level) { return level != last; }).base();
        end = settled + std::min(settled - fall + 1, levels.end() - settled);
    }
    const std::span<const uint8_t> falling(fall - 1, end);
    fit.decay_ms = falling.size() > 1 ? FitFall(falling, fit.sustain_cb) : 0.0;

    return fit;
}

// Returns the fit of `e`, from a cache of the envelopes fitted so far. Conversions on several threads share the
// cache, under the lock. The entries never move or change.
const EnvelopeFit& FitOf(const EnvelopeValues& e)
{
    static std::mutex mutex;
    static std::map<std::array<uint8_t, 8>, EnvelopeFit> fits;
    const std::array<uint8_t, 8> key = {e.multiplier,    e.attack_begin, e.attack_time,  e.decay_time,
                                        e.sustain_level, e.hold_time,    e.sustain_time, e.release_time};
    const std::lock_guard lock(mutex);
    if (const auto found = fits.find(key); found != fits.end())
    {
        return found->second;
    }

    return fits.emplace(key, FitEnvelope(e)).first->second;
}

// The split's value, or the channel's where the channel sets one.
uint8_t Pick(uint8_t channel, uint8_t split)
{
    return channel <= 0x7F ? channel : split;
}

// Decodes `words` words of the DS's wave `format` (0 PCM8, 1 PCM16, 2 IMA-ADPCM) from `data`.
std::vector<int16_t> Decode(std::span<const uint8_t> data, uint32_t words, uint8_t format)
{
    std::vector<int16_t> samples;
    switch (format)
    {
    case 0:
        for (std::size_t i = 0; i < std::size_t{words} * 4; i++)
        {
            samples.push_back(static_cast<int16_t>(static_cast<int8_t>(data[i]) * 256));
        }
        break;
    case 1:
        for (std::size_t i = 0; i < std::size_t{words} * 2; i++)
        {
            samples.push_back(static_cast<int16_t>(xsflib::LoadLe16(&data[2 * i])));
        }
        break;
    default:
        samples = ntdlib::sdat::DecodeAdpcm(data.first(std::size_t{words} * 4), words > 0 ? (words - 1) * 8 : 0);
        break;
    }

    return samples;
}

} // namespace

std::vector<PresetZone> PresetZones(const ntdlib::dse::WaveBank& bank, const PresetKey& key)
{
    const auto count = static_cast<int16_t>(xsflib::LoadLe16(&bank.header[0x48]));
    if (!bank.instruments || static_cast<int16_t>(key.program) >= count ||
        key.program >= bank.instruments->programs.size() || !bank.instruments->programs[key.program])
    {
        return {};
    }

    // The driver's search for a note's splits (0x0206D5C8): the splits from 0x60 on, up to the one whose index is the
    // last of the program's count.
    const std::vector<uint8_t> program = bank.instruments->programs[key.program]->Bytes();
    const auto wave_count = static_cast<int16_t>(xsflib::LoadLe16(&bank.header[0x46]));
    std::vector<PresetZone> zones;
    for (std::size_t at = 0x60; program[2] != 0 && at + SplitView::kSize <= program.size(); at += SplitView::kSize)
    {
        const SplitView split(std::span(program).subspan(at).first<SplitView::kSize>());
        const auto wave = static_cast<int16_t>(split.Wave());
        if (wave >= 0 && wave < wave_count && static_cast<std::size_t>(wave) < bank.waves.size() &&
            bank.waves[static_cast<std::size_t>(wave)] && split.LowKey() <= split.HighKey() &&
            split.LowVelocity() <= split.HighVelocity())
        {
            PresetZone zone;
            zone.low_key = std::min<uint8_t>(split.LowKey(), 127);
            zone.high_key = std::min<uint8_t>(split.HighKey(), 127);
            zone.low_velocity = std::min<uint8_t>(split.LowVelocity(), 127);
            zone.high_velocity = std::min<uint8_t>(split.HighVelocity(), 127);
            zone.wave = static_cast<uint16_t>(wave);
            zone.format = bank.waves[static_cast<std::size_t>(wave)]->bytes[0x13];
            zone.tuning = static_cast<int16_t>(split.Tuning() + (split.CoarseTuning() << 8));
            zone.volume = static_cast<uint8_t>(program[4] * split.Volume() / 127);
            zone.pan = split.Pan();

            const EnvelopeValues& c = key.envelope;
            zone.envelope = {.multiplier = Pick(c.multiplier, split.EnvelopeMultiplier()),
                             .attack_begin = Pick(c.attack_begin, split.AttackBegin()),
                             .attack_time = Pick(c.attack_time, split.AttackTime()),
                             .decay_time = Pick(c.decay_time, split.DecayTime()),
                             .sustain_level = Pick(c.sustain_level, split.SustainLevel()),
                             .hold_time = Pick(c.hold_time, split.HoldTime()),
                             .sustain_time = Pick(c.sustain_time, split.SustainTime()),
                             .release_time = Pick(c.release_time, split.ReleaseTime())};

            // The driver takes the limits of key group 0 for a group past the bank's count (0x02073EDC). Group 0
            // allows any number of voices.
            const std::size_t group = split.KeyGroup() < bank.header[0x4A] ? split.KeyGroup() : 0;
            if (group < bank.instruments->key_groups.size() && bank.instruments->key_groups[group][0] != 0 &&
                bank.instruments->key_groups[group][2] == 1)
            {
                zone.exclusive_class = split.KeyGroup();
            }

            zones.push_back(zone);
        }

        if (split.Index() >= program[2] - 1)
        {
            break;
        }
    }

    return zones;
}

Sf2Envelope ConvertEnvelope(const EnvelopeValues& e)
{
    using seqlib::sf2::ToTimecents;
    const EnvelopeFit& fit = FitOf(e);

    Sf2Envelope out;
    out.attack = ToTimecents(fit.ramp_ms, -12000, 8000);
    out.hold = ToTimecents(fit.hold_ms, -12000, 5000);
    out.decay = ToTimecents(fit.decay_ms, -12000, 8000);
    out.sustain = static_cast<int>(std::lround(std::clamp(fit.sustain_cb, 0.0, 1440.0)));
    out.release = ToTimecents(fit.release_ms, -12000, 8000);

    return out;
}

SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& e)
{
    const EnvelopeFit& fit = FitOf(e);

    SfzEnvelope out;
    out.attack = fit.ramp_ms / 1000.0;
    out.hold = fit.hold_ms / 1000.0;
    out.decay = fit.decay_ms * kSfzFallDb / kSf2FallDb / 1000.0;
    out.sustain = 100.0 * std::pow(10.0, -fit.sustain_cb / 200.0);
    out.release = fit.release_ms * kSfzFallDb / kSf2FallDb / 1000.0;

    return out;
}

std::vector<LfoShape> PresetLfos(const PresetKey& key)
{
    // A voice's LFOs (0x020752F8): the waveform's value over 256 times the LFO's level, which the fade raises to full
    // for an LFO of type 1, and which event 0xBE gives the others. The waveforms (0x020753AC to 0x020755B8) swing over
    // the LFO's amplitude times 1,024 in each phase of `phase_ms`. Waveform 3 rises to half of it in half a phase, then
    // swings between its two halves. Waveform 1 jumps between the full amplitude above and below 0, and waveform 6
    // holds random values up to half of it. The others swing between 0 and the full amplitude. Waveforms 4 to 7 take a
    // phase for a cycle, the others two.
    std::vector<LfoShape> shapes;
    for (const ntdlib::dse::LfoValues& lfo : key.lfos)
    {
        if (lfo.type == 0 || lfo.output < 1 || lfo.output > 3 || lfo.phase_ms == 0 || lfo.amplitude == 0 ||
            lfo.waveform > 7)
        {
            continue;
        }

        const double level = lfo.type == 1 ? 1.0 : std::trunc(key.lfo_level * 256.0 / 127.0) / 256.0;
        const double swing = lfo.waveform == 1 ? 4.0 : lfo.waveform == 6 ? 1.0 : 2.0;
        double depth = swing * lfo.amplitude * level;
        switch (lfo.output)
        {
        case 1:
            depth *= 100.0 / 256.0; // cents
            break;
        default:
            depth /= 64.0; // steps of the level or the pan
            break;
        }
        if (depth == 0.0)
        {
            continue;
        }

        LfoShape shape;
        shape.output = lfo.output;
        shape.waveform = lfo.waveform;
        shape.hz = 1000.0 / (lfo.waveform >= 4 ? lfo.phase_ms : 2.0 * lfo.phase_ms);
        shape.delay = lfo.type == 1 ? lfo.delay_ms / 1000.0 : 0.0;
        shape.fade = lfo.type == 1 ? lfo.envelope_ms / 1000.0 : 0.0;
        shape.depth = depth;
        shape.triangle = lfo.waveform == 3;
        shapes.push_back(shape);
    }

    return shapes;
}

double VolumeLfoDb(double depth, const PresetZone& zone)
{
    const double level = std::max<double>(zone.volume, 1.0);
    const double high = std::min(level + std::abs(depth), 127.0);
    const double low = std::max(level - std::abs(depth), 1.0);
    const double db = 20.0 * std::log10(high / low);
    return depth < 0.0 ? -db : db;
}

BankSet::BankSet(const SequenceFiles& files) : main_bank_(files.main_bank)
{
    for (const ntdlib::dse::WaveBank* bank : files.banks)
    {
        banks_.try_emplace(xsflib::LoadLe16(&bank->header[0x0E]), bank);
    }
}

const ntdlib::dse::WaveBank* BankSet::Bank(uint16_t id) const
{
    const auto found = banks_.find(id);
    return found != banks_.end() ? found->second : nullptr;
}

std::string BankSet::WaveName(uint16_t bank, uint16_t wave)
{
    return "w" + std::to_string(bank) + "_" + std::to_string(wave);
}

const Pcm* BankSet::Wave(uint16_t bank_id, uint16_t wave)
{
    const auto key = std::pair{bank_id, wave};
    if (const auto found = waves_.find(key); found != waves_.end())
    {
        return found->second ? &*found->second : nullptr;
    }

    // A bank without samples plays its waves from the main bank's, at each wave's place there. The bank's entry gives
    // the wave's format, loop and size (Player::PlaceWaves).
    std::optional<Pcm> pcm;
    const std::string name = WaveName(bank_id, wave);
    const ntdlib::dse::WaveBank* bank = Bank(bank_id);
    const ntdlib::dse::WaveInfo* info =
        bank != nullptr && wave < bank->waves.size() && bank->waves[wave] ? &*bank->waves[wave] : nullptr;
    std::span<const uint8_t> samples;
    uint32_t position = 0;
    if (info != nullptr && bank->samples)
    {
        samples = *bank->samples;
        position = info->SamplePosition();
    }
    else if (info != nullptr && main_bank_ != nullptr && main_bank_->samples && wave < main_bank_->waves.size() &&
             main_bank_->waves[wave])
    {
        samples = *main_bank_->samples;
        position = main_bank_->waves[wave]->SamplePosition();
    }

    const uint8_t format = info != nullptr ? info->bytes[0x13] : 0;
    if (info == nullptr)
    {
        warnings_.push_back(name + ": the bank has no such wave");
    }
    else if (format == kPsgFormat || format == kNoiseFormat)
    {
        // The PSG takes its duty from the wave's byte 0x19. Both loop for as long as they play.
        Pcm& decoded = pcm.emplace();
        decoded.channels.push_back(format == kPsgFormat ? ntdlib::sequence_export::PsgSamples(info->bytes[0x19] & 7)
                                                        : ntdlib::sequence_export::NoiseSamples());
        decoded.sample_rate = ntdlib::sequence_export::kPsgSampleRate;
        decoded.loop = true;
        decoded.loop_end = static_cast<uint32_t>(decoded.channels[0].size());
    }
    else if (samples.empty())
    {
        warnings_.push_back(name + ": no bank holds the wave's samples");
    }
    else if (format > 2)
    {
        warnings_.push_back(name + ": the wave's format " + std::to_string(format) + " is unknown");
    }
    else
    {
        // The sound unit reads past the samples' end what lies in memory. The player places zeros there.
        const uint32_t words = info->LoopStart() + info->LoopLength();
        std::vector<uint8_t> data(std::size_t{words} * 4, 0);
        if (position < samples.size())
        {
            const std::size_t copied = std::min<std::size_t>(data.size(), samples.size() - position);
            std::copy_n(samples.begin() + position, copied, data.begin());
            if (copied < data.size())
            {
                warnings_.push_back(name + ": the wave runs past its samples' end (played with zeros)");
            }
        }

        Pcm& decoded = pcm.emplace();
        decoded.channels.push_back(Decode(data, words, format));
        decoded.sample_rate = info->SampleRate();

        const auto length = static_cast<uint32_t>(decoded.channels[0].size());
        const uint32_t loop_start =
            format == 2 ? (info->LoopStart() > 0 ? (info->LoopStart() - 1) * 8 : 0) : info->LoopStart() * (4 >> format);
        if (info->Loops() && loop_start < length)
        {
            decoded.loop = true;
            decoded.loop_start = loop_start;
            decoded.loop_end = length;
        }

        // A SoundFont's sample needs a rate. The zones' tunings give the pitch at any rate.
        if (decoded.sample_rate == 0)
        {
            decoded.sample_rate = 32728;
        }
    }

    const std::optional<Pcm>& added = waves_.emplace(key, std::move(pcm)).first->second;
    return added ? &*added : nullptr;
}

} // namespace ntdlib::sequence_export::dse
