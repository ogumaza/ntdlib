// SPDX-License-Identifier: MIT

// Checks the Wii U's model of nw::snd: its voices on AX, the engine on a small sound archive, and the effects on the
// aux buses. ntdlib's tests check the arithmetic helpers, the envelope and the LFO.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/effects.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

namespace fsar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;

namespace
{

const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kCafe, {}, false, ntdlib::sar::Rules::kStopAtFirstProblem};

using ntdlib::cafe::snd::AxVoice;
using ntdlib::cafe::snd::AxVoices;
using ntdlib::cafe::snd::EffectSetup;
using ntdlib::cafe::snd::EffectType;
using ntdlib::cafe::snd::OutputMode;
using ntdlib::cafe::snd::WaveBuffer;

// A constant PCM16 wave of `count` samples.
std::vector<uint8_t> ConstantWave(int count, int16_t value)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    }

    return bytes;
}

void TestAxVoice()
{
    // A voice set up to play a buffer starts its AX voice with the buffer's offsets, the volume, the TV's mix and the
    // ratio. The ratio chooses the 4-tap filter.
    const std::vector<uint8_t> wave = ConstantWave(100, 1000);
    ntdlib::cafe::ax::Memory memory;
    memory.Map(0x10000000, wave);
    ntdlib::cafe::ax::System system(memory);
    AxVoices voices(system, memory);
    constexpr std::array<OutputMode, 2> kStereo = {OutputMode::kStereo, OutputMode::kStereo};
    AxVoice* voice = voices.AllocVoice(64, {});
    if (!Check(voice != nullptr, "an AX voice is acquired"))
    {
        return;
    }

    WaveBuffer buffer;
    buffer.address = 0x10000000;
    buffer.sample_length = 100;
    buffer.loop = true;
    voice->SetSampleFormat(ntdlib::cafe::snd::SampleFormat::kPcm16);
    voice->SetSampleRate(48000);
    voice->Param().volume = 0.5f;
    voice->Param().tv[0][0] = 1.0f;
    voice->Param().tv[0][1] = 0.25f;
    voice->AppendWaveBuffer(buffer);
    voice->SetState(AxVoice::State::kPlay);

    voices.UpdateAllVoices(kStereo);

    const ntdlib::cafe::ax::Pb& pb = voice->SdkVoice()->game;
    XSFLIB_CHECK(pb.state == ntdlib::cafe::ax::VoiceState::kRun && buffer.status == WaveBuffer::Status::kPlay);
    XSFLIB_CHECK(pb.loop_flag == 1 && pb.current_address == 0x10000000 / 2 && pb.end_address == 0x10000000 / 2 + 99);
    XSFLIB_CHECK(pb.ve.volume == 0x4000 && pb.ve.delta == 0);
    XSFLIB_CHECK(pb.src.ratio == 0x18000 && pb.src_mode == ntdlib::cafe::ax::SrcMode::kFourTap &&
                 pb.four_tap_filter == 0);
    XSFLIB_CHECK(pb.tv_mix[0][0].volume == 0x8000 && pb.tv_mix[1][0].volume == 0x2000 && pb.tv_mix[2][0].volume == 0);

    // A change of volume ramps across the next frame, from where the last ramp reached.
    voice->Param().volume = 1.0f;

    system.RenderFrame();
    voices.UpdateAllVoices(kStereo);

    XSFLIB_CHECK(pb.ve.volume == 0x4000 && pb.ve.delta == 0x4000 / 96);

    // A pitch change moves the ratio, but the filter chosen at the start stays.
    voice->Param().pitch = 0.5f;

    system.RenderFrame();
    voices.UpdateAllVoices(kStereo);

    XSFLIB_CHECK(pb.src.ratio == 0xC000 && pb.four_tap_filter == 0);

    // Freeing the voice frees its AX voice and finishes its buffer.
    voices.Free(*voice);
    XSFLIB_CHECK(!voice->IsAvailable() && buffer.status == WaveBuffer::Status::kDone);
}

