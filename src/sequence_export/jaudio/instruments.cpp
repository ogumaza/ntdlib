// SPDX-License-Identifier: MIT

// Instrument lookup (see instruments.h). Four Swords Adventures' JAudio2 applies an instrument's effects and selects
// its key and velocity regions as The Wind Waker's JAudio does: TBasicInst::getParam, TDrumSet::getParam and the
// effects' getY.

#include "ntdlib/sequence_export/jaudio/instruments.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/sequence_export/jaudio/envelope.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/jaudio/wave.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// The value of a key or velocity effect (JASInstSense::getY): a line from `low` at 0 to `high` at 127, or through 1 at
// the center when it has one.
float SenseValue(const InstEffect& e, int key, int velocity)
{
    int x = 0;
    if (e.trigger == 1)
    {
        x = velocity;
    }
    else if (e.trigger == 2)
    {
        x = key;
    }

    const auto fx = static_cast<float>(x);
    if (e.center == 127 || e.center == 0)
    {
        return e.low + fx * (e.high - e.low) / 127.0f;
    }
    if (x < e.center)
    {
        return e.low + (1.0f - e.low) * (fx / static_cast<float>(e.center));
    }

    return (e.high - 1.0f) * ((fx - e.center) / static_cast<float>(127 - e.center)) + 1.0f;
}

// Apply `effects` at `key` and `velocity` to `sound`: volume and pitch multiply, and pan moves by the effect's value.
// Effects on the effect send and surround are left out. A zone can't play them.
void ApplyEffects(const std::vector<InstEffect>& effects, int key, int velocity, NoteSound& sound)
{
    for (const InstEffect& e : effects)
    {
        float y = e.base;
        if (e.kind == InstEffect::Kind::kRandom)
        {
            sound.random = sound.random || e.width != 0.0f;
        }
        else
        {
            y = SenseValue(e, key, velocity);
            sound.velocity_sense = sound.velocity_sense || e.trigger == 1;
        }

        switch (e.target)
        {
        case 0:
            sound.volume *= y;
            break;
        case 1:
            sound.pitch *= y;
            break;
        case 2:
            sound.pan += y - 0.5f;
            sound.effect_pan += y - 0.5f;
            break;
        default:
            break;
        }
    }
}

// The first velocity region whose high velocity is at or above `velocity`.
const VelocityRegion* FindVelocityRegion(const std::vector<VelocityRegion>& regions, int velocity)
{
    for (const VelocityRegion& region : regions)
    {
        if (velocity <= region.high_velocity)
        {
            return &region;
        }
    }

    return nullptr;
}

// The DSP program's oscillators that make a waveform from their pitch alone, by number (the DSP program's OscSub
// functions). Each makes its waveform from a 16-bit phase that moves on by half the channel's pitch, in 4096ths, at
// each output sample. A cycle therefore lasts 32 samples of a wave at the pitch's rate. The square wave and the pulse
// go between -0x4000 and 0x4000. The sawtooth is the phase itself. The sine and the triangle read a table of 64 samples
// at the phase's top 6 bits. The game hands the DSP program the tables at start-up, after the resampler's filter. Four
// Swords Adventures' main.dol holds them at 0x8045cd80, The Wind Waker's at 0x80399120 and Twilight Princess's at
// 0x8039b5a0. They hold 32767 times a sine and a triangle that peak at a quarter of the cycle, within 1. The other
// oscillators read a mixing bus, make noise or a ramp from tables that the DSP program changes as it runs, or output
// their pitch.
enum class OscillatorShape
{
    kNone,
    kSquare,   // oscillator 0: high while the phase's top bit is set
    kSawtooth, // oscillator 1
    kPulse,    // oscillator 3: high while the phase's top two bits are set
    kTriangle, // oscillator 4
    kSine,     // oscillator 7
};

OscillatorShape ShapeOf(uint32_t oscillator)
{
    switch (oscillator)
    {
    case 0:
        return OscillatorShape::kSquare;
    case 1:
        return OscillatorShape::kSawtooth;
    case 3:
        return OscillatorShape::kPulse;
    case 4:
        return OscillatorShape::kTriangle;
    case 7:
        return OscillatorShape::kSine;
    default:
        return OscillatorShape::kNone;
    }
}

// A triangle wave `t` cycles from its start: up from 0 to 1 at a quarter of the cycle, and down to -1 at three
// quarters.
double Triangle(double t)
{
    if (t < 0.25)
    {
        return 4.0 * t;
    }
    if (t < 0.75)
    {
        return 2.0 - 4.0 * t;
    }

    return 4.0 * t - 4.0;
}

