// SPDX-License-Identifier: MIT

// Checks the model of nn::atk's voices, channels and sequence player on the model of the renderer, without game data: a
// low-level voice on the renderer, its queue of wave buffers, and small archives that play notes through the engine,
// with effects on aux bus A, and an add-on archive beside the engine's archive. nx_atk_engine_sounds_test checks the
// wave sounds, the stream sounds and the enhanced mixing mode.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/atk/channel.h"
#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/hardware.h"
#include "ntdlib/nx/atk/low_level_voice.h"
#include "ntdlib/nx/atk/sequence.h"
#include "ntdlib/nx/atk/util.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/sar/formats.h"
#include "nwsnd_engine_files.h"
#include "nx_atk_engine_test_util.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;
using nx_atk_engine_tests::ConstantPcmWave;
using nx_atk_engine_tests::ConstantWave;
using nx_atk_engine_tests::kBigEndian;
using nx_atk_engine_tests::kSarOptions;
using nx_atk_engine_tests::kStageReverb;
using nx_atk_engine_tests::LoopingAdpcmWave;
using nx_atk_engine_tests::LoopingArchive;
using nx_atk_engine_tests::PcmStream;
using nx_atk_engine_tests::PeaksOf;
using nx_atk_engine_tests::PlainWaveSound;
using nx_atk_engine_tests::ProgramArchive;
using nx_atk_engine_tests::RunFrames;
using nx_atk_engine_tests::Sounds;
using nx_atk_engine_tests::StageReverbOptions;
using nx_atk_engine_tests::StreamArchive;
using nx_atk_engine_tests::StreamTrack;
using nx_atk_engine_tests::WaveSoundArchive;

namespace
{

namespace atk = ntdlib::nx::atk;
namespace audio = ntdlib::nx::audio;

// A renderer at 48 kHz with a final mix of six channels, whose front left and right the device sink takes, and the
// low-level voices on it.
struct Stage
{
    Stage() : renderer(Parameter()), voices(renderer, hardware, 4)
    {
        renderer.AcquireFinalMix(6);
        constexpr std::array<int, 2> kInputs = {0, 1};
        renderer.AddDeviceSink(kInputs);
        receiver.mix = audio::Renderer::kFinalMix;
        receiver.channel_count = 6;
        receiver.bus_count = 1;
        receiver.clamp_send = {true, true, true, true};
    }

    static audio::RendererParameter Parameter()
    {
        audio::RendererParameter parameter;
        parameter.mix_buffer_count = 6;
        parameter.voice_count = 4;

        return parameter;
    }

    // A frame: the voices' statuses, their parameters and states, then the renderer's frame.
    void Frame()
    {
        voices.UpdateAllVoiceStatus();
        voices.UpdateAllVoice(atk::OutputMode::kStereo);
        renderer.RenderFrame();
    }