void TestWaveBufferQueue()
{
    // A voice moves on to the buffer queued behind the one it's playing. The queued buffer lies past the first buffer's
    // end. The voice moves on when the loop count shows that it has reached the queued buffer. It then plays that
    // buffer to its end.
    const std::vector<uint8_t> first_wave = ConstantWave(100, 1000);
    const std::vector<uint8_t> second_wave = ConstantWave(300, 2000);
    ntdlib::cafe::ax::Memory memory;
    memory.Map(0x10000000, first_wave);
    memory.Map(0x10001000, second_wave);
    ntdlib::cafe::ax::System system(memory);
    AxVoices voices(system, memory);
    constexpr std::array<OutputMode, 2> kStereo = {OutputMode::kStereo, OutputMode::kStereo};
    AxVoice* voice = voices.AllocVoice(64, {});
    if (!Check(voice != nullptr, "an AX voice is acquired"))
    {
        return;
    }

    WaveBuffer first;
    first.address = 0x10000000;
    first.sample_length = 100;
    WaveBuffer second;
    second.address = 0x10001000;
    second.sample_length = 300;
    voice->Param().tv[0][0] = 1.0f;
    voice->Param().interpolation = 1; // linear: it passes a constant through unchanged
    voice->AppendWaveBuffer(first);
    voice->AppendWaveBuffer(second);
    voice->SetState(AxVoice::State::kPlay);

    for (int frame = 0; frame < 4; ++frame)
    {
        voices.UpdateAllVoices(kStereo);
        system.RenderFrame();
    }

    const ntdlib::cafe::ax::Pb& pb = voice->SdkVoice()->game;
    XSFLIB_CHECK(first.status == WaveBuffer::Status::kDone && second.status == WaveBuffer::Status::kPlay);
    XSFLIB_CHECK(pb.end_address == 0x10001000 / 2 + 299 && pb.loop_flag == 0);
    XSFLIB_CHECK(system.VoiceBuses().tv[0][0][0] == 2000);
}

// The engine plays a sequence from a small archive: at a timebase of 48 and 120 beats a minute, a note of 24 ticks
// (250 ms) on a PCM16 wave of 100 ms, then the end. The note starts in the second frame. The first frame takes the
// start to the sound thread. AX sets the note's voice up at the end of the third frame and plays it from the fourth.
// The sequence ends after its 24 ticks.
// An archive with one sequence, SEQ_NOTE, that plays `bytecode` on a bank whose one note is a constant wave.
ntdlib::sar::SoundArchive NoteArchive(const std::vector<uint8_t>& bytecode)
{
    std::vector<uint8_t> samples(2 * 3200);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[i] = 0x10; // 0x1000, big-endian
    }
    fsar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 3200;
    wave.channels = {samples};

    fsar_files::Region region;
    region.original_key = 60;
    fsar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    fsar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    fsar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    fsar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_NOTE", 1}};
    spec.wave_archives = {{"WARC_NOTE", 2}};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{fsar_files::Sequence(true, bytecode, {{"Note", 0}}), "", false},
                  {fsar_files::Bank(true, bank), "", false},
                  {fsar_files::WaveArchive(true, {fsar_files::Wave(true, wave)}), "", false}};

    return ntdlib::sar::SoundArchive::Load(fsar_files::Archive(true, spec), kSarOptions);
}

void TestEngine()
{
    const ntdlib::sar::SoundArchive archive = NoteArchive({0xB0, 48, 0xE1, 0x00, 120, 60, 127, 24, 0xFF});
    ntdlib::cafe::snd::ArchiveModel model(archive);
    ntdlib::cafe::snd::Engine& engine = *model.engine;

    XSFLIB_CHECK(engine.StartSequence(0) != nullptr);
    XSFLIB_CHECK(engine.StartSequence(1) == nullptr);

    const auto sounds = [&model]
    {
        for (const int16_t sample : model.system.Output())
        {
            if (sample != 0)
            {
                return true;
            }
        }

        return false;
    };

    engine.RunFrame();
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0);
    engine.RunFrame();
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 1);
    engine.RunFrame();
    XSFLIB_CHECK(!sounds());
    engine.RunFrame();
    XSFLIB_CHECK(sounds());

    // The sequence ends at its 24th tick, about 250 ms after it started. The bank's default release is the fastest. So
    // the note ends with the sequence.
    int frames = 4;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }

    XSFLIB_CHECK(!engine.IsBusy() && frames > 80 && frames <= 90);
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
}

uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

// The effects that the game sets up for a sound: those that list it, and on each bus where none lists it, those that
// list no sounds.
void TestEffectsFor()
{
    const std::vector<EffectSetup> setups = {
        {0, 0, EffectType::kReverbStd, {}, {}},
        {0, 0, EffectType::kReverbHi, {}, {5, 6}},
        {0, 1, EffectType::kChorus, {}, {}},
        {1, 0, EffectType::kDelay, {}, {6}},
    };

    const auto types = [&setups](uint32_t sound)
    {
        std::vector<EffectType> found;
        for (const EffectSetup& setup : ntdlib::cafe::snd::EffectsFor(setups, sound))
        {
            found.push_back(setup.type);
        }

        return found;
    };

    XSFLIB_CHECK(types(1) == std::vector<EffectType>({EffectType::kReverbStd, EffectType::kChorus}));
    XSFLIB_CHECK(types(5) == std::vector<EffectType>({EffectType::kReverbHi, EffectType::kChorus}));
    XSFLIB_CHECK(types(6) == std::vector<EffectType>({EffectType::kReverbHi, EffectType::kChorus, EffectType::kDelay}));
}

