// SPDX-License-Identifier: MIT

// Checks the Wii U's model of nw::snd: its voices on AX, the engine on a small sound archive, the effects on the aux
// buses, the snapshots, and the host's mutes, meters and open hook. cafe_snd_sounds_test checks the wave sounds and the
// stream sounds. The tests of the nwsnd module check the arithmetic helpers, the envelope and the LFO.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "cafe_snd_test_util.h"
#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/channel.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/effects.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "nwsnd_engine_files.h"

namespace fsar_files = ntdlib::testing::sar_files;
namespace snd = ntdlib::cafe::snd;

using cafe_snd_tests::AxVoice;
using cafe_snd_tests::AxVoices;
using cafe_snd_tests::Bits;
using cafe_snd_tests::ConstantPcmWave;
using cafe_snd_tests::ConstantWave;
using cafe_snd_tests::EffectSetup;
using cafe_snd_tests::EffectType;
using cafe_snd_tests::kBigEndian;
using cafe_snd_tests::kSarOptions;
using cafe_snd_tests::LoopingAdpcmWave;
using cafe_snd_tests::LoopingArchive;
using cafe_snd_tests::OutputMode;
using cafe_snd_tests::PcmStream;
using cafe_snd_tests::PlainWaveSound;
using cafe_snd_tests::ProgramArchive;
using cafe_snd_tests::ReverbOptions;
using cafe_snd_tests::RunFrames;
using cafe_snd_tests::Sounds;
using cafe_snd_tests::StreamArchive;
using cafe_snd_tests::StreamTrack;
using cafe_snd_tests::WaveBuffer;
using cafe_snd_tests::WaveSoundArchive;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

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
    // A voice moves on to the buffer queued behind the one it's playing. The queued buffer is past the first buffer's
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

// Nintendo Land's delay on the TV's aux A (nintendo_land::Delay) at one sample, without feedback and at full level. The
// threaded path puts each frame out 3 frames late. AX returns the bus 2 frames later.
void TestNintendoLandEffects()
{
    const ntdlib::sar::SoundArchive archive = NoteArchive({0xD9, 127, 0xB0, 48, 0xE1, 0x00, 120, 60, 127, 24, 0xFF});
    const EffectSetup nintendo_land{ntdlib::cafe::snd::kEffectDeviceTv,
                                    0,
                                    EffectType::kNintendoLandDelay,
                                    {Bits(0.04f), Bits(0.0f), Bits(1.0f), Bits(1.0f), 1, 0x01000000, 8, 4},
                                    {}};
    const EffectSetup nw_snd{ntdlib::cafe::snd::kEffectDeviceTv,
                             0,
                             EffectType::kDelay,
                             {Bits(30.0f), Bits(0.0f), Bits(1.0f), Bits(50.0f), Bits(1.0f)},
                             {}};

    // The frames from the first with sound to the first in which `effect` changes the TV's output, or -1.
    const auto lag = [&archive](const EffectSetup& effect)
    {
        ntdlib::cafe::snd::EngineOptions options;
        ntdlib::cafe::snd::ArchiveModel dry(archive, options);
        options.effects = {effect};
        ntdlib::cafe::snd::ArchiveModel wet(archive, options);
        if (!dry.engine->StartSequence(0) || !wet.engine->StartSequence(0))
        {
            return -1;
        }

        int first_sound = -1;
        for (int frame = 0; frame < 100; ++frame)
        {
            dry.engine->RunFrame();
            wet.engine->RunFrame();
            const std::span<const int16_t> a = dry.system.Output();
            const std::span<const int16_t> b = wet.system.Output();
            if (first_sound < 0 && xsflib::rip::Audible(a))
            {
                first_sound = frame;
            }
            if (!std::equal(a.begin(), a.end(), b.begin(), b.end()))
            {
                return first_sound < 0 ? -1 : frame - first_sound;
            }
        }

        return -1;
    };

    XSFLIB_CHECK(lag(nintendo_land) == 2 + 3);
    XSFLIB_CHECK(lag(nw_snd) == 2 + 10);
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

// The output of the next `frames` frames: the 16-bit samples, and in the enhanced mode the floating-point ones too.
struct Rendered
{
    friend bool operator==(const Rendered&, const Rendered&) = default;

    std::vector<int16_t> samples;
    std::vector<float> floats;
};

Rendered Render(snd::ArchiveModel& model, int frames)
{
    Rendered rendered;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.system.Output();
        const std::span<const float> floats = model.system.OutputFloat();
        rendered.samples.insert(rendered.samples.end(), frame.begin(), frame.end());
        rendered.floats.insert(rendered.floats.end(), floats.begin(), floats.end());
    }

    return rendered;
}

