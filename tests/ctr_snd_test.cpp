// SPDX-License-Identifier: MIT

// Plays a small sound archive of the 3DS in archive mode (ntdlib/ctr/snd/engine.h), on the model of the DSP's audio
// firmware. Sound 0 plays one note on a constant wave. Sound 1 plays notes on a looped DSP-ADPCM wave, a PCM8 wave and
// the constant PCM16 wave at several keys: three at once, then one at a time. Sound 2 sends a note on the DSP-ADPCM
// wave to both aux buses. Callbacks on the buses change the samples there, and the firmware adds the buses back to the
// output. Sounds 3 to 5 are wave sounds, and sounds 6 to 10 stream sounds (see Archive). With a DSP firmware that the
// environment variable NTDLIB_CTR_FIRMWARE names, such as a game's dspaudio.cdc, the tests also run on Teakra running
// the firmware, and the model has to give Teakra's samples for the sounds. Without one, the model's samples have to
// hash to the record. ntdlib holds no firmware.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr/snd/sound_players.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace files = ntdlib::testing::sar_files;
namespace sar = ntdlib::sar;
using ntdlib::ctr::snd::ArchiveModel;
using ntdlib::ctr::snd::DspKind;
using ntdlib::ctr::snd::kNoVoice;

// The frames of a second of output: 160 samples each at about 32,728 Hz.
constexpr int kSecond = 205;

// The first 2.5 s of each sound's output, in values of both channels, and their recorded hashes (Hash), by sound. Sound
// 2 played with the aux buses' callbacks (SetAuxEffects), at full return volumes and with kLowerReturns.
constexpr std::size_t kHashedValues = 2 * 160 * 5 * kSecond / 2;
constexpr std::array<std::pair<uint32_t, uint64_t>, 9> kFirmwareHashes = {{{0, 0x4749cb75b1518f45},
                                                                           {1, 0x1d54b553beb75ad6},
                                                                           {3, 0xc7fe21f91967b7e4},
                                                                           {4, 0x06407603adbc30dc},
                                                                           {6, 0x1ff6e46ef088282a},
                                                                           {7, 0x1ff6e46ef088282a},
                                                                           {8, 0x6114a2f33abe9c25},
                                                                           {9, 0x6f295aa04b29822d},
                                                                           {10, 0x3e34c519ba2e27f3}}};
constexpr uint64_t kAuxHash = 0x88659fac062c0bf9;
constexpr uint64_t kLowerReturnsHash = 0xd5afca4418f2a6e9;

// A new return volume of an aux bus, set before frame `frame` of a sound's output.
struct ReturnChange
{
    int frame;
    int bus;
    float volume;
};

// Bus 0 returns at half its volume from 1 s on, and bus 1 at a quarter from 2 s on.
constexpr std::array<ReturnChange, 2> kLowerReturns = {{{kSecond, 0, 0.5f}, {2 * kSecond, 1, 0.25f}}};

// The archive's files of the stream sounds, which lie outside the archive: a looped stream, then one that plays once.
constexpr uint32_t kFirstStreamFile = 6;

// The stream sound whose track plays on the DSP's bus of two channels. In stereo, the firmware mixes that bus into no
// output.
constexpr uint32_t kTwoChannelBusSound = 8;

uint64_t FirmwareHash(uint32_t sound)
{
    for (const auto& [each, hash] : kFirmwareHashes)
    {
        if (each == sound)
        {
            return hash;
        }
    }

    return 0;
}

// The firmware's bytes, or nothing if the variable names no file that can be read.
std::optional<std::vector<uint8_t>> Firmware()
{
    const char* path = std::getenv("NTDLIB_CTR_FIRMWARE");
    if (path == nullptr || *path == '\0')
    {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A looped DSP-ADPCM wave of 400 frames of 14 samples, from a fixed run of numbers. Each frame's header picks a
// predictor of 0 to 7 and a scale of 0 to 11. The loop runs from frame 100 to the end.
files::WaveSpec AdpcmWave()
{
    std::vector<uint8_t> bytes(400 * 8);
    uint32_t state = 1;
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        state = state * 1103515245u + 12345u;
        const auto value = static_cast<uint8_t>(state >> 16);
        bytes[i] = i % 8 == 0 ? static_cast<uint8_t>((value & 0x70) | (value % 12)) : value;
    }

    files::WaveSpec wave;
    wave.encoding = 2;
    wave.loop = true;
    wave.loop_start = 100 * 14;
    wave.loop_end = 400 * 14;
    wave.channels = {bytes};

    return wave;
}

// A PCM8 wave of a sawtooth, 0.5 s long. It plays once.
files::WaveSpec Pcm8Wave()
{
    std::vector<uint8_t> bytes(16000);
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = static_cast<uint8_t>(3 * i);
    }

    files::WaveSpec wave;
    wave.encoding = 0;
    wave.loop_end = static_cast<uint32_t>(bytes.size());
    wave.channels = {bytes};

    return wave;
}