// The effect classes' SetParam (effects.h): the game's parameters into the library's. The classes clamp each
// parameter. A NaN takes the lower end of its range.
void TestEffectParams()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto words = [](std::initializer_list<float> values)
    {
        std::vector<uint32_t> out;
        for (const float value : values)
        {
            out.push_back(Bits(value));
        }

        return out;
    };

    // FxDelay: delay, feedback, outGain, maxDelay, lpf.
    auto delay = ntdlib::cafe::snd::DelayParams(words({300.0f, 0.4f, 1.0f, 500.0f, 1.0f}));
    XSFLIB_CHECK(delay.max_delay == 500.0f && delay.delay == 300.0f && delay.feedback == 0.4f && delay.lpf == 1.0f &&
                 delay.out_gain == 1.0f && delay.send_gain == 0.0f);
    delay = ntdlib::cafe::snd::DelayParams(words({600.0f, 1.5f, -1.0f, 500.0f, 2.0f}));
    XSFLIB_CHECK(delay.delay == 500.0f && delay.feedback == 0.99f && delay.out_gain == 0.0f && delay.lpf == 1.0f);
    // A longest delay below 0.04 ms takes 0.04. A delay longer than the raw maximum takes the raw maximum.
    delay = ntdlib::cafe::snd::DelayParams(words({0.01f, -0.5f, nan, 0.0f, nan}));
    XSFLIB_CHECK(delay.max_delay == 0.04f && delay.delay == 0.0f && delay.feedback == 0.0f && delay.out_gain == 0.0f &&
                 delay.lpf == 0.0f);
    delay = ntdlib::cafe::snd::DelayParams(words({0.01f, nan, 0.5f, 100.0f, 0.5f}));
    XSFLIB_CHECK(delay.max_delay == 100.0f && delay.delay == 0.04f && delay.feedback == 0.0f);

    // FxChorus: delayTime, depth, rate, feedback, outGain.
    auto chorus = ntdlib::cafe::snd::ChorusParams(words({60.0f, 0.5f, 3.0f, 1.2f, 0.8f}));
    XSFLIB_CHECK(chorus.delay_time == 50.0f && chorus.depth == 0.5f && chorus.rate == 2.0f &&
                 chorus.feedback == 0.99f && chorus.out_gain == 0.8f && chorus.send_gain == 0.0f);
    chorus = ntdlib::cafe::snd::ChorusParams(words({0.05f, -1.0f, nan, 0.3f, 1.5f}));
    XSFLIB_CHECK(chorus.delay_time == 0.1f && chorus.depth == 0.0f && chorus.rate == 0.1f && chorus.feedback == 0.3f &&
                 chorus.out_gain == 1.0f);

    // FxReverbStd: preDelayTime, fusedTime, coloration, damping, outGain, earlyMode, preDelayTimeMax, fusedMode,
    // earlyGain, fusedGain.
    std::vector<uint32_t> std_words = words({0.01f, 2.2f, 0.6f, 0.3f, 1.0f, 0.0f, 0.05f, 0.0f, 0.3f, 0.55f});
    std_words[5] = 4;
    std_words[7] = 2;
    auto reverb_std = ntdlib::cafe::snd::ReverbStdParams(std_words);
    XSFLIB_CHECK(reverb_std.early_mode == 4 && reverb_std.pre_delay_time_max == 0.05f &&
                 reverb_std.pre_delay_time == 0.01f && reverb_std.fused_mode == 2 && reverb_std.fused_time == 2.2f &&
                 reverb_std.coloration == 0.6f && reverb_std.damping == 0.3f && reverb_std.early_gain == 0.3f &&
                 reverb_std.fused_gain == 0.55f && reverb_std.out_gain == 1.0f && reverb_std.send_gain == 0.0f);
    std_words = words({0.1f, -2.0f, 1.5f, nan, -1.0f, 0.0f, 0.05f, 0.0f, 2.0f, nan});
    reverb_std = ntdlib::cafe::snd::ReverbStdParams(std_words);
    XSFLIB_CHECK(reverb_std.pre_delay_time == 0.05f && reverb_std.fused_time == 0.0f && reverb_std.coloration == 1.0f &&
                 reverb_std.damping == 0.0f && reverb_std.out_gain == 0.0f && reverb_std.early_gain == 1.0f &&
                 reverb_std.fused_gain == 0.0f);

    // FxReverbHi: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain, earlyMode, preDelayTimeMax,
    // fusedMode, earlyGain, fusedGain.
    std::vector<uint32_t> hi_words = words({0.02f, 3.0f, 0.5f, 0.4f, 0.1f, 0.9f, 0.0f, nan, 0.0f, 0.7f, 1.1f});
    hi_words[6] = 3;
    hi_words[8] = 1;
    const auto reverb_hi = ntdlib::cafe::snd::ReverbHiParams(hi_words);
    XSFLIB_CHECK(reverb_hi.early_mode == 3 && reverb_hi.pre_delay_time_max == 0.0f &&
                 reverb_hi.pre_delay_time == 0.02f && reverb_hi.fused_mode == 1 && reverb_hi.fused_time == 3.0f &&
                 reverb_hi.coloration == 0.5f && reverb_hi.damping == 0.4f && reverb_hi.crosstalk == 0.1f &&
                 reverb_hi.early_gain == 0.7f && reverb_hi.fused_gain == 1.0f && reverb_hi.out_gain == 0.9f &&
                 reverb_hi.send_gain == 0.0f);
}