    atk::HardwareSettings hardware;
    audio::Renderer renderer;
    atk::LowLevelVoiceAllocator voices;
    atk::OutputReceiver receiver;
};

void TestLowLevelVoice()
{
    // A voice that starts to play acquires a voice of the renderer. The voice starts the renderer's voice with its
    // first buffer and gives the renderer's voice the volume and the gains. The first frame ramps the volume up from
    // silence, and the second has it. A constant comes out of the medium resampler at ratio 1 as 9959 in 10000.
    Stage stage;
    atk::LowLevelVoice* voice = stage.voices.AllocVoice();
    if (!Check(voice != nullptr, "a low-level voice is allocated"))
    {
        return;
    }

    const std::vector<uint8_t> wave = ConstantWave(kBigEndian, 4800, 10000);
    atk::WaveBuffer buffer;
    buffer.data = wave;
    buffer.sample_length = 4800;
    buffer.loop = true;
    voice->SetWave(atk::SampleFormat::kPcm16, 48000, {}, &stage.receiver);
    voice->Param().volume = 0.5f;
    voice->Param().mix[0] = 1.0f;
    voice->Param().mix[1] = 0.25f;
    voice->AppendWaveBuffer(buffer);
    voice->SetState(atk::LowLevelVoice::State::kPlay);

    stage.Frame();
    const bool started = voice->Slot() >= 0;
    stage.Frame();

    XSFLIB_CHECK(started && buffer.status == atk::WaveBuffer::Status::kPlay);
    XSFLIB_CHECK(stage.renderer.GetVoicePlayState(voice->Slot()) == audio::PlayState::kStarted);
    const std::span<const int16_t> output = stage.renderer.DeviceOutput();
    XSFLIB_CHECK(output.size() == 480 && output[0] == 4980 && output[1] == 1245 && output[478] == 4980);

    // Stopping releases the renderer's voice at once, and the buffer is done.
    voice->FreeAllWaveBuffer();
    voice->SetState(atk::LowLevelVoice::State::kStop);
    XSFLIB_CHECK(voice->Slot() < 0 && buffer.status == atk::WaveBuffer::Status::kDone);
}

void TestWaveBufferQueue()
{
    // A voice gives the renderer one buffer ahead of the one that plays, one a frame. So a first buffer shorter than a
    // frame leaves a gap before the second buffer. The second buffer arrives with the next frame. The renderer's report
    // that it has finished with a buffer reaches the voice two frames later, one buffer a frame. After the last buffer,
    // the voice stops the renderer's voice.
    Stage stage;
    atk::LowLevelVoice* voice = stage.voices.AllocVoice();
    if (!Check(voice != nullptr, "a low-level voice is allocated"))
    {
        return;
    }

    std::vector<uint8_t> wave = ConstantWave(kBigEndian, 100, 1000);
    const std::vector<uint8_t> rest = ConstantWave(kBigEndian, 300, 2000);
    wave.insert(wave.end(), rest.begin(), rest.end());
    atk::WaveBuffer first;
    first.data = wave;
    first.sample_length = 100;
    atk::WaveBuffer second;
    second.data = wave;
    second.sample_offset = 100;
    second.sample_length = 400;
    voice->SetWave(atk::SampleFormat::kPcm16, 48000, {}, &stage.receiver);
    voice->Param().mix[0] = 1.0f;
    voice->AppendWaveBuffer(first);
    voice->AppendWaveBuffer(second);
    voice->SetState(atk::LowLevelVoice::State::kPlay);

    stage.Frame();
    const std::vector<int16_t> frame1(stage.renderer.DeviceOutput().begin(), stage.renderer.DeviceOutput().end());
    stage.Frame();
    const std::vector<int16_t> frame2(stage.renderer.DeviceOutput().begin(), stage.renderer.DeviceOutput().end());
    stage.Frame();
    stage.Frame();
    const bool moved_on =
        first.status == atk::WaveBuffer::Status::kDone && second.status == atk::WaveBuffer::Status::kPlay;
    stage.Frame();
    stage.Frame();

    XSFLIB_CHECK(frame1[2 * 50] == 995 && frame1[2 * 150] == 0 && frame1[2 * 239] == 0);
    XSFLIB_CHECK(frame2[2 * 100] == 1991 && frame2[2 * 239] == 1991);
    XSFLIB_CHECK(moved_on);
    XSFLIB_CHECK(second.status == atk::WaveBuffer::Status::kDone);
    XSFLIB_CHECK(stage.renderer.GetVoicePlayState(voice->Slot()) == audio::PlayState::kStopped);
}

void TestEngine()
{
    // At a timebase of 48 and 120 beats a minute, a note of 24 ticks (250 ms) on a wave of 100 ms, then the end. The
    // first frame takes the start to the sound thread. The note starts in the second, and its voice sounds in the same
    // frame. The wave ends after 100 ms, and the sequence after its 24 ticks.
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kBytecode, {ConstantPcmWave(kBigEndian, 4800)}), kSarOptions);
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    XSFLIB_CHECK(engine.StartSequence(0) != nullptr);
    XSFLIB_CHECK(engine.StartSequence(1) == nullptr);

    engine.RunFrame();
    const bool silent_at_first = engine.Channels().ActiveCount() == 0 && !Sounds(model.renderer.DeviceOutput());
    engine.RunFrame();
    const bool sounds_next = engine.Channels().ActiveCount() == 1 && Sounds(model.renderer.DeviceOutput());
    int frames = 2;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }

    XSFLIB_CHECK(silent_at_first);
    XSFLIB_CHECK(sounds_next);
    Check(!engine.IsBusy() && frames >= 50 && frames <= 54, "the sequence ends after " + std::to_string(frames));
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
    XSFLIB_CHECK(engine.LowLevelVoices().AllocatedCount() == 0);
}

void TestNoteOffBeforeUpdate()
{
    // At the fastest tempo, a note of one tick ends in the frame it starts in, before the channel's first update. The
    // release waits for that update's envelope step. The note therefore sounds for a frame. Released at once, as on the
    // Wii U, it would be silent.
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 0xFF, 0x03, 60, 127, 1, 0x80, 48, 0xFF};
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kBytecode, {ConstantPcmWave(kBigEndian, 4800)}), kSarOptions);
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    engine.StartSequence(0);

    engine.RunFrame();
    engine.RunFrame();
    const bool sounds = Sounds(model.renderer.DeviceOutput());
    engine.RunFrame();
    engine.RunFrame();

    XSFLIB_CHECK(sounds);
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0);
}