// The level of `shape` at 16-bit phase `phase`.
int16_t OscillatorLevel(OscillatorShape shape, uint32_t phase)
{
    // The tables' sample at the phase's top 6 bits, as a fraction of a cycle.
    const double table_phase = static_cast<double>(phase >> 10) / 64.0;
    switch (shape)
    {
    case OscillatorShape::kSquare:
        return (phase & 0x8000) != 0 ? 0x4000 : -0x4000;
    case OscillatorShape::kSawtooth:
        return static_cast<int16_t>(static_cast<uint16_t>(phase));
    case OscillatorShape::kPulse:
        return (phase & 0xc000) == 0xc000 ? 0x4000 : -0x4000;
    case OscillatorShape::kTriangle:
        return static_cast<int16_t>(std::lround(32767.0 * Triangle(table_phase)));
    case OscillatorShape::kSine:
        return static_cast<int16_t>(std::lround(32767.0 * std::sin(2.0 * std::numbers::pi * table_phase)));
    case OscillatorShape::kNone:
        break;
    }

    return 0;
}

// JASBank::noteOnOsc's pitch at key 60 is this rate, 16,736.016 (0x4682c008), over the output rate. A cycle of the
// waveform lasts 32 samples at this rate, and key 60 plays 523 Hz.
const float kOscillatorPitchRate = std::bit_cast<float>(0x4682c008u);

// An oscillator's wave holds its cycle in 32 samples at a quarter of that rate, and its note plays it at 4 times its
// rate. The PCM holds the cycle at 8 times the wave's rate, in 256 samples. Players then follow the DSP's 16-bit phase
// closely at every key the games play. The PCM's rate, 33,472 Hz, stays within SoundFont's range of rates.
constexpr float kOscillatorPitch = 4.0f;
constexpr uint32_t kOscillatorWaveCycle = 32;
constexpr uint32_t kOscillatorPcmCycle = 256;

// The oscillators' envelope (OSC_ENV): full volume until the release, then the release table (OSC_REL). The table
// falls to 0 over 10 units on curve 1 and stops.
const Oscillator kOscillatorEnvelope{0, 1.0f, {}, {{1, 10, 0}, {15, 0, 1}}, false, 1.0f, 0.0f};

// The waves of the oscillators that FindOscillatorSound plays: one looping cycle each, with key 60 at the wave's rate.
const std::map<uint16_t, WaveInfo>& OscillatorWaves()
{
    static const std::map<uint16_t, WaveInfo> kWaves = []
    {
        std::map<uint16_t, WaveInfo> waves;
        for (uint16_t oscillator = 0; oscillator < 0x100 - kFirstOscillatorProgram; oscillator++)
        {
            if (ShapeOf(oscillator) != OscillatorShape::kNone)
            {
                WaveInfo& wave = waves[oscillator];
                wave.format = WaveFormat::kPcm16;
                wave.base_key = 60;
                wave.sample_rate = kOscillatorPitchRate / kOscillatorPitch;
                wave.loop = true;
                wave.loop_end = kOscillatorWaveCycle;
                wave.sample_count = kOscillatorWaveCycle;
            }
        }

        return waves;
    }();

    return kWaves;
}

// The wave of `oscillator` (OscillatorWaves), or nullptr for an oscillator that FindOscillatorSound leaves out.
const WaveInfo* OscillatorWave(uint16_t oscillator)
{
    const auto found = OscillatorWaves().find(oscillator);

    return found != OscillatorWaves().end() ? &found->second : nullptr;
}

// One cycle of `oscillator`'s waveform (OscillatorShape), looping.
Pcm OscillatorPcm(uint16_t oscillator)
{
    std::vector<int16_t> cycle(kOscillatorPcmCycle);
    for (uint32_t i = 0; i < kOscillatorPcmCycle; i++)
    {
        cycle[i] = OscillatorLevel(ShapeOf(oscillator), i * (0x10000 / kOscillatorPcmCycle));
    }

    Pcm pcm;
    pcm.loop = true;
    pcm.loop_start = 0;
    pcm.loop_end = kOscillatorPcmCycle;
    pcm.channels.push_back(std::move(cycle));
    pcm.sample_rate = kOscillatorPitchRate / kOscillatorPitch * (kOscillatorPcmCycle / kOscillatorWaveCycle);

    return pcm;
}

} // namespace

