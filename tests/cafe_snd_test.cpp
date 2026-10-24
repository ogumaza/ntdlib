// SPDX-License-Identifier: MIT

// Checks the Wii U's model of nw::snd: its voices on AX, the engine on a small sound archive, and the effects on the
// aux buses. The tests of the nwsnd module check the arithmetic helpers, the envelope and the LFO.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/channel.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/effects.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/cafe/snd/sound_players.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/testing/sar_files.h"
#include "nwsnd_engine_files.h"

namespace fsar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kCafe, {}, false, ntdlib::sar::Rules::kStopAtFirstProblem};

using ntdlib::cafe::snd::AxVoice;
using ntdlib::cafe::snd::AxVoices;
using ntdlib::cafe::snd::EffectSetup;
using ntdlib::cafe::snd::EffectType;
using ntdlib::cafe::snd::OutputMode;
using ntdlib::cafe::snd::WaveBuffer;
using nwsnd_engine_files::ConstantPcmWave;
using nwsnd_engine_files::ConstantWave;
using nwsnd_engine_files::LoopingAdpcmWave;
using nwsnd_engine_files::PcmStream;
using nwsnd_engine_files::PlainWaveSound;
using nwsnd_engine_files::ProgramArchive;
using nwsnd_engine_files::StreamArchive;
using nwsnd_engine_files::StreamTrack;
using nwsnd_engine_files::WaveSoundArchive;

// The Wii U's files and samples are big-endian.
constexpr bool kBigEndian = true;

void TestAxVoice()
{
    // A voice set up to play a buffer starts its AX voice with the buffer's offsets, the volume, the TV's mix and the
    // ratio. The ratio chooses the 4-tap filter.
    const std::vector<uint8_t> wave = ConstantWave(kBigEndian, 100, 1000);
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
    const std::vector<uint8_t> first_wave = ConstantWave(kBigEndian, 100, 1000);
    const std::vector<uint8_t> second_wave = ConstantWave(kBigEndian, 300, 2000);
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

// The engine plays a sequence from a small archive: at a timebase of 48 and 120 beats a minute, a note of 24 ticks
// (250 ms) on a PCM16 wave of 100 ms, then the end. The note starts in the second frame. The first frame takes the
// start to the sound thread. AX sets the note's voice up at the end of the third frame and plays it from the fourth.
// The sequence ends after its 24 ticks.
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
    const std::vector<EffectSetup> kSetups = {
        {0, 0, EffectType::kReverbStd, {}, {}},
        {0, 0, EffectType::kReverbHi, {}, {5, 6}},
        {0, 1, EffectType::kChorus, {}, {}},
        {1, 0, EffectType::kDelay, {}, {6}},
    };

    const auto types = [&kSetups](uint32_t sound)
    {
        std::vector<EffectType> found;
        for (const EffectSetup& setup : ntdlib::cafe::snd::EffectsFor(kSetups, sound))
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

    return ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, tracks, {LoopingAdpcmWave(), ConstantPcmWave(kBigEndian, 1000, 0x7000)}, 0x3),
        kSarOptions);
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
    return !ErrorOf<std::invalid_argument>([&] { model.Restore(snapshot); }).empty();
}

// After a restore, a model renders what it rendered after the snapshot the first time, sample for sample, and does so
// again from the same snapshot. A player started after the snapshot goes, and so do its notes. A snapshot taken before
// the sound starts holds the start that waits. A later snapshot restored straight after the earliest renders the same
// samples again too.
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
    model.Restore(at_start);
    model.Restore(later);
    const std::vector<int16_t> fourth = RunFrames(model, 1000);

    XSFLIB_CHECK(voices > 0 && Sounds(first));
    XSFLIB_CHECK(second == first);
    XSFLIB_CHECK(third == first);
    XSFLIB_CHECK(again == opening);
    XSFLIB_CHECK(fourth == first);
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
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, {0xB0, 48, 0xE1, 0x00, 120, 0xD3, 100, 60, 127, 24, 0xFF},
                       {ConstantPcmWave(kBigEndian, 48000)}),
        kSarOptions);
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

// An open that the open hook got: the track that ran the command, the track that it opened, and where that track
// starts.
struct Open
{
    bool operator==(const Open&) const = default;

    int track;
    int opened;
    uint32_t offset;
};

