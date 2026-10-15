// SPDX-License-Identifier: MIT

// Checks the model of nn::atk's voices, channels and sequence player on the model of the renderer, without game data: a
// low-level voice on the renderer, its queue of wave buffers, and small archives that play notes through the engine.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/low_level_voice.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

using namespace ntdlib::testing;
using namespace xsflib::testing;

namespace
{

namespace atk = ntdlib::nx::atk;
namespace audio = ntdlib::nx::audio;

// How the players read the Switch's archives. Those rules stop at the first problem.
const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kNx, {}, false, ntdlib::sar::Rules::kStopAtFirstProblem};

// A reverb like the ones the reference game's stages use.
constexpr audio::I3dl2ReverbParameterSet kStageReverb{-1000.0f, -1000.0f, 2.0f,  0.8f, -700.0f, 0.005f,
                                                      -500.0f,  0.04f,    50.0f, 3.0f, 6000.0f, 0.25f};

// `count` PCM16 samples of `value`, little-endian.
std::vector<uint8_t> ConstantWave(int count, int16_t value)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value)));
        bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
    }

    return bytes;
}

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

    const std::vector<uint8_t> wave = ConstantWave(4800, 10000);
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

    std::vector<uint8_t> wave = ConstantWave(100, 1000);
    const std::vector<uint8_t> rest = ConstantWave(300, 2000);
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

// A PCM16 wave of `samples` samples of `value` at 48 kHz.
sar_files::WaveSpec ConstantPcmWave(int samples, int16_t value = 0x1000)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.sample_rate = 48000;
    wave.loop_end = static_cast<uint32_t>(samples);
    wave.channels = {ConstantWave(samples, value)};

    return wave;
}

// A stereo DSP-ADPCM wave at 32 kHz that loops from sample 2800 to its end at sample 15988. Each frame's header picks
// one of the 8 coefficient pairs and a scale. The nibbles are pseudo-random.
sar_files::WaveSpec LoopingAdpcmWave()
{
    constexpr int kSamples = 15988;
    sar_files::WaveSpec wave;
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
std::vector<uint8_t> NoteArchive(const std::vector<uint8_t>& bytecode, const std::vector<sar_files::WaveSpec>& waves,
                                 uint32_t tracks = 1)
{
    sar_files::BankSpec bank;
    std::vector<std::vector<uint8_t>> wave_files;
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        sar_files::Region region;
        region.wave = static_cast<uint32_t>(i);
        region.original_key = 60;
        sar_files::VelocityTable velocities;
        velocities.entries = {{0, region}};
        sar_files::KeyTable keys;
        keys.entries = {{0, velocities}};
        bank.waves.push_back({0x05000000, static_cast<uint32_t>(i)});
        bank.instruments.push_back(keys);
        wave_files.push_back(sar_files::Wave(false, waves[i]));
    }

    sar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    sound.allocate_track_flags = tracks;
    sar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_NOTE", 1}};
    spec.wave_archives = {{"WARC_NOTE", 2}};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{sar_files::Sequence(false, bytecode, {{"Note", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, wave_files), "", false}};

    return sar_files::Archive(false, spec);
}

// Whether a frame's output has any sound.
bool Sounds(std::span<const int16_t> output)
{
    for (const int16_t sample : output)
    {
        if (sample != 0)
        {
            return true;
        }
    }

    return false;
}

void TestEngine()
{
    // At a timebase of 48 and 120 beats a minute, a note of 24 ticks (250 ms) on a wave of 100 ms, then the end. The
    // first frame takes the start to the sound thread. The note starts in the second, and its voice sounds in the same
    // frame. The wave ends after 100 ms, and the sequence after its 24 ticks.
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const ntdlib::sar::SoundArchive archive =
        ntdlib::sar::SoundArchive::Load(NoteArchive(kBytecode, {ConstantPcmWave(4800)}), kSarOptions);
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
    const ntdlib::sar::SoundArchive archive =
        ntdlib::sar::SoundArchive::Load(NoteArchive(kBytecode, {ConstantPcmWave(4800)}), kSarOptions);
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

// The output of the model's next `frames` frames.
std::vector<int16_t> RunFrames(atk::ArchiveModel& model, int frames)
{
    std::vector<int16_t> output;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.renderer.DeviceOutput();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// The output of `frames` frames of the archive's sound.
std::vector<int16_t> Render(const std::vector<uint8_t>& bytecode, int frames, const atk::EngineOptions& options = {})
{
    const ntdlib::sar::SoundArchive archive =
        ntdlib::sar::SoundArchive::Load(NoteArchive(bytecode, {ConstantPcmWave(48000)}), kSarOptions);
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

// An archive whose sequence plays two tracks that loop for ever. Track 0 plays a looping stereo wave in DSP-ADPCM at
// 32 kHz, in notes of random lengths from 12 to 48 ticks, and sends to aux bus A. Track 1 plays a loud wave of 21 ms
// that doesn't loop. Its voices stop at the wave's end, and the renderer's depop fades their last samples out. Without
// `track_1_plays`, track 1 rests for each of its notes' lengths instead. `send` is track 0's send to aux bus A.
ntdlib::sar::SoundArchive LoopingArchive(bool track_1_plays = true, uint8_t send = 80)
{
    std::vector<uint8_t> tracks = {
        0xFE, 0x03, 0x00,                       // tracks 0 and 1
        0xB0, 48,   0xE1, 120,  0x00,           // a timebase of 48, 120 beats a minute
        0x88, 0x01, 0x1F, 0x00, 0x00,           // track 1 at 0x1F
        0xD9, send,                             // track 0 sends to aux bus A
        0xA0, 60,   100,  12,   0x00, 48, 0x00, // 0x0F: key 60 at a random length
        64,   80,   24,                         // key 64
        0x80, 8,                                // a rest
        0x89, 0x0F, 0x00, 0x00,                 // back to 0x0F
        0x81, 1,                                // 0x1F: track 1 plays program 1
        0xC1, 127,                              // at full volume
        67,   127,  48,                         // 0x23: key 67
        0x80, 24,                               // a rest
        0x89, 0x23, 0x00, 0x00,                 // back to 0x23
    };
    if (!track_1_plays)
    {
        // A rest of 48 ticks takes the place of key 67. The jump back to 0x23 still reaches it.
        tracks.erase(tracks.begin() + 0x23, tracks.begin() + 0x26);
        tracks.insert(tracks.begin() + 0x23, {0x80, 48});
    }

    return ntdlib::sar::SoundArchive::Load(
        NoteArchive(tracks, {LoopingAdpcmWave(), ConstantPcmWave(1000, 0x7000)}, 0x3), kSarOptions);
}

// Options with the reverb of the reference game's stages on aux bus A.
atk::EngineOptions StageReverbOptions()
{
    atk::EngineOptions options;
    options.aux_reverbs[0] = kStageReverb;

    return options;
}

// Whether `model` refuses `snapshot` with std::invalid_argument.
bool Refuses(atk::ArchiveModel& model, const atk::ArchiveModel::Snapshot& snapshot)
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
    const ntdlib::sar::SoundArchive archive =
        ntdlib::sar::SoundArchive::Load(NoteArchive(kBytecode, {ConstantPcmWave(48000)}), kSarOptions);
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

} // namespace

int main()
{
    TestLowLevelVoice();
    TestWaveBufferQueue();
    TestEngine();
    TestNoteOffBeforeUpdate();
    TestAuxSend();
    TestAuxReverb();
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
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