// A delay on the TV's aux A: the note that the sequence sends there comes back 30 ms later. The delay takes 960
// samples of the 32 kHz renderer, 10 frames. The aux bus adds 2 frames (ax::System). Before the echo, the output is
// the same as without the delay. The echo ends 12 frames after the note. The effects ring until a second (334 frames)
// has passed without sound.
void TestEngineEffects()
{
    // Command 0xD9 sets the track's send to aux A.
    const ntdlib::sar::SoundArchive archive = NoteArchive({0xD9, 127, 0xB0, 48, 0xE1, 0x00, 120, 60, 127, 24, 0xFF});

    ntdlib::cafe::snd::EngineOptions options;
    options.effects = {{ntdlib::cafe::snd::kEffectDeviceTv,
                        0,
                        EffectType::kDelay,
                        {Bits(30.0f), Bits(0.0f), Bits(1.0f), Bits(50.0f), Bits(1.0f)},
                        {}}};
    ntdlib::cafe::snd::ArchiveModel dry(archive);
    ntdlib::cafe::snd::ArchiveModel wet(archive, options);
    XSFLIB_CHECK(!dry.engine->HasEffects() && wet.engine->HasEffects());
    XSFLIB_CHECK(dry.engine->StartSequence(0) && wet.engine->StartSequence(0));

    int first_sound = -1;
    int first_difference = -1;
    int last_dry = -1;
    int last_wet = -1;
    int rang_out = -1;
    for (int frame = 0; frame < 1000 && rang_out < 0; ++frame)
    {
        dry.engine->RunFrame();
        wet.engine->RunFrame();
        const std::span<const int16_t> a = dry.system.Output();
        const std::span<const int16_t> b = wet.system.Output();
        if (xsflib::rip::Audible(a))
        {
            first_sound = first_sound < 0 ? frame : first_sound;
            last_dry = frame;
        }
        if (xsflib::rip::Audible(b))
        {
            last_wet = frame;
        }
        if (first_difference < 0 && !std::equal(a.begin(), a.end(), b.begin(), b.end()))
        {
            first_difference = frame;
        }
        if (last_wet >= 0 && !wet.engine->EffectsRinging())
        {
            rang_out = frame;
        }
    }

    XSFLIB_CHECK(first_sound == 3 && first_difference == first_sound + 2 + 10);
    XSFLIB_CHECK(last_wet == last_dry + 2 + 10 && rang_out == last_wet + 334);
    XSFLIB_CHECK(!dry.engine->IsBusy() && !wet.engine->IsBusy() && !dry.engine->EffectsRinging());
}

namespace snd = ntdlib::cafe::snd;

// A PCM16 wave of `samples` samples of `value` at 48 kHz.
fsar_files::WaveSpec ConstantPcmWave(int samples, int16_t value = 0x1000)
{
    fsar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.sample_rate = 48000;
    wave.loop_end = static_cast<uint32_t>(samples);
    wave.channels = {ConstantWave(samples, value)};

    return wave;
}

// A stereo DSP-ADPCM wave at 32 kHz that loops from sample 2800 to its end at sample 15988. Each frame's header picks
// one of the 8 coefficient pairs and a scale. The nibbles are pseudo-random.
fsar_files::WaveSpec LoopingAdpcmWave()
{
    constexpr int kSamples = 15988;
    fsar_files::WaveSpec wave;
    wave.encoding = 2;
    wave.loop = true;
    wave.sample_rate = 32000;
    wave.loop_start = 2800;
    wave.loop_end = kSamples;
    uint32_t seed = 1;
    for (int channel = 0; channel < 2; ++channel)
    {
        std::vector<uint8_t> bytes;
        for (int frame = 0; frame < kSamples / 14; ++frame)
        {
            bytes.push_back(static_cast<uint8_t>((frame % 8) << 4 | (frame + channel) % 12));
            for (int i = 0; i < 7; ++i)
            {
                seed = seed * 1103515245u + 12345u;
                bytes.push_back(static_cast<uint8_t>(seed >> 16));
            }
        }
        wave.channels.push_back(std::move(bytes));
    }

    return wave;
}