// A stream as the 3DS's tools lay one out in version 2.3.1.0, `samples` long: two channels of DSP-ADPCM at 32,728 Hz.
// Channel 0 holds a sawtooth and channel 1 a square wave. A loop from `loop_start` moves to the start of the next
// block.
std::vector<uint8_t> Stream(uint32_t samples, std::optional<uint32_t> loop_start)
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(samples));
    for (uint32_t i = 0; i < samples; i++)
    {
        channels[0][i] = static_cast<int16_t>(static_cast<int>(i % 100) * 120 - 6000);
        channels[1][i] = static_cast<int16_t>(i % 64 < 32 ? 3000 : -3000);
    }

    return sar::MakeStream(sar::Flavor::kCtr, 0x02030100, sar::WaveEncoding::kDspAdpcm, 32728, channels, loop_start)
        .Write();
}

// The stream files: 1 s with a loop from 8,000 samples, then 0.4 s without a loop.
const std::array<std::vector<uint8_t>, 2>& Streams()
{
    static const std::array<std::vector<uint8_t>, 2> streams = {Stream(160 * kSecond, 8000),
                                                                Stream(13091, std::nullopt)};

    return streams;
}

// The wave sound file of sounds 3 to 5 (see Archive). Its waves are the constant wave, the DSP-ADPCM wave and a wave
// that the wave archive lacks.
files::WaveSoundFileSpec WaveSounds()
{
    constexpr uint32_t kWarc = sar::kWaveArchiveItem << 24;
    files::WaveSoundFileSpec spec;
    spec.waves = {{kWarc, 0}, {kWarc, 1}, {kWarc, 9}};
    spec.sounds = {{.pan = 32,
                    .pitch = 1.5f,
                    .sends = {100, 40, 20},
                    .adshr = std::array<uint8_t, 5>{127, 127, 127, 0, 100},
                    .notes = {{.wave = 0}}},
                   {.pan = 100,
                    .span = 30,
                    .pitch = 0.75f,
                    .adshr = std::array<uint8_t, 5>{120, 127, 127, 0, 127},
                    .notes = {{.wave = 1}}},
                   {.notes = {{.wave = 2}}}};

    return spec;
}

// A stream sound's details: the tracks that play, two channels and `tracks`.
sar::StreamSoundInfo StreamDetails(uint16_t valid, std::vector<sar::StreamTrackInfo> tracks)
{
    sar::StreamSoundInfo stream;
    stream.valid_tracks = valid;
    stream.channel_count = 2;
    stream.tracks = std::move(tracks);

    return stream;
}