// An archive whose track 0 opens track 1 at 0x1D again at the start of each of its passes of 24 ticks. Track 1 plays a
// note of 12 ticks and rests for 48, and never reaches its end. Before its loop, track 0 opens itself and track 2,
// which the sound doesn't allocate. The player ignores both.
ntdlib::sar::SoundArchive ReopeningArchive()
{
    const std::vector<uint8_t> kTracks = {
        0xFE, 0x00, 0x03,             // tracks 0 and 1
        0xB0, 48,   0xE1, 0x00, 120,  // a timebase of 48, 120 beats a minute
        0x88, 0x00, 0x00, 0x00, 0x1D, // track 0 at 0x1D: the track itself
        0x88, 0x02, 0x00, 0x00, 0x1D, // track 2 at 0x1D: not allocated
        0x88, 0x01, 0x00, 0x00, 0x1D, // 0x12: track 1 at 0x1D
        0x80, 24,                     // a rest
        0x89, 0x00, 0x00, 0x12,       // back to 0x12
        60,   127,  12,               // 0x1D: track 1's key 60
        0x80, 48,                     // a rest
        0xFF,                         // the end, which track 1 never reaches
    };

    return ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kTracks, {ConstantPcmWave(kBigEndian, 4800)}, 0x3), kSarOptions);
}

// The open hook gets every open of track 1 by track 0, the first included, without the loop hook. Track 0's passes
// start at ticks 0, 24, 48 and so on, and each opens track 1. Track 0's opens of itself and of track 2 don't reach the
// hook. The hook changes nothing in the output.
void TestOpenTrackHook()
{
    const ntdlib::sar::SoundArchive archive = ReopeningArchive();
    snd::ArchiveModel hooked(archive);
    snd::ArchiveModel plain(archive);
    std::vector<Open> opens;
    const auto record_open = [&opens](int track, int opened, uint32_t offset)
    {
        opens.push_back({track, opened, offset});
    };
    hooked.engine->SetOpenTrackHook(record_open);
    const snd::SequenceSoundPlayer* player = hooked.engine->StartSequence(0);
    plain.engine->StartSequence(0);
    if (!Check(player != nullptr, "the sequence starts"))
    {
        return;
    }

    const std::vector<int16_t> heard = RunFrames(hooked, 600);
    const std::vector<int16_t> expected = RunFrames(plain, 600);

    const uint32_t ticks = player->TickCount();
    const auto of_track_1 = [](const Open& open)
    {
        return open == Open{0, 1, 0x1D};
    };
    Check(ticks >= 48 && opens.size() == (ticks + 23) / 24 && std::all_of(opens.begin(), opens.end(), of_track_1),
          "every open of track 1 reaches the hook: " + std::to_string(opens.size()) + " in " + std::to_string(ticks) +
              " ticks");
    XSFLIB_CHECK(Sounds(heard) && heard == expected);
}

// The archive of a wave sound with the settings of `sound`, at `volume`, that plays a PCM16 wave of 100 ms at 48 kHz.
ntdlib::sar::SoundArchive WaveSoundOf(const fsar_files::WaveSoundSpec& sound, uint8_t volume = 127,
                                      std::optional<uint32_t> channel_priority = std::nullopt)
{
    return ntdlib::sar::SoundArchive::Load(
        WaveSoundArchive(kBigEndian, sound, {ConstantPcmWave(kBigEndian, 4800)}, volume, channel_priority),
        kSarOptions);
}

// The output of a wave sound with the settings of `sound`, at `volume`, a frame at a time, until it ends.
std::vector<std::vector<int16_t>> WaveSoundFrames(const fsar_files::WaveSoundSpec& sound, uint8_t volume = 127)
{
    const ntdlib::sar::SoundArchive archive = WaveSoundOf(sound, volume);
    snd::ArchiveModel model(archive);
    model.engine->StartWaveSound(0);
    std::vector<std::vector<int16_t>> frames;
    while (model.engine->IsBusy() && frames.size() < 600)
    {
        frames.push_back(RunFrames(model, 1));
    }

    return frames;
}

// The largest magnitudes of an output frame's left and right samples.
std::array<int32_t, 2> PeaksOf(std::span<const int16_t> frame)
{
    std::array<int32_t, 2> peaks = {0, 0};
    for (std::size_t t = 0; t < frame.size(); ++t)
    {
        peaks[t % 2] = std::max(peaks[t % 2], std::abs(static_cast<int32_t>(frame[t])));
    }

    return peaks;
}