// The output of `frames` frames of the archive's sound.
std::vector<int16_t> Render(const std::vector<uint8_t>& bytecode, int frames, const atk::EngineOptions& options = {})
{
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, bytecode, {ConstantPcmWave(kBigEndian, 48000)}), kSarOptions);
    atk::ArchiveModel model(archive, options);
    model.engine->StartSequence(0);

    return RunFrames(model, frames);
}

void TestAuxSend()
{
    // A note sent only to aux bus A, with no effect on it, reaches the output as a note sent only to the main bus does.
    const std::vector<uint8_t> kMain = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const std::vector<uint8_t> kAux = {0xB0, 48, 0xE1, 120, 0x00, 0xDB, 0, 0xD9, 127, 60, 127, 24, 0xFF};

    const std::vector<int16_t> through_main = Render(kMain, 20);
    const std::vector<int16_t> through_aux = Render(kAux, 20);

    XSFLIB_CHECK(Sounds(through_main) && through_aux == through_main);
}

void TestAuxReverb()
{
    // A note sent only to aux bus A. An I3DL2 reverb there whose reflections and late reverb are silent, at a dry gain
    // of 1, passes it through 4 frames late, the ring's round trip. The reverb the reference game's stages use goes on
    // after the note has ended.
    const std::vector<uint8_t> kAux = {0xB0, 48, 0xE1, 120, 0x00, 0xDB, 0, 0xD9, 127, 60, 127, 24, 0xFF};
    ntdlib::nx::audio::I3dl2ReverbParameterSet silent;
    silent.room_gain = -10000.0f;
    silent.reflections_gain = -10000.0f;
    silent.reverb_gain = -10000.0f;
    silent.late_reverb_decay_time = 1.0f;
    silent.late_reverb_hf_decay_ratio = 0.5f;
    silent.hf_reference = 5000.0f;
    silent.dry_gain = 1.0f;
    atk::EngineOptions transparent;
    transparent.aux_reverbs[0] = silent;
    atk::EngineOptions stage;
    stage.aux_reverbs[0] = kStageReverb;
    constexpr std::size_t kRoundTrip = 4 * 240 * 2;

    const std::vector<int16_t> dry = Render(kAux, 200);
    const std::vector<int16_t> delayed = Render(kAux, 200, transparent);
    const std::vector<int16_t> reverberant = Render(kAux, 200, stage);

    const auto last = std::find_if(dry.rbegin(), dry.rend(), [](int16_t s) { return s != 0; });
    const std::size_t end = static_cast<std::size_t>(dry.rend() - last);
    XSFLIB_CHECK(Sounds(dry) && end + kRoundTrip < dry.size());
    XSFLIB_CHECK(!Sounds(std::span(delayed).first(kRoundTrip)));
    XSFLIB_CHECK(std::equal(dry.begin(), dry.end() - kRoundTrip, delayed.begin() + kRoundTrip));
    XSFLIB_CHECK(Sounds(std::span(reverberant).subspan(end + kRoundTrip)));
}

// The output of `frames` frames of the archive's sound, with `effect` on aux bus A.
std::vector<int16_t> RenderWith(const std::vector<uint8_t>& bytecode, int frames,
                                std::unique_ptr<atk::EffectBase> effect)
{
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, bytecode, {ConstantPcmWave(kBigEndian, 48000)}), kSarOptions);
    atk::ArchiveModel model(archive);
    model.engine->AppendEffect(0, std::move(effect));
    model.engine->StartSequence(0);

    return RunFrames(model, frames);
}

void TestRendererEffects()
{
    // A note sent only to aux bus A. A delay of 5 ms there, with the input at full gain, no dry gain, no feedback and
    // no low-pass filter, passes it through a frame late. A light limiter there with a threshold and gains of 1 and no
    // look-ahead time passes it through a sample late, at the look-ahead of a sample.
    const std::vector<uint8_t> kAux = {0xB0, 48, 0xE1, 120, 0x00, 0xDB, 0, 0xD9, 127, 60, 127, 24, 0xFF};
    auto delay = std::make_unique<atk::EffectDelay>();
    const bool longest_set = delay->SetDelayTimeMax(1000000000);
    delay->SetDelayTime(5000000);
    delay->SetInGain(1.0f);
    delay->SetDryGain(0.0f);
    delay->SetFeedbackGain(0.0f);
    delay->SetLowPassAmount(0.0f);
    auto limiter = std::make_unique<atk::EffectLightLimiter>();
    limiter->SetLookAheadTime(0);
    limiter->SetThreshold(1.0f);
    constexpr std::size_t kFrame = 240 * 2;
    constexpr std::size_t kSample = 2;

    const std::vector<int16_t> dry = Render(kAux, 100);
    const std::vector<int16_t> delayed = RenderWith(kAux, 100, std::move(delay));
    const std::vector<int16_t> limited = RenderWith(kAux, 100, std::move(limiter));

    XSFLIB_CHECK(longest_set && Sounds(dry));
    XSFLIB_CHECK(!Sounds(std::span(delayed).first(kFrame)));
    XSFLIB_CHECK(std::equal(dry.begin(), dry.end() - kFrame, delayed.begin() + kFrame));
    XSFLIB_CHECK(!Sounds(std::span(limited).first(kSample)));
    XSFLIB_CHECK(std::equal(dry.begin(), dry.end() - kSample, limited.begin() + kSample));
}

