// SPDX-License-Identifier: MIT

// Checks the Wii U's model of nw::snd on wave sounds and stream sounds: their settings and tracks, their snapshots,
// and the unmap of their memory.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "cafe_snd_test_util.h"
#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/sound_players.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/sound_players.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
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

// The output of the archive's stream sound with the stream file `file` and the tracks `tracks`, a frame at a time,
// until it ends or for `limit` frames.
std::vector<std::vector<int16_t>> StreamFrames(const ntdlib::sar::SoundArchive& archive,
                                               const std::vector<uint8_t>& file, std::size_t limit = 600,
                                               uint16_t tracks = ntdlib::nwsnd::kAllStreamTracks)
{
    snd::ArchiveModel model(archive);
    model.engine->StartStreamSound(0, file, tracks);
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

// A ramp of `length` samples: it climbs by `step` from `step * period / -2` and starts again every `period` samples.
std::vector<int16_t> Ramp(std::size_t length, int period, int step)
{
    std::vector<int16_t> ramp(length);
    for (std::size_t i = 0; i < ramp.size(); ++i)
    {
        ramp[i] =
            static_cast<int16_t>(static_cast<int>(i % static_cast<std::size_t>(period)) * step - step * period / 2);
    }

    return ramp;
}

// A stream of 400 ms with a ramp on each of its two channels, of different periods.
std::vector<uint8_t> TwoRamps()
{
    return PcmStream(ntdlib::sar::Flavor::kCafe, {Ramp(19200, 400, 40), Ramp(19200, 300, -50)});
}

// The archive of a stream sound whose two tracks both play, each on one channel. The second track's volume is
// `second_volume`.
ntdlib::sar::SoundArchive TwoTracks(uint8_t second_volume = 127)
{
    ntdlib::sar::StreamTrackInfo second = StreamTrack({1});
    second.volume = second_volume;

    return StreamSoundOf({StreamTrack({0}), second}, 0x3);
}

// The frames of `model` from frame `from` to frame `to`, after a start.
std::vector<std::vector<int16_t>> FramesOf(snd::ArchiveModel& model, int from, int to)
{
    std::vector<std::vector<int16_t>> frames;
    for (int frame = 0; frame < to; ++frame)
    {
        std::vector<int16_t> output = RunFrames(model, 1);
        if (frame >= from)
        {
            frames.push_back(std::move(output));
        }
    }

    return frames;
}

// The volumes that a game's code sets on a stream's tracks through the sound's handle. UpdateVoiceParams multiplies the
// volume of each track's voices by the track's volume (0x022B5024). The default tracks, the mask of every track and the
// mask of the two tracks alone play the same frames: a bit past the tracks changes nothing. A track that the start
// leaves out plays as a track whose volume in the archive is 0, and the other track plays on. The start of an info
// takes the mask too. A later volume below 0 plays at 0, and one of 0.5 plays at half the level
// (StreamSound::SetTrackVolume, 0x022C2814). A snapshot keeps the tracks' volumes.
void TestStreamTrackVolumes()
{
    constexpr uint16_t kAll = ntdlib::nwsnd::kAllStreamTracks;
    const std::vector<uint8_t> file = TwoRamps();
    const ntdlib::sar::SoundArchive archive = TwoTracks();
    const std::vector<std::vector<int16_t>> every = StreamFrames(archive, file);
    const std::vector<std::vector<int16_t>> first_only = StreamFrames(archive, file, 600, 0x1);
    const ntdlib::sar::SoundArchive quiet_archive = TwoTracks(0);
    const std::vector<std::vector<int16_t>> quiet_second = StreamFrames(quiet_archive, file);

    snd::ArchiveModel outside(archive);
    outside.engine->StartStreamSound(archive.Sounds()[0], file, 0x1);
    const std::vector<std::vector<int16_t>> outside_frames = FramesOf(outside, 0, 100);

    // A later volume. The fourth frame after the change is the first whose output changes. From the fifth on, the
    // output is the quiet output.
    snd::ArchiveModel later(archive);
    snd::StreamSoundPlayer* player = later.engine->StartStreamSound(0, file);
    RunFrames(later, 20);
    player->SetTrackVolume(0x2, -1.0f);
    const std::vector<std::vector<int16_t>> after_below = FramesOf(later, 4, 60);
    snd::ArchiveModel quiet(quiet_archive);
    quiet.engine->StartStreamSound(0, file);
    RunFrames(quiet, 20);
    const std::vector<std::vector<int16_t>> quiet_after = FramesOf(quiet, 4, 60);

    snd::ArchiveModel half(archive);
    half.engine->StartStreamSound(0, file, 0x2)->SetTrackVolume(0x2, 0.5f);
    const std::vector<std::vector<int16_t>> half_frames = FramesOf(half, 0, 20);
    const std::vector<std::vector<int16_t>> second_only = StreamFrames(archive, file, 20, 0x2);

    // A restore brings back the volumes of the snapshot's moment.
    snd::ArchiveModel restored(archive);
    snd::StreamSoundPlayer* silenced = restored.engine->StartStreamSound(0, file, 0x1);
    RunFrames(restored, 10);
    const snd::ArchiveModel::Snapshot snapshot = restored.Save();
    const std::vector<int16_t> silent_after = RunFrames(restored, 50);
    silenced->SetTrackVolume(0x2, 1.0f);
    restored.Restore(snapshot);
    const std::vector<int16_t> again = RunFrames(restored, 50);

    XSFLIB_CHECK(AnySound(every) && StreamFrames(archive, file, 600, kAll) == every &&
                 StreamFrames(archive, file, 600, 0x3) == every);
    XSFLIB_CHECK(AnySound(first_only) && first_only == quiet_second && first_only != every);
    XSFLIB_CHECK(outside_frames == std::vector(first_only.begin(), first_only.begin() + 100));
    XSFLIB_CHECK(after_below == quiet_after);
    XSFLIB_CHECK(half_frames.size() == 20 && second_only.size() == 20 && AnySound(second_only) &&
                 LevelIs(SteadyPeaks(half_frames), SteadyPeaks(second_only), 0.5));
    XSFLIB_CHECK(Sounds(silent_after) && again == silent_after);
}

// The tracks that Nintendo Land, Game & Wario and amiibo Festival play of their stream sounds as they start, by the
// archives of the USA releases and the sounds' names. Each game's code plays track 0 alone as these start: Nintendo
// Land's 25, Game & Wario's 4 and amiibo Festival's board BGMs. Nintendo Land's Twister Race starts a section on track
// 1 and then fades track 0 in: its streams play every track. So does every stream sound that the games leave alone,
// every sound of an unknown archive, and a name in other letters.
void TestStartingStreamTracks()
{
    constexpr std::string_view kNintendoLand = "1379ab0c17a80e0c38e0c9e90aaf92e971f2c1a4";
    constexpr std::string_view kGameAndWario = "4d2c7f19370a0eea57de309b950c6af937e6e01f";
    constexpr std::string_view kAmiiboFestival = "5784acb45299369f8388246f0ba4311610c709bc";
    constexpr uint16_t kAll = ntdlib::nwsnd::kAllStreamTracks;
    const std::initializer_list<std::string_view> nintendo_land = {"BGM_TTL_MAIN_DAYTIME",
                                                                   "BGM_TTL_MAIN_EVENING",
                                                                   "BGM_TTL_MAIN_NIGHT",
                                                                   "BGM_BLF_MAIN_MORNING",
                                                                   "BGM_BLF_MAIN_NOON",
                                                                   "BGM_BLF_MAIN_EVENING",
                                                                   "BGM_BLF_MAIN_NIGHT",
                                                                   "BGM_DKB_DEMO_OPENING",
                                                                   "BGM_DKB_RESULT",
                                                                   "BGM_MTR_STAGE_01",
                                                                   "BGM_MTR_STAGE_02",
                                                                   "BGM_MTR_STAGE_03",
                                                                   "BGM_MTR_STAGE_05",
                                                                   "BGM_MTR_STAGE_06",
                                                                   "BGM_MTR_STAGE_07",
                                                                   "BGM_PKM_MAIN_SPRING",
                                                                   "BGM_PKM_MAIN_SUMMER",
                                                                   "BGM_PKM_MAIN_AUTUMN",
                                                                   "BGM_PKM_MAIN_WINTER",
                                                                   "BGM_PKM_MAIN_UNDER",
                                                                   "BGM_ZLD_YAMA",
                                                                   "BGM_ZLD_YAMA_YUGATA",
                                                                   "BGM_ZLD_YAMA_YUGATA_FIELD_6",
                                                                   "BGM_ZLD_TATEMONO",
                                                                   "BGM_ZLD_TATEMONO_PRE_BOSS"};
    const std::initializer_list<std::string_view> twister_race = {"BGM_FZR_MAIN_1_4",    "BGM_FZR_MAIN_5_8",
                                                                  "BGM_FZR_MAIN_9_12",   "BGM_FZR_MAIN_EX_1_4",
                                                                  "BGM_FZR_MAIN_EX_5_8", "BGM_FZR_MAIN_EX_9_12"};

    for (const std::string_view sound : nintendo_land)
    {
        Check(snd::StartingStreamTracks(kNintendoLand, sound) == 0x1, std::string(sound) + " plays track 0");
    }
    for (const std::string_view sound : twister_race)
    {
        Check(snd::StartingStreamTracks(kNintendoLand, sound) == kAll, std::string(sound) + " plays every track");
    }
    for (const std::string_view sound : {"BGM_AR_01", "BGM_MM_01", "BGM_SK_ENDLESS1", "BGM_BO_CHALLENGE"})
    {
        Check(snd::StartingStreamTracks(kGameAndWario, sound) == 0x1, std::string(sound) + " plays track 0");
    }
    for (const std::string_view sound : {"SM_BGM_BD_TH_01_SUNNY", "SM_BGM_BD_TH_02_SUNNY"})
    {
        Check(snd::StartingStreamTracks(kAmiiboFestival, sound) == 0x1, std::string(sound) + " plays track 0");
    }

    XSFLIB_CHECK(snd::StartingStreamTracks(kNintendoLand, "BGM_PKM_DEMO_KAERU") == kAll);
    XSFLIB_CHECK(snd::StartingStreamTracks(kGameAndWario, "BGM_SH_MU_AR") == kAll);
    XSFLIB_CHECK(snd::StartingStreamTracks(kAmiiboFestival, "SM_BGM_LOUNGE") == kAll);
    XSFLIB_CHECK(snd::StartingStreamTracks(kGameAndWario, "BGM_TTL_MAIN_DAYTIME") == kAll);
    XSFLIB_CHECK(snd::StartingStreamTracks(kNintendoLand, "bgm_ttl_main_daytime") == kAll);
    XSFLIB_CHECK(snd::StartingStreamTracks("0000000000000000000000000000000000000000", "BGM_TTL_MAIN_DAYTIME") == kAll);
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
    TestWaveSound();
    TestWaveSoundSettings();
    TestStreamSound();
    TestStreamTracks();
    TestStreamSnapshot();
    TestSnapshotPlayersOfOtherKinds();
    TestStreamTrackVolumes();
    TestStartingStreamTracks();
    TestMemoryUnmap();

    return xsflib::testing::Summary("cafe_snd_sounds");
}