// An archive with one sequence sound that plays `bytecode` on a bank with an instrument for each of `waves`. Program i
// plays wave i for every key, at original key 60. The sound allocates the tracks in `tracks`, a bit each.
ntdlib::sar::SoundArchive ProgramArchive(const std::vector<uint8_t>& bytecode,
                                         const std::vector<fsar_files::WaveSpec>& waves, uint32_t tracks = 1)
{
    fsar_files::BankSpec bank;
    std::vector<std::vector<uint8_t>> wave_files;
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        fsar_files::Region region;
        region.wave = static_cast<uint32_t>(i);
        region.original_key = 60;
        fsar_files::VelocityTable velocities;
        velocities.entries = {{0, region}};
        fsar_files::KeyTable keys;
        keys.entries = {{0, velocities}};
        bank.waves.push_back({0x05000000, static_cast<uint32_t>(i)});
        bank.instruments.push_back(keys);
        wave_files.push_back(fsar_files::Wave(true, waves[i]));
    }

    fsar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    sound.allocate_track_flags = tracks;
    fsar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_NOTE", 1}};
    spec.wave_archives = {{"WARC_NOTE", 2}};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{fsar_files::Sequence(true, bytecode, {{"Note", 0}}), "", false},
                  {fsar_files::Bank(true, bank), "", false},
                  {fsar_files::WaveArchive(true, wave_files), "", false}};

    return ntdlib::sar::SoundArchive::Load(fsar_files::Archive(true, spec), kSarOptions);
}

// An archive whose sequence plays two tracks that loop for ever. Track 0 plays a looping stereo wave in DSP-ADPCM at
// 32 kHz, in notes of random lengths from 12 to 48 ticks, and sends to aux bus A. Track 1 plays a loud wave of 21 ms
// that doesn't loop, 7 semitones up. Each note sounds for 14 ms, until the wave's end. Without `track_1_plays`, track 1
// rests for each of its notes' lengths instead. `send` is track 0's send to aux bus A.
ntdlib::sar::SoundArchive LoopingArchive(bool track_1_plays = true, uint8_t send = 80)
{
    std::vector<uint8_t> tracks = {
        0xFE, 0x00, 0x03,                       // tracks 0 and 1
        0xB0, 48,   0xE1, 0x00, 120,            // a timebase of 48, 120 beats a minute
        0x88, 0x01, 0x00, 0x00, 0x1F,           // track 1 at 0x1F
        0xD9, send,                             // track 0 sends to aux bus A
        0xA0, 60,   100,  0x00, 12,   0x00, 48, // 0x0F: key 60 at a random length
        64,   80,   24,                         // key 64
        0x80, 8,                                // a rest
        0x89, 0x00, 0x00, 0x0F,                 // back to 0x0F
        0x81, 1,                                // 0x1F: track 1 plays program 1
        0xC1, 127,                              // at full volume
        67,   127,  48,                         // 0x23: key 67
        0x80, 24,                               // a rest
        0x89, 0x00, 0x00, 0x23,                 // back to 0x23
    };
    if (!track_1_plays)
    {
        // A rest of 48 ticks takes the place of key 67. The jump back to 0x23 still reaches it.
        tracks.erase(tracks.begin() + 0x23, tracks.begin() + 0x26);
        tracks.insert(tracks.begin() + 0x23, {0x80, 48});
    }

    return ProgramArchive(tracks, {LoopingAdpcmWave(), ConstantPcmWave(1000, 0x7000)}, 0x3);
}

// Options with a standard reverb on the TV's aux bus A.
snd::EngineOptions ReverbOptions()
{
    std::vector<uint32_t> params = {Bits(0.01f), Bits(2.2f),  Bits(0.6f), Bits(0.3f), Bits(1.0f),
                                    4,           Bits(0.05f), 2,          Bits(0.3f), Bits(0.55f)};
    snd::EngineOptions options;
    options.effects = {{snd::kEffectDeviceTv, 0, EffectType::kReverbStd, params, {}}};

    return options;
}