// Whether every frame is silent on the side other than `side`, and some frame sounds on `side`.
bool OnOneSide(const std::vector<std::vector<int16_t>>& frames, std::size_t side)
{
    bool heard = false;
    for (const std::vector<int16_t>& frame : frames)
    {
        const std::array<int32_t, 2> peaks = PeaksOf(frame);
        if (peaks[1 - side] != 0)
        {
            return false;
        }
        heard = heard || peaks[side] != 0;
    }

    return heard;
}

// Whether any frame sounds.
bool AnySound(const std::vector<std::vector<int16_t>>& frames)
{
    return std::any_of(frames.begin(), frames.end(), [](const std::vector<int16_t>& frame) { return Sounds(frame); });
}

// The steady level of a wave sound's or a stream's output: the peaks of its left and right samples in frame 12.
std::array<int32_t, 2> SteadyPeaks(const std::vector<std::vector<int16_t>>& frames)
{
    return frames.size() > 12 ? PeaksOf(frames[12]) : std::array<int32_t, 2>{0, 0};
}

// Whether `level` is `expected`'s level times `ratio`, within 1 %, on both sides.
bool LevelIs(const std::array<int32_t, 2>& level, const std::array<int32_t, 2>& expected, double ratio)
{
    for (std::size_t side = 0; side < 2; ++side)
    {
        if (std::abs(level[side] - expected[side] * ratio) > expected[side] * 0.01)
        {
            return false;
        }
    }

    return expected[0] > 0 && expected[1] > 0;
}

// A wave sound plays the wave of its first note once on a channel, at the sound's channel priority plus 64. The first
// frame takes the start to the sound thread. The channel starts in the second, and AX plays it from the fourth. The
// sound ends with the wave, after 100 ms, and frees the channel. A mute of track 0 mutes the channel. A wave sound
// without notes has no wave to play, and ends in the frame that it starts in.
void TestWaveSound()
{
    const ntdlib::sar::SoundArchive archive = WaveSoundOf(PlainWaveSound(), 127, 20);
    snd::ArchiveModel model(archive);
    snd::ArchiveModel muted(archive);
    snd::Engine& engine = *model.engine;
    const snd::WaveSoundPlayer* player = engine.StartWaveSound(0);
    muted.engine->StartWaveSound(0);
    muted.engine->MuteTrack(0, 0, true);
    XSFLIB_CHECK(engine.StartWaveSound(1) == nullptr && engine.StartSequence(0) == nullptr);
    if (!Check(player != nullptr, "the wave sound starts"))
    {
        return;
    }

    engine.RunFrame();
    const bool none_at_first = engine.Channels().ActiveCount() == 0;
    engine.RunFrame();
    const bool channel_next = engine.Channels().ActiveCount() == 1;
    const bool priority = player->ChannelPlace() != ntdlib::nwsnd::kNoChannel &&
                          engine.Channels().At(player->ChannelPlace()).Voice(engine).Priority() == 84;
    engine.RunFrame();
    const bool silent_third = !Sounds(model.system.Output());
    engine.RunFrame();
    const bool sounds_fourth = Sounds(model.system.Output());
    int frames = 4;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }
    const std::vector<int16_t> heard = RunFrames(muted, frames);

    XSFLIB_CHECK(none_at_first && channel_next && priority);
    XSFLIB_CHECK(silent_third && sounds_fourth);
    Check(!engine.IsBusy() && frames >= 36 && frames <= 40, "the wave sound ends after " + std::to_string(frames));
    XSFLIB_CHECK(player->IsFinished() && engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
    XSFLIB_CHECK(!Sounds(heard) && !muted.engine->IsBusy());

    const ntdlib::sar::SoundArchive empty = WaveSoundOf({});
    snd::ArchiveModel without_notes(empty);
    XSFLIB_CHECK(without_notes.engine->StartWaveSound(0) != nullptr);
    const std::vector<int16_t> nothing = RunFrames(without_notes, 2);
    XSFLIB_CHECK(!Sounds(nothing) && !without_notes.engine->IsBusy());
}

