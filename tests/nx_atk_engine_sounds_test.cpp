// SPDX-License-Identifier: MIT

// Checks the model of nn::atk on the Switch's renderer on wave sounds and stream sounds (their settings, tracks and
// snapshots), and in the enhanced mixing mode.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/sound_players.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/testing/sar_files.h"
#include "nwsnd_engine_files.h"
#include "nx_atk_engine_test_util.h"

namespace atk = ntdlib::nx::atk;
namespace sar_files = ntdlib::testing::sar_files;

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
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// The archive of a wave sound with the settings of `sound`, at `volume`, that plays a PCM16 wave of 100 ms at 48 kHz.
ntdlib::sar::SoundArchive WaveSoundOf(const sar_files::WaveSoundSpec& sound, uint8_t volume = 127,
                                      std::optional<uint32_t> channel_priority = std::nullopt)
{
    return ntdlib::sar::SoundArchive::Load(
        WaveSoundArchive(kBigEndian, sound, {ConstantPcmWave(kBigEndian, 4800)}, volume, channel_priority),
        kSarOptions);
}

// The output of a wave sound with the settings of `sound`, at `volume`, a frame at a time, until it ends.
std::vector<std::vector<int16_t>> WaveSoundFrames(const sar_files::WaveSoundSpec& sound, uint8_t volume = 127)
{
    const ntdlib::sar::SoundArchive archive = WaveSoundOf(sound, volume);
    atk::ArchiveModel model(archive);
    model.engine->StartWaveSound(0);
    std::vector<std::vector<int16_t>> frames;
    while (model.engine->IsBusy() && frames.size() < 400)
    {
        frames.push_back(RunFrames(model, 1));
    }

    return frames;
}