std::optional<NoteSound> FindSound(const Bank& bank, uint32_t program, int key, int velocity)
{
    const auto found = bank.programs.find(program);
    if (found == bank.programs.end())
    {
        return std::nullopt;
    }

    NoteSound sound;
    sound.bank = bank.number;
    sound.program = program;
    sound.wave_system = bank.wave_system;
    const VelocityRegion* region = nullptr;
    if (const auto* inst = std::get_if<Instrument>(&found->second))
    {
        sound.volume = inst->volume;
        sound.pitch = inst->pitch;
        ApplyEffects(inst->effects, key, velocity, sound);

        for (const KeyRegion& keys : inst->keys)
        {
            if (key <= keys.high_key)
            {
                region = FindVelocityRegion(keys.velocities, velocity);
                break;
            }
        }

        for (std::size_t index : inst->oscillators)
        {
            sound.oscillators.push_back(&bank.oscillators[index]);
        }
    }
    else
    {
        const auto& drums = std::get<DrumSet>(found->second);
        if (key < 0 || key >= 128 || !drums.keys[static_cast<std::size_t>(key)])
        {
            return std::nullopt;
        }

        const Drum& drum = *drums.keys[static_cast<std::size_t>(key)];
        sound.drum = true;
        sound.volume = drum.volume;
        sound.pitch = drum.pitch;
        sound.pan = drum.pan;
        sound.direct_release = drum.release;
        ApplyEffects(drum.effects, key, velocity, sound);
        region = FindVelocityRegion(drum.velocities, velocity);
    }
    if (region == nullptr)
    {
        return std::nullopt;
    }

    sound.volume *= region->volume;
    sound.pitch *= region->pitch;
    sound.wave_id = region->wave_id;
    if (const auto named = bank.named_systems.find(region->system_id); named != bank.named_systems.end())
    {
        sound.wave_system = named->second;
    }

    return sound;
}

std::optional<NoteSound> FindOscillatorSound(uint32_t bank, uint32_t program)
{
    // JASBank::noteOnOsc: the oscillator's envelope, and its pitch at key 60.
    const uint32_t oscillator = program - kFirstOscillatorProgram;
    if (program < kFirstOscillatorProgram || ShapeOf(oscillator) == OscillatorShape::kNone)
    {
        return std::nullopt;
    }

    NoteSound sound;
    sound.bank = bank;
    sound.program = program;
    sound.wave_system = kOscillatorWaveSystem;
    sound.wave_id = static_cast<uint16_t>(oscillator);
    sound.pitch = kOscillatorPitch;
    sound.oscillators.push_back(&kOscillatorEnvelope);

    return sound;
}

const WaveInfo* FindWave(const SoundArchive& archive, uint32_t wave_system, uint16_t wave_id)
{
    if (wave_system == kOscillatorWaveSystem)
    {
        return OscillatorWave(wave_id);
    }

    const WaveSystem* ws = archive.FindWaveSystem(wave_system);
    if (ws == nullptr)
    {
        return nullptr;
    }

    const auto found = ws->waves.find(wave_id);

    return found != ws->waves.end() ? &found->second : nullptr;
}

std::vector<PlayedRegion> ProgramRegions(const Bank& bank, uint32_t program)
{
    std::vector<PlayedRegion> regions;
    const auto found = bank.programs.find(program);
    if (found == bank.programs.end())
    {
        return regions;
    }

    // Each velocity region plays from the velocity after the one before.
    auto add = [&](int low_key, int high_key, const std::vector<VelocityRegion>& velocities)
    {
        int low_velocity = 0;
        for (const VelocityRegion& v : velocities)
        {
            if (v.high_velocity >= low_velocity)
            {
                regions.push_back({low_key, high_key, low_velocity, v.high_velocity, v.wave_id});
            }
            low_velocity = std::max<int>(low_velocity, v.high_velocity + 1);
        }
    };

    if (const auto* inst = std::get_if<Instrument>(&found->second))
    {
        int low_key = 0;
        for (const KeyRegion& keys : inst->keys)
        {
            if (keys.high_key >= low_key)
            {
                add(low_key, keys.high_key, keys.velocities);
            }
            low_key = std::max<int>(low_key, keys.high_key + 1);
        }
    }
    else
    {
        const auto& drums = std::get<DrumSet>(found->second);
        for (int key = 0; key < 128; key++)
        {
            if (drums.keys[static_cast<std::size_t>(key)])
            {
                add(key, key, drums.keys[static_cast<std::size_t>(key)]->velocities);
            }
        }
    }

    return regions;
}

