// SPDX-License-Identifier: MIT

// Tests of the conversion of the DS's sound archives (ntdlib/sequence_export/ntr): the pan law, the envelopes, the
// presets, the waves, the timers' tuning, the pan LFO and the performance of a sequence on archives that
// sdat::BuildArchive lays out. The expected values are worked out by hand from the sound unit and the driver: the
// mixer's linear pan, the timers' rate, the periods that the driver's timer drops the fraction of, the PSG's duties,
// the noise's shift register, the driver's tempo, its rates of attack, decay and release, and its table of levels.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/sdat_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export_ntr_cases.h"

namespace
{

using namespace sequence_export_ntr_cases;

// A frame of the driver in milliseconds: 2728 * 64 cycles of the system clock of 33,513,982 Hz.
constexpr double kFrameMs = 1000.0 * 2728 * 64 / 33513982.0;

// The pan law. The DS's mixer plays the left side at (128 - p) / 128 and the right at p / 128, and the right at 128/128
// for a pan of 127. The sin/cos law at PanPosition, times PanGain, gives those levels.
void TestPanLaw()
{
    bool matches = true;
    for (int pan = 0; pan < 128; pan++)
    {
        const double right = pan == 127 ? 1.0 : pan / 128.0;
        const double angle = (PanPosition(pan) + 1.0) * std::numbers::pi / 4.0;
        matches = matches && std::abs(PanGain(pan) * std::cos(angle) - (1.0 - right)) < 1e-12 &&
                  std::abs(PanGain(pan) * std::sin(angle) - right) < 1e-12;
    }

    Check(matches, "the sin/cos law at PanPosition, times PanGain, gives the DS's levels of both sides");
    Check(PanPosition(0) == -1.0 && PanPosition(127) == 1.0 && std::abs(PanPosition(64)) < 1e-12,
          "the pans of 0, 64 and 127 are full left, the centre and full right");
    Check(PanGain(0) == 1.0 && PanGain(127) == 1.0 && std::abs(PanGain(64) - std::sqrt(0.5)) < 1e-12,
          "the centre plays at 0.707 of the sides");
}

// The envelopes. A decay or release of v falls 128 units of 0.1 dB a frame at the driver's rate: 0xFFFF for 127, v * 2
// + 1 below 50, and 0x1E00 / (126 - v) from 50 to 125. SoundFont's times give a fall of 100 dB, and SFZ's a fall of
// e^-8. A sustain of v plays at kDecibelSquare[v]: -11.9 dB for 64 and -1.0 dB for 120. An attack of a below 109
// multiplies the attenuation by (255 - a) / 256 each frame, from -92544. It reaches full level after 1653 frames for 0,
// 36 for 64 and 22 for 100. 109 and above take their factors from a table: 19 frames for 109 and 3 for 126.
void TestEnvelopes()
{
    const ntdlib::ntr::snd::Build build;
    const Sf2Envelope fastest = ConvertEnvelope({}, build);
    const Sf2Envelope envelope = ConvertEnvelope({.decay = 30, .sustain = 64, .release = 100}, build);
    const Sf2Envelope silent = ConvertEnvelope({.sustain = 0}, build);
    const Sf2Envelope slow = ConvertEnvelope({.decay = 0, .sustain = 120}, build);
    const SfzEnvelope sfz = ConvertEnvelopeForSfz({.decay = 30, .sustain = 64, .release = 100}, build);

    // 127's decay and release: 128000 / 0xFFFF frames, -7942.6 timecents. Its attack reaches full level in 1 frame.
    Check(fastest.attack == -12000 && fastest.hold == -9102 && fastest.decay == -7943 && fastest.sustain == 0 &&
              fastest.release == -7943,
          "the fastest envelope: no ramp, a frame of hold, and the fastest fall");
    // A decay of 30 falls 61 units a frame: 10931.5 ms for 100 dB. A release of 100 falls 295: 2260.4 ms.
    Check(envelope.decay == 4140 && envelope.sustain == 119 && envelope.release == 1412,
          "the decay's and release's times for 100 dB, and the sustain from the driver's table");
    Check(silent.sustain == 1440, "a sustain of 0 is silent");
    // A decay of 0 falls one unit a frame: 666,819 ms for 100 dB, slower than SoundFont's slowest. The hold gets half
    // the difference of the times that the two decays take to fall 1.0 dB.
    Check(slow.decay == 8000 && slow.hold == 1802, "a decay slower than SoundFont's holds longer");
    // SFZ's e^-8 is 69.49 dB: 88943.5 units.
    Check(std::abs(sfz.decay - 7.595961) < 1e-6 && std::abs(sfz.release - 1.570690) < 1e-6 &&
              std::abs(sfz.sustain - 25.409727) < 1e-6,
          "SFZ's decay and release for e^-8, and its sustain in percent");

    const int frames[] = {1653, 36, 22, 19, 3, 1};
    const uint8_t attacks[] = {0, 64, 100, 109, 126, 127};
    bool full = true;
    bool ramps = true;
    for (std::size_t i = 0; i < std::size(attacks); i++)
    {
        const SfzEnvelope e = ConvertEnvelopeForSfz({.attack = attacks[i]}, build);
        full = full && std::abs(e.attack + e.hold - frames[i] * kFrameMs / 1000.0) < 1e-9;
        ramps = ramps && e.attack >= 0.0 && e.hold >= 0.0 && (attacks[i] == 127 || e.attack > 0.0);
    }
    Check(full, "an attack's ramp and hold take the frames that the driver's attack takes to reach full level");
    Check(ramps, "every attack but 127's has a ramp");
}

// The zones of the bank's programs.
void TestPresetZones()
{
    const TestArchive test(kOneNote);
    BankSet banks(test.archive, 0);
    const sdat::Bank* bank = banks.Bank();
    Check(bank != nullptr && bank->instruments.size() == 8, "the bank reads");
    if (bank == nullptr || bank->instruments.size() != 8)
    {
        return;
    }

    const std::vector<PresetZone> pcm = PresetZones(*bank, {.program = 0});
    const std::vector<PresetZone> drums = PresetZones(*bank, {.program = 3});
    const std::vector<PresetZone> split = PresetZones(*bank, {.program = 4});
    const std::vector<PresetZone> region_envelope = PresetZones(*bank, {.program = 7});
    const std::vector<PresetZone> track_envelope = PresetZones(*bank, {.program = 7, .attack = 127, .release = 50});

    Check(pcm.size() == 1 && pcm[0].low_key == 0 && pcm[0].high_key == 127 && pcm[0].region.wave == 0,
          "a PCM instrument plays every key");
    Check(drums.size() == 3 && drums[0].low_key == 36 && drums[0].high_key == 36 && drums[0].region.root_key == 36 &&
              drums[1].low_key == 37 && drums[1].region.type == sdat::InstrumentType::kPsg && drums[2].low_key == 38 &&
              drums[2].region.type == sdat::InstrumentType::kNoise,
          "a drum set gives each of its keys a zone");
    Check(split.size() == 2 && split[0].low_key == 0 && split[0].high_key == 48 && split[0].region.wave == 0 &&
              split[1].low_key == 49 && split[1].high_key == 127 && split[1].region.type == sdat::InstrumentType::kPsg,
          "a key split plays the first region whose last key is at or above the note's");
    Check(PresetZones(*bank, {.program = 5}).empty() && PresetZones(*bank, {.program = 6}).empty() &&
              PresetZones(*bank, {.program = 8}).empty(),
          "a wave at an address, no instrument, and a program past the bank have no zones");
    Check(region_envelope.size() == 1 && region_envelope[0].envelope.attack == 100 &&
              region_envelope[0].envelope.decay == 30 && region_envelope[0].envelope.sustain == 64 &&
              region_envelope[0].envelope.release == 0 && region_envelope[0].region.pan == 0,
          "the region's envelope, with a release of 0xFF as 0");
    Check(track_envelope.size() == 1 && track_envelope[0].envelope.attack == 127 &&
              track_envelope[0].envelope.decay == 30 && track_envelope[0].envelope.release == 50,
          "the track's values take the place of the region's");
}

// The waves. A PCM wave plays at the rate of its timer: 16,756,991 Hz over the timer. The PSG's duty d holds its last
// d + 1 steps of 8 high, and duty 7 stays low. The noise's shift register starts at 0x7FFF. Each step shifts it right
// once, and a bit shifted out turns the output low and the register's bits 13 and 14 over: 14 low steps, then 10 high.
void TestWaves()
{
    const TestArchive test(kOneNote);
    BankSet banks(test.archive, 0);
    const auto region = [](sdat::InstrumentType type, uint16_t wave, uint16_t wave_archive = 0)
    {
        sdat::Region r;
        r.type = type;
        r.wave = wave;
        r.wave_archive = wave_archive;
        return r;
    };
    using enum sdat::InstrumentType;
    const Pcm* square = banks.Wave(region(kPcm, 0));
    const Pcm* ramp = banks.Wave(region(kPcm, 1));
    const Pcm* half = banks.Wave(region(kPsg, 3));
    const Pcm* eighth = banks.Wave(region(kPsg, 0));
    const Pcm* low = banks.Wave(region(kPsg, 7));
    const Pcm* noise = banks.Wave(region(kNoise, 0));
    const Pcm* no_slot = banks.Wave(region(kPcm, 0, 1));
    const Pcm* no_wave = banks.Wave(region(kPcm, 5));

    const auto runs = [](const Pcm* pcm, const std::vector<std::pair<int, int16_t>>& expected)
    {
        if (pcm == nullptr || pcm->channels.size() != 1)
        {
            return false;
        }
        std::size_t at = 0;
        for (const auto& [count, value] : expected)
        {
            for (int i = 0; i < count; i++, at++)
            {
                if (at >= pcm->channels[0].size() || pcm->channels[0][at] != value)
                {
                    return false;
                }
            }
        }
        return true;
    };
    const auto looped = [](const Pcm* pcm, std::size_t length)
    {
        return pcm != nullptr && pcm->channels[0].size() == length && pcm->loop && pcm->loop_start == 0 &&
               pcm->loop_end == length && pcm->sample_rate == kPsgSampleRate;
    };

    Check(square != nullptr && square->sample_rate == 32793 && square->loop && square->loop_start == 0 &&
              square->loop_end == 16 && runs(square, {{2, 16384}, {2, -16384}, {2, 16384}}),
          "a PCM8 wave plays at its timer's rate, and loops");
    Check(ramp != nullptr && ramp->sample_rate == 16396 && !ramp->loop && runs(ramp, {{1, 0}, {1, 0x1000}}) &&
              ramp->channels[0].size() == 8,
          "a PCM16 wave that plays once");
    Check(looped(half, 128) && runs(half, {{64, -0x7FFF}, {64, 0x7FFF}}), "duty 3 holds half its steps high");
    Check(looped(eighth, 128) && runs(eighth, {{112, -0x7FFF}, {16, 0x7FFF}}), "duty 0 holds one step high");
    Check(looped(low, 128) && runs(low, {{128, 0}}), "duty 7 stays low");
    Check(looped(noise, std::size_t{0x7FFF} * 16) && runs(noise, {{14 * 16, -0x7FFF}, {10 * 16, 0x7FFF}}),
          "the noise's steps");
    Check(banks.WaveName(region(kPcm, 1)) == "w0_1" && banks.WaveName(region(kPsg, 3)) == "psg3" &&
              banks.WaveName(region(kNoise, 0)) == "noise" && banks.WaveName(region(kPcm, 0, 1)).empty(),
          "the waves' names");
    Check(no_slot == nullptr && no_wave == nullptr &&
              banks.Warnings() == std::vector<std::string>{"bank 0 has no wave archive in its slot 1",
                                                           "w0_5 can't be read: wave archive 0 has no wave 5"},
          "a missing wave archive and a missing wave give warnings");
}

// The tuning of a key at its timer's rate. Wave 1's timer is 1022, and its sample plays at 16,756,991 / 1022 Hz,
// 16,396 rounded. The driver's period at n octaves above the root key is 1022 / 2^n without its fraction: 255 two
// octaves up, 127 three and 63 four. The PSG's timer of 8006 two octaves up gives 2001, and the PSG drops the low two
// bits: 2000. Its sample plays 16 times the steps of 8006 at the root key, at 33,489 Hz.
void TestTimerTuning()
{
    const auto cents = [](double period, double rate)
    {
        return 1200.0 * std::log2(16756991.0 / period / rate);
    };
    Check(KeyTuning(0, 1022, 60, 60) == 0 && KeyTuning(0, 1022, 60, 84) == std::lround(cents(255, 16396 * 4.0)) &&
              KeyTuning(0, 1022, 60, 96) == std::lround(cents(127, 16396 * 8.0)) &&
              KeyTuning(0, 1022, 60, 108) == std::lround(cents(63, 16396 * 16.0)),
          "a key takes the cents by which its period plays sharp: 0, 3, 10 and 24");
    Check(KeyTuning(0, 1022, 60, 108) == 24 &&
              KeyTuning(1, 8006, 69, 93) == std::lround(cents(2000, 33489 * 4.0 / 16)) &&
              KeyTuning(2, 8006, 69, 93) == std::lround(cents(2001, 33489 * 4.0 / 16)) &&
              KeyTuning(1, 8006, 69, 93) == 1 && KeyTuning(2, 8006, 69, 93) == 0,
          "the PSG's period loses its low two bits, and the noise's doesn't");

    // A note of key 84 bent up an octave plays the period of key 96. Its zone gives key 84 3 cents.
    const double bend = TimerBend(0, 1022, 60, 84, 36 * 64);
    Check(std::abs(bend - (cents(127, 16396) - 2400.0 - 3.0) / 100.0) < 1e-9, "a bend to the period of key 96");
    Check(std::abs(TimerBend(0, 0, 60, 84, 36 * 64) - 12.0) < 1e-9, "a wave without a timer bends by the semitones");

    // TimerBend differs from the bend by up to the rounding of the bent period or of the key's period, and half a cent
    // for the zone's whole cents. Key 84 bent an octave plays period 127. That period can round by 1200 log2(128 / 127)
    // cents. Key 84 itself plays period 255, the larger rounding where the bend goes an octave down. The PSG's period
    // of 2000 at key 93 can lose up to four counts.
    const auto lost = [](double period, double counts)
    {
        return 12.0 * std::log2((period + counts) / period) + 0.005;
    };
    Check(std::abs(TimerRounding(0, 1022, 60, 84, 36 * 64) - lost(127, 1)) < 1e-9 &&
              std::abs(TimerRounding(0, 1022, 60, 84, 12 * 64) - lost(255, 1)) < 1e-9 &&
              std::abs(TimerRounding(1, 8006, 69, 93, 24 * 64) - lost(2000, 4)) < 1e-9 &&
              TimerRounding(0, 0, 60, 84, 36 * 64) == 0.0,
          "the timer's rounding: the larger period's loss, and none without a timer");

    // Wave 1's region over every key: runs of keys, each at the tuning of every key in it.
    const TestArchive test(kOneNote);
    BankSet banks(test.archive, 0);
    sdat::Region region;
    region.wave = 1;
    region.root_key = 60;
    const Pcm* ramp = banks.Wave(region);
    const std::vector<PresetZone> zones =
        ramp != nullptr ? TunedZones(PresetZone{.region = region}, *ramp) : std::vector<PresetZone>();
    bool runs = !zones.empty() && zones.front().low_key == 0 && zones.back().high_key == 127;
    for (std::size_t i = 0; runs && i < zones.size(); i++)
    {
        runs = (i == 0 || (zones[i].low_key == zones[i - 1].high_key + 1 && zones[i].tuning != zones[i - 1].tuning));
        for (int key = zones[i].low_key; runs && key <= zones[i].high_key; key++)
        {
            runs = KeyTuning(0, 1022, 60, key) == zones[i].tuning;
        }
    }
    Check(ramp != nullptr && ramp->channel_type == 0 && ramp->timer == 1022 && runs,
          "a region's keys in runs of one tuning");

    // A sequence that plays keys 84 and 108 only: those keys take 3 and 24 cents. The keys below 84 and up to 107 join
    // key 84's run, and the keys above 108 join key 108's.
    const std::set<uint8_t> played = {84, 108};
    const std::vector<PresetZone> sparse =
        ramp != nullptr ? TunedZones(PresetZone{.region = region}, *ramp, &played) : std::vector<PresetZone>();
    const auto zone = [](uint8_t low, uint8_t high, int tuning)
    {
        return std::tuple<uint8_t, uint8_t, int>(low, high, tuning);
    };
    std::vector<std::tuple<uint8_t, uint8_t, int>> found;
    for (const PresetZone& z : sparse)
    {
        found.push_back(zone(z.low_key, z.high_key, z.tuning));
    }
    Check(found == std::vector{zone(0, 107, 3), zone(108, 127, 24)},
          "only the keys that a sequence plays take their tuning, and the other keys join their runs");

    // The zone's edges: a played key at a zone's low key or top key alone tunes the zone, as each played key of a drum
    // set's zones of one key does. A zone without a played key keeps the region's tuning of 0.
    const auto tuned = [&](uint8_t low, uint8_t high, const std::set<uint8_t>& keys)
    {
        PresetZone edges{.region = region};
        edges.low_key = low;
        edges.high_key = high;
        std::vector<std::tuple<uint8_t, uint8_t, int>> runs_found;
        if (ramp != nullptr)
        {
            for (const PresetZone& z : TunedZones(edges, *ramp, &keys))
            {
                runs_found.push_back(zone(z.low_key, z.high_key, z.tuning));
            }
        }
        return runs_found;
    };
    Check(tuned(108, 108, {108}) == std::vector{zone(108, 108, 24)}, "a zone of one key, played");
    Check(tuned(84, 108, {84}) == std::vector{zone(84, 108, 3)}, "a zone whose low key alone is played");
    Check(tuned(84, 108, {108}) == std::vector{zone(84, 108, 24)}, "a zone whose top key alone is played");
    Check(tuned(0, 83, {84, 108}) == std::vector{zone(0, 83, 0)}, "a zone without a played key keeps the tuning 0");
}

// A pan LFO: a sine of 127 * 127 / 16384 of the pan's 64 steps a side at a depth of 127 and a range of 1, 63 steps
// either way, on wave 0. Wave 0 loops. A note that sounds alone on its part takes its channel's pan in CC10 and the
// pan's level in CC11, frame by frame, with CC1 at 0. SoundFont has no LFO that moves the pan, and CC1 at 0 stills the
// SFZ files' LFO. Two notes that overlap on a part keep the track's pan, with CC1 at the depth for the SFZ files.
void TestPanLfo()
{
    const auto controls = [](const Performance& p, EventKind kind, uint32_t until)
    {
        std::vector<int> values;
        for (const seqlib::midi::TrackEvent& e : EventsOf(p, 0, kind))
        {
            if (e.tick < until)
            {
                values.push_back(e.value);
            }
        }
        return values;
    };

    // 0: the LFO moves the pan; 2: a depth of 127; 4: note 60 for 48 ticks; 7: a wait of 48 ticks; 9: the end.
    const TestArchive alone({0xCC, 0x02, 0xCA, 0x7F, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance p = Perform(alone.archive, {0});
    const std::vector<int> pans = controls(p, EventKind::kPan, 48);
    const std::vector<int> levels = controls(p, EventKind::kExpression, 48);
    const std::set<int> distinct(pans.begin(), pans.end());
    Check(distinct.size() > 10 && *distinct.begin() <= ntdlib::sequence_export::PanController(8) &&
              *distinct.rbegin() >= ntdlib::sequence_export::PanController(120),
          "CC10 follows the pan of a note alone on its part, from side to side");
    Check(!levels.empty() && *std::max_element(levels.begin(), levels.end()) > levels.front(),
          "CC11 follows the level of the pan");
    Check(controls(p, EventKind::kModulation, 48) == std::vector<int>{0} && p.approximations.empty(),
          "CC1 is 0 while the note plays alone");

    // 0: no note waits; then the same with notes 60 and 64 together.
    const TestArchive together(
        {0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x7F, 0x3C, 0x7F, 0x30, 0x40, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance q = Perform(together.archive, {0});
    Check(controls(q, EventKind::kPan, 48) == std::vector<int>{64} &&
              controls(q, EventKind::kModulation, 48) == std::vector<int>{127} &&
              q.approximations ==
                  std::map<std::string, uint32_t>{
                      {"pan LFO on a note that overlaps another on its part (omitted from SoundFont)", 2}},
          "two notes on a part keep the track's pan, and the SFZ files' LFO");

    // 0: no note waits; 4: a release of 100, 2.3 s; then notes 60 and 64, each for 48 ticks. Note 64 starts while note
    // 60's release still sounds, with an LFO of its own.
    const TestArchive in_release({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x7F, 0xD3, 0x64, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0x40,
                                  0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance r = Perform(in_release.archive, {0});
    std::vector<int> late_pans;
    for (const seqlib::midi::TrackEvent& e : EventsOf(r, 0, EventKind::kPan))
    {
        if (e.tick >= 48 && e.tick < 96)
        {
            late_pans.push_back(e.value);
        }
    }
    Check(late_pans == std::vector<int>{64} &&
              r.approximations ==
                  std::map<std::string, uint32_t>{
                      {"pan LFO on a note that overlaps another on its part (omitted from SoundFont)", 2}},
          "a note that starts in another note's release keeps the track's pan");

    // 4: a release of 100; note 60 for 24 ticks and a wait of 12; then an expression of 32 during the release, a wait
    // of 36 and the end. The driver keeps a released channel's volume. CC11 therefore keeps the level of expression
    // 127. The pan still moves.
    const TestArchive quieter_tail(
        {0xCC, 0x02, 0xCA, 0x7F, 0xD3, 0x64, 0x3C, 0x7F, 0x18, 0x80, 0x0C, 0xD5, 0x20, 0x80, 0x24, 0xFF});
    const Performance t = Perform(quieter_tail.archive, {0});
    std::vector<int> tail_levels;
    std::set<int> tail_pans;
    for (const seqlib::midi::TrackEvent& e : t.tracks[0])
    {
        if (e.tick >= 36 && e.tick < 72 && e.kind == EventKind::kExpression)
        {
            tail_levels.push_back(e.value);
        }
        if (e.tick >= 36 && e.tick < 72 && e.kind == EventKind::kPan)
        {
            tail_pans.insert(e.value);
        }
    }
    Check(
        !tail_levels.empty() && *std::min_element(tail_levels.begin(), tail_levels.end()) > 90 && tail_pans.size() > 5,
        "a release tail keeps its level from the release, and its pan follows the LFO");
}

// The value of `kind` that the first note-on of `key` on track `track` starts with, in the order of time that the MIDI
// file plays: the last event of `kind` before the note-on, by tick and `after`. Events at the same time keep their
// order. -1 without such an event.
int ValueAtNote(const Performance& p, int track, EventKind kind, uint8_t key)
{
    std::vector<seqlib::midi::TrackEvent> events = p.tracks[static_cast<std::size_t>(track)];
    std::ranges::stable_sort(events, {}, [](const seqlib::midi::TrackEvent& e) { return std::pair(e.tick, e.after); });

    int value = -1;
    for (const seqlib::midi::TrackEvent& e : events)
    {
        if (e.kind == EventKind::kNoteOn && e.key == key)
        {
            return value;
        }
        if (e.kind == kind)
        {
            value = e.value;
        }
    }

    return -1;
}

// A note that follows its pan LFO ends in a later frame of a tick. The tempo of 30 gives a tick every eight frames.
// The part goes back to the track's pan and level at that frame, after the LFO's values of the tick's earlier frames,
// and the next note starts at them.
void TestPanLfoEndsLateInATick()
{
    // 0: a tempo of 30; 3: a pan of 32; 5: a wait of 1 tick; 7: the LFO moves the pan; 9: a depth of 127; 11: a
    // release of 126; 13: note 60 for 2 ticks; 16: a wait of 16; 18: a depth of 0; 20: note 64 for 2 ticks; 23: a wait
    // of 16; 25: the end. Note 60 releases at tick 3, and its channel stops in the tick's fifth frame.
    const TestArchive test({0xE1, 0x1E, 0x00, 0xC0, 0x20, 0x80, 0x01, 0xCC, 0x02, 0xCA, 0x7F, 0xD3, 0x7E,
                            0x3C, 0x7F, 0x02, 0x80, 0x10, 0xCA, 0x00, 0x40, 0x7F, 0x02, 0x80, 0x10, 0xFF});
    const Performance p = Perform(test.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> pans = EventsOf(p, 0, EventKind::kPan);
    const std::vector<seqlib::midi::TrackEvent> levels = EventsOf(p, 0, EventKind::kExpression);
    const bool late_in_tick = std::ranges::any_of(
        pans, [](const seqlib::midi::TrackEvent& e) { return e.tick == 3 && e.after > 0.25 && e.after < 1.0; });

    Check(late_in_tick, "the release tail follows its LFO into a later frame of tick 3");
    Check(pans.size() > 2 && pans.front().tick == 0 && ValueAtNote(p, 0, EventKind::kPan, 64) == pans.front().value,
          "a note after a pan LFO that ends late in a tick starts at the track's pan");
    Check(!levels.empty() && levels.front().tick == 0 &&
              ValueAtNote(p, 0, EventKind::kExpression, 64) == levels.front().value,
          "and at the track's level");
}

// The performer. The driver's tempo of 120 adds 120 a frame, and a tick takes 240: a tick every two frames.
void TestPerform()
{
    const TestArchive one_note(kOneNote);
    const Performance p = Perform(one_note.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> ons = EventsOf(p, 0, EventKind::kNoteOn);
    const std::vector<seqlib::midi::TrackEvent> offs = EventsOf(p, 0, EventKind::kNoteOff);

    Check(p.bank == 0 && p.presets.size() == 1 && p.presets[0] == PresetKey{}, "one preset, of program 0");
    Check(ons.size() == 1 && ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 127,
          "the note starts at tick 0, at its velocity");
    Check(offs.size() == 1 && offs[0].tick == 48, "and ends at tick 48");
    Check(!p.tempo.empty() && std::abs(p.tempo[0].bpm - 60000.0 / (96.0 * kFrameMs)) < 1e-9 && p.timebase == 48,
          "the tempo: a tick every two frames");
    Check(!p.loop && p.loops == 0 && !p.truncated, "the sequence ends");

    // CC91 from the start: 0 without a game's reverb, and the reverb's volume with one. A synth that follows General
    // MIDI 2 gives a channel without CC91 a send of 40.
    const std::vector<seqlib::midi::TrackEvent> dry = EventsOf(p, 0, EventKind::kReverb);
    const std::vector<seqlib::midi::TrackEvent> wet =
        EventsOf(Perform(one_note.archive, {0}, {.reverb = ntdlib::ntr::Reverb{.volume = 80}}), 0, EventKind::kReverb);
    Check(dry.size() == 1 && dry[0].tick == 0 && dry[0].value == 0, "no reverb: CC91 0 at tick 0");
    Check(wet.size() == 1 && wet[0].tick == 0 && wet[0].value == 80, "a reverb at volume 80: CC91 80 at tick 0");
    Check(ntdlib::testing::ReverbBeforeNotes(p.tracks[0]), "CC91 before the first note-on of its tick");

    // A track that ends more than 10 s after its last sound has died away: the note, a wait of 2000 ticks (20.9 s) and
    // the end. The performance ends where the sound dies away. After a wait of 900 ticks (9.4 s), it ends with the
    // track.
    const TestArchive late_end({0x3C, 0x7F, 0x30, 0x80, 0x8F, 0x50, 0xFF});
    const TestArchive end_in_time({0x3C, 0x7F, 0x30, 0x80, 0x87, 0x04, 0xFF});
    const Performance late = Perform(late_end.archive, {0});
    const Performance in_time = Perform(end_in_time.archive, {0});
    Check(EventsOf(late, 0, EventKind::kNoteOff).size() == 1 && late.end_tick >= 48 && late.end_tick < 96 &&
              late.seconds < 1.0 && !late.truncated,
          "a track's end long after the last sound: the performance ends at the sound");
    Check(in_time.end_tick == 948 && !in_time.truncated, "a track's end within 10 s of it: the performance ends there");

    // The archive's volume of the sequence. At 64, the SDK gives the player a fader of -60: -6.0 dB, the level of
    // 64/127. CC11 holds the square root of the gain. At the centre, the note's CC11 falls from 127 times
    // sqrt(PanGain(64)), 107, to that times 10^(-60/400), 76.
    const TestArchive quieter(kOneNote, 0, 64);
    const std::vector<seqlib::midi::TrackEvent> full = EventsOf(p, 0, EventKind::kExpression);
    const std::vector<seqlib::midi::TrackEvent> quiet =
        EventsOf(Perform(quieter.archive, {0}), 0, EventKind::kExpression);
    Check(!full.empty() && full[0].tick == 0 && full[0].value == 107 && !quiet.empty() && quiet[0].tick == 0 &&
              quiet[0].value == 76,
          "the sequence's volume in CC11: 107 at 127, 76 at 64");

    // A game's commands once the sequence has started run in its first frame. A track volume of 0 gives track 0 a
    // fader of -32768, and its CC11 falls to 0. A later command for the same track takes the place of the first. A
    // tempo's ratio of 128 (the player's parameter at 0x1A, 2 bytes) halves the tempo: a tick every four frames.
    const std::vector<seqlib::midi::TrackEvent> silent = EventsOf(
        Perform(one_note.archive, {0}, {.commands = {TrackVolume(0x0001, -32768)}}), 0, EventKind::kExpression);
    const std::vector<seqlib::midi::TrackEvent> restored =
        EventsOf(Perform(one_note.archive, {0}, {.commands = {TrackVolume(0x0001, -32768), TrackVolume(0x0001, 0)}}), 0,
                 EventKind::kExpression);
    Check(!silent.empty() && silent[0].tick == 0 && silent[0].value == 0 && !restored.empty() &&
              restored[0].tick == 0 && restored[0].value == 107,
          "a game's track volume after the start: CC11 0 at volume 0, and 107 once a later command gives 127");
    const ntdlib::ntr::snd::Command half_tempo = {.id = ntdlib::ntr::snd::CommandId::kPlayerParameter,
                                                  .args = {0, 0x1A, 128, 2}};
    const Performance slow = Perform(one_note.archive, {0}, {.commands = {half_tempo}});
    Check(!slow.tempo.empty() && std::abs(slow.tempo[0].bpm - 60000.0 / (192.0 * kFrameMs)) < 1e-9,
          "a game's tempo ratio of 128 after the start: a tick every four frames");

    // A game's commands once the sequence has run some ticks: the note's track goes silent, then back to full volume.
    // At a tempo of 120, tick k runs in the driver's frame 2k, and frame f follows the ticks 0 to (f - 1) / 2. The
    // game's frame n, of one frame of the screen (560,190 clocks), goes before the driver's first frame f at or after
    // it: f * 174,592 >= n * 560,190. Its frame 14 goes before frame 45 and reads 23 ticks. Its frame 15 goes before
    // frame 49 and reads 25 ticks, a count equal to the tick, and starts the lists. Its frame 16 goes before frame 52,
    // after tick 25, and sends the first list. Its frame 17 goes before frame 55, after tick 27, and sends the second.
    // A rule that required a count above the tick would send them at ticks 27 and 29. A tick that the sequence never
    // reaches sends nothing.
    const std::vector<seqlib::midi::TrackEvent> dipped =
        EventsOf(Perform(one_note.archive, {0},
                         {.tick_commands = {{.tick = 25,
                                             .video_frames = 1,
                                             .frames = {{TrackVolume(0x0001, -32768)}, {TrackVolume(0x0001, 0)}}}}}),
                 0, EventKind::kExpression);
    const std::vector<seqlib::midi::TrackEvent> never = EventsOf(
        Perform(one_note.archive, {0}, {.tick_commands = {{.tick = 1000, .frames = {{TrackVolume(0x0001, -32768)}}}}}),
        0, EventKind::kExpression);
    Check(dipped.size() == 3 && dipped[0].tick == 0 && dipped[0].value == 107 && dipped[1].tick == 25 &&
              dipped[1].value == 0 && dipped[2].tick == 27 && dipped[2].value == 107,
          "a game's commands after 25 ticks, a frame of the screen apart: CC11 0 from tick 25, and 107 from tick 27");
    Check(never.size() == 1 && never[0].value == 107, "a game's commands after a tick that never comes: none");

    // A game's channels for the tracks: a track that may take no channel plays no note. The mask of another track
    // leaves track 0 alone.
    const Performance no_channels = Perform(one_note.archive, {0}, {.track_channels = {{0x0001, 0}}});
    const Performance other_track = Perform(one_note.archive, {0}, {.track_channels = {{0x0002, 0}}});
    Check(EventsOf(no_channels, 0, EventKind::kNoteOn).empty() &&
              EventsOf(other_track, 0, EventKind::kNoteOn).size() == 1,
          "a track takes only the channels that its mask gives");

    // At a tempo of 100, a tick takes 2.4 frames. The player's counter starts at 240. Each frame runs a tick while the
    // counter holds 240, then adds 100. Ticks 1 to 5 run in frames 3, 5, 8, 10 and 12, from 0.6, 0.2, 0.8, 0.4 and 0
    // frames after their times at MIDI's tempo.
    const TestArchive slower({0xE1, 0x64, 0x00, 0x3C, 0x7F, 0x30, 0xFF});
    const Performance hundred = Perform(slower.archive, {0});
    const double starts[] = {0.0, 1.25, 2.0 + 0.2 / 2.4, 3.0 + 0.8 / 2.4, 4.0 + 0.4 / 2.4, 5.0};
    bool on_frames = hundred.frame_ticks.size() > std::size(starts);
    for (std::size_t k = 0; on_frames && k < std::size(starts); k++)
    {
        on_frames = std::abs(hundred.frame_ticks[k] - starts[k]) < 1e-9;
    }
    Check(!hundred.tempo.empty() && std::abs(hundred.tempo[0].bpm - 60000.0 / (48 * 2.4 * kFrameMs)) < 1e-9,
          "a tempo of 100: a tick every 2.4 frames");
    Check(on_frames, "each tick's events go at the start of the frame that runs it");

    // A pass of the loop takes 48 ticks: 96 frames, 500.1 ms. The performance plays passes for 10 s at least.
    const TestArchive loop(kLoop);
    const Performance looped = Perform(loop.archive, {0});
    const Performance twice = Perform(loop.archive, {0}, {.loops = 2});
    Check(looped.loop && looped.loop->start == 0 && looped.loop->end == 48, "the loop's markers on its first pass");
    Check(looped.loops == 20 && looped.end_tick == 960 && EventsOf(looped, 0, EventKind::kNoteOn).size() == 20,
          "20 passes reach 10 s");
    Check(twice.loop && twice.loop->start == 0 && twice.loop->end == 48 && twice.loops == 20,
          "the 10 s hold more than two passes");

    // Ties (0xC8 1), then an endless loop (0xD4 0 to 0xFC) of note 60 for 24 ticks. Each pass ties the note again and
    // changes nothing. The sound holds until the game stops it. The conversion ends 10 s after the sound settles.
    const TestArchive tie({0xC8, 0x01, 0xD4, 0x00, 0x3C, 0x7F, 0x18, 0xFC, 0xFF});
    const Performance tied = Perform(tie.archive, {0});
    Check(tied.holds && !tied.loop && !tied.truncated && EventsOf(tied, 0, EventKind::kNoteOn).size() == 1,
          "a tie struck again in each pass holds, without loop markers");
    Check(tied.seconds >= 10.5 && tied.seconds < 10.5 + 2.0 * kFrameMs / 1000.0,
          "the hold ends 10 s after the second pass, where the sound settles");

    // Track 0 strikes a note of 2 ticks at volume 0 in an endless loop, as a track does that only keeps a sequence
    // busy. Track 1 plays note 64 for 24 ticks and a wait of 24 ticks in a loop. The silent passes aren't the
    // sequence's loop. The markers go on track 1's 48 ticks, and 20 passes reach 10 s.
    const TestArchive busy({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x11, 0x00, 0x00, // 3: track 1 starts at 0x11
        0xD4, 0x00,                   // 8: an endless loop
        0xC1, 0x00,                   // 10: the volume takes 0
        0x3C, 0x02, 0x02,             // 12: note 60 at velocity 2 for 2 ticks
        0xFC,                         // 15: the loop's end
        0xFF,                         // 16
        0x40, 0x7F, 0x18,             // 17: note 64 for 24 ticks
        0x80, 0x18,                   // 20: a wait of 24 ticks
        0x94, 0x11, 0x00, 0x00,       // 22: back to 17
    });
    const Performance kept = Perform(busy.archive, {0});
    Check(kept.loop && kept.loop->start == 0 && kept.loop->end == 48 && kept.end_tick == 960,
          "a track of silent notes doesn't own the loop");

    // Track 0 strikes note 60 at velocity 127 and volume 0 every 10 ticks, in an endless loop. Track 1 waits 190 ticks,
    // then plays note 64 for 24 ticks and waits 742 ticks, in a loop of 766 ticks. 766 isn't a multiple of 10. No
    // single pass of track 1 repeats. The two loops come round together every 3,830 ticks, 5 of track 1's passes. The
    // markers go on those 5 passes, from tick 190 to 4020.
    const TestArchive misaligned({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x11, 0x00, 0x00, // 3: track 1 starts at 0x11
        0xC1, 0x00,                   // 8: the volume takes 0
        0xD4, 0x00,                   // 10: an endless loop
        0x3C, 0x7F, 0x0A,             // 12: note 60 for 10 ticks
        0xFC,                         // 15: the loop's end
        0xFF,                         // 16
        0x80, 0x81, 0x3E,             // 17: a wait of 190 ticks
        0x40, 0x7F, 0x18,             // 20: note 64 for 24 ticks
        0x80, 0x85, 0x66,             // 23: a wait of 742 ticks
        0x94, 0x14, 0x00, 0x00,       // 26: back to 20
    });
    const Performance offset = Perform(misaligned.archive, {0});
    Check(offset.loop && offset.loop->start == 190 && offset.loop->end == 4020 && offset.loops == 1 &&
              offset.end_tick == 4020 && !offset.truncated,
          "a silent loop of 10 ticks beside a loop of 766: the markers on the 5 passes of their whole period");

    // Track 0's silent loop plays keys 60 to 68 for 383 ticks each: 3,447 ticks, 4.5 of track 1's passes. No pass of
    // track 1 repeats the next. Track 0's loop ends at tick 3447, and the main loop's passes count again from track 1's
    // pass that holds that tick, from 3254 (OnLoop). The loops come round together after 9 passes, from 3254 to 10148.
    const TestArchive nine_keys({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x31, 0x00, 0x00, // 3: track 1 starts at 0x31
        0xC1, 0x00,                   // 8: the volume takes 0
        0xD4, 0x00,                   // 10: an endless loop
        0x3C, 0x7F, 0x82, 0x7F,       // 12: key 60 for 383 ticks
        0x3D, 0x7F, 0x82, 0x7F,       // 16: key 61
        0x3E, 0x7F, 0x82, 0x7F,       // 20: key 62
        0x3F, 0x7F, 0x82, 0x7F,       // 24: key 63
        0x40, 0x7F, 0x82, 0x7F,       // 28: key 64
        0x41, 0x7F, 0x82, 0x7F,       // 32: key 65
        0x42, 0x7F, 0x82, 0x7F,       // 36: key 66
        0x43, 0x7F, 0x82, 0x7F,       // 40: key 67
        0x44, 0x7F, 0x82, 0x7F,       // 44: key 68
        0xFC,                         // 48: the loop's end
        0x80, 0x81, 0x3E,             // 49: a wait of 190 ticks
        0x40, 0x7F, 0x18,             // 52: note 64 for 24 ticks
        0x80, 0x85, 0x66,             // 55: a wait of 742 ticks
        0x94, 0x34, 0x00, 0x00,       // 58: back to 52
    });
    const Performance nine = Perform(nine_keys.archive, {0});
    Check(nine.loop && nine.loop->start == 3254 && nine.loop->end == 10148 && nine.loops == 1,
          "a silent loop of 4.5 passes beside a loop of 766: the markers on the 9 passes of their whole period");

    // Track 0's silent loop lasts 761 ticks. The loops come round together only after 761 of track 1's passes, past the
    // file's limit. The markers stay on track 1's first pass, and the approximations say why.
    const TestArchive apart({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x11, 0x00, 0x00, // 3: track 1 starts at 0x11
        0xC1, 0x00,                   // 8: the volume takes 0
        0xD4, 0x00,                   // 10: an endless loop
        0x3C, 0x7F, 0x85, 0x79,       // 12: note 60 for 761 ticks
        0xFC,                         // 16: the loop's end
        0x80, 0x81, 0x3E,             // 17: a wait of 190 ticks
        0x40, 0x7F, 0x18,             // 20: note 64 for 24 ticks
        0x80, 0x85, 0x66,             // 23: a wait of 742 ticks
        0x94, 0x14, 0x00, 0x00,       // 26: back to 20
    });
    const Performance unaligned = Perform(apart.archive, {0});
    const auto approximation =
        unaligned.approximations.find("tracks loop at other lengths (their loops line up every 761 passes)");
    Check(unaligned.loop && unaligned.loop->start == 190 && unaligned.loop->end == 956 &&
              approximation != unaligned.approximations.end() && approximation->second > 0,
          "loops that come round together past the file's limit: the first pass, and an approximation");

    // A note at velocity 1 and volume 100 is at -76.3 dB, and plays at a volume of 0. The loop goes straight back to
    // it. The first pass ends at tick 24. It changed nothing and played no note that sounds, and nothing sounds. The
    // sequence ends there, without loop markers.
    const Bytes faint_sequence = {
        0xC1, 0x64,             // 0: the volume takes 100
        0x3C, 0x01, 0x18,       // 2: note 60 for 24 ticks at velocity 1
        0x94, 0x02, 0x00, 0x00, // 5: back to 2
    };
    const TestArchive faint(faint_sequence);
    const Performance faded = Perform(faint.archive, {0});
    Check(!faded.loop && !faded.truncated && !faded.holds && faded.end_tick == 24,
          "a loop of silent notes ends after its first pass");

    // Track 0 plays note 60 for 48 ticks, waits 48 ticks and ends. Track 1 strikes note 60 at volume 0 for 24 ticks in
    // a loop, back to back, or tied (0xC8 1). Once track 0 has ended at tick 96, track 1 passes still and silent, and
    // nothing sounds. The sequence ends at tick 96, as track 0 does alone.
    const auto one_shot = [](const Bytes& silent_track)
    {
        Bytes bytecode = {
            0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
            0x93, 0x01, 0x0E, 0x00, 0x00, // 3: track 1 starts at 0x0E
            0x3C, 0x7F, 0x30,             // 8: note 60 for 48 ticks
            0x80, 0x30,                   // 11: a wait of 48 ticks
            0xFF,                         // 13
        };
        bytecode.insert(bytecode.end(), silent_track.begin(), silent_track.end());
        const TestArchive test(bytecode);
        return Perform(test.archive, {0});
    };
    for (const Bytes& silent_track : {
             Bytes{0xC1, 0x00, 0x3C, 0x7F, 0x18, 0x94, 0x10, 0x00, 0x00},             // 14: back to 16
             Bytes{0xC8, 0x01, 0xC1, 0x00, 0x3C, 0x7F, 0x18, 0x94, 0x12, 0x00, 0x00}, // 14: back to 18
         })
    {
        const Performance ended = one_shot(silent_track);
        Check(!ended.loop && !ended.truncated && !ended.holds && ended.end_tick == 96 &&
                  EventsOf(ended, 0, EventKind::kNoteOn).size() == 1,
              "a one-shot piece beside a loop of silent notes ends with the piece");
    }

    // Notes don't wait (0xC7 0). The track strikes note 60 for 48 ticks at volume 0 and raises the volume to 127 at
    // tick 12. The note sounds from then to tick 48 and its release. The track waits 2000 ticks (20.9 s) and ends,
    // more than 10 s after the sound has died away. The performance ends at the sound, as for a note that sounds from
    // the start.
    const TestArchive swell({
        0xC7,
        0x00, // 0: notes don't wait
        0xC1,
        0x00, // 2: the volume takes 0
        0x3C,
        0x7F,
        0x30, // 4: note 60 for 48 ticks
        0x80,
        0x0C, // 7: a wait of 12 ticks
        0xC1,
        0x7F, // 9: the volume takes 127
        0x80,
        0x8F,
        0x50, // 11: a wait of 2000 ticks
        0xFF, // 14
    });
    const Performance swelled = Perform(swell.archive, {0});
    Check(swelled.end_tick >= 48 && swelled.end_tick < 96 && !swelled.truncated,
          "a note struck silent sounds once its track raises the volume");

    // Track 0 plays note 60 for 48 ticks, waits 48 ticks and ends at tick 96. Track 1's notes don't wait. In a loop of
    // 36 ticks, it sets the expression to 0, strikes note 60 for 24 ticks, waits 12 ticks, raises the expression to 127
    // and waits 24 ticks. Each note is struck silent and sounds from 12 ticks on. Track 1's passes end at ticks 36, 72
    // and 108. At 36 and 72, track 0 still plays, and track 1 isn't the main track. At 108, track 0 has ended. The pass
    // from 72 played a note that sounds, and its markers go from 72 to 108. The loop plays until the performance
    // reaches 10 s, 960 ticks at the default tempo. Its 25th pass ends at tick 972.
    const TestArchive swell_loop({
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x0E, 0x00, 0x00, // 3: track 1 starts at 0x0E
        0x3C, 0x7F, 0x30,             // 8: note 60 for 48 ticks
        0x80, 0x30,                   // 11: a wait of 48 ticks
        0xFF,                         // 13
        0xC7, 0x00,                   // 14: notes don't wait
        0xD5, 0x00,                   // 16: the expression takes 0
        0x3C, 0x7F, 0x18,             // 18: note 60 for 24 ticks
        0x80, 0x0C,                   // 21: a wait of 12 ticks
        0xD5, 0x7F,                   // 23: the expression takes 127
        0x80, 0x18,                   // 25: a wait of 24 ticks
        0x94, 0x10, 0x00, 0x00,       // 27: back to 16
    });
    const Performance swell_looped = Perform(swell_loop.archive, {0});
    Check(swell_looped.loop && swell_looped.loop->start == 72 && swell_looped.loop->end == 108 &&
              swell_looped.loops == 25 && swell_looped.end_tick == 972 && !swell_looped.truncated,
          "a loop of notes struck silent and swelled later keeps its markers once track 0 has ended");

    // Program 7, an attack of 100 and a release of 50, then the note.
    const TestArchive program({0x81, 0x07, 0xD0, 100, 0xD3, 50, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance seven = Perform(program.archive, {0});
    Check(seven.presets.size() == 1 && seven.presets[0] == PresetKey{.program = 7, .attack = 100, .release = 50},
          "the preset takes the program and the track's envelope");

    // The note plays only when variable 0 is 1: a comparison (0xB8) and the if prefix (0xA2).
    const TestArchive on_variable({0xB8, 0x00, 0x01, 0x00, 0xA2, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance unset = Perform(on_variable.archive, {0});
    const Performance set = Perform(on_variable.archive, {0}, {.variables = {{0, 1}}});
    Check(EventsOf(unset, 0, EventKind::kNoteOn).empty() && EventsOf(set, 0, EventKind::kNoteOn).size() == 1,
          "a variable set before the sequence starts");

    // A wait of 1 to 96 ticks drawn at random (the random prefix, 0xA0), then a note. The seed sets the driver's
    // random numbers.
    const TestArchive drawn({0xA0, 0x80, 0x01, 0x00, 0x60, 0x00, 0x3C, 0x7F, 0x30, 0xFF});
    const auto start = [&drawn](uint32_t seed)
    {
        const std::vector<seqlib::midi::TrackEvent> on =
            EventsOf(Perform(drawn.archive, {0}, {.seed = seed}), 0, EventKind::kNoteOn);
        return on.size() == 1 ? static_cast<int>(on[0].tick) : -1;
    };
    const int first = start(1);
    const int second = start(2);
    Check(first >= 1 && first <= 96 && second >= 1 && second <= 96 && first != second && start(1) == first,
          "the seed draws the wait: " + std::to_string(first) + " and " + std::to_string(second));
}

// A note whose channel another note takes. The player has channel 0 alone. Track 0 plays note 60 for 96 ticks. At tick
// 24, track 1 plays note 64 at a higher priority, and takes the channel. Track 0's note ends at once. Nothing else of
// track 0 sounds on its part. CC120 then stops the note in the synthesizer. A game's changes of a variable while the
// sequence plays. The track plays a note in each pass where variable 0 holds 5. A tick runs every second frame. A
// change at frame 3,000, about 15.6 s in, comes at tick 1,500. That is past the 10 s after which the performer stops at
// a steady pass without a change.
void TestChanges()
{
    const TestArchive sometimes({
        0xB8,
        0x00,
        0x05,
        0x00, // 0: whether variable 0 is 5
        0xA2,
        0x3C,
        0x7F,
        0x18, // 4: if so, a note of 24 ticks
        0x80,
        0x18, // 8: a wait of 24 ticks
        0x94,
        0x00,
        0x00,
        0x00, // 10: back to 0
    });

    const Performance without = Perform(sometimes.archive, {0});
    Check(EventsOf(without, 0, EventKind::kNoteOn).empty(), "without a change, the note never plays");

    const Performance changed = Perform(sometimes.archive, {0}, {.changes = {{3000, 0, 5}}});
    const std::vector<seqlib::midi::TrackEvent> ons = EventsOf(changed, 0, EventKind::kNoteOn);
    Check(!ons.empty() && ons[0].tick >= 1500 && ons[0].tick < 1524,
          "the change plays the note from the first pass after its tick");
    Check(changed.loop && changed.loop->start >= 1500,
          "the performance plays on past the change, and its loop starts after it");

    // The changes come in the order of their frames, whatever the list's order. The last one sets the variable back,
    // and the notes stop.
    const Performance back = Perform(sometimes.archive, {0}, {.changes = {{3000, 0, 1}, {2000, 0, 5}}});
    const std::vector<seqlib::midi::TrackEvent> between = EventsOf(back, 0, EventKind::kNoteOn);
    Check(!between.empty() && between.front().tick >= 1000 && between.front().tick < 1024 && between.back().tick < 1500,
          "two changes: the note plays only between them");

    // A global variable, by index 16 on.
    const TestArchive global({0xB8, 0x10, 0x05, 0x00, 0xA2, 0x3C, 0x7F, 0x18, 0x80, 0x18, 0x94, 0x00, 0x00, 0x00});
    const Performance from_global = Perform(global.archive, {0}, {.changes = {{100, 16, 5}}});
    Check(!EventsOf(from_global, 0, EventKind::kNoteOn).empty(), "a change of a global variable reaches the track");
}

void TestStolenChannel()
{
    // 0: alloc tracks 0 and 1; 3: open track 1 at 14; 8: note 60 for 96; 11: wait 96; 13: end.
    // 14: wait 24; 16: priority 127; 18: note 64 for 24; 21: wait 24; 23: end.
    const Bytes alone = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x0E, 0x00, 0x00, 0x3C, 0x7F, 0x60, 0x80,
                         0x60, 0xFF, 0x80, 0x18, 0xC6, 0x7F, 0x40, 0x7F, 0x18, 0x80, 0x18, 0xFF};
    const TestArchive test(alone, 0x0001);
    const Performance p = Perform(test.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> offs = EventsOf(p, 0, EventKind::kNoteOff);
    const std::vector<seqlib::midi::TrackEvent> stops = EventsOf(p, 0, EventKind::kSoundOff);
    const std::vector<seqlib::midi::TrackEvent> taken = EventsOf(p, 1, EventKind::kNoteOn);

    Check(taken.size() == 1 && taken[0].tick == 24, "track 1's note starts at tick 24");
    Check(offs.size() == 1 && offs[0].tick == 24, "track 0's note ends there");
    Check(stops.size() == 1 && stops[0].tick == 24 && p.approximations.empty(), "and CC120 stops its sound");

    // With two channels, track 0 turns its notes' waits off and plays a chord of notes 60 and 67. Track 1 takes one of
    // their channels. The other note still sounds on the part, and CC120 would stop it too. The note that ends rings
    // on in its release.
    // 0: alloc tracks 0 and 1; 3: open track 1 at 19; 8: no note waits; 10: note 60 for 96; 13: note 67 for 96;
    // 16: wait 96; 18: end. 19: wait 24; 21: priority 127; 23: note 64 for 24; 26: wait 24; 28: end.
    const Bytes chord = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x13, 0x00, 0x00, 0xC7, 0x00, 0x3C, 0x7F, 0x60, 0x43, 0x7F,
                         0x60, 0x80, 0x60, 0xFF, 0x80, 0x18, 0xC6, 0x7F, 0x40, 0x7F, 0x18, 0x80, 0x18, 0xFF};
    const TestArchive two(chord, 0x0003);
    const Performance q = Perform(two.archive, {0});
    const std::vector<seqlib::midi::TrackEvent> chord_offs = EventsOf(q, 0, EventKind::kNoteOff);

    Check(EventsOf(q, 0, EventKind::kSoundOff).empty() && chord_offs.size() == 2 && chord_offs[0].tick == 24 &&
              chord_offs[1].tick == 96,
          "a note whose channel another note takes ends; the other note plays on");
    Check(q.approximations.size() == 1 &&
              q.approximations.begin()->first == "note cut short by another note's channel (rings on in its release)",
          "the release that the synthesizer plays counts as an approximation");
}

} // namespace

int main()
{
    TestPanLaw();
    TestEnvelopes();
    TestPresetZones();
    TestWaves();
    TestTimerTuning();
    TestPanLfo();
    TestPanLfoEndsLateInATick();
    TestPerform();
    TestChanges();
    TestStolenChannel();

    return xsflib::testing::Summary("DS conversion");
}
