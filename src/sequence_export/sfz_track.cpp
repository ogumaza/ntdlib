// SPDX-License-Identifier: MIT

// The SFZ text that every bank model shares (see sfz_track.h).

#include "sequence_export/sfz_track.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "seqlib/loop.h"
#include "seqlib/sf2/units.h"
#include "seqlib/sfz/opcodes.h"
#include "seqlib/sfz/text.h"
#include "seqlib/wav.h"
#include "sequence_export/pcm.h"

namespace ntdlib::sequence_export
{
namespace
{

using seqlib::sfz::Curve;
using seqlib::sfz::Number;

// Custom curve IDs; sfizz reserves 0-6, and seqlib's track file uses 7 for CC10's pan.
constexpr int kLowPassCurve = 8;          // kSfzLowPassController to the low-pass's cutoff
constexpr int kBiquadCutoffCurve = 9;     // kSfzBiquadController to the biquad filter's cutoff
constexpr int kBiquadResonanceCurve = 10; // ... and resonance
constexpr int kBiquadGainCurve = 11;      // ... and gain

// Modulation amounts at curve value 1: cutoff in cents, resonance and volume in dB. Curves stay within -1..1 for
// compatibility with other SFZ players.
constexpr int kCutoffSpan = 12000;
constexpr int kResonanceSpan = 60;
constexpr int kGainSpan = 2;

// Returns opcode `name` for filter 1 or 2, e.g. "cutoff" or "cutoff2".
std::string FilterOpcode(const char* name, int index)
{
    return index == 1 ? name : std::string(name) + "2";
}

// Writes preset `key`'s track LFO as SFZ LFO 1: a sine with depth from CC1 (depth / 2). MML depth / 128 scales `range`
// semitones, 6 * range dB, or range times the engine's full pan. Omits LFOs with no effect or a delay that prevents
// them from starting.
std::string LfoOpcodes(const PresetKey& key, const Settings& settings)
{
    constexpr double kDepthScale = 2.0 * 127.0 / 128.0; // sfizz divides controller values by 127
    const double hz = LfoHz(key, settings);
    if (key.lfo_type > 2 || hz <= 0.0)
    {
        return "";
    }

    std::string s = "lfo1_wave=1 lfo1_freq=" + Number(hz, 6);
    if (key.lfo_delay != 0)
    {
        s += " lfo1_delay=" + Number(LfoDelayMs(key, settings) / 1000.0, 6);
    }
    if (key.lfo_type == 0)
    {
        s += " lfo1_pitch_oncc1=" + Number(100.0 * key.lfo_range * kDepthScale, 6);
    }
    else if (key.lfo_type == 1)
    {
        s += " lfo1_volume_oncc1=" + Number(6.0 * key.lfo_range * kDepthScale, 6);
    }
    else
    {
        // Near centre, sfizz's sin/cos pan law moves 2 / pi as far as the engine's curve (see seqlib/sf2/units.h).
        s += " lfo1_pan_oncc1=" + Number(100.0 * key.lfo_range * kDepthScale * 2.0 / std::numbers::pi, 6);
    }

    return s + "\n";
}

// Writes opcodes for biquad preset `type` in filter slot `index`.
std::string BiquadOpcodes(int type, int index, const Settings& settings)
{
    constexpr const char* kKinds[] = {"lpf_2p", "hpf_2p", "bpf_2p", "bpf_2p", "bpf_2p"};
    const std::string cc = "_oncc" + std::to_string(kSfzBiquadController);
    const std::string curve = "_curvecc" + std::to_string(kSfzBiquadController);
    const int shape = BiquadShape(type, settings);
    return FilterOpcode("fil", index) + "_type=" + kKinds[shape - 1] + " " + FilterOpcode("cutoff", index) + "=" +
           std::to_string(kSfzBiquadBase) + " " + FilterOpcode("cutoff", index) + cc + "=" +
           std::to_string(kCutoffSpan) + " " + FilterOpcode("cutoff", index) + curve + "=" +
           std::to_string(kBiquadCutoffCurve) + " " + FilterOpcode("resonance", index) + "=0 " +
           FilterOpcode("resonance", index) + cc + "=" + std::to_string(kResonanceSpan) + " " +
           FilterOpcode("resonance", index) + curve + "=" + std::to_string(kBiquadResonanceCurve) + " volume" + cc +
           "=" + std::to_string(kGainSpan) + " volume" + curve + "=" + std::to_string(kBiquadGainCurve);
}

// The text that track `t`'s file adds to seqlib's track file: the filters' curves and opcodes, each preset's LFO, and
// the biquad's groups around each preset's regions.
seqlib::sfz::TrackText TrackAdditions(const Performance& perf, int t, const Settings& settings,
                                      const PresetRegionsFunction& regions_of)
{
    // Check whether the track enables its low-pass filter.
    bool low_pass = false;
    for (const TrackEvent& e : perf.tracks[t])
    {
        if (e.kind == kSfzLowPassEvent && TrackFilters(0x40 - e.value, 0, 0, settings).low_pass >= 0)
        {
            low_pass = true;
        }
    }

    const int biquad = perf.biquad_types[t];
    const int biquad_filter = low_pass ? 2 : 1;

    seqlib::sfz::TrackText text;
    if (low_pass)
    {
        const auto cutoff = [&settings](int v)
        {
            const int index = TrackFilters(0x40 - v, 0, 0, settings).low_pass;
            return index < 0 ? 0.0 : SfzLowPassCutoff(index, settings) / static_cast<double>(kCutoffSpan);
        };
        text.curves += Curve(kLowPassCurve,
                             "Low-pass cutoff by controller value: units of 12000 cents relative to 20 kHz", cutoff);
    }
    if (biquad != 0)
    {
        auto step = [biquad, &settings](int v)
        {
            return SfzBiquadStep(biquad, TrackFilters(0x40, biquad, std::max(v, 1), settings).biquad_step, settings);
        };
        text.curves += Curve(kBiquadCutoffCurve, "Biquad cutoff: units of 12000 cents relative to 1000 Hz",
                             [&](int v) { return step(v).cutoff / static_cast<double>(kCutoffSpan); });
        text.curves += Curve(kBiquadResonanceCurve, "Biquad resonance (Q in dB): units of 60 dB",
                             [&](int v) { return step(v).resonance / 100.0 / kResonanceSpan; });
        text.curves += Curve(kBiquadGainCurve, "Biquad gain: units of 2 dB",
                             [&](int v) { return step(v).gain / 100.0 / kGainSpan; });
    }
    if (low_pass)
    {
        const std::string cc = std::to_string(kSfzLowPassController);
        text.global = "fil_type=lpf_1p cutoff=" + std::to_string(kSfzLowPassOpen) + " cutoff_oncc" + cc + "=" +
                      std::to_string(kCutoffSpan) + " cutoff_curvecc" + cc + "=" + std::to_string(kLowPassCurve) + "\n";
    }

    text.preset = [&perf, &settings, &regions_of, biquad, biquad_filter](uint32_t p)
    {
        const PresetKey& key = perf.presets[p];
        std::string s = LfoOpcodes(key, settings);
        const std::string regions = regions_of(key, perf.level_cut_db);
        if (biquad == 0)
        {
            s += "<group>\n" + regions;
        }
        else
        {
            const std::string cc = std::to_string(kSfzBiquadController);
            s += "<group> hicc" + cc + "=0\n" + regions;
            s += "<group> locc" + cc + "=1 " + BiquadOpcodes(biquad, biquad_filter, settings) + "\n" + regions;
        }

        return s;
    };

    return text;
}

} // namespace

const SfzWave& SfzSampleSet::Get(std::pair<uint32_t, uint32_t> key, const std::function<const wave::Pcm*()>& pcm)
{
    if (auto it = waves_.find(key); it != waves_.end())
    {
        return it->second;
    }

    SfzWave made;
    if (const wave::Pcm* decoded = pcm())
    {
        const seqlib::PreparedWave w =
            seqlib::PrepareForSfz(ToSeqlib(*decoded), (decoded->sample_rate * 4 + 999) / 1000); // 4 ms
        const std::size_t n = std::min<std::size_t>(w.channels.size(), 2);
        const std::string base = "w" + std::to_string(key.first) + "_" + std::to_string(key.second);
        for (std::size_t c = 0; c < n; c++)
        {
            const std::string name = base + (n == 1 ? "" : (c == 0 ? "L" : "R")) + ".wav";
            samples_.push_back({name, seqlib::WavFile(w.channels[c], decoded->sample_rate)});
            made.names.push_back(name);
        }

        made.loop = w.loop;
        if (w.loop)
        {
            made.loop_start = w.loop_start;
            made.loop_end = w.loop_end - 1;
        }
    }

    return waves_.emplace(key, std::move(made)).first->second;
}

std::string SfzRegionStart(const std::string& sample, std::pair<int, int> keys, std::pair<int, int> velocities,
                           double pan)
{
    std::string s = "<region> sample=" + sample + " lokey=" + std::to_string(keys.first) +
                    " hikey=" + std::to_string(keys.second) + " lovel=" + std::to_string(velocities.first) +
                    " hivel=" + std::to_string(velocities.second);
    const int pan_tenths = static_cast<int>(std::lround(seqlib::sf2::SoundFontPan(pan) * 1000.0));
    if (pan_tenths != 0)
    {
        s += " pan=" + Number(pan_tenths / 10.0, 1);
    }

    return s;
}

std::string SfzPitchOpcodes(uint8_t original_key, float tune)
{
    std::string s = " pitch_keycenter=" + std::to_string(original_key);
    const double cents = tune > 0 ? std::clamp(1200.0 * std::log2(tune), -12000.0, 12000.0) : 0.0;
    s += seqlib::sfz::TuneOpcodes(static_cast<int>(std::lround(cents)));

    return s;
}

std::string SfzVolumeOpcode(uint8_t volume, double gain_db, bool silent, int16_t lift)
{
    // The region's linear volume, in hundredths of a dB. A lift raises it up to full level (PresetKey::lift).
    double level = volume != 0 ? 2000.0 * std::log10(volume / 127.0) : 0.0;
    if (lift > 0)
    {
        level = std::min(0.0, level + 10.0 * lift);
    }
    const int hundredths = volume == 0 || silent ? -14400 : static_cast<int>(std::lround(level + 100.0 * gain_db));
    if (hundredths == 0)
    {
        return {};
    }

    return " volume=" + Number(hundredths / 100.0, 2);
}

std::string SfzEnvelopeOpcodes(const PresetKey& key, const EnvelopeValues& env, bool ignore_note_off,
                               const SfzWave& wave, const Settings& settings)
{
    const EnvelopeValues played = OverrideEnvelope(env, key);
    SfzEnvelope e = ConvertEnvelopeForSfz(played, settings);
    if (key.attack_timecents)
    {
        e.attack = std::exp2(*key.attack_timecents / 1200.0);
    }

    // Keep decaying at the faster of the decay and release rates (see the SoundFont's regions).
    if (ignore_note_off && -nwsnd::DecibelSquare(played.sustain) >= kInaudibleSustain)
    {
        e.release = std::min(e.release, e.decay);
    }

    return seqlib::sfz::AmpEnvelopeOpcodes(
               {.attack = e.attack, .hold = e.hold, .decay = e.decay, .sustain = e.sustain, .release = e.release}) +
           seqlib::sfz::LoopOpcodes(wave.loop, wave.loop_start, wave.loop_end);
}

SfzInstruments BuildTrackFiles(const Performance& perf, const seqlib::midi::MidiReport& midi, const std::string& title,
                               const std::vector<std::string>& preset_names, const Settings& settings,
                               const std::string& program, const PresetRegionsFunction& regions)
{
    seqlib::sfz::TrackFileOptions options;
    options.title = title;
    options.program = program;
    options.preset_names = preset_names;
    options.presets = perf.presets.size();
    options.level_cut_db = perf.level_cut_db;
    auto text = [&](int t)
    {
        return TrackAdditions(perf, t, settings, regions);
    };

    SfzInstruments out;
    out.files = seqlib::sfz::TrackFiles(perf, midi, options, text);

    return out;
}

} // namespace ntdlib::sequence_export