float DspPitch(float init_pitch, float osc_pitch, float octaves, float track_pitch)
{
    return init_pitch * osc_pitch * std::exp2(octaves) * track_pitch * 4096.0f;
}

bool PastPitchLimit(float pitch)
{
    return std::trunc(pitch) > kPitchLimit;
}

namespace
{

// Builds a preset's zones (PresetZones and OscillatorZones). The track's oscillators replace the sound's, and the
// track's direct release replaces the sound's. Each set of volume oscillators and release has its envelope traced once.
// With `limit_pitch`, the keys past the DSP's pitch limit get zones that play at the limit.
class ZoneBuilder
{
public:
    ZoneBuilder(const Oscillator* volume_override, const Oscillator* pitch_override, uint16_t direct_release,
                uint16_t default_release, bool limit_pitch)
        : volume_override_(volume_override),
          pitch_override_(pitch_override),
          direct_release_(direct_release),
          default_release_(default_release),
          limit_pitch_(limit_pitch)
    {
    }

    // Add the zone of `sound` over `region`'s keys and velocities. `wave` is the sound's wave, or nullptr if it's
    // missing.
    void Add(const NoteSound& sound, const WaveInfo* wave, const PlayedRegion& region);

    std::vector<PresetZone> Take()
    {
        return std::move(zones_);
    }

private:
    const Oscillator* volume_override_;
    const Oscillator* pitch_override_;
    uint16_t direct_release_;
    uint16_t default_release_;
    bool limit_pitch_;
    std::map<std::tuple<const Oscillator*, const Oscillator*, uint16_t>, EnvelopeCurve> curves_;
    std::vector<PresetZone> zones_;
};

void ZoneBuilder::Add(const NoteSound& sound, const WaveInfo* wave, const PlayedRegion& region)
{
    // The channel's oscillators: the sound's, or a drum's constant one, with the track's in their place.
    static const Oscillator kConstant;
    std::array<const Oscillator*, 2> oscs{};
    for (std::size_t i = 0; i < 2 && i < sound.oscillators.size(); i++)
    {
        oscs[i] = sound.oscillators[i];
    }
    if (sound.drum)
    {
        oscs = {&kConstant, nullptr};
    }
    if (volume_override_ != nullptr)
    {
        oscs[0] = volume_override_;
    }
    if (pitch_override_ != nullptr)
    {
        oscs[1] = pitch_override_;
    }

    PresetZone z;
    z.low_key = region.low_key;
    z.high_key = region.high_key;
    z.low_velocity = region.low_velocity;
    z.high_velocity = region.high_velocity;
    z.wave_system = sound.wave_system;
    z.wave_id = sound.wave_id;
    z.root_key = sound.drum ? static_cast<uint8_t>(region.low_key) : (wave ? wave->base_key : 60);
    z.cents = sound.pitch > 0.0f ? 1200.0 * std::log2(static_cast<double>(sound.pitch)) : 0.0;
    z.volume = sound.volume;
    z.pan = sound.pan;
    z.effect_pan = sound.effect_pan;

    std::vector<const Oscillator*> volume;
    for (const Oscillator* osc : oscs)
    {
        if (osc != nullptr && osc->target == 0)
        {
            volume.push_back(osc);
        }
    }
    const uint16_t release = direct_release_ != 0 ? direct_release_ : sound.direct_release;
    const auto key =
        std::make_tuple(volume.empty() ? nullptr : volume[0], volume.size() > 1 ? volume[1] : nullptr, release);
    auto [it, added] = curves_.try_emplace(key);
    if (added)
    {
        it->second = TraceEnvelope(volume, release, default_release_);
    }
    z.envelope = it->second;

    // The first key whose notes start past the DSP's pitch limit. The keys above it pass the limit too. A separate zone
    // plays them all at the limit.
    int limited = z.high_key + 1;
    if (limit_pitch_ && wave != nullptr)
    {
        const float init_pitch = sound.pitch * (wave->sample_rate / kDacRate);
        for (int k = z.low_key; k <= z.high_key; k++)
        {
            if (PastPitchLimit(DspPitch(init_pitch, 1.0f, static_cast<float>(k - z.root_key) / 12.0f, 1.0f)))
            {
                limited = k;
                break;
            }
        }
    }
    if (limited <= z.high_key)
    {
        PresetZone fixed = z;
        fixed.low_key = limited;
        fixed.fixed_pitch = true;
        fixed.cents = 1200.0 * std::log2(static_cast<double>(kPitchLimit) / 4096.0 * static_cast<double>(kDacRate) /
                                         static_cast<double>(wave->sample_rate));
        if (limited > z.low_key)
        {
            z.high_key = limited - 1;
            zones_.push_back(std::move(z));
        }
        zones_.push_back(std::move(fixed));
        return;
    }

    zones_.push_back(std::move(z));
}

} // namespace