// The output of the next `frames` frames.
std::vector<int16_t> RunFrames(snd::ArchiveModel& model, int frames)
{
    std::vector<int16_t> output;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.system.Output();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// Whether any sample isn't 0.
bool Sounds(std::span<const int16_t> output)
{
    return std::any_of(output.begin(), output.end(), [](int16_t sample) { return sample != 0; });
}

// Whether `model` refuses `snapshot` with std::invalid_argument.
bool Refuses(snd::ArchiveModel& model, const snd::ArchiveModel::Snapshot& snapshot)
{
    try
    {
        model.Restore(snapshot);
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// After a restore, a model renders what it rendered after the snapshot the first time, sample for sample, and does so
// again from the same snapshot. A player started after the snapshot goes, and so do its notes. A snapshot taken before
// the sound starts holds the start that waits.
void TestSnapshotRenders()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel model(archive, ReverbOptions());
    model.engine->StartSequence(0);
    const snd::ArchiveModel::Snapshot at_start = model.Save();
    const std::vector<int16_t> opening = RunFrames(model, 300);
    const snd::ArchiveModel::Snapshot later = model.Save();
    const int voices = model.engine->Voices().ActiveCount();
    const std::vector<int16_t> first = RunFrames(model, 1000);
    model.engine->StartSequence(0);
    RunFrames(model, 150);

    model.Restore(later);
    const std::vector<int16_t> second = RunFrames(model, 1000);
    model.Restore(later);
    const std::vector<int16_t> third = RunFrames(model, 1000);
    model.Restore(at_start);
    const std::vector<int16_t> again = RunFrames(model, 300);

    XSFLIB_CHECK(voices > 0 && Sounds(first));
    XSFLIB_CHECK(second == first);
    XSFLIB_CHECK(third == first);
    XSFLIB_CHECK(again == opening);
}

// From frame 240 to 440, a snapshot taken at every other frame and restored two frames later renders those two frames
// again. Notes of both tracks start in that stretch. Track 1's second note sounds from frame 252 to the wave's end.
void TestSnapshotEveryFrame()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel model(archive, ReverbOptions());
    model.engine->StartSequence(0);
    RunFrames(model, 240);
    int mismatches = 0;

    for (int frame = 0; frame < 100; ++frame)
    {
        const snd::ArchiveModel::Snapshot each = model.Save();
        const std::vector<int16_t> ahead = RunFrames(model, 2);
        model.Restore(each);
        if (RunFrames(model, 2) != ahead)
        {
            ++mismatches;
        }
    }

    Check(mismatches == 0, std::to_string(mismatches) + " of 100 snapshots render other frames when restored");
}

// The players keep their places through a restore, and the next player started takes the next place. A snapshot that
// holds a start that waits restores the start onto the player in its place. That holds when an earlier restore removed
// the player and a new player took its place.
void TestSnapshotPlayers()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel model(archive, ReverbOptions());
    model.engine->StartSequence(0);
    RunFrames(model, 80);
    const snd::ArchiveModel::Snapshot one_player = model.Save();
    model.engine->StartSequence(0);
    const snd::ArchiveModel::Snapshot start_waits = model.Save();
    const std::vector<int16_t> first = RunFrames(model, 150);

    model.Restore(one_player);
    const snd::SequenceSoundPlayer* next = model.engine->StartSequence(0);
    model.Restore(start_waits);
    const std::vector<int16_t> second = RunFrames(model, 150);

    XSFLIB_CHECK(next != nullptr && next->Id() == 1);
    XSFLIB_CHECK(Sounds(first) && second == first);
}

// A model refuses another model's snapshot. It also refuses a snapshot that combines the state of its engine with the
// state of the other model's AX. The refusal changes nothing.
void TestSnapshotOfAnotherModel()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel model(archive, ReverbOptions());
    snd::ArchiveModel other(archive, ReverbOptions());
    model.engine->StartSequence(0);
    const snd::ArchiveModel::Snapshot early = model.Save();
    RunFrames(model, 150);
    const snd::ArchiveModel::Snapshot late = model.Save();
    const std::vector<int16_t> expected = RunFrames(model, 150);
    const snd::ArchiveModel::Snapshot mixed{other.Save().system, early.engine};
    model.Restore(late);

    const bool refused = Refuses(other, late);
    const bool refused_mixed = Refuses(model, mixed);
    const std::vector<int16_t> after = RunFrames(model, 150);

    XSFLIB_CHECK(refused && refused_mixed);
    XSFLIB_CHECK(after == expected);
}

