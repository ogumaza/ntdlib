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
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/units.h"
#include "seqlib/sfz/opcodes.h"
#include "xsflib/bytes.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/psg_samples.h"
#include "ntdlib/sequence_export/sf2_limits.h"
#include "sequence_export/key_runs.h"

namespace ntdlib::sequence_export::dse
{
namespace
{

using ntdlib::dse::EnvelopeValues;
using ntdlib::dse::SplitView;

// The ticks of an envelope that the fits read: 200 s. SoundFont's slowest decay takes about 102 s.
constexpr int kEnvelopeTicks = 20000;

// The most samples that a fit sums. A longer stretch of levels takes every nth.
constexpr std::size_t kFitSamples = 2000;

// The formats of a wave's byte 0x13 that play the PSG and the noise.
constexpr uint8_t kPsgFormat = 3;
constexpr uint8_t kNoiseFormat = 4;

// The rate of a SoundFont's sample whose wave gives a rate of 0. A SoundFont's sample requires a rate.
constexpr uint32_t kDefaultSampleRate = 32728;

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
    return amplitude > 0.0 ? std::min<double>(-200.0 * std::log10(amplitude), seqlib::sf2::kMaxCentibels)
                           : double{seqlib::sf2::kMaxCentibels};
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
// The ramp ends within the levels. A tick takes `tick_ms`.
double FitRamp(std::span<const uint8_t> levels, double tick_ms)
{
    const double full_ms = static_cast<double>(levels.size()) * tick_ms;
    const auto error = [&](double ramp_ms)
    {
        double sum = 0.0;
        for (std::size_t k = 0; k < levels.size(); k++)
        {
            const double t = (static_cast<double>(k) + 0.5) * tick_ms;
            const double d = Amplitude(levels[k]) - std::min(1.0, t / ramp_ms);
            sum += d * d;
        }

        return sum;
    };
    return Minimum(error, 0.001, full_ms);
}

// The time of a fall of 100 dB that comes closest to `levels`, tick by tick, in amplitude in the least squares. The
// fall starts from full level and stops at `sustain_cb`. A tick takes `tick_ms`.
double FitFall(std::span<const uint8_t> levels, double sustain_cb, double tick_ms)
{
    const std::size_t step = std::max<std::size_t>(levels.size() / kFitSamples, 1);
    const double floor_db = -sustain_cb / 10.0;
    const auto error = [&](double log_ms)
    {
        const double fall_ms = std::exp(log_ms);
        double sum = 0.0;
        for (std::size_t k = 0; k < levels.size(); k += step)
        {
            const double t = (static_cast<double>(k) + 0.5) * tick_ms;
            const double db = std::max(-seqlib::sf2::kEnvelopeFallDb * t / fall_ms, floor_db);
            const double d = Amplitude(levels[k]) - std::pow(10.0, db / 20.0);
            sum += d * d;
        }

        return sum;
    };
    return std::exp(Minimum(error, std::log(0.001), std::log(kSlowestSf2DecayMs)));
}

// The release's time of a fall of 100 dB for a release that takes `ticks` of `tick_ms`. The driver's release falls
// linearly in level from where it starts to 0, in the same time from any level. Its shape relative to its start is
// therefore the same from any level, and so is its fit.
double FitRelease(int32_t ticks, double tick_ms)
{
    // The fit of a release from full level over 1,000 ticks, for each length of a tick. Conversions on several threads
    // share the fits, under the lock.
    static std::mutex mutex;
    static std::map<double, double> ratios;
    double ratio = 0.0;
    {
        const std::lock_guard lock(mutex);
        auto found = ratios.find(tick_ms);
        if (found == ratios.end())
        {
            std::vector<uint8_t> levels(2000, 0);
            for (std::size_t k = 0; k < 1000; k++)
            {
                levels[k] = static_cast<uint8_t>(std::lround(127.0 * (1.0 - (static_cast<double>(k) + 1.0) / 1000.0)));
            }
            found = ratios.emplace(tick_ms, FitFall(levels, seqlib::sf2::kMaxCentibels, tick_ms) / (1000.0 * tick_ms))
                        .first;
        }
        ratio = found->second;
    }

    return ratio * ticks * tick_ms;
}

// Fits the driver's envelope of `e`. The levels before the release start at the attack's start level, rise to their
// peak, hold it, then fall to a level that they keep or to 0. The ramp fits the rise. The hold takes the rest of the
// rise and the ticks at the peak. The decay fits the fall to a sustain at the level that the envelope keeps, or to
// silence. A fall can start in the first tick or in the tick that ends the attack. The peak is then a step below full
// level. An envelope that stays at a peak below full level keeps it as its sustain. The driver's ticks take
// `tick_microseconds` in the game's settings.
EnvelopeFit FitEnvelope(const EnvelopeValues& e, int tick_microseconds)
{
    const double tick_ms = ntdlib::dse::TickMs(tick_microseconds);
    const std::vector<uint8_t> levels = ntdlib::dse::EnvelopeLevels(e, tick_microseconds, kEnvelopeTicks);
    EnvelopeFit fit;
    if (const std::optional<int32_t> release =
            ntdlib::dse::EnvelopeTicks(e.multiplier, e.release_time, tick_microseconds))
    {
        fit.release_ms = FitRelease(std::max(*release, 1), tick_ms);
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
        fit.ramp_ms = FitRamp(std::span(levels).first(rise + 1), tick_ms);
    }
    fit.hold_ms = static_cast<double>(fall - levels.begin()) * tick_ms - fit.ramp_ms;

    if (fall == levels.end())
    {
        return fit;
    }

    // The level that the envelope keeps: its last, where it holds it for the last tenth of the ticks.
    const uint8_t last = levels.back();
    const bool keeps =
        std::all_of(levels.end() - kEnvelopeTicks / 10, levels.end(), [last](uint8_t level) { return level == last; });
    fit.sustain_cb = keeps ? Centibels(Amplitude(last)) : double{seqlib::sf2::kMaxCentibels};

    // The fit reads the fall and as many ticks again after it settles, or the rest of the ticks.
    auto end = levels.end();
    if (keeps)
    {
        const auto settled =
            std::find_if(levels.rbegin(), levels.rend(), [last](uint8_t level) { return level != last; }).base();
        end = settled + std::min(settled - fall + 1, levels.end() - settled);
    }
    const std::span<const uint8_t> falling(fall - 1, end);
    fit.decay_ms = falling.size() > 1 ? FitFall(falling, fit.sustain_cb, tick_ms) : 0.0;

    return fit;
}

// Returns the fit of `e` with a tick of `tick_microseconds`, from a cache of the envelopes fitted so far. Conversions
// on several threads share the cache, under the lock. The entries never move or change.
const EnvelopeFit& FitOf(const EnvelopeValues& e, int tick_microseconds)
{
    static std::mutex mutex;
    static std::map<std::pair<int, std::array<uint8_t, 8>>, EnvelopeFit> fits;
    const std::array<uint8_t, 8> values = {e.multiplier,    e.attack_begin, e.attack_time,  e.decay_time,
                                           e.sustain_level, e.hold_time,    e.sustain_time, e.release_time};
    const std::pair key{tick_microseconds, values};
    const std::lock_guard lock(mutex);
    if (const auto found = fits.find(key); found != fits.end())
    {
        return found->second;
    }

    return fits.emplace(key, FitEnvelope(e, tick_microseconds)).first->second;
}

// The split's value, or the channel's where the channel sets one.
uint8_t Pick(uint8_t channel, uint8_t split)
{
    return channel <= 0x7F ? channel : split;
}

// The rate of the sound unit's timers: half the system clock.
constexpr double kTimerHz = ntdlib::ntr::kSystemClockHz / 2.0;

// The cents above `sample_rate` at which a voice of a wave of `format` plays at `pitch`, in 256ths of a semitone, or
// nothing where the timer gives no rate. The driver's commands give the channel the timer's low 16 bits. The PSG's
// lowest keys pass 0xFFFF and play higher.
std::optional<double> PitchCents(uint8_t format, uint32_t sample_rate, uint16_t pitch)
{
    const int32_t timer = ntdlib::dse::VoiceTimer(pitch, format) & 0xFFFF;
    if (timer == 0 || sample_rate == 0)
    {
        return std::nullopt;
    }

    // The PSG's and the noise's timers step them once a period, kPsgStepSamples samples of their waves.
    const double rate = kTimerHz / timer * (format >= kPsgFormat ? ntdlib::sequence_export::kPsgStepSamples : 1);
    return 1200.0 * std::log2(rate / sample_rate);
}

// PitchCents for key `key` of `zone` without a bend.
std::optional<double> KeyCents(const PresetZone& zone, uint32_t sample_rate, int key)
{
    return PitchCents(zone.format, sample_rate, static_cast<uint16_t>(zone.tuning + (key << 8)));
}

// The sum of `value` of the volume swings of the volume LFOs in `shapes` for `zone`, in dB.
double SumOfVolumeLfos(const std::vector<LfoShape>& shapes, const PresetZone& zone, double VolumeSwing::* value)
{
    double sum = 0.0;
    for (const LfoShape& shape : shapes)
    {
        if (shape.output == kVolumeOutput)
        {
            sum += VolumeSwingOf(shape, zone).*value;
        }
    }

    return sum;
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

            // The oldest build plays a split's multiplier of 16 or more as 1 (0x020C44B4 in Luminous Arc's ARM9
            // binary).
            const EnvelopeValues& c = key.envelope;
            const uint8_t multiplier = key.build == ntdlib::dse::Build::kOldest && split.EnvelopeMultiplier() >= 0x10
                                           ? 1
                                           : split.EnvelopeMultiplier();
            zone.envelope = {.multiplier = Pick(c.multiplier, multiplier),
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

Sf2Envelope ConvertEnvelope(const EnvelopeValues& e, int tick_microseconds)
{
    using seqlib::sf2::ToTimecents;
    const EnvelopeFit& fit = FitOf(e, tick_microseconds);

    Sf2Envelope out;
    out.attack = ToTimecents(fit.ramp_ms, seqlib::sf2::kShortestTimecents, seqlib::sf2::kLongestTimecents);
    out.hold = ToTimecents(fit.hold_ms, seqlib::sf2::kShortestTimecents, seqlib::sf2::kLongestHoldTimecents);
    out.decay = ToTimecents(fit.decay_ms, seqlib::sf2::kShortestTimecents, seqlib::sf2::kLongestTimecents);
    out.sustain = static_cast<int>(std::lround(std::clamp<double>(fit.sustain_cb, 0.0, seqlib::sf2::kMaxCentibels)));
    out.release = ToTimecents(fit.release_ms, seqlib::sf2::kShortestTimecents, seqlib::sf2::kLongestTimecents);

    return out;
}

SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& e, int tick_microseconds)
{
    const EnvelopeFit& fit = FitOf(e, tick_microseconds);

    SfzEnvelope out;
    out.attack = fit.ramp_ms / 1000.0;
    out.hold = fit.hold_ms / 1000.0;
    out.decay = fit.decay_ms * seqlib::sfz::kEnvelopeFallDb / seqlib::sf2::kEnvelopeFallDb / 1000.0;
    out.sustain = 100.0 * std::pow(10.0, -fit.sustain_cb / 200.0);
    out.release = fit.release_ms * seqlib::sfz::kEnvelopeFallDb / seqlib::sf2::kEnvelopeFallDb / 1000.0;

    return out;
}

std::vector<LfoShape> PresetLfos(const PresetKey& key, int tick_microseconds)
{
    // A voice's LFOs (0x020752F8): the waveform's value over 256 times the LFO's level. The fade raises the level to
    // full for an LFO of type 1, and event 0xBE sets it for the others. The waveforms (0x020753AC to 0x020755B8) swing
    // over the LFO's amplitude times 1,024 in each phase of `phase_ms`. Waveform 3 rises to half of it in half a phase,
    // then swings between its two halves. Waveform 1 jumps between the full amplitude above and below 0. Waveform 6
    // holds random values from 0 up to half of it, and waveform 7 random values within half of it around 0. The others
    // swing between 0 and the full amplitude. Waveforms 4 to 7 take a phase for a cycle, the others two. A phase lasts
    // phase_ms * 1000 / tick whole ticks, at least one (0x020750B4).
    std::vector<LfoShape> shapes;
    for (const ntdlib::dse::LfoValues& lfo : key.lfos)
    {
        if (lfo.type == 0 || lfo.output < kPitchOutput || lfo.output > kPanOutput || lfo.phase_ms == 0 ||
            lfo.amplitude == 0 || lfo.waveform > 7)
        {
            continue;
        }

        const double level = lfo.type == 1 ? 1.0 : std::trunc(key.lfo_level * 256.0 / 127.0) / 256.0;
        // The oldest build's waveform 3 swings between 0 and twice the amplitude, in four phases after its first
        // cycle (0x020C572C in Luminous Arc's ARM9 binary). SoundFont's LFO keeps the swing around 0, and `offset`
        // keeps the rest.
        const bool oldest_triangle = key.build == ntdlib::dse::Build::kOldest && lfo.waveform == 3;
        const double swing = lfo.waveform == 1 || oldest_triangle ? 4.0 : lfo.waveform == 6 ? 1.0 : 2.0;
        double depth = swing * lfo.amplitude * level;
        switch (lfo.output)
        {
        case kPitchOutput:
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
        const int phase_ticks = std::max(lfo.phase_ms * 1000 / std::max(tick_microseconds, 1), 1);
        const double phase_ms = phase_ticks * tick_microseconds / 1000.0;
        shape.hz = 1000.0 / (lfo.waveform >= 4 ? phase_ms : (oldest_triangle ? 4.0 : 2.0) * phase_ms);
        shape.delay = lfo.type == 1 ? lfo.delay_ms / 1000.0 : 0.0;
        shape.fade = lfo.type == 1 ? lfo.envelope_ms / 1000.0 : 0.0;
        shape.depth = depth;
        // Waveforms 1, 3 and 7 swing around 0. In the oldest build, waveform 3 swings from 0 to twice the amplitude,
        // and waveform 7 takes its values from the whole amplitude below 0 (0x020C5808). The others swing from 0 to
        // the side of the amplitude.
        const bool oldest = key.build == ntdlib::dse::Build::kOldest;
        if (lfo.waveform == 7)
        {
            shape.offset = oldest ? -depth : 0.0;
        }
        else if (lfo.waveform != 1 && (lfo.waveform != 3 || oldest_triangle))
        {
            shape.offset = depth;
        }
        shape.triangle = lfo.waveform == 3;
        shapes.push_back(shape);
    }

    return shapes;
}

VolumeSwing VolumeSwingOf(const LfoShape& lfo, const PresetZone& zone)
{
    // The driver's level over the swing is v + offset + depth * w for w from -1 to 1, held from 0 to 127 and squared.
    // A held level stays at 127 or 0 for part of each cycle. The shift and the swing are the straight line in dB over w
    // that fits the driver's level best, by least squares. Waveforms 0 and 1 are squares (0x020753AC): the level sits
    // at w = -1 and at w = 1 for half a cycle each, and the line runs through both. The other waveforms take every w
    // between them. A level of 0 is silent, and the fit counts a level more than 40 dB below the zone's at 40 dB below.
    // The mean power takes the same values of w, and counts a level of 0 as silent.
    constexpr int kPoints = 256;
    constexpr double kLowestDb = -40.0;
    const bool square = lfo.waveform == 0 || lfo.waveform == 1;
    const int points = square ? 2 : kPoints;
    const double level = std::max<double>(zone.volume, 1.0);

    double sum = 0.0;
    double sum_w = 0.0;
    double sum_ww = 0.0;
    double power = 0.0;
    for (int i = 0; i < points; i++)
    {
        const double w = square ? (i == 0 ? -1.0 : 1.0) : (2.0 * i + 1.0) / points - 1.0;
        const double held = std::clamp(level + lfo.offset + lfo.depth * w, 0.0, 127.0);
        const double db = held > 0.0 ? std::max(40.0 * std::log10(held / level), kLowestDb) : kLowestDb;
        sum += db;
        sum_w += db * w;
        sum_ww += w * w;
        power += std::pow(held / level, 4.0);
    }

    VolumeSwing out;
    out.shift = sum / points;
    out.swing = sum_w / sum_ww;
    out.power_mean = power > 0.0 ? 10.0 * std::log10(power / points) : kLowestDb;
    return out;
}

double VolumeLfoDb(double depth, const PresetZone& zone)
{
    const double level = std::max<double>(zone.volume, 1.0);
    const double high = std::min(level + std::abs(depth), 127.0);
    const double low = std::max(level - std::abs(depth), 1.0);
    const double db = 20.0 * std::log10(high / low);
    return depth < 0.0 ? -db : db;
}

double VolumeShiftDb(const std::vector<LfoShape>& shapes, const PresetZone& zone)
{
    return SumOfVolumeLfos(shapes, zone, &VolumeSwing::shift);
}

double VolumeMeanDb(const std::vector<LfoShape>& shapes, const PresetZone& zone)
{
    return SumOfVolumeLfos(shapes, zone, &VolumeSwing::power_mean);
}

ZoneTuning TuningOf(const PresetZone& zone, uint32_t sample_rate)
{
    return TuningOf(zone, sample_rate, std::clamp(60, static_cast<int>(zone.low_key), static_cast<int>(zone.high_key)));
}

ZoneTuning TuningOf(const PresetZone& zone, uint32_t sample_rate, int key)
{
    ZoneTuning tuning;
    tuning.root_key = std::clamp(60, static_cast<int>(zone.low_key), static_cast<int>(zone.high_key));
    const std::optional<double> at_key = KeyCents(zone, sample_rate, key);
    if (!at_key)
    {
        return tuning;
    }

    // A SoundFont or SFZ player plays `key` 100 cents a key from the root key.
    const double cents = *at_key - 100.0 * (key - tuning.root_key);
    tuning.coarse = static_cast<int>(std::lround(cents / 100.0));
    tuning.fine = static_cast<int>(std::lround(cents - 100.0 * tuning.coarse));
    return tuning;
}

std::vector<TunedZone> TunedZones(const PresetZone& zone, uint32_t sample_rate, const std::set<uint8_t>* keys)
{
    // A run contains the keys whose tuning in whole cents above the key's nominal pitch is the same. A zone without a
    // played key takes the tuning at its root key.
    const auto offset = [&](int key)
    {
        const std::optional<double> cents = KeyCents(zone, sample_rate, key);
        return cents ? static_cast<int>(std::lround(*cents - 100.0 * key)) : 0;
    };

    std::vector<TunedZone> tuned_zones;
    for (const KeyRun& r : TuningRuns(zone.low_key, zone.high_key, keys, offset))
    {
        PresetZone run = zone;
        run.low_key = static_cast<uint8_t>(r.low_key);
        run.high_key = static_cast<uint8_t>(r.high_key);
        const ZoneTuning tuning =
            r.tuned_key >= 0 ? TuningOf(run, sample_rate, r.tuned_key) : TuningOf(run, sample_rate);
        tuned_zones.push_back({run, tuning});
    }

    return tuned_zones;
}

std::optional<double> TimerBend(const PresetZone& zone, uint32_t sample_rate, int key, uint16_t pitch)
{
    const std::optional<double> timer = PitchCents(zone.format, sample_rate, pitch);
    if (!timer || !KeyCents(zone, sample_rate, key))
    {
        return std::nullopt;
    }

    // The zone plays the key at its root key's pitch, its tuning and a semitone a key.
    const ZoneTuning tuning = TuningOf(zone, sample_rate, key);
    const double zone_cents = 100.0 * (key - tuning.root_key) + 100.0 * tuning.coarse + tuning.fine;

    return (*timer - zone_cents) / 100.0;
}

uint32_t SampleRateOf(uint8_t format, uint32_t rate)
{
    if (format == kPsgFormat || format == kNoiseFormat)
    {
        return ntdlib::sequence_export::kPsgSampleRate;
    }

    return rate != 0 ? rate : kDefaultSampleRate;
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

void BankSet::SetSfzNames(std::map<uint16_t, std::string> names)
{
    sfz_names_ = std::move(names);
}

std::string BankSet::SfzWaveName(uint16_t bank, uint16_t wave) const
{
    const auto found = sfz_names_.find(bank);
    return found != sfz_names_.end() ? found->second + "_" + std::to_string(wave) : WaveName(bank, wave);
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
        decoded.sample_rate = SampleRateOf(format, info->SampleRate());
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
        // Past the samples' end, the sound unit reads whatever memory contains. The player places zeros there.
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
        // A format past 2 decodes as IMA-ADPCM.
        const auto encoding = static_cast<ntdlib::sdat::WaveEncoding>(std::min<uint8_t>(format, 2));
        decoded.channels.push_back(ntdlib::sdat::DecodeWords(data, words, encoding));
        decoded.sample_rate = SampleRateOf(format, info->SampleRate());

        const auto length = static_cast<uint32_t>(decoded.channels[0].size());
        const uint32_t loop_start =
            format == 2 ? (info->LoopStart() > 0 ? (info->LoopStart() - 1) * 8 : 0) : info->LoopStart() * (4 >> format);
        if (info->Loops() && loop_start < length)
        {
            decoded.loop = true;
            decoded.loop_start = loop_start;
            decoded.loop_end = length;
        }
    }

    const std::optional<Pcm>& added = waves_.emplace(key, std::move(pcm)).first->second;
    return added ? &*added : nullptr;
}

} // namespace ntdlib::sequence_export::dse