std::vector<PresetZone> PresetZones(const Bank& bank, const SoundArchive& archive, uint32_t program,
                                    const Oscillator* volume_override, const Oscillator* pitch_override,
                                    uint16_t direct_release, uint16_t default_release, bool limit_pitch)
{
    const auto found = bank.programs.find(program);
    if (found == bank.programs.end())
    {
        return {};
    }

    // Instruments whose effects follow the key play each key differently.
    bool per_key = false;
    if (const auto* inst = std::get_if<Instrument>(&found->second))
    {
        per_key = std::ranges::any_of(
            inst->effects, [](const InstEffect& e) { return e.kind == InstEffect::Kind::kSense && e.trigger == 2; });
    }

    ZoneBuilder builder(volume_override, pitch_override, direct_release, default_release, limit_pitch);
    for (const PlayedRegion& region : ProgramRegions(bank, program))
    {
        for (int low = region.low_key; low <= region.high_key; low = per_key ? low + 1 : region.high_key + 1)
        {
            const int high = per_key ? low : region.high_key;
            const std::optional<NoteSound> sound = FindSound(bank, program, low, region.high_velocity);
            if (!sound)
            {
                continue;
            }

            builder.Add(*sound, FindWave(archive, sound->wave_system, sound->wave_id),
                        {low, high, region.low_velocity, region.high_velocity, sound->wave_id});
        }
    }

    return builder.Take();
}

std::vector<PresetZone> OscillatorZones(uint32_t program, const Oscillator* volume_override,
                                        const Oscillator* pitch_override, uint16_t direct_release,
                                        uint16_t default_release, bool limit_pitch)
{
    const std::optional<NoteSound> sound = FindOscillatorSound(0, program);
    if (!sound)
    {
        return {};
    }

    ZoneBuilder builder(volume_override, pitch_override, direct_release, default_release, limit_pitch);
    builder.Add(*sound, OscillatorWave(sound->wave_id), {0, 127, 0, 127, sound->wave_id});

    return builder.Take();
}

BankSet::BankSet(const SoundArchive& archive, AwReader read_aw) : archive_(archive), read_aw_(std::move(read_aw))
{
}

const Pcm* BankSet::GetWave(uint32_t wave_system, uint16_t wave_id)
{
    const auto key = std::make_pair(wave_system, wave_id);
    if (const auto it = waves_.find(key); it != waves_.end())
    {
        return it->second.get();
    }

    std::unique_ptr<Pcm>& slot = waves_[key];
    if (wave_system == kOscillatorWaveSystem && OscillatorWave(wave_id) != nullptr)
    {
        slot = std::make_unique<Pcm>(OscillatorPcm(wave_id));
        return slot.get();
    }

    const WaveSystem* ws = archive_.FindWaveSystem(wave_system);
    const WaveInfo* info = nullptr;
    if (ws != nullptr)
    {
        const auto found = ws->waves.find(wave_id);
        info = found != ws->waves.end() ? &found->second : nullptr;
    }
    if (info == nullptr)
    {
        errors_.push_back("wave " + std::to_string(wave_id) + " of wave system " + std::to_string(wave_system) +
                          " is missing");
        return nullptr;
    }

    // An archive that can't be read, such as one on a truncated disc image, stays unread. Its notes play nothing.
    auto [aw, added] = aws_.try_emplace(info->archive);
    if (added)
    {
        try
        {
            aw->second = read_aw_(info->archive);
            if (!aw->second)
            {
                errors_.push_back("wave archive " + info->archive + " is missing");
            }
        }
        catch (const FormatError& e)
        {
            errors_.push_back("wave archive " + info->archive + ": " + e.what());
        }
    }
    if (!aw->second)
    {
        return nullptr;
    }

    try
    {
        slot = std::make_unique<Pcm>(DecodeWave(*info, *aw->second));
    }
    catch (const FormatError& e)
    {
        errors_.push_back("wave " + std::to_string(wave_id) + " of " + info->archive + ": " + e.what());
    }

    return slot.get();
}

} // namespace ntdlib::sequence_export::jaudio