// Whether `model` refuses `snapshot` with std::invalid_argument.
bool Refuses(atk::ArchiveModel& model, const atk::ArchiveModel::Snapshot& snapshot)
{
    return !ErrorOf<std::invalid_argument>([&] { model.Restore(snapshot); }).empty();
}

// After a restore, a model renders what it rendered after the snapshot the first time, sample for sample, and does so
// again from the same snapshot. A player started after the snapshot goes, and so do its notes. A snapshot taken before
// the sound starts holds the start that waits.
void TestSnapshotRenders()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel model(archive, StageReverbOptions());
    model.engine->StartSequence(0);
    const atk::ArchiveModel::Snapshot at_start = model.Save();
    const std::vector<int16_t> opening = RunFrames(model, 200);
    const atk::ArchiveModel::Snapshot later = model.Save();
    const int voices = model.engine->Voices().ActiveCount();
    const std::vector<int16_t> first = RunFrames(model, 600);
    model.engine->StartSequence(0);
    RunFrames(model, 100);

    model.Restore(later);
    const std::vector<int16_t> second = RunFrames(model, 600);
    model.Restore(later);
    const std::vector<int16_t> third = RunFrames(model, 600);
    model.Restore(at_start);
    const std::vector<int16_t> again = RunFrames(model, 200);

    XSFLIB_CHECK(voices > 0 && Sounds(first));
    XSFLIB_CHECK(second == first);
    XSFLIB_CHECK(third == first);
    XSFLIB_CHECK(again == opening);
}

// From frame 200 to 400, a snapshot taken at every other frame and restored two frames later renders those two frames
// again. A note of track 1 starts in that stretch, and its voice stops at the wave's end.
void TestSnapshotEveryFrame()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel model(archive, StageReverbOptions());
    model.engine->StartSequence(0);
    RunFrames(model, 200);
    int mismatches = 0;

    for (int frame = 0; frame < 100; ++frame)
    {
        const atk::ArchiveModel::Snapshot each = model.Save();
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
    atk::ArchiveModel model(archive, StageReverbOptions());
    model.engine->StartSequence(0);
    RunFrames(model, 50);
    const atk::ArchiveModel::Snapshot one_player = model.Save();
    model.engine->StartSequence(0);
    const atk::ArchiveModel::Snapshot start_waits = model.Save();
    const std::vector<int16_t> first = RunFrames(model, 100);

    model.Restore(one_player);
    const atk::SequenceSoundPlayer* next = model.engine->StartSequence(0);
    model.Restore(start_waits);
    const std::vector<int16_t> second = RunFrames(model, 100);

    XSFLIB_CHECK(next != nullptr && next->Id() == 1);
    XSFLIB_CHECK(Sounds(first) && second == first);
}

// The frames, from the start, in which a channel sounds: until a note's one-shot wave ends.
int SoundingFrames(atk::ArchiveModel& model, int frames)
{
    int sounding = 0;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        sounding += model.engine->Channels().ActiveCount() > 0 ? 1 : 0;
    }

    return sounding;
}