// A muted track plays on, and none of its sound reaches the output. With track 1 muted from the start, the output is
// the output of the archive whose track 1 rests in place of its notes, sample for sample, with the reverb on track 0's
// sends. Each frame has as many channels and voices as it has without the mute.
void TestMuteTrack()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    const ntdlib::sar::SoundArchive resting = LoopingArchive(false);
    snd::ArchiveModel muted(archive, ReverbOptions());
    snd::ArchiveModel playing(archive, ReverbOptions());
    snd::ArchiveModel rests(resting, ReverbOptions());
    muted.engine->StartSequence(0);
    playing.engine->StartSequence(0);
    rests.engine->StartSequence(0);
    muted.engine->MuteTrack(0, 1, true);
    std::vector<int16_t> heard;
    std::vector<int16_t> played;
    bool same_channels = true;

    for (int frame = 0; frame < 1000; ++frame)
    {
        const std::vector<int16_t> muted_frame = RunFrames(muted, 1);
        const std::vector<int16_t> playing_frame = RunFrames(playing, 1);
        heard.insert(heard.end(), muted_frame.begin(), muted_frame.end());
        played.insert(played.end(), playing_frame.begin(), playing_frame.end());
        same_channels = same_channels &&
                        muted.engine->Channels().ActiveCount() == playing.engine->Channels().ActiveCount() &&
                        muted.engine->Voices().ActiveCount() == playing.engine->Voices().ActiveCount();
    }
    const std::vector<int16_t> rested = RunFrames(rests, 1000);

    XSFLIB_CHECK(muted.engine->IsTrackMuted(0, 1) && !muted.engine->IsTrackMuted(0, 0));
    XSFLIB_CHECK(Sounds(heard) && heard == rested && played != rested);
    XSFLIB_CHECK(same_channels);
}

// At 120 beats a minute, a note of 24 ticks with a slow release on a wave of 1 s, then the end. The track ends after
// its note, at 250 ms, and lets the note go. The release sounds on without an owner until the wave ends. Muting the
// track silences the release after a frame's ramp and the start of the next frame. The FIR upsampler takes the first
// 15 samples of that frame's output from the ramp's last inputs. The release goes on. Unmuting brings it back. So does
// a restore of a snapshot taken while the track was muted, once the track is unmuted.
void TestMuteRelease()
{
    const ntdlib::sar::SoundArchive archive =
        ProgramArchive({0xB0, 48, 0xE1, 0x00, 120, 0xD3, 100, 60, 127, 24, 0xFF}, {ConstantPcmWave(48000)});
    snd::ArchiveModel model(archive);
    snd::Engine& engine = *model.engine;
    engine.StartSequence(0);
    const bool sounds = Sounds(RunFrames(model, 134));
    const snd::ChannelManager& channels = engine.Channels();
    const bool released = channels.ActiveCount() == 1 && channels.At(channels.InUse().front()).owner_track == -1 &&
                          channels.At(channels.InUse().front()).source_track == 0;
    engine.MuteTrack(0, 0, true);
    RunFrames(model, 2);

    const snd::ArchiveModel::Snapshot while_muted = model.Save();
    const std::vector<int16_t> muted = RunFrames(model, 30);
    const bool going_on = channels.ActiveCount() == 1;
    engine.MuteTrack(0, 0, false);
    RunFrames(model, 1);
    const std::vector<int16_t> unmuted = RunFrames(model, 1);
    model.Restore(while_muted);
    RunFrames(model, 1);
    const std::vector<int16_t> restored = RunFrames(model, 1);

    XSFLIB_CHECK(sounds && released);
    XSFLIB_CHECK(!Sounds(muted) && going_on);
    XSFLIB_CHECK(Sounds(unmuted) && Sounds(restored));
}

// The mutes stay through a restore, and hold at once after it. One model plays track 1 up to a snapshot while track 1's
// second note sounds (from frame 252, for 14 ms). It then mutes track 1 and restores the snapshot. From there, it
// renders what a model with track 1 muted from the start renders, sample for sample, from the 16th sample on. The FIR
// upsampler's history in the snapshot holds track 1's last inputs, and the first 15 samples of each channel take them
// in. Track 1 sends nothing to the aux buses.
void TestMuteThroughRestore()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel muted(archive, ReverbOptions());
    snd::ArchiveModel playing(archive, ReverbOptions());
    muted.engine->StartSequence(0);
    playing.engine->StartSequence(0);
    muted.engine->MuteTrack(0, 1, true);
    RunFrames(muted, 254);
    RunFrames(playing, 254);
    const snd::ArchiveModel::Snapshot during_note = playing.Save();
    const std::vector<int16_t> heard = RunFrames(playing, 1);
    playing.engine->MuteTrack(0, 1, true);
    playing.Restore(during_note);

    const std::vector<int16_t> restored = RunFrames(playing, 300);
    const std::vector<int16_t> expected = RunFrames(muted, 300);
    constexpr std::ptrdiff_t kHistory = 2 * 15; // stereo samples

    XSFLIB_CHECK(!std::equal(heard.begin(), heard.end(), expected.begin()));
    XSFLIB_CHECK(std::equal(restored.begin() + kHistory, restored.end(), expected.begin() + kHistory, expected.end()));
}

// The peaks of the voices' mix into the TV's main bus in the last frame, left and right.
std::array<int32_t, 2> MainPeaks(const ntdlib::cafe::ax::System& system)
{
    std::array<int32_t, 2> peaks = {0, 0};
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        const auto& samples = system.VoiceBuses().tv[0][channel];
        for (int i = 0; i < system.SamplesPerFrame(); ++i)
        {
            peaks[channel] = std::max(peaks[channel], std::abs(samples[static_cast<std::size_t>(i)]));
        }
    }

    return peaks;
}