// A wave sound plays the wave of its first note once on a channel, at the sound's channel priority plus 64. The first
// frame takes the start to the sound thread. The channel starts and sounds in the second. The sound ends with the wave,
// after 100 ms, and frees the channel. A mute of track 0 mutes the channel. A wave sound without notes has no wave to
// play, and ends in the frame that it starts in.
void TestWaveSound()
{
    const ntdlib::sar::SoundArchive archive = WaveSoundOf(PlainWaveSound(), 127, 20);
    atk::ArchiveModel model(archive);
    atk::ArchiveModel muted(archive);
    atk::Engine& engine = *model.engine;
    const atk::WaveSoundPlayer* player = engine.StartWaveSound(0);
    muted.engine->StartWaveSound(0);
    muted.engine->MuteTrack(0, 0, true);
    XSFLIB_CHECK(engine.StartWaveSound(1) == nullptr && engine.StartSequence(0) == nullptr);
    if (!Check(player != nullptr, "the wave sound starts"))
    {
        return;
    }

    engine.RunFrame();
    const bool silent_at_first = engine.Channels().ActiveCount() == 0 && !Sounds(model.renderer.DeviceOutput());
    engine.RunFrame();
    const bool sounds_next = engine.Channels().ActiveCount() == 1 && Sounds(model.renderer.DeviceOutput());
    const bool priority = player->ChannelPlace() != ntdlib::nwsnd::kNoChannel &&
                          engine.Channels().At(player->ChannelPlace()).Voice(engine).Priority() == 84;
    int frames = 2;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }
    const std::vector<int16_t> heard = RunFrames(muted, frames);

    XSFLIB_CHECK(silent_at_first);
    XSFLIB_CHECK(sounds_next && priority);
    Check(!engine.IsBusy() && frames >= 21 && frames <= 24, "the wave sound ends after " + std::to_string(frames));
    XSFLIB_CHECK(player->IsFinished() && engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
    XSFLIB_CHECK(engine.LowLevelVoices().AllocatedCount() == 0);
    XSFLIB_CHECK(!Sounds(heard) && !muted.engine->IsBusy());

    const ntdlib::sar::SoundArchive empty = WaveSoundOf({});
    atk::ArchiveModel without_notes(empty);
    XSFLIB_CHECK(without_notes.engine->StartWaveSound(0) != nullptr);
    const std::vector<int16_t> nothing = RunFrames(without_notes, 2);
    XSFLIB_CHECK(!Sounds(nothing) && !without_notes.engine->IsBusy());
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

// The steady level of a wave sound's or a stream's output: the peaks of its left and right samples in frame 8.
std::array<int32_t, 2> SteadyPeaks(const std::vector<std::vector<int16_t>>& frames)
{
    return frames.size() > 8 ? PeaksOf(frames[8]) : std::array<int32_t, 2>{0, 0};
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

// The wave sound's settings reach its channel, and its note's settings don't: a note at another original key, with a
// volume, a pan, a pitch and an envelope, plays as the plain note does. A pan of 0 puts the sound on the left alone,
// and 127 on the right alone. A pitch of 2 ends the sound in half the time. A main send of 0 silences it, and a
// low-pass filter changes it. The sound's volume of 64 gives 64/127 of the level. A slow attack starts the sound
// quieter, and its level rises.
void TestWaveSoundSettings()
{
    const std::vector<std::vector<int16_t>> plain = WaveSoundFrames(PlainWaveSound());
    sar_files::WaveSoundSpec note = PlainWaveSound();
    note.notes[0].original_key = 72;
    note.notes[0].volume = 30;
    note.notes[0].pan = 0;
    note.notes[0].pitch = 2.0f;
    note.notes[0].adshr = std::array<uint8_t, 5>{20, 20, 20, 20, 20};
    sar_files::WaveSoundSpec left = PlainWaveSound();
    left.pan = 0;
    sar_files::WaveSoundSpec right = PlainWaveSound();
    right.pan = 127;
    sar_files::WaveSoundSpec fast = PlainWaveSound();
    fast.pitch = 2.0f;
    sar_files::WaveSoundSpec unsent = PlainWaveSound();
    unsent.sends = {0, 3, 0, 0, 0};
    sar_files::WaveSoundSpec filtered = PlainWaveSound();
    filtered.filter = 10;
    sar_files::WaveSoundSpec slow = PlainWaveSound();
    slow.adshr = std::array<uint8_t, 5>{100, 127, 127, 127, 127};

    const std::vector<std::vector<int16_t>> faster = WaveSoundFrames(fast);
    const std::vector<std::vector<int16_t>> slower = WaveSoundFrames(slow);

    XSFLIB_CHECK(plain.size() > 8 && WaveSoundFrames(note) == plain);
    XSFLIB_CHECK(OnOneSide(WaveSoundFrames(left), 0) && OnOneSide(WaveSoundFrames(right), 1));
    Check(faster.size() + 9 <= plain.size() && faster.size() + 12 >= plain.size(),
          "a pitch of 2 plays for " + std::to_string(faster.size()) + " frames, the plain sound for " +
              std::to_string(plain.size()));
    XSFLIB_CHECK(!AnySound(WaveSoundFrames(unsent)) && AnySound(plain));
    XSFLIB_CHECK(WaveSoundFrames(filtered) != plain);
    XSFLIB_CHECK(LevelIs(SteadyPeaks(WaveSoundFrames(PlainWaveSound(), 64)), SteadyPeaks(plain), 64.0 / 127.0));
    XSFLIB_CHECK(slower.size() > 8 && PeaksOf(slower[1])[0] * 10 < PeaksOf(plain[1])[0] &&
                 PeaksOf(slower[8])[0] > PeaksOf(slower[4])[0] && PeaksOf(slower[4])[0] > PeaksOf(slower[1])[0]);
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
                                               const std::vector<uint8_t>& file, std::size_t limit = 400)
{
    atk::ArchiveModel model(archive);
    model.engine->StartStreamSound(0, file);
    std::vector<std::vector<int16_t>> frames;
    while (model.engine->IsBusy() && frames.size() < limit)
    {
        frames.push_back(RunFrames(model, 1));
    }

    return frames;
}

// A mono stream of 200 ms plays on a voice at priority 255, from the second frame, and ends with its samples. The
// voice and the player then go. Bytes that aren't a stream file throw.
void TestStreamSound()
{
    const std::vector<uint8_t> file = PcmStream(ntdlib::sar::Flavor::kNx, {std::vector<int16_t>(9600, 0x1000)});
    const ntdlib::sar::SoundArchive archive = StreamSoundOf({StreamTrack({0})}, 1);
    atk::ArchiveModel model(archive);
    atk::Engine& engine = *model.engine;
    const atk::StreamSoundPlayer* player = engine.StartStreamSound(0, file);
    if (!Check(player != nullptr && player->ChannelCount() == 1, "the stream sound starts"))
    {
        return;
    }

    engine.RunFrame();
    const bool silent_at_first = engine.Voices().ActiveCount() == 0 && !Sounds(model.renderer.DeviceOutput());
    engine.RunFrame();
    const bool sounds_next = engine.Voices().ActiveCount() == 1 && Sounds(model.renderer.DeviceOutput());
    const ntdlib::nwsnd::VoiceId voice = player->ChannelVoice(0);
    const bool priority = voice != ntdlib::nwsnd::kNoVoice && engine.Voices().At(voice).Priority() == 255 &&
                          engine.Voices().At(voice).Side() == 0;
    int frames = 2;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }

    const bool refused =
        !ErrorOf<xsflib::FormatError>([&] { engine.StartStreamSound(0, std::vector<uint8_t>(64)); }).empty();

    XSFLIB_CHECK(silent_at_first);
    XSFLIB_CHECK(sounds_next && priority);
    Check(!engine.IsBusy() && frames >= 41 && frames <= 44, "the stream sound ends after " + std::to_string(frames));
    XSFLIB_CHECK(player->IsFinished() && engine.Voices().ActiveCount() == 0);
    XSFLIB_CHECK(engine.LowLevelVoices().AllocatedCount() == 0);
    XSFLIB_CHECK(refused && engine.StartWaveSound(0) == nullptr);
    XSFLIB_CHECK(StreamFrames(StreamSoundOf({StreamTrack({0})}, 0), file).empty());
}

// A track of two channels plays the first on the left and the second on the right, on sides 1 and 2. A track's volume
// of 64 gives 64/127 of the level. A track that doesn't play leaves its channel silent. A stream that loops plays on.
// In a file of version 2.0.0 or earlier, the file's tracks take the place of the archive's: one at volume 0 silences
// the stream. A later file's tracks don't count.
void TestStreamTracks()
{
    constexpr auto kNx = ntdlib::sar::Flavor::kNx;
    const std::vector<int16_t> kLoud(9600, 0x1000);
    const std::vector<int16_t> kSilent(9600, 0);
    const std::vector<uint8_t> left_only = PcmStream(kNx, {kLoud, kSilent});
    const std::vector<uint8_t> right_only = PcmStream(kNx, {kSilent, kLoud});
    const ntdlib::sar::SoundArchive pair = StreamSoundOf({StreamTrack({0, 1})}, 1);
    const ntdlib::sar::SoundArchive two = StreamSoundOf({StreamTrack({0}), StreamTrack({1})}, 2);
    const ntdlib::sar::SoundArchive both = StreamSoundOf({StreamTrack({0}), StreamTrack({1})}, 3);
    ntdlib::sar::StreamTrackInfo quiet = StreamTrack({0});
    quiet.volume = 64;
    const std::vector<uint8_t> mono = PcmStream(kNx, {kLoud});
    const ntdlib::sar::SoundArchive single = StreamSoundOf({StreamTrack({0})}, 1);
    const std::vector<std::vector<int16_t>> full = StreamFrames(single, mono);
    const std::vector<uint8_t> looped = PcmStream(kNx, {std::vector<int16_t>(4800, 0x1000)}, 0);
    const std::vector<ntdlib::sar::StreamFile::Track> kMuted = {{0, 64, 0, 0, {0}}};
    const std::vector<uint8_t> old_file = PcmStream(kNx, {kLoud}, std::nullopt, kMuted, 0x00020000);
    const std::vector<uint8_t> new_file = PcmStream(kNx, {kLoud}, std::nullopt, kMuted);

    atk::ArchiveModel model(pair);
    const atk::StreamSoundPlayer* player = model.engine->StartStreamSound(0, left_only);
    RunFrames(model, 2);
    const bool sides = player != nullptr && player->ChannelCount() == 2 &&
                       model.engine->Voices().At(player->ChannelVoice(0)).Side() == 1 &&
                       model.engine->Voices().At(player->ChannelVoice(1)).Side() == 2;
    const std::vector<std::vector<int16_t>> looping = StreamFrames(single, looped, 200);

    XSFLIB_CHECK(sides);
    XSFLIB_CHECK(OnOneSide(StreamFrames(pair, left_only), 0) && OnOneSide(StreamFrames(pair, right_only), 1));
    XSFLIB_CHECK(LevelIs(SteadyPeaks(StreamFrames(StreamSoundOf({quiet}, 1), mono)), SteadyPeaks(full), 64.0 / 127.0));
    XSFLIB_CHECK(!AnySound(StreamFrames(two, left_only)) && AnySound(StreamFrames(two, right_only)) &&
                 AnySound(StreamFrames(both, left_only)));
    XSFLIB_CHECK(looping.size() == 200 && Sounds(looping.back()));
    XSFLIB_CHECK(!AnySound(StreamFrames(single, old_file)) && StreamFrames(single, new_file) == full);
}

// A snapshot taken while a stream plays renders the same frames again after a restore. A stream sound of an info
// outside the archive plays as the archive's does.
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
    const std::vector<uint8_t> file = PcmStream(ntdlib::sar::Flavor::kNx, {samples, samples}, 4000);
    const ntdlib::sar::SoundArchive archive = StreamSoundOf({StreamTrack({0, 1})}, 1);
    atk::ArchiveModel model(archive);
    model.engine->StartStreamSound(0, file);
    RunFrames(model, 30);
    const atk::ArchiveModel::Snapshot snapshot = model.Save();
    const std::vector<int16_t> first = RunFrames(model, 200);
    model.Restore(snapshot);
    const std::vector<int16_t> second = RunFrames(model, 200);

    atk::ArchiveModel outside(archive);
    outside.engine->StartStreamSound(archive.Sounds()[0], file);
    atk::ArchiveModel inside(archive);
    inside.engine->StartStreamSound(0, file);

    XSFLIB_CHECK(Sounds(first) && second == first);
    XSFLIB_CHECK(RunFrames(outside, 100) == RunFrames(inside, 100));
}