// An add-on archive's sequence plays the notes of its bank beside the engine's archive. Its global variables are the
// engine's. The engine's archive plays a note on a wave of 100 ms, and the add-on the same note on a wave of 50 ms,
// after it writes 5 to the global variable 20. Each note sounds for its wave and the same few frames of release. The
// engine's note therefore sounds 10 frames (50 ms) longer. An archive past the add-ons starts nothing. A snapshot keeps
// each player's archive: after a restore to before the add-on's start, the player in the add-on's place plays the
// engine's archive again.
void TestAddonArchive()
{
    const std::vector<uint8_t> kNote = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const std::vector<uint8_t> kWriteAndNote = {0xB0, 48,   0xE1, 120, 0x00, 0xF0, 0x80,
                                                20,   0x05, 0x00, 60,  127,  24,   0xFF};
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kNote, {ConstantPcmWave(kBigEndian, 4800)}), kSarOptions);
    const ntdlib::sar::SoundArchive addon = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kWriteAndNote, {ConstantPcmWave(kBigEndian, 2400)}), kSarOptions);

    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    XSFLIB_CHECK(engine.AddArchive(addon) == 1);
    XSFLIB_CHECK(engine.StartSequence(0, 2) == nullptr && engine.StartSequence(0, -1) == nullptr &&
                 engine.StartSequence(1, 1) == nullptr);

    XSFLIB_CHECK(engine.StartSequence(0) != nullptr);
    const int own = SoundingFrames(model, 60);
    const atk::ArchiveModel::Snapshot before = model.Save();
    const atk::SequenceSoundPlayer* added = engine.StartSequence(0, 1);
    const int added_place = added ? added->Id() : -1;
    const int addon_frames = SoundingFrames(model, 60);
    const int16_t written = engine.GlobalVariables()[4];

    model.Restore(before);
    const atk::SequenceSoundPlayer* again = engine.StartSequence(0);
    const int restored = SoundingFrames(model, 60);

    Check(own - addon_frames == 10 && addon_frames >= 10 && addon_frames <= 13,
          "the engine's archive and the add-on each play their wave: " + std::to_string(own) + " and " +
              std::to_string(addon_frames) + " frames");
    Check(added_place == 1 && written == 5, "the add-on's sequence writes the engine's global variable");
    Check(again != nullptr && again->Id() == added_place && restored == own,
          "after a restore, the player in the add-on's place plays the engine's archive");
}

// A model refuses another model's snapshot. It also refuses a snapshot that combines the state of its engine with the
// state of the other model's renderer. The refusal changes nothing.
void TestSnapshotOfAnotherModel()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel model(archive, StageReverbOptions());
    atk::ArchiveModel other(archive, StageReverbOptions());
    model.engine->StartSequence(0);
    const atk::ArchiveModel::Snapshot early = model.Save();
    RunFrames(model, 100);
    const atk::ArchiveModel::Snapshot late = model.Save();
    const std::vector<int16_t> expected = RunFrames(model, 100);
    const atk::ArchiveModel::Snapshot mixed{other.Save().renderer, early.engine};
    model.Restore(late);

    const bool refused = Refuses(other, late);
    const bool refused_mixed = Refuses(model, mixed);
    const std::vector<int16_t> after = RunFrames(model, 100);

    XSFLIB_CHECK(refused && refused_mixed);
    XSFLIB_CHECK(after == expected);
}

// A host may append the reverb itself, through a std::unique_ptr to the reverb's class. The call takes the overload for
// aux effects, and the output is that of the reverb in the engine's options.
void TestAppendReverb()
{
    enum class Bus
    {
        kA,
    };
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel appended(archive);
    atk::ArchiveModel optioned(archive, StageReverbOptions());
    const std::optional<audio::I3dl2ReverbParameterSet> reverb = kStageReverb;
    const Bus bus = Bus::kA;
    appended.engine->AppendEffect(static_cast<int>(bus), std::make_unique<ntdlib::nx::atk::I3dl2AuxEffect>(*reverb));
    appended.engine->StartSequence(0);
    optioned.engine->StartSequence(0);

    const std::vector<int16_t> out = RunFrames(appended, 300);

    XSFLIB_CHECK(Sounds(out) && out == RunFrames(optioned, 300));
}

// A snapshot holds the renderer's effects on the buses: a delay with its feedback and a light limiter on aux bus A. The
// frames after a restore repeat those after the save. A snapshot from before the effects were appended is refused.
void TestSnapshotRendererEffects()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel model(archive);
    const atk::ArchiveModel::Snapshot without_effects = model.Save();
    model.engine->AppendEffect(0, std::make_unique<atk::EffectDelay>());
    auto limiter = std::make_unique<atk::EffectLightLimiter>();
    limiter->SetThreshold(0.05f);
    model.engine->AppendEffect(0, std::move(limiter));
    model.engine->StartSequence(0);
    RunFrames(model, 100);
    const atk::ArchiveModel::Snapshot later = model.Save();
    const std::vector<int16_t> first = RunFrames(model, 200);

    model.Restore(later);
    const std::vector<int16_t> second = RunFrames(model, 200);

    XSFLIB_CHECK(Sounds(first) && second == first);
    XSFLIB_CHECK(Refuses(model, without_effects));
}