// With track 1 muted, the TV's main bus holds track 0's notes alone. Track 0's meter then has the main bus's peaks in
// every frame. Track 1's meter is silent from the mute, and the tracks without notes are silent too. Unmuted, track
// 1's meter shows its notes. The meters change nothing in the output, and a model without meters gives no peaks.
void TestMeterTracks()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel metered(archive);
    snd::ArchiveModel plain(archive);
    snd::ArchiveModel both(archive);
    for (snd::ArchiveModel* model : {&metered, &plain, &both})
    {
        model->engine->StartSequence(0);
    }
    metered.engine->MeterTracks(0);
    metered.engine->MuteTrack(0, 1, true);
    plain.engine->MuteTrack(0, 1, true);
    both.engine->MeterTracks(0);
    bool track_0_is_main = true;
    bool same_output = true;
    bool others_silent = true;
    bool track_1_heard = false;
    bool plain_silent = true;

    for (int frame = 0; frame < 700; ++frame)
    {
        const std::vector<int16_t> out = RunFrames(metered, 1);
        same_output = same_output && out == RunFrames(plain, 1);
        track_0_is_main = track_0_is_main && metered.engine->TrackPeaks(0) == MainPeaks(metered.system);
        for (int track = 1; track < 16; ++track)
        {
            others_silent = others_silent && metered.engine->TrackPeaks(track) == std::array<int32_t, 2>{0, 0};
        }
        plain_silent = plain_silent && plain.engine->TrackPeaks(0) == std::array<int32_t, 2>{0, 0};
        RunFrames(both, 1);
        track_1_heard = track_1_heard || both.engine->TrackPeaks(1) != std::array<int32_t, 2>{0, 0};
    }

    XSFLIB_CHECK(track_0_is_main && Sounds(RunFrames(metered, 1)));
    XSFLIB_CHECK(same_output && others_silent && plain_silent);
    XSFLIB_CHECK(track_1_heard);
}

// With the reverb on aux bus A, track 0's meter leaves out the track's send to bus A and the reverb's output. With
// track 1 muted, the meter's peaks are those of the main bus of the same sequence without the send and the reverb.
void TestMeterLeavesOutEffects()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    const ntdlib::sar::SoundArchive unsent = LoopingArchive(true, 0);
    snd::ArchiveModel reverberant(archive, ReverbOptions());
    snd::ArchiveModel dry(unsent);
    for (snd::ArchiveModel* model : {&reverberant, &dry})
    {
        model->engine->StartSequence(0);
        model->engine->MuteTrack(0, 1, true);
    }
    reverberant.engine->MeterTracks(0);
    bool dry_peaks = true;
    bool reverb_heard = false;

    for (int frame = 0; frame < 700; ++frame)
    {
        const std::vector<int16_t> wet_frame = RunFrames(reverberant, 1);
        const std::vector<int16_t> dry_frame = RunFrames(dry, 1);
        dry_peaks = dry_peaks && reverberant.engine->TrackPeaks(0) == MainPeaks(dry.system);
        reverb_heard = reverb_heard || wet_frame != dry_frame;
    }

    XSFLIB_CHECK(dry_peaks && reverb_heard);
}

// The meters take only the metered player's tracks. With the first player muted and metered, and a second player of
// the same sequence heard, the first player's meters stay silent.
void TestMeterOnePlayer()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel model(archive);
    snd::Engine& engine = *model.engine;
    const int first = engine.StartSequence(0)->Id();
    const bool second = engine.StartSequence(0) != nullptr;
    engine.MuteTrack(first, 0, true);
    engine.MuteTrack(first, 1, true);
    engine.MeterTracks(first);
    bool heard = false;
    bool silent = true;

    for (int frame = 0; frame < 300; ++frame)
    {
        heard = heard || Sounds(RunFrames(model, 1));
        for (int track = 0; track < 16; ++track)
        {
            silent = silent && engine.TrackPeaks(track) == std::array<int32_t, 2>{0, 0};
        }
    }

    XSFLIB_CHECK(second && heard && silent);
}

} // namespace

int main()
{
    TestAxVoice();
    TestWaveBufferQueue();
    TestEngine();
    TestEffectsFor();
    TestEffectParams();
    TestEngineEffects();
    TestSnapshotRenders();
    TestSnapshotEveryFrame();
    TestSnapshotPlayers();
    TestSnapshotOfAnotherModel();
    TestMuteTrack();
    TestMuteRelease();
    TestMuteThroughRestore();
    TestMeterTracks();
    TestMeterLeavesOutEffects();
    TestMeterOnePlayer();

    return xsflib::testing::Summary("cafe_snd");
}