// A 3DS archive. Sound 0 plays key 60 at velocity 127 for 96 ticks on program 0, waits 96 ticks and ends: 2 s at the
// default tempo. Program 0 plays a constant PCM16 wave of 2 s. Sound 1 sets the pan to 48. With the note wait off, it
// starts key 64 on program 1, the DSP-ADPCM wave, key 55 at velocity 80 on program 2, the PCM8 wave, and key 72 on
// program 0, and waits 96 ticks. With the note wait on, it then plays keys 48, 60 and 67 on the three programs for 48
// ticks each. Sound 2 sets the send to aux bus A to 64 and the send to bus B to 100, plays key 64 on program 1 for 96
// ticks, waits 96 ticks and ends.
//
// Sound 3, a wave sound, plays the constant wave at 1.5 times its pitch, at pan 32, with sends of 100, 40 and 20, a
// channel priority of 70 and a release of 100. Sound 4 plays the DSP-ADPCM wave at 0.75 times its pitch, at pan 100
// and surround pan 30, with an attack of 120. Sound 5's wave is missing. Sounds 6 to 9 play the looped stream: sound 6
// on a track of both channels at volume 100, sound 7 with the front bypass, and sound 8 on the bus of two channels.
// Sound 9 has three tracks, and tracks 0 and 2 play: track 0 plays channel 0 at pan 20, track 1 channel 1, and track 2
// channel 0 at volume 60, pan 110 and surround pan 30. Sound 10 plays the stream without a loop on a track of both
// channels.
std::vector<uint8_t> Archive()
{
    constexpr sar::Flavor kCtr = sar::Flavor::kCtr;
    files::ArchiveSpec spec;
    using Sound = files::SoundSpec;
    const sar::StreamTrackInfo both{.channels = {0, 1}, .volume = 100};
    sar::StreamTrackInfo bypass = both;
    bypass.flags = 1;
    sar::StreamTrackInfo bus = both;
    bus.flags = 2;
    const std::vector<sar::StreamTrackInfo> three = {
        {.channels = {0}, .pan = 20}, {.channels = {1}}, {.channels = {0}, .volume = 60, .pan = 110, .span = 30}};
    spec.sounds = {{.name = "SEQ_NOTE", .file = 0, .banks = {0}},
                   {.name = "SEQ_NOTES", .file = 3, .banks = {0}},
                   {.name = "SEQ_SENDS", .file = 4, .banks = {0}},
                   {.name = "WSD_CONSTANT", .kind = Sound::kWaveSound, .file = 5, .channel_priority = 70},
                   {.name = "WSD_LOOP", .kind = Sound::kWaveSound, .file = 5, .wave_sound_index = 1},
                   {.name = "WSD_MISSING", .kind = Sound::kWaveSound, .file = 5, .wave_sound_index = 2},
                   {.name = "STM_STEREO", .kind = Sound::kStream, .file = 6, .stream = StreamDetails(1, {both})},
                   {.name = "STM_BYPASS", .kind = Sound::kStream, .file = 6, .stream = StreamDetails(1, {bypass})},
                   {.name = "STM_BUS", .kind = Sound::kStream, .file = 6, .stream = StreamDetails(1, {bus})},
                   {.name = "STM_TRACKS", .kind = Sound::kStream, .file = 6, .stream = StreamDetails(5, three)},
                   {.name = "STM_ONCE", .kind = Sound::kStream, .file = 7, .stream = StreamDetails(1, {both})}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    const std::vector<uint8_t> note = {0x3C, 0x7F, 0x60, 0x80, 0x60, 0xFF};
    const std::vector<uint8_t> notes = {0xC0, 0x30, 0xC7, 0x00, 0x81, 0x01, 0x40, 0x7F, 0x60, 0x81, 0x02, 0x37, 0x50,
                                        0x60, 0x81, 0x00, 0x48, 0x7F, 0x60, 0x80, 0x60, 0xC7, 0x01, 0x81, 0x01, 0x30,
                                        0x7F, 0x30, 0x81, 0x02, 0x3C, 0x7F, 0x30, 0x81, 0x00, 0x43, 0x7F, 0x30, 0xFF};
    const std::vector<uint8_t> sends = {0xD9, 0x40, 0xDA, 0x64, 0x81, 0x01, 0x40, 0x7F, 0x60, 0x80, 0x60, 0xFF};
    spec.files = {{files::Sequence(kCtr, note, {{"SEQ_NOTE", 0}})},
                  {files::Bank(kCtr, files::OneRegionBank(3))},
                  {files::WaveArchive(kCtr, {files::Wave(kCtr, files::ConstantWave(false, 64000)),
                                             files::Wave(kCtr, AdpcmWave()), files::Wave(kCtr, Pcm8Wave())})},
                  {files::Sequence(kCtr, notes, {{"SEQ_NOTES", 0}})},
                  {files::Sequence(kCtr, sends, {{"SEQ_SENDS", 0}})},
                  {files::WaveSoundFile(kCtr, WaveSounds())},
                  {.external = "stream/STM_LOOP.bcstm"},
                  {.external = "stream/STM_ONCE.bcstm"}};

    return files::Archive(kCtr, spec);
}

// An archive whose one sequence starts 100 notes at once, with the note wait off: keys 10 to 109 at velocity 30 for 96
// ticks on the constant wave. A wait of 96 ticks follows.
std::vector<uint8_t> ChordArchive()
{
    constexpr sar::Flavor kCtr = sar::Flavor::kCtr;
    std::vector<uint8_t> chord = {0xC7, 0x00};
    for (uint8_t key = 10; key < 110; key++)
    {
        chord.insert(chord.end(), {key, 0x1E, 0x60});
    }
    chord.insert(chord.end(), {0x80, 0x60, 0xFF});

    files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_CHORD", .file = 0, .banks = {0}}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{files::Sequence(kCtr, chord, {{"SEQ_CHORD", 0}})},
                  {files::Bank(kCtr, files::OneRegionBank())},
                  {files::WaveArchive(kCtr, {files::Wave(kCtr, files::ConstantWave(false, 64000))})}};

    return files::Archive(kCtr, spec);
}

// Starts sound `sound` of `archive`, of whichever kind. A stream sound plays the stream of its file. Returns whether it
// started.
bool StartSound(ArchiveModel& model, const sar::SoundArchive& archive, uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    switch (info.type)
    {
    case sar::SoundType::kSequence:
        return model.engine->StartSequence(sound) != nullptr;
    case sar::SoundType::kWaveSound:
        return model.engine->StartWaveSound(sound) != nullptr;
    case sar::SoundType::kStream:
        return model.engine->StartStreamSound(sound, Streams()[info.file_id - kFirstStreamFile]) != nullptr;
    default:
        return false;
    }
}