// A muted track plays on, and none of its sound reaches the output. With track 1 muted from the start, the output is
// the output of the archive whose track 1 rests in place of its notes, sample for sample, with the reverb on track 0's
// sends. Each frame has as many channels and voices as it has without the mute.
void TestMuteTrack()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    const ntdlib::sar::SoundArchive resting = LoopingArchive(false);
    atk::ArchiveModel muted(archive, StageReverbOptions());
    atk::ArchiveModel playing(archive, StageReverbOptions());
    atk::ArchiveModel rests(resting, StageReverbOptions());
    muted.engine->StartSequence(0);
    playing.engine->StartSequence(0);
    rests.engine->StartSequence(0);
    muted.engine->MuteTrack(0, 1, true);
    std::vector<int16_t> heard;
    std::vector<int16_t> played;
    bool same_channels = true;

    for (int frame = 0; frame < 600; ++frame)
    {
        const std::vector<int16_t> muted_frame = RunFrames(muted, 1);
        const std::vector<int16_t> playing_frame = RunFrames(playing, 1);
        heard.insert(heard.end(), muted_frame.begin(), muted_frame.end());
        played.insert(played.end(), playing_frame.begin(), playing_frame.end());
        same_channels = same_channels &&
                        muted.engine->Channels().ActiveCount() == playing.engine->Channels().ActiveCount() &&
                        muted.engine->Voices().ActiveCount() == playing.engine->Voices().ActiveCount();
    }
    const std::vector<int16_t> rested = RunFrames(rests, 600);

    XSFLIB_CHECK(muted.engine->IsTrackMuted(0, 1) && !muted.engine->IsTrackMuted(0, 0));
    XSFLIB_CHECK(Sounds(heard) && heard == rested && played != rested);
    XSFLIB_CHECK(same_channels);
}

// With track 1 muted, the output is track 0's alone. Track 0's meter then has the output's peaks in every frame, its
// sends to aux bus A included. Track 1's meter is silent from the mute, and the tracks without notes are silent too.
// Unmuted, track 1's meter shows its notes. The meters change nothing in the output, and a model without meters gives
// no peaks.
void TestMeterTracks()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel metered(archive);
    atk::ArchiveModel plain(archive);
    atk::ArchiveModel both(archive);
    for (atk::ArchiveModel* model : {&metered, &plain, &both})
    {
        model->engine->StartSequence(0);
    }
    metered.engine->MeterTracks(0);
    metered.engine->MuteTrack(0, 1, true);
    plain.engine->MuteTrack(0, 1, true);
    both.engine->MeterTracks(0);
    bool track_0_is_output = true;
    bool same_output = true;
    bool others_silent = true;
    bool track_1_heard = false;
    bool plain_silent = true;

    for (int frame = 0; frame < 400; ++frame)
    {
        const std::vector<int16_t> out = RunFrames(metered, 1);
        same_output = same_output && out == RunFrames(plain, 1);
        track_0_is_output = track_0_is_output && metered.engine->TrackPeaks(0) == PeaksOf(out);
        for (int track = 1; track < 16; ++track)
        {
            others_silent = others_silent && metered.engine->TrackPeaks(track) == std::array<int32_t, 2>{0, 0};
        }
        plain_silent = plain_silent && plain.engine->TrackPeaks(0) == std::array<int32_t, 2>{0, 0};
        RunFrames(both, 1);
        track_1_heard = track_1_heard || both.engine->TrackPeaks(1) != std::array<int32_t, 2>{0, 0};
    }

    XSFLIB_CHECK(track_0_is_output && Sounds(RunFrames(metered, 1)));
    XSFLIB_CHECK(same_output && others_silent && plain_silent);
    XSFLIB_CHECK(track_1_heard);
}

// With the stage's reverb on aux bus A, track 0's meter leaves out the track's send to bus A. The reverb's output takes
// the bus's place in the output. With track 1 muted, the meter's peaks are those of the same sequence without the send
// and the reverb.
void TestMeterLeavesOutEffects()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    const ntdlib::sar::SoundArchive unsent = LoopingArchive(true, 0);
    atk::ArchiveModel reverberant(archive, StageReverbOptions());
    atk::ArchiveModel dry(unsent);
    for (atk::ArchiveModel* model : {&reverberant, &dry})
    {
        model->engine->StartSequence(0);
        model->engine->MuteTrack(0, 1, true);
    }
    reverberant.engine->MeterTracks(0);
    bool dry_peaks = true;
    bool reverb_heard = false;

    for (int frame = 0; frame < 400; ++frame)
    {
        const std::vector<int16_t> wet_frame = RunFrames(reverberant, 1);
        const std::vector<int16_t> dry_frame = RunFrames(dry, 1);
        dry_peaks = dry_peaks && reverberant.engine->TrackPeaks(0) == PeaksOf(dry_frame);
        reverb_heard = reverb_heard || wet_frame != dry_frame;
    }

    XSFLIB_CHECK(dry_peaks && reverb_heard);
}