// A place that a restore empties can take a player of another kind: here a stream sound outside the archive. A restore
// of a snapshot that holds a sequence there puts the sequence back, with its start that waits.
void TestSnapshotPlayersOfOtherKinds()
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
    ntdlib::sar::SoundInfo info;
    info.type = ntdlib::sar::SoundType::kStream;
    info.stream.valid_tracks = 1;
    info.stream.tracks = {StreamTrack({0})};
    const atk::StreamSoundPlayer* stream =
        model.engine->StartStreamSound(info, PcmStream(ntdlib::sar::Flavor::kNx, {std::vector<int16_t>(9600, 0x1000)}));
    RunFrames(model, 20);
    model.Restore(start_waits);
    const std::vector<int16_t> second = RunFrames(model, 100);

    XSFLIB_CHECK(stream != nullptr && stream->Id() == 1);
    XSFLIB_CHECK(Sounds(first) && second == first);
}

// Options of the enhanced mixing mode, with the reverb of the reference game's stages on aux bus A.
atk::EngineOptions EnhancedOptions()
{
    atk::EngineOptions options = StageReverbOptions();
    options.enhanced = true;

    return options;
}

// The output in floating point of the model's next `frames` frames.
std::vector<float> RunFloatFrames(atk::ArchiveModel& model, int frames)
{
    std::vector<float> output;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const float> frame = model.renderer.DeviceOutputFloat();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

void TestEnhancedVoices()
{
    // 120 notes start in the same tick. The console's 96 voices take 96 of them. In the enhanced mode, the sound system
    // takes a voice for each: 120 channels, MultiVoices and low-level voices.
    std::vector<uint8_t> bytecode = {0xB0, 48, 0xE1, 120, 0x00, 0xC7, 0x00};
    for (int note = 0; note < 120; ++note)
    {
        bytecode.insert(bytecode.end(), {60, 100, 96});
    }
    bytecode.insert(bytecode.end(), {0x80, 96, 0xFF});
    const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, bytecode, {ConstantPcmWave(kBigEndian, 48000)}), kSarOptions);
    const auto counts = [&](const atk::EngineOptions& options)
    {
        atk::ArchiveModel model(archive, options);
        model.engine->StartSequence(0);
        RunFrames(model, 4);
        atk::Engine& engine = *model.engine;

        return std::array<int, 3>{engine.Channels().ActiveCount(), engine.Voices().ActiveCount(),
                                  engine.LowLevelVoices().AllocatedCount()};
    };

    const std::array<int, 3> console = counts({});
    const std::array<int, 3> enhanced = counts(EnhancedOptions());

    XSFLIB_CHECK(console[0] <= 96 && console[1] <= 96 && console[2] <= 96);
    XSFLIB_CHECK(enhanced[0] == 120 && enhanced[1] == 120 && enhanced[2] == 120);
}