// A model on `kind` whose DSP's output goes to `out`, started on `firmware` with `archive`.
std::unique_ptr<ArchiveModel> Start(DspKind kind, std::span<const uint8_t> firmware, const sar::SoundArchive& archive,
                                    std::vector<int16_t>& out)
{
    auto model = std::make_unique<ArchiveModel>(kind);
    model->dsp->SetSampleSink([&out](std::array<int16_t, 2> sample)
                              { out.insert(out.end(), sample.begin(), sample.end()); });
    XSFLIB_CHECK(model->Start(firmware, archive));

    return model;
}

// The 64-bit FNV-1a hash of `values`' bytes, low byte first.
uint64_t Hash(std::span<const int16_t> values)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (const int16_t value : values)
    {
        for (const int shift : {0, 8})
        {
            hash = (hash ^ ((static_cast<uint16_t>(value) >> shift) & 0xff)) * 0x100000001b3;
        }
    }

    return hash;
}

// The aux buses' callbacks of the tests. Bus A's channels come back reversed in time and halved. Bus B's come back
// negated, each in the place of the next channel.
void SetAuxEffects(ArchiveModel& model)
{
    model.snd.SetAuxCallback(0,
                             [](const std::array<int32_t*, 4>& channels, std::size_t samples)
                             {
                                 for (int32_t* const channel : channels)
                                 {
                                     std::reverse(channel, channel + samples);
                                     std::transform(channel, channel + samples, channel,
                                                    [](int32_t sample) { return sample / 2; });
                                 }
                             });
    model.snd.SetAuxCallback(1,
                             [](const std::array<int32_t*, 4>& channels, std::size_t samples)
                             {
                                 for (std::size_t i = 0; i < samples; i++)
                                 {
                                     const int32_t last = channels[3][i];
                                     channels[3][i] = -channels[2][i];
                                     channels[2][i] = -channels[1][i];
                                     channels[1][i] = -channels[0][i];
                                     channels[0][i] = -last;
                                 }
                             });
}

// The output of sound `sound` for 3 s of frames, on `kind`, with the aux buses' callbacks when `aux` is set. The buses'
// return volumes change as `changes` say.
std::vector<int16_t> Play(DspKind kind, std::span<const uint8_t> firmware, const sar::SoundArchive& archive,
                          uint32_t sound, bool aux = false, std::span<const ReturnChange> changes = {})
{
    std::vector<int16_t> out;
    const std::unique_ptr<ArchiveModel> model = Start(kind, firmware, archive, out);
    if (aux)
    {
        SetAuxEffects(*model);
    }

    XSFLIB_CHECK(StartSound(*model, archive, sound));
    for (int i = 0; i < 3 * kSecond; i++)
    {
        for (const ReturnChange& change : changes)
        {
            if (change.frame == i)
            {
                model->snd.SetAuxReturnVolume(change.bus, change.volume);
            }
        }
        XSFLIB_CHECK(model->engine->RunFrame());
    }

    return out;
}

bool Sounds(std::span<const int16_t> samples)
{
    for (const int16_t sample : samples)
    {
        if (sample != 0)
        {
            return true;
        }
    }

    return false;
}

// The note sounds, the sequence ends and the channel stops. Two models play the same samples.
void TestPlay(DspKind kind, std::span<const uint8_t> firmware, const sar::SoundArchive& archive)
{
    std::vector<int16_t> first;
    std::vector<int16_t> second;
    const std::unique_ptr<ArchiveModel> a = Start(kind, firmware, archive, first);
    const std::unique_ptr<ArchiveModel> b = Start(kind, firmware, archive, second);
    XSFLIB_CHECK(a->engine->StartSequence(0) != nullptr && b->engine->StartSequence(0) != nullptr);
    XSFLIB_CHECK(a->engine->StartSequence(3) == nullptr);

    int frames = 0;
    while (frames < 4 * kSecond && a->engine->IsBusy())
    {
        XSFLIB_CHECK(a->engine->RunFrame() && b->engine->RunFrame());
        frames++;
    }

    XSFLIB_CHECK(!a->engine->IsBusy() && frames >= 2 * kSecond && frames < 3 * kSecond);
    XSFLIB_CHECK(Sounds(first) && first == second);
}

// A restore of a snapshot plays the frames of sound `sound` after it again, sample for sample.
void TestSnapshot(DspKind kind, std::span<const uint8_t> firmware, const sar::SoundArchive& archive, uint32_t sound = 1)
{
    std::vector<int16_t> out;
    const std::unique_ptr<ArchiveModel> model = Start(kind, firmware, archive, out);
    XSFLIB_CHECK(StartSound(*model, archive, sound));
    for (int i = 0; i < 20; i++)
    {
        XSFLIB_CHECK(model->engine->RunFrame());
    }

    const ArchiveModel::Snapshot snapshot = model->Save(nullptr);
    out.clear();
    for (int i = 0; i < 40; i++)
    {
        XSFLIB_CHECK(model->engine->RunFrame());
    }

    const std::vector<int16_t> after = out;

    model->Restore(snapshot);
    out.clear();
    for (int i = 0; i < 40; i++)
    {
        XSFLIB_CHECK(model->engine->RunFrame());
    }

    XSFLIB_CHECK(Sounds(after) && out == after);
}