// The wave sound's settings reach its channel, and its note's settings don't: a note at another original key, with a
// volume, a pan, a pitch and an envelope, plays as the plain note does. A pan of 0 puts the sound on the left alone,
// and 127 on the right alone. A pitch of 2 ends the sound in half the time. A main send of 0 silences it, and a
// low-pass filter changes it. The sound's volume of 64 gives 64/127 of the level. A slow attack starts the sound
// quieter, and its level rises.
void TestWaveSoundSettings()
{
    const std::vector<std::vector<int16_t>> plain = WaveSoundFrames(PlainWaveSound());
    fsar_files::WaveSoundSpec note = PlainWaveSound();
    note.notes[0].original_key = 72;
    note.notes[0].volume = 30;
    note.notes[0].pan = 0;
    note.notes[0].pitch = 2.0f;
    note.notes[0].adshr = std::array<uint8_t, 5>{20, 20, 20, 20, 20};
    fsar_files::WaveSoundSpec left = PlainWaveSound();
    left.pan = 0;
    fsar_files::WaveSoundSpec right = PlainWaveSound();
    right.pan = 127;
    fsar_files::WaveSoundSpec fast = PlainWaveSound();
    fast.pitch = 2.0f;
    fsar_files::WaveSoundSpec unsent = PlainWaveSound();
    unsent.sends = {0, 3, 0, 0, 0};
    fsar_files::WaveSoundSpec filtered = PlainWaveSound();
    filtered.filter = 10;
    fsar_files::WaveSoundSpec slow = PlainWaveSound();
    slow.adshr = std::array<uint8_t, 5>{100, 127, 127, 127, 127};
    const std::vector<std::vector<int16_t>> faster = WaveSoundFrames(fast);
    const std::vector<std::vector<int16_t>> slower = WaveSoundFrames(slow);

    XSFLIB_CHECK(plain.size() > 12 && WaveSoundFrames(note) == plain);
    XSFLIB_CHECK(OnOneSide(WaveSoundFrames(left), 0) && OnOneSide(WaveSoundFrames(right), 1));
    Check(faster.size() + 14 <= plain.size() && faster.size() + 20 >= plain.size(),
          "a pitch of 2 plays for " + std::to_string(faster.size()) + " frames, the plain sound for " +
              std::to_string(plain.size()));
    XSFLIB_CHECK(!AnySound(WaveSoundFrames(unsent)) && AnySound(plain));
    XSFLIB_CHECK(WaveSoundFrames(filtered) != plain);
    XSFLIB_CHECK(LevelIs(SteadyPeaks(WaveSoundFrames(PlainWaveSound(), 64)), SteadyPeaks(plain), 64.0 / 127.0));
    XSFLIB_CHECK(slower.size() > 12 && PeaksOf(slower[3])[0] * 10 < PeaksOf(plain[3])[0] &&
                 PeaksOf(slower[12])[0] > PeaksOf(slower[6])[0] && PeaksOf(slower[6])[0] > PeaksOf(slower[3])[0]);
}

// The archive of a stream sound with `tracks`, those of the bits of `valid` playing, at `volume`.
ntdlib::sar::SoundArchive StreamSoundOf(std::vector<ntdlib::sar::StreamTrackInfo> tracks, uint16_t valid,
                                        uint8_t volume = 127)
{
    ntdlib::sar::StreamSoundInfo stream;
    stream.valid_tracks = valid;
    stream.tracks = std::move(tracks);

    return ntdlib::sar::SoundArchive::Load(StreamArchive(kBigEndian, stream, volume), kSarOptions);
}

// The output of the archive's stream sound with the stream file `file`, a frame at a time, until it ends or for
// `limit` frames.
std::vector<std::vector<int16_t>> StreamFrames(const ntdlib::sar::SoundArchive& archive,
                                               const std::vector<uint8_t>& file, std::size_t limit = 600)
{
    snd::ArchiveModel model(archive);
    model.engine->StartStreamSound(0, file);
    std::vector<std::vector<int16_t>> frames;
    while (model.engine->IsBusy() && frames.size() < limit)
    {
        frames.push_back(RunFrames(model, 1));
    }

    return frames;
}