// The meters take only the metered player's tracks. With the first player muted and metered, and a second player of
// the same sequence heard, the first player's meters stay silent.
void TestMeterOnePlayer()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    const int first = engine.StartSequence(0)->Id();
    const bool second = engine.StartSequence(0) != nullptr;
    engine.MuteTrack(first, 0, true);
    engine.MuteTrack(first, 1, true);
    engine.MeterTracks(first);
    bool heard = false;
    bool silent = true;

    for (int frame = 0; frame < 200; ++frame)
    {
        heard = heard || Sounds(RunFrames(model, 1));
        for (int track = 0; track < 16; ++track)
        {
            silent = silent && engine.TrackPeaks(track) == std::array<int32_t, 2>{0, 0};
        }
    }

    XSFLIB_CHECK(second && heard && silent);
}

// At 120 beats a minute, a note of 24 ticks with a slow release on a wave of 1 s, then the end. The track ends after
// its note, at 250 ms, and lets the note go. The release sounds on without an owner until the wave ends. Muting the
// track silences the release after a frame's ramp, and the release goes on. Unmuting brings it back. So does a restore
// of a snapshot taken while the track was muted, once the track is unmuted.
void TestMuteRelease()
{
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xD3, 100, 60, 127, 24, 0xFF};
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kBytecode, {ConstantPcmWave(kBigEndian, 48000)}), kSarOptions);
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    engine.StartSequence(0);
    const bool sounds = Sounds(RunFrames(model, 80));
    const atk::ChannelManager& channels = engine.Channels();
    const bool released = channels.ActiveCount() == 1 && channels.At(channels.InUse().front()).owner_track == -1 &&
                          channels.At(channels.InUse().front()).source_track == 0;
    engine.MuteTrack(0, 0, true);
    RunFrames(model, 1);

    const atk::ArchiveModel::Snapshot while_muted = model.Save();
    const std::vector<int16_t> muted = RunFrames(model, 20);
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

// The mutes stay through a restore, and hold at once after it. One model plays track 1 up to a snapshot after 151
// frames, while track 1's second note sounds (frames 150 to 152). It then mutes track 1 and restores the snapshot. From
// there, it renders what a model with track 1 muted from the start renders, sample for sample.
void TestMuteThroughRestore()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel muted(archive, StageReverbOptions());
    atk::ArchiveModel playing(archive, StageReverbOptions());
    muted.engine->StartSequence(0);
    playing.engine->StartSequence(0);
    muted.engine->MuteTrack(0, 1, true);
    RunFrames(muted, 151);
    RunFrames(playing, 151);
    const atk::ArchiveModel::Snapshot during_note = playing.Save();
    const std::vector<int16_t> heard = RunFrames(playing, 1);
    playing.engine->MuteTrack(0, 1, true);
    playing.Restore(during_note);

    const std::vector<int16_t> restored = RunFrames(playing, 200);
    const std::vector<int16_t> expected = RunFrames(muted, 200);

    XSFLIB_CHECK(!std::equal(heard.begin(), heard.end(), expected.begin()));
    XSFLIB_CHECK(restored == expected);
}