void TestEnhancedAuxSend()
{
    // In the enhanced mode too, a note sent only to aux bus A, with no effect on it, reaches the output as a note sent
    // only to the main bus does.
    const std::vector<uint8_t> kMain = {0xB0, 48, 0xE1, 120, 0x00, 60, 127, 24, 0xFF};
    const std::vector<uint8_t> kAux = {0xB0, 48, 0xE1, 120, 0x00, 0xDB, 0, 0xD9, 127, 60, 127, 24, 0xFF};
    atk::EngineOptions options;
    options.enhanced = true;
    const auto render = [&](const std::vector<uint8_t>& bytecode)
    {
        const ntdlib::sar::SoundArchive archive = ntdlib::sar::SoundArchive::Load(
            ProgramArchive(kBigEndian, bytecode, {ConstantPcmWave(kBigEndian, 48000)}), kSarOptions);
        atk::ArchiveModel model(archive, options);
        model.engine->StartSequence(0);

        return RunFloatFrames(model, 20);
    };

    const std::vector<float> through_main = render(kMain);
    const std::vector<float> through_aux = render(kAux);

    XSFLIB_CHECK(std::any_of(through_main.begin(), through_main.end(), [](float s) { return s != 0.0f; }));
    XSFLIB_CHECK(through_aux == through_main);
}