// A mono stream of 200 ms plays on a voice at priority 255, from the fourth frame, and ends with its samples. The
// voice and the player then go. Bytes that aren't a stream file throw.
void TestStreamSound()
{
    const std::vector<uint8_t> file = PcmStream(ntdlib::sar::Flavor::kCafe, {std::vector<int16_t>(9600, 0x1000)});
    const ntdlib::sar::SoundArchive archive = StreamSoundOf({StreamTrack({0})}, 1);
    snd::ArchiveModel model(archive);
    snd::Engine& engine = *model.engine;
    const snd::StreamSoundPlayer* player = engine.StartStreamSound(0, file);
    if (!Check(player != nullptr && player->ChannelCount() == 1, "the stream sound starts"))
    {
        return;
    }

    engine.RunFrame();
    const bool none_at_first = engine.Voices().ActiveCount() == 0;
    engine.RunFrame();
    const ntdlib::nwsnd::VoiceId voice = player->ChannelVoice(0);
    const bool voice_next = engine.Voices().ActiveCount() == 1 && voice != ntdlib::nwsnd::kNoVoice &&
                            engine.Voices().At(voice).Priority() == 255;
    engine.RunFrame();
    const bool silent_third = !Sounds(model.system.Output());
    engine.RunFrame();
    const bool sounds_fourth = Sounds(model.system.Output());
    int frames = 4;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }

    const bool refused =
        !ErrorOf<xsflib::FormatError>([&] { engine.StartStreamSound(0, std::vector<uint8_t>(64)); }).empty();

    XSFLIB_CHECK(none_at_first && voice_next);
    XSFLIB_CHECK(silent_third && sounds_fourth);
    Check(!engine.IsBusy() && frames >= 69 && frames <= 73, "the stream sound ends after " + std::to_string(frames));
    XSFLIB_CHECK(player->IsFinished() && engine.Voices().ActiveCount() == 0);
    XSFLIB_CHECK(refused && engine.StartWaveSound(0) == nullptr);
    XSFLIB_CHECK(StreamFrames(StreamSoundOf({StreamTrack({0})}, 0), file).empty());
}

// A track of two channels pans the first fully left and the second fully right, by the TV's mix mode. A track's volume
// of 64 gives 64/127 of the level. A track that doesn't play leaves its channel silent. A stream that loops plays on.
// In a file of version 2.0.0 or earlier, the file's tracks take the place of the archive's: one at volume 0 silences
// the stream. A later file's tracks don't count.
void TestStreamTracks()
{
    constexpr auto kCafe = ntdlib::sar::Flavor::kCafe;
    const std::vector<int16_t> kLoud(9600, 0x1000);
    const std::vector<int16_t> kSilent(9600, 0);
    const std::vector<uint8_t> left_only = PcmStream(kCafe, {kLoud, kSilent});
    const std::vector<uint8_t> right_only = PcmStream(kCafe, {kSilent, kLoud});
    const ntdlib::sar::SoundArchive pair = StreamSoundOf({StreamTrack({0, 1})}, 1);
    const ntdlib::sar::SoundArchive two = StreamSoundOf({StreamTrack({0}), StreamTrack({1})}, 2);
    const ntdlib::sar::SoundArchive both = StreamSoundOf({StreamTrack({0}), StreamTrack({1})}, 3);
    ntdlib::sar::StreamTrackInfo quiet = StreamTrack({0});
    quiet.volume = 64;
    const std::vector<uint8_t> mono = PcmStream(kCafe, {kLoud});
    const ntdlib::sar::SoundArchive single = StreamSoundOf({StreamTrack({0})}, 1);
    const std::vector<std::vector<int16_t>> full = StreamFrames(single, mono);
    const std::vector<uint8_t> looped = PcmStream(kCafe, {std::vector<int16_t>(4800, 0x1000)}, 0);
    const std::vector<ntdlib::sar::StreamFile::Track> kMuted = {{0, 64, 0, 0, {0}}};
    const std::vector<uint8_t> old_file = PcmStream(kCafe, {kLoud}, std::nullopt, kMuted, 0x00020000);
    const std::vector<uint8_t> new_file = PcmStream(kCafe, {kLoud}, std::nullopt, kMuted, 0x00030000);
    const std::vector<std::vector<int16_t>> looping = StreamFrames(single, looped, 300);

    XSFLIB_CHECK(OnOneSide(StreamFrames(pair, left_only), 0) && OnOneSide(StreamFrames(pair, right_only), 1));
    XSFLIB_CHECK(LevelIs(SteadyPeaks(StreamFrames(StreamSoundOf({quiet}, 1), mono)), SteadyPeaks(full), 64.0 / 127.0));
    XSFLIB_CHECK(!AnySound(StreamFrames(two, left_only)) && AnySound(StreamFrames(two, right_only)) &&
                 AnySound(StreamFrames(both, left_only)));
    XSFLIB_CHECK(looping.size() == 300 && Sounds(looping.back()));
    XSFLIB_CHECK(!AnySound(StreamFrames(single, old_file)) && StreamFrames(single, new_file) == full);
}