// While the loop hook is set, each pass of a track reaches the track loop hook at the start of the track's loop. Track
// 0's loop starts at 0x0F and track 1's at 0x23. Without the loop hook, the track loop hook gets no pass.
void TestTrackLoopHook()
{
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel following(archive);
    atk::ArchiveModel unhooked(archive);
    std::vector<std::pair<int, uint32_t>> passes;
    std::vector<std::pair<int, uint32_t>> unhooked_passes;
    const auto record_pass = [&passes](int track, uint32_t target)
    {
        passes.emplace_back(track, target);
    };
    const auto record_unhooked_pass = [&unhooked_passes](int track, uint32_t target)
    {
        unhooked_passes.emplace_back(track, target);
    };
    following.engine->SetLoopHook([](int, uint32_t) {});
    following.engine->SetTrackLoopHook(record_pass);
    unhooked.engine->SetTrackLoopHook(record_unhooked_pass);
    following.engine->StartSequence(0);
    unhooked.engine->StartSequence(0);

    RunFrames(following, 600);
    RunFrames(unhooked, 600);

    // Each track's passes at its loop's start, and the passes anywhere else.
    std::array<int, 2> at_start{};
    int elsewhere = 0;
    for (const auto& [track, target] : passes)
    {
        if ((track == 0 && target == 0x0F) || (track == 1 && target == 0x23))
        {
            ++at_start[static_cast<std::size_t>(track)];
        }
        else
        {
            ++elsewhere;
        }
    }

    Check(at_start[0] >= 3 && at_start[1] >= 3 && elsewhere == 0,
          "every pass reaches the hook: " + std::to_string(at_start[0]) + " of track 0's, " +
              std::to_string(at_start[1]) + " of track 1's and " + std::to_string(elsewhere) + " elsewhere");
    XSFLIB_CHECK(unhooked_passes.empty());
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
        0xFE, 0x03, 0x00,             // tracks 0 and 1
        0xB0, 48,   0xE1, 120,  0x00, // a timebase of 48, 120 beats a minute
        0x88, 0x00, 0x1D, 0x00, 0x00, // track 0 at 0x1D: the track itself
        0x88, 0x02, 0x1D, 0x00, 0x00, // track 2 at 0x1D: not allocated
        0x88, 0x01, 0x1D, 0x00, 0x00, // 0x12: track 1 at 0x1D
        0x80, 24,                     // a rest
        0x89, 0x12, 0x00, 0x00,       // back to 0x12
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
    atk::ArchiveModel hooked(archive);
    atk::ArchiveModel plain(archive);
    std::vector<Open> opens;
    const auto record_open = [&opens](int track, int opened, uint32_t offset)
    {
        opens.push_back({track, opened, offset});
    };
    hooked.engine->SetOpenTrackHook(record_open);
    const atk::SequenceSoundPlayer* player = hooked.engine->StartSequence(0);
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
// The command 0xF0E0 calls the engine's user procedure hook with the procedure's number and the track. A write to a
// variable in the hook reaches the track's next command. The sequence sets its variable 14 to 4, calls procedure 2,
// then plays its note only if the variable holds -1. A hook that writes -1 there lets the note play. Without a hook,
// the command changes nothing, and the sequence stays silent. A condition that fails skips the call.
void TestUserProcedureHook()
{
    const std::vector<uint8_t> kAsks = {0xB0, 48,   0xE1, 120,  0x00, 0xF0, 0x80, 14,   0x04, 0x00,
                                        0xF0, 0xE0, 0x02, 0x00, 0xF0, 0x90, 14,   0xFF, 0xFF, 0xA2,
                                        0xF0, 0xE0, 0x07, 0x00, 0xA2, 60,   127,  24,   0xFF};
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, kAsks, {ConstantPcmWave(kBigEndian, 4800)}), kSarOptions);

    atk::ArchiveModel hooked(archive);
    std::vector<std::pair<int, uint16_t>> calls;
    hooked.engine->SetUserProcedureHook(
        [&hooked, &calls](atk::SequenceSoundPlayer& player, int track, uint16_t procedure)
        {
            calls.push_back({track, procedure});
            player.SetVariable(*hooked.engine, 14, -1);
        });
    XSFLIB_CHECK(hooked.engine->StartSequence(0) != nullptr);
    const int heard = SoundingFrames(hooked, 60);

    atk::ArchiveModel quiet(archive);
    std::vector<std::pair<int, uint16_t>> quiet_calls;
    quiet.engine->SetUserProcedureHook([&quiet_calls](atk::SequenceSoundPlayer&, int track, uint16_t procedure)
                                       { quiet_calls.push_back({track, procedure}); });
    XSFLIB_CHECK(quiet.engine->StartSequence(0) != nullptr);
    const int unwritten = SoundingFrames(quiet, 60);

    atk::ArchiveModel plain(archive);
    XSFLIB_CHECK(plain.engine->StartSequence(0) != nullptr);
    const int silent = SoundingFrames(plain, 60);

    const std::vector<std::pair<int, uint16_t>> kBoth = {{0, 2}, {0, 7}};
    const std::vector<std::pair<int, uint16_t>> kFirst = {{0, 2}};
    Check(calls == kBoth, "the hook gets procedure 2, then procedure 7 behind the condition that its write met: " +
                              std::to_string(calls.size()) + " calls");
    Check(quiet_calls == kFirst,
          "a hook that writes nothing gets procedure 2 only: " + std::to_string(quiet_calls.size()) + " calls");
    Check(heard > 0 && unwritten == 0 && silent == 0, "the hook's write lets the note play: " + std::to_string(heard) +
                                                          ", " + std::to_string(unwritten) + " and " +
                                                          std::to_string(silent) + " frames");
}

} // namespace

int main()
{
    TestLowLevelVoice();
    TestWaveBufferQueue();
    TestEngine();
    TestNoteOffBeforeUpdate();
    TestAuxSend();
    TestAuxReverb();
    TestRendererEffects();
    TestSnapshotRenders();
    TestSnapshotEveryFrame();
    TestSnapshotPlayers();
    TestSnapshotOfAnotherModel();
    TestAppendReverb();
    TestSnapshotRendererEffects();
    TestMuteTrack();
    TestMuteRelease();
    TestMuteThroughRestore();
    TestMeterTracks();
    TestMeterLeavesOutEffects();
    TestMeterOnePlayer();
    TestTrackLoopHook();
    TestOpenTrackHook();
    TestAddonArchive();
    TestUserProcedureHook();

    return xsflib::testing::Summary("nx_atk_engine");
}