// A restore can bring back voices that an earlier restore took away. In the enhanced mode, the chord's 100 notes take
// 100 voices of nw::snd and 100 of nn::snd. After a restore of a snapshot from before the chord, a restore of a
// snapshot from during it plays the frames after it again, sample for sample.
void TestRestoreOfGrownVoices()
{
    const std::vector<uint8_t> bytes = ChordArchive();
    const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);
    ArchiveModel model(DspKind::kEnhancedModel);
    std::vector<float> out;
    const auto keep = [&out](std::span<const float> frame)
    {
        out.insert(out.end(), frame.begin(), frame.end());
    };
    model.dsp->SetFrameSink(keep);

    XSFLIB_CHECK(model.Start(archive) && model.engine->StartSequence(0) != nullptr);
    const ArchiveModel::Snapshot before = model.Save(nullptr);
    for (int i = 0; i < 20; i++)
    {
        XSFLIB_CHECK(model.engine->RunFrame());
    }

    const ArchiveModel::Snapshot during = model.Save(&before);
    out.clear();
    for (int i = 0; i < 40; i++)
    {
        XSFLIB_CHECK(model.engine->RunFrame());
    }

    const std::vector<float> after = out;

    model.Restore(before);
    XSFLIB_CHECK(model.snd.VoiceCount() == ntdlib::ctr::nnsnd::kNumSources);
    model.Restore(during);
    XSFLIB_CHECK(model.snd.VoiceCount() == 100);
    out.clear();
    for (int i = 0; i < 40; i++)
    {
        XSFLIB_CHECK(model.engine->RunFrame());
    }

    XSFLIB_CHECK(std::any_of(after.begin(), after.end(), [](float sample) { return sample != 0.0f; }));
    XSFLIB_CHECK(out == after);
}

// A muted track plays on, silent. Once it's unmuted, the output catches up with a track that was never muted within a
// few frames: the DSP takes the frame's parameters in its next frame, and its output lags its mix. The two streams of
// samples then match. The frames' ends can fall a sample apart in them for a while.
void TestMute(DspKind kind, std::span<const uint8_t> firmware, const sar::SoundArchive& archive)
{
    std::vector<int16_t> muted_out;
    std::vector<int16_t> plain_out;
    const std::unique_ptr<ArchiveModel> muted = Start(kind, firmware, archive, muted_out);
    const std::unique_ptr<ArchiveModel> plain = Start(kind, firmware, archive, plain_out);
    const int player = muted->engine->StartSequence(0)->Id();
    XSFLIB_CHECK(plain->engine->StartSequence(0) != nullptr);
    muted->engine->MuteTrack(player, 0, true);
    XSFLIB_CHECK(muted->engine->IsTrackMuted(player, 0) && !muted->engine->IsTrackMuted(player, 1));

    for (int i = 0; i < 100; i++)
    {
        XSFLIB_CHECK(muted->engine->RunFrame() && plain->engine->RunFrame());
        XSFLIB_CHECK(muted->engine->Channels().ActiveCount() == plain->engine->Channels().ActiveCount());
    }

    XSFLIB_CHECK(!Sounds(muted_out) && Sounds(plain_out));

    // 8 frames after the unmute, the streams match.
    muted->engine->MuteTrack(player, 0, false);
    const std::size_t settled = plain_out.size() + 8 * 2 * 160;
    for (int i = 0; i < 60; i++)
    {
        XSFLIB_CHECK(muted->engine->RunFrame() && plain->engine->RunFrame());
    }

    const std::size_t end = std::min(muted_out.size(), plain_out.size());
    XSFLIB_CHECK(end > settled + 40 * 2 * 160);
    const auto from = static_cast<std::ptrdiff_t>(settled);
    const auto to = static_cast<std::ptrdiff_t>(end);
    XSFLIB_CHECK(Sounds(std::span(plain_out).subspan(settled, end - settled)) &&
                 std::equal(muted_out.begin() + from, muted_out.begin() + to, plain_out.begin() + from));
}