// A snapshot taken while a stream plays renders the same frames again after a restore. A stream sound of an info
// outside the archive plays as the archive's does. The engine takes its streams out of AX's memory when it goes.
void TestStreamSnapshot()
{
    const std::vector<int16_t> samples = []
    {
        std::vector<int16_t> ramp(24000);
        for (std::size_t i = 0; i < ramp.size(); ++i)
        {
            ramp[i] = static_cast<int16_t>(static_cast<int>(i % 400) * 40 - 8000);
        }

        return ramp;
    }();
    const std::vector<uint8_t> file = PcmStream(ntdlib::sar::Flavor::kCafe, {samples, samples}, 4000);
    const ntdlib::sar::SoundArchive archive = StreamSoundOf({StreamTrack({0, 1})}, 1);
    snd::ArchiveModel model(archive);
    model.engine->StartStreamSound(0, file);
    RunFrames(model, 30);
    const snd::ArchiveModel::Snapshot snapshot = model.Save();
    const std::vector<int16_t> first = RunFrames(model, 300);
    model.Restore(snapshot);
    const std::vector<int16_t> second = RunFrames(model, 300);

    snd::ArchiveModel outside(archive);
    outside.engine->StartStreamSound(archive.Sounds()[0], file);
    snd::ArchiveModel inside(archive);
    inside.engine->StartStreamSound(0, file);
    // The stream's first channel starts at 0x10000000, the first address that the engine maps.
    const bool mapped = model.memory.Read8(0x10000000) != 0;
    model.engine.reset();

    XSFLIB_CHECK(Sounds(first) && second == first);
    XSFLIB_CHECK(RunFrames(outside, 100) == RunFrames(inside, 100));
    XSFLIB_CHECK(mapped && model.memory.Read8(0x10000000) == 0);
}

// A region of AX's memory reads as its bytes until it's unmapped. It then reads as 0, and the other regions stay.
void TestMemoryUnmap()
{
    const std::vector<uint8_t> kFirst = {1, 2, 3, 4};
    const std::vector<uint8_t> kSecond = {5, 6, 7, 8};
    ntdlib::cafe::ax::Memory memory;
    memory.Map(0x10000000, kFirst);
    memory.Map(0x10000100, kSecond);
    const bool mapped = memory.Read8(0x10000002) == 3 && memory.Read8(0x10000101) == 6;
    memory.Unmap(0x10000000);

    XSFLIB_CHECK(mapped);
    XSFLIB_CHECK(memory.Read8(0x10000002) == 0 && memory.Read8(0x10000101) == 6);
}

// A place that a restore empties can take a player of another kind: here a stream sound outside the archive. A restore
// of a snapshot that holds a sequence there puts the sequence back, with its start that waits.
void TestSnapshotPlayersOfOtherKinds()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    snd::ArchiveModel model(archive, ReverbOptions());
    model.engine->StartSequence(0);
    RunFrames(model, 50);
    const snd::ArchiveModel::Snapshot one_player = model.Save();
    model.engine->StartSequence(0);
    const snd::ArchiveModel::Snapshot start_waits = model.Save();
    const std::vector<int16_t> first = RunFrames(model, 100);

    model.Restore(one_player);
    ntdlib::sar::SoundInfo info;
    info.type = ntdlib::sar::SoundType::kStream;
    info.stream.valid_tracks = 1;
    info.stream.tracks = {StreamTrack({0})};
    const snd::StreamSoundPlayer* stream = model.engine->StartStreamSound(
        info, PcmStream(ntdlib::sar::Flavor::kCafe, {std::vector<int16_t>(9600, 0x1000)}));
    RunFrames(model, 20);
    model.Restore(start_waits);
    const std::vector<int16_t> second = RunFrames(model, 100);

    XSFLIB_CHECK(stream != nullptr && stream->Id() == 1);
    XSFLIB_CHECK(Sounds(first) && second == first);
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
    TestOpenTrackHook();
    TestWaveSound();
    TestWaveSoundSettings();
    TestStreamSound();
    TestStreamTracks();
    TestStreamSnapshot();
    TestSnapshotPlayersOfOtherKinds();
    TestMemoryUnmap();

    return xsflib::testing::Summary("cafe_snd");
}