// `first` followed by `second`.
Rendered Joined(Rendered first, const Rendered& second)
{
    first.samples.insert(first.samples.end(), second.samples.begin(), second.samples.end());
    first.floats.insert(first.floats.end(), second.floats.begin(), second.floats.end());

    return first;
}

// A snapshot taken while notes sound, and restored after later frames, renders what a model that never took a snapshot
// renders, sample for sample. That holds with the reverb on aux A, in the console's mixing and in the enhanced mixing
// mode, and at the tempo ratio and the pitch that the sequence got as it started. A change of the ratio and the pitch
// after the snapshot goes with the restore. A sequence at 1 and 1 renders other samples.
void TestSnapshotAgainstPlainRun()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    constexpr float kRatio = 200.0f / 120.0f;
    constexpr float kPitch = 1.587401f;
    for (const bool enhanced : {false, true})
    {
        snd::EngineOptions options = ReverbOptions();
        options.enhanced = enhanced;
        const std::string mode = enhanced ? "in the enhanced mode" : "in the console's mixing";
        const auto start = [](snd::ArchiveModel& model, float ratio, float pitch)
        {
            snd::SequenceSoundPlayer* player = model.engine->StartSequence(0);
            player->SetTempoRatio(ratio);
            player->SetPitch(pitch);

            return player;
        };

        snd::ArchiveModel plain(archive, options);
        start(plain, kRatio, kPitch);
        const Rendered expected = Render(plain, 1300);

        snd::ArchiveModel restored(archive, options);
        snd::SequenceSoundPlayer* player = start(restored, kRatio, kPitch);
        const Rendered opening = Render(restored, 300);
        const int voices = restored.engine->Voices().ActiveCount();
        const snd::ArchiveModel::Snapshot snapshot = restored.Save();
        Render(restored, 200);
        player->SetTempoRatio(1.0f);
        player->SetPitch(1.0f);
        Render(restored, 200);
        restored.Restore(snapshot);
        const Rendered after = Joined(opening, Render(restored, 1000));

        snd::ArchiveModel at_one(archive, options);
        start(at_one, 1.0f, 1.0f);
        const Rendered plain_at_one = Render(at_one, 1300);

        Check(voices > 0 && Sounds(expected.samples) && expected.floats.empty() == !enhanced,
              "notes sound at the snapshot " + mode);
        Check(after == expected, "a restored snapshot renders the run without one " + mode);
        Check(plain_at_one.samples != expected.samples, "the tempo ratio and the pitch change the output " + mode);
    }
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
// note of 12 ticks and rests for 48, and never reaches its end. Before its loop, track 0 opens itself and track 2. The
// sound doesn't allocate track 2. The player ignores both.
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
        0xFF,                         // the end; track 1 never reaches it
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

} // namespace

int main()
{
    TestAxVoice();
    TestWaveBufferQueue();
    TestEngine();
    TestEffectsFor();
    TestEffectParams();
    TestEngineEffects();
    TestNintendoLandEffects();
    TestSnapshotRenders();
    TestSnapshotEveryFrame();
    TestSnapshotPlayers();
    TestSnapshotOfAnotherModel();
    TestSnapshotAgainstPlainRun();
    TestMuteTrack();
    TestMuteRelease();
    TestMuteThroughRestore();
    TestMeterTracks();
    TestMeterLeavesOutEffects();
    TestMeterOnePlayer();
    TestOpenTrackHook();

    return xsflib::testing::Summary("cafe_snd");
}