// A track's meter follows its notes. Sound 1 plays every note on track 0, and the other tracks' meters stay at 0. Track
// 0's peaks follow the frames that the DSP mixed. The limiter of the output squeezes the loud frames, and the quiet
// ones come out at the meter's level. The meters change nothing in the output. Each frame that the DSP mixes comes out
// of the output later.
void TestMeters(const sar::SoundArchive& archive)
{
    std::vector<int16_t> metered_out;
    std::vector<int16_t> plain_out;
    const std::unique_ptr<ArchiveModel> metered = Start(DspKind::kModel, {}, archive, metered_out);
    const std::unique_ptr<ArchiveModel> plain = Start(DspKind::kModel, {}, archive, plain_out);
    const int player = metered->engine->StartSequence(1)->Id();
    XSFLIB_CHECK(plain->engine->StartSequence(1) != nullptr);
    metered->engine->MeterTracks(player);

    int quiet_frames = 0;
    std::vector<int16_t> mixed;
    for (int i = 0; i < 3 * kSecond; i++)
    {
        XSFLIB_CHECK(metered->engine->RunFrame() && plain->engine->RunFrame());
        XSFLIB_CHECK(metered->engine->TrackPeaks(1) == (std::array<int32_t, 2>{0, 0}));

        // A frame between 1,000 and 8,000 comes out within 2 % of its meter.
        const std::span<const int16_t> frame = metered->dsp->MixedFrame();
        XSFLIB_CHECK(frame.size() == 2 * 160);
        mixed.insert(mixed.end(), frame.begin(), frame.end());
        const std::array<int32_t, 2> peaks = metered->engine->TrackPeaks(0);
        int32_t loudest = 0;
        for (const int16_t sample : frame)
        {
            loudest = std::max(loudest, std::abs(int32_t{sample}));
        }

        const int32_t meter = std::max(peaks[0], peaks[1]);
        if (loudest >= 1000 && loudest <= 8000)
        {
            quiet_frames++;
            XSFLIB_CHECK(std::abs(meter - loudest) <= loudest / 50);
        }
    }

    XSFLIB_CHECK(quiet_frames > kSecond / 2);
    XSFLIB_CHECK(metered_out == plain_out);

    // The mixed frames come out a few frames later.
    bool found = false;
    for (std::size_t offset = 0; offset < 8 * 2 * 160 && !found; offset += 2)
    {
        const std::size_t count = std::min(mixed.size(), metered_out.size() - offset) - 4 * 2 * 160;
        found = std::equal(mixed.begin(), mixed.begin() + static_cast<std::ptrdiff_t>(count),
                           metered_out.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    XSFLIB_CHECK(found);
}

// The frames that sound `sound` plays for before the engine has nothing more to do, up to 4 s, and its output.
int PlayToEnd(const sar::SoundArchive& archive, uint32_t sound, std::vector<int16_t>& out)
{
    const std::unique_ptr<ArchiveModel> model = Start(DspKind::kModel, {}, archive, out);
    XSFLIB_CHECK(StartSound(*model, archive, sound));
    int frames = 0;
    while (frames < 4 * kSecond && model->engine->IsBusy())
    {
        XSFLIB_CHECK(model->engine->RunFrame());
        frames++;
    }

    return frames;
}

// A wave sound starts its channel in its first frame, at its channel priority plus 64, and plays the wave once. At 1.5
// times its pitch, the constant wave of 2 s ends after 1.33 s, and the player finishes in the next frame. A looped wave
// plays on. A wave sound whose wave is missing finishes in its first frame. The starts refuse a sound of another kind.
void TestWaveSounds(const sar::SoundArchive& archive)
{
    std::vector<int16_t> out;
    const std::unique_ptr<ArchiveModel> model = Start(DspKind::kModel, {}, archive, out);
    ntdlib::ctr::snd::WaveSoundPlayer* player = model->engine->StartWaveSound(3);
    XSFLIB_CHECK(player != nullptr && model->engine->StartWaveSound(0) == nullptr &&
                 model->engine->StartWaveSound(6) == nullptr && model->engine->StartSequence(3) == nullptr);
    XSFLIB_CHECK(player->IsActive() && player->ChannelPlace() == ntdlib::nwsnd::kNoChannel);
    XSFLIB_CHECK(model->engine->RunFrame() && player->ChannelPlace() != ntdlib::nwsnd::kNoChannel);
    const ntdlib::ctr::snd::Channel& channel = model->engine->Channels().At(player->ChannelPlace());
    XSFLIB_CHECK(channel.voice != kNoVoice && model->engine->Voices().At(channel.voice).priority_ == 70 + 64);
    XSFLIB_CHECK(channel.owner_player == player->Id() && channel.owner_track == 0);

    std::vector<int16_t> constant;
    const int frames = PlayToEnd(archive, 3, constant);
    XSFLIB_CHECK(Sounds(constant) && frames > 4 * kSecond / 3 && frames < 4 * kSecond / 3 + 8);

    std::vector<int16_t> looped;
    XSFLIB_CHECK(PlayToEnd(archive, 4, looped) == 4 * kSecond && Sounds(looped));

    std::vector<int16_t> missing;
    const std::unique_ptr<ArchiveModel> other = Start(DspKind::kModel, {}, archive, missing);
    ntdlib::ctr::snd::WaveSoundPlayer* silent = other->engine->StartWaveSound(5);
    XSFLIB_CHECK(silent != nullptr && other->engine->RunFrame() && silent->IsFinished() && !other->engine->IsBusy());
}

// A stream sound plays each channel on a voice of one channel at the highest priority. The voices of the tracks that
// play start, and the stream plays on through its loop. A track that doesn't play leaves its channel's voice unstarted.
// A stream without a loop finishes once its voices have played it: 0.4 s. A track whose flags send its voices to the
// bus of two channels is silent in stereo, and its voices play. The starts refuse a sound of another kind.
void TestStreams(const sar::SoundArchive& archive)
{
    std::vector<int16_t> out;
    const std::unique_ptr<ArchiveModel> model = Start(DspKind::kModel, {}, archive, out);
    XSFLIB_CHECK(model->engine->StartStreamSound(3, Streams()[0]) == nullptr);
    ntdlib::ctr::snd::StreamSoundPlayer* player = model->engine->StartStreamSound(9, Streams()[0]);
    XSFLIB_CHECK(player != nullptr && player->ChannelCount() == 2);
    for (int i = 0; i < kSecond; i++)
    {
        XSFLIB_CHECK(model->engine->RunFrame());
    }

    XSFLIB_CHECK(player->IsActive() && player->ChannelVoice(0) != kNoVoice && player->ChannelVoice(1) != kNoVoice);
    const ntdlib::ctr::snd::Voice& first = model->engine->Voices().At(player->ChannelVoice(0));
    const ntdlib::ctr::snd::Voice& second = model->engine->Voices().At(player->ChannelVoice(1));
    XSFLIB_CHECK(first.started_ && first.priority_ == 255 && !second.started_ && Sounds(out));

    std::vector<int16_t> looped;
    XSFLIB_CHECK(PlayToEnd(archive, 6, looped) == 4 * kSecond && Sounds(looped));

    std::vector<int16_t> once;
    const int frames = PlayToEnd(archive, 10, once);
    XSFLIB_CHECK(Sounds(once) && frames > 2 * kSecond / 5 && frames < 2 * kSecond / 5 + 8);

    std::vector<int16_t> bus_out;
    const std::unique_ptr<ArchiveModel> bus = Start(DspKind::kModel, {}, archive, bus_out);
    ntdlib::ctr::snd::StreamSoundPlayer* on_bus = bus->engine->StartStreamSound(8, Streams()[0]);
    for (int i = 0; i < kSecond; i++)
    {
        XSFLIB_CHECK(bus->engine->RunFrame());
    }

    XSFLIB_CHECK(on_bus != nullptr && on_bus->IsActive() && !Sounds(bus_out));
    XSFLIB_CHECK(on_bus != nullptr && bus->engine->Voices().At(on_bus->ChannelVoice(0)).started_);
}

// The model plays sound `sound` as Teakra running `firmware` plays it, sample for sample, for 3 s. The frames' ends can
// fall a few samples apart: the model's frames end at about the firmware's times. Every sound sounds but the stream on
// the bus of two channels. Teakra's samples also hash to the record.
void TestModelMatchesFirmware(std::span<const uint8_t> firmware, const sar::SoundArchive& archive, uint32_t sound)
{
    const std::vector<int16_t> model_out = Play(DspKind::kModel, {}, archive, sound);
    const std::vector<int16_t> teakra_out = Play(DspKind::kTeakra, firmware, archive, sound);
    const std::size_t end = std::min(model_out.size(), teakra_out.size());
    const auto to = static_cast<std::ptrdiff_t>(end);
    XSFLIB_CHECK(std::max(model_out.size(), teakra_out.size()) - end < 2 * 64);
    XSFLIB_CHECK(Sounds(teakra_out) == (sound != kTwoChannelBusSound) &&
                 std::equal(model_out.begin(), model_out.begin() + to, teakra_out.begin()));
    XSFLIB_CHECK(end >= kHashedValues &&
                 Hash(std::span(teakra_out).first(std::min(end, kHashedValues))) == FirmwareHash(sound));
}

// Without a firmware, the model's first 2.5 s of sound `sound` hash to the record.
void TestModelMatchesRecord(const sar::SoundArchive& archive, uint32_t sound)
{
    const std::vector<int16_t> out = Play(DspKind::kModel, {}, archive, sound);
    XSFLIB_CHECK(out.size() >= kHashedValues);
    XSFLIB_CHECK(Hash(std::span(out).first(std::min(out.size(), kHashedValues))) == FirmwareHash(sound));
}

// The aux buses' callbacks change the output of sound 2, and the model's first 2.5 s hash to the record. Callbacks
// that are set and then taken away leave the buses off. A return volume of 0 returns nothing. A lower volume during
// playback changes the output after its frame, and the output hashes to the record. With `firmware`, the model plays
// as Teakra plays.
void TestAux(std::span<const uint8_t> firmware, const sar::SoundArchive& archive)
{
    const std::vector<int16_t> dry = Play(DspKind::kModel, {}, archive, 2);
    const std::vector<int16_t> wet = Play(DspKind::kModel, {}, archive, 2, true);
    XSFLIB_CHECK(Sounds(dry) && wet.size() >= kHashedValues && wet != dry);
    XSFLIB_CHECK(Hash(std::span(wet).first(std::min(wet.size(), kHashedValues))) == kAuxHash);

    // The buses' samples come back two frames after the firmware mixed them: 320 samples of both channels.
    const auto first_sound = std::find_if(dry.begin(), dry.end(), [](int16_t sample) { return sample != 0; });
    const auto first_return = std::mismatch(dry.begin(), dry.end(), wet.begin()).first;
    XSFLIB_CHECK(first_return - first_sound == 2 * 2 * 160);

    std::vector<int16_t> off;
    const std::unique_ptr<ArchiveModel> model = Start(DspKind::kModel, {}, archive, off);
    SetAuxEffects(*model);
    model->snd.SetAuxCallback(0, {});
    model->snd.SetAuxCallback(1, {});
    XSFLIB_CHECK(model->engine->StartSequence(2) != nullptr);
    for (int i = 0; i < 3 * kSecond; i++)
    {
        XSFLIB_CHECK(model->engine->RunFrame());
    }

    XSFLIB_CHECK(off == dry);

    constexpr std::array<ReturnChange, 2> kSilent = {{{0, 0, 0.0f}, {0, 1, 0.0f}}};
    XSFLIB_CHECK(Play(DspKind::kModel, {}, archive, 2, true, kSilent) == dry);
    const std::vector<int16_t> lower = Play(DspKind::kModel, {}, archive, 2, true, kLowerReturns);
    const auto first_change = std::mismatch(wet.begin(), wet.end(), lower.begin(), lower.end()).first;
    XSFLIB_CHECK(first_change != wet.end() && first_change - wet.begin() >= 2 * 160 * kSecond);
    XSFLIB_CHECK(lower.size() >= kHashedValues);
    XSFLIB_CHECK(Hash(std::span(lower).first(std::min(lower.size(), kHashedValues))) == kLowerReturnsHash);

    if (!firmware.empty())
    {
        for (const auto& [model_out, changes] : {std::pair(&wet, std::span<const ReturnChange>()),
                                                 std::pair(&lower, std::span<const ReturnChange>(kLowerReturns))})
        {
            const std::vector<int16_t> teakra = Play(DspKind::kTeakra, firmware, archive, 2, true, changes);
            const std::size_t end = std::min(model_out->size(), teakra.size());
            XSFLIB_CHECK(std::max(model_out->size(), teakra.size()) - end < 2 * 64);
            XSFLIB_CHECK(
                std::equal(model_out->begin(), model_out->begin() + static_cast<std::ptrdiff_t>(end), teakra.begin()));
        }
    }
}

} // namespace

int main()
{
    const std::vector<uint8_t> bytes = Archive();
    const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, ntdlib::ctr::kSarOptions);
    TestPlay(DspKind::kModel, {}, archive);

    for (const uint32_t sound : {1, 4, 6})
    {
        TestSnapshot(DspKind::kModel, {}, archive, sound);
    }

    TestRestoreOfGrownVoices();
    TestMute(DspKind::kModel, {}, archive);
    TestMeters(archive);
    TestWaveSounds(archive);
    TestStreams(archive);

    for (const auto& [sound, hash] : kFirmwareHashes)
    {
        TestModelMatchesRecord(archive, sound);
    }

    // Teakra too, on a firmware.
    const std::optional<std::vector<uint8_t>> firmware = Firmware();
    TestAux(firmware ? std::span<const uint8_t>(*firmware) : std::span<const uint8_t>(), archive);
    if (firmware)
    {
        TestPlay(DspKind::kTeakra, *firmware, archive);

        for (const uint32_t sound : {1, 4, 6})
        {
            TestSnapshot(DspKind::kTeakra, *firmware, archive, sound);
        }

        TestMute(DspKind::kTeakra, *firmware, archive);

        for (const auto& [sound, hash] : kFirmwareHashes)
        {
            TestModelMatchesFirmware(*firmware, archive, sound);
        }
    }
    else
    {
        std::printf("NTDLIB_CTR_FIRMWARE names no DSP firmware that can be read: the model plays alone\n");
    }

    return xsflib::testing::Summary("3DS archive model");
}