void TestEnhancedSnapshots()
{
    // In the enhanced mode, a restore renders the same samples again, in floating point. A snapshot restored two frames
    // after each other frame renders those two frames again.
    const ntdlib::sar::SoundArchive archive = LoopingArchive();
    atk::ArchiveModel model(archive, EnhancedOptions());
    model.engine->StartSequence(0);
    const atk::ArchiveModel::Snapshot at_start = model.Save();
    const std::vector<float> opening = RunFloatFrames(model, 200);
    const atk::ArchiveModel::Snapshot later = model.Save();
    const std::vector<float> first = RunFloatFrames(model, 600);
    model.engine->StartSequence(0);
    RunFloatFrames(model, 100);

    model.Restore(later);
    const std::vector<float> second = RunFloatFrames(model, 600);
    model.Restore(at_start);
    const std::vector<float> again = RunFloatFrames(model, 200);
    int mismatches = 0;
    for (int frame = 0; frame < 100; ++frame)
    {
        const atk::ArchiveModel::Snapshot each = model.Save();
        const std::vector<float> ahead = RunFloatFrames(model, 2);
        model.Restore(each);
        if (RunFloatFrames(model, 2) != ahead)
        {
            ++mismatches;
        }
    }

    XSFLIB_CHECK(std::any_of(first.begin(), first.end(), [](float s) { return s != 0.0f; }));
    XSFLIB_CHECK(second == first);
    XSFLIB_CHECK(again == opening);
    Check(mismatches == 0, std::to_string(mismatches) + " of 100 snapshots render other frames when restored");
}

} // namespace

int main()
{
    TestWaveSound();
    TestWaveSoundSettings();
    TestStreamSound();
    TestStreamTracks();
    TestStreamSnapshot();
    TestSnapshotPlayersOfOtherKinds();
    TestEnhancedVoices();
    TestEnhancedAuxSend();
    TestEnhancedSnapshots();

    return xsflib::testing::Summary("nx_atk_engine_sounds");
}
