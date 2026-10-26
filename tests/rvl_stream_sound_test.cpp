// SPDX-License-Identifier: MIT

// Tests of NW4R SND's stream sounds on the model of the Wii (rvl/snd/stream_sound.h), on small archives and streams
// built here. The builds before March 2008 play the stream's first two channels on one voice. The builds from March
// 2008 play the tracks that the sound's entry allocates, each on a voice of its channels, with the stream file's volume
// and pan of the track. A voice plays its channels from the stream's start, with the history of the DSP-ADPCM info and
// the first frame's predictor, and loops to the start of the block that holds the loop's start, with the info's state
// for the loop. A stream without a loop ends the sound in the frame after its voices end. A snapshot keeps the player.
// An entry that the archive's sound table doesn't hold plays as the table's entry does.
// The expected values are the ones that NW4R SND's StrmPlayer and SoundArchiveFileReader give in the code of Wii Sports
// and Mario Kart Wii (see stream_sound.h).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/stream_sound.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_streams.h"

namespace
{

using namespace ntdlib;
namespace snd = ntdlib::rvl::snd;
namespace ax = ntdlib::rvl::ax;
namespace streams = ntdlib::testing::rsar_streams;
using xsflib::testing::Check;

// The build of 2007 that Wii Sports links, and the build of 2008 that Mario Kart Wii links.
constexpr SndBuild k2007{20070608, false};
constexpr SndBuild k2008{20080308, false};

// `count` samples of a tone whose period of 100 samples divides each loop below. The channels start at other phases.
std::vector<int16_t> Tone(std::size_t count, int channel)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        samples[i] = static_cast<int16_t>(
            std::lround(9000.0 * std::sin((static_cast<double>(i) + 13.0 * channel) * 2.0 * 3.14159265358979 / 100.0)));
    }

    return samples;
}

// A stream sound of an archive of `version`: its volume, and the channels and tracks of its entry.
struct EntrySpec
{
    uint16_t version = 0x0104;
    uint8_t volume = 127;
    uint16_t channels = 2;
    uint16_t tracks = 1;
};

// An archive of a stream sound and a sequence sound with a bank. The stream sound's file lies beside the archive.
struct TestArchive
{
    explicit TestArchive(const EntrySpec& entry) : bytes(Build(entry)), archive(Load(bytes))
    {
    }

    static std::vector<uint8_t> Build(const EntrySpec& entry)
    {
        rsar::ArchiveModel model;
        model.version = entry.version;
        model.players = {{"PLAYER_BGM", 1, 0}};
        model.banks = {{"BANK_OTHER", 1}};
        model.files = {{.path = "stream/BGM.brstm", .path_size = 0x1000}, {.bytes = std::vector<uint8_t>(32, 0)}};
        rsar::SoundModel stream;
        stream.name = "STRM_BGM";
        stream.type = rsar::SoundType::kStream;
        stream.volume = entry.volume;
        stream.allocate_channel_count = entry.channels;
        stream.stream_track_flags = entry.tracks;
        rsar::SoundModel sequence;
        sequence.name = "SEQ_OTHER";
        sequence.file_id = 1;
        model.sounds = {stream, sequence};

        return rsar::BuildArchive(model);
    }

    static rsar::SoundArchive Load(const std::vector<uint8_t>& bytes)
    {
        return rsar::SoundArchive::Load(bytes, snd::kRsarOptions);
    }

    std::vector<uint8_t> bytes;
    rsar::SoundArchive archive;
};

// A model of `build` on the renderer, with stream sound 0 of `test` started on `stream`.
struct Playing
{
    Playing(const TestArchive& test, SndBuild build, const std::vector<uint8_t>& stream)
    {
        model->build = build;
        model->StartWithRenderer(test.archive);
        player = model->engine->StartStreamSound(0, stream);
    }

    void Run(int frames)
    {
        for (int frame = 0; frame < frames; frame++)
        {
            model->RunFrame(output);
        }
    }

    // The PB of channel `channel` of track `track`'s voice.
    ax::Pb PbOf(int track, std::size_t channel) const
    {
        const snd::VoiceId voice = player->TrackVoice(track);
        const std::vector<uint32_t> indices = model->engine->Voices().At(voice).AxVoiceIndices();

        return model->ax.ReadPb(indices.at(channel));
    }

    std::unique_ptr<snd::ArchiveModel> model = std::make_unique<snd::ArchiveModel>();
    snd::StrmPlayer* player = nullptr;
    std::vector<int16_t> output;
};

// The volume that the envelope of a voice of `gain` reaches. In the voice's first frame, the volume ramps from 0 in 96
// steps of a 96th of the target, rounded towards 0. It then stays where the ramp ends.
int RampedVolume(float gain)
{
    return static_cast<int>(32767.0f * gain) / 96 * 96;
}

// The nibble of DSP-ADPCM sample `sample` from the channel's start.
uint32_t NibbleOf(uint32_t sample)
{
    return snd::AxVoice::DspAddressBySample(0, sample, ax::SampleFormat::kAdpcm);
}

// The build of 2007 plays the stream's first two channels on one voice of the highest priority, whatever the entry
// allocates. Each channel's PB starts at the channel's first sample with the info's history and the first frame's
// predictor. It loops to the start of the loop's block with the info's state for the loop, and ends at the stream's
// last sample. The model without the DSP leaves the PBs as the engine sets them. The voice takes the entry's volume and
// keeps the dual pan mode: each channel plays at its side.
void CheckVoiceOf2007()
{
    streams::StreamSpec spec;
    spec.loop_start = 14336;
    spec.channels = {Tone(30000, 0), Tone(30000, 1), Tone(30000, 2)};
    spec.tracks = {{100, 30, {0, 1}}, {127, 64, {2}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const rsar::StreamFile stream = rsar::StreamFile::Read(file);
    const TestArchive test({.version = 0x0101, .volume = 100, .channels = 7, .tracks = 3});

    auto model = std::make_unique<snd::ArchiveModel>();
    model->build = k2007;
    model->StartWithoutDsp(test.archive);
    snd::StrmPlayer* player = model->engine->StartStreamSound(0, file);
    Check(player != nullptr && player->TrackCount() == 1, "the build of 2007 plays one track");
    if (player == nullptr)
    {
        return;
    }

    model->RunFrameWithoutDsp();
    model->RunFrameWithoutDsp();
    const snd::VoiceId voice = player->TrackVoice(0);
    Check(voice != snd::kNoVoice && model->engine->Voices().At(voice).AxVoiceCount() == 2 &&
              model->engine->Voices().At(voice).Priority() == snd::Voice::kPriorityMax,
          "one voice of two channels at the highest priority");
    if (voice == snd::kNoVoice)
    {
        return;
    }

    const std::vector<uint32_t> indices = model->engine->Voices().At(voice).AxVoiceIndices();
    for (std::size_t c = 0; c < indices.size(); c++)
    {
        const ax::Pb pb = model->ax.ReadPb(indices[c]);
        const rsar::DspAdpcmInfo& info = *stream.channels[c].adpcm;
        const uint32_t loop = ax::Join(pb.addr.loop_address_hi, pb.addr.loop_address_lo);
        const uint32_t end = ax::Join(pb.addr.end_address_hi, pb.addr.end_address_lo);
        const uint32_t base = loop - NibbleOf(14336);
        const std::string name = "channel " + std::to_string(c);
        Check(pb.addr.format == static_cast<uint16_t>(ax::SampleFormat::kAdpcm) && pb.addr.loop_flag == 1,
              name + ": DSP-ADPCM that loops");
        Check(base % 16 == 0 && end - base == NibbleOf(29999), name + ": the loop's block and the last sample");
        Check(std::equal(info.coefs.begin(), info.coefs.end(), pb.adpcm.a.begin(),
                         [](int16_t a, uint16_t b) { return static_cast<uint16_t>(a) == b; }),
              name + ": the coefficients");
        Check(pb.adpcm.pred_scale == stream.channels[c].samples.front() &&
                  pb.adpcm.yn1 == static_cast<uint16_t>(info.yn1) && pb.adpcm.yn2 == static_cast<uint16_t>(info.yn2),
              name + ": the first frame's predictor and the info's history");
        Check(pb.adpcm_loop.loop_pred_scale == info.loop_pred_scale &&
                  pb.adpcm_loop.loop_yn1 == static_cast<uint16_t>(info.loop_yn1) &&
                  pb.adpcm_loop.loop_yn2 == static_cast<uint16_t>(info.loop_yn2),
              name + ": the state for the loop");
        Check(pb.type == static_cast<uint16_t>(ax::VoiceType::kNormal), name + ": the voice's type");
    }

    // On the renderer, the voices take the entry's volume and their sides.
    Playing playing(test, k2007, file);
    playing.Run(3);
    for (std::size_t c = 0; c < 2; c++)
    {
        const ax::Pb pb = playing.PbOf(0, c);
        const std::string name = "channel " + std::to_string(c);
        const int volume = pb.ve.current_volume;
        Check(volume == RampedVolume(100.0f / 127.0f), name + ": the entry's volume (" + std::to_string(volume) + ")");
        Check(c == 0 ? pb.mix.l != 0 && pb.mix.r == 0 : pb.mix.l == 0 && pb.mix.r != 0, name + ": its side");
    }
}

// The builds from March 2008 play each track that the entry allocates on a voice of the track's channels, with the
// stream file's volume and pan of the track. An archive of version 1.4 gives the channels as a count.
void CheckTracksOf2008()
{
    streams::StreamSpec spec;
    spec.channels = {Tone(20000, 0), Tone(20000, 1), Tone(20000, 2), Tone(20000, 3)};
    spec.tracks = {{100, 64, {0, 1}}, {127, 64, {2, 3}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const TestArchive test({.version = 0x0104, .volume = 127, .channels = 4, .tracks = 3});
    Playing playing(test, k2008, file);
    Check(playing.player != nullptr && playing.player->TrackCount() == 2, "the build of 2008 plays two tracks");
    if (playing.player == nullptr)
    {
        return;
    }

    playing.Run(3);
    const ax::Pb first = playing.PbOf(0, 0);
    const ax::Pb third = playing.PbOf(1, 0);
    const int quiet = first.ve.current_volume;
    const int loud = third.ve.current_volume;
    Check(quiet == RampedVolume(100.0f / 127.0f) && loud == RampedVolume(1.0f),
          "each track's volume (" + std::to_string(quiet) + " and " + std::to_string(loud) + ")");

    // Each voice plays its track's channels: the stream's third channel follows its first and second in MEM2.
    const uint32_t channel_0 = ax::Join(first.addr.end_address_hi, first.addr.end_address_lo);
    const uint32_t channel_2 = ax::Join(third.addr.end_address_hi, third.addr.end_address_lo);
    Check(channel_2 > channel_0 && first.addr.loop_flag == 0, "the second track plays the later channels");

    // In the dual pan mode, each channel of a stereo voice plays from its side. Without a pan, the first plays at the
    // left only and the second at the right only. A track's pan of 1 brings the second channel near the centre, and a
    // pan of 127 the first.
    Check(first.mix.l != 0 && first.mix.r == 0 && third.mix.l != 0 && third.mix.r == 0, "the channels at their sides");
    spec.tracks = {{127, 1, {0, 1}}, {127, 127, {2, 3}}};
    Playing panned(test, k2008, streams::MakeStream(spec));
    panned.Run(3);
    const ax::Pb second = panned.PbOf(0, 1);
    const ax::Pb centred = panned.PbOf(1, 0);
    Check(second.mix.l != 0 && second.mix.r != 0 && centred.mix.l != 0 && centred.mix.r != 0, "the tracks' pans");
}

// The sounds that don't start: one whose entry allocates no track, one whose stream lacks a track that the entry
// allocates, one whose track plays a channel that the entry doesn't allocate, and before version 1.4 one whose bits of
// channels have a gap. A count of 0 keeps all of the stream's channels. A sequence sound doesn't start as a stream.
void CheckRefusals()
{
    streams::StreamSpec spec;
    spec.channels = {Tone(5000, 0), Tone(5000, 1), Tone(5000, 2)};
    spec.tracks = {{127, 64, {0, 1}}, {127, 64, {2}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const auto starts = [&](const EntrySpec& entry, SndBuild build)
    {
        const TestArchive test(entry);
        Playing playing(test, build, file);

        return playing.player != nullptr;
    };

    Check(!starts({.tracks = 0}, k2008), "a sound without tracks doesn't start");
    Check(!starts({.channels = 3, .tracks = 7}, k2008), "a sound whose stream lacks a track doesn't start");
    Check(!starts({.channels = 2, .tracks = 3}, k2008), "a track of a channel that the sound lacks doesn't start");
    Check(starts({.channels = 3, .tracks = 3}, k2008) && starts({.channels = 0, .tracks = 3}, k2008),
          "the sounds of three channels start");
    Check(!starts({.version = 0x0103, .channels = 5, .tracks = 3}, k2008) &&
              starts({.version = 0x0103, .channels = 7, .tracks = 3}, k2008),
          "before version 1.4, a bit for each channel");
    Check(starts({.tracks = 0}, k2007), "the build of 2007 reads nothing of the entry");

    const TestArchive test({});
    auto model = std::make_unique<snd::ArchiveModel>();
    model->StartWithRenderer(test.archive);
    Check(model->engine->StartStreamSound(1, file) == nullptr, "a sequence sound doesn't start as a stream");
    Check(model->engine->StartStreamSound(2, file) == nullptr, "a sound that the archive lacks doesn't start");
    bool refused = false;
    try
    {
        model->engine->StartStreamSound(0, std::vector<uint8_t>(64, 0));
    }
    catch (const xsflib::FormatError&)
    {
        refused = true;
    }
    Check(refused, "a file that isn't a stream is refused");
}

// A stream sound of an entry that the archive's sound table doesn't hold, such as an editor's new sound, plays as the
// table's sound with the same entry plays. An entry of a sequence sound doesn't start as a stream.
void CheckEntriesOutsideTable()
{
    streams::StreamSpec spec;
    spec.loop_start = 14336;
    spec.channels = {Tone(30000, 0), Tone(30000, 1)};
    spec.tracks = {{100, 30, {0, 1}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const TestArchive test({.volume = 100});
    Playing table(test, k2008, file);
    table.Run(200);

    auto model = std::make_unique<snd::ArchiveModel>();
    model->build = k2008;
    model->StartWithRenderer(test.archive);
    const rsar::SoundInfo entry = test.archive.Sounds()[0];
    Check(model->engine->StartStreamSound(entry, file) != nullptr, "an entry outside the table starts");
    std::vector<int16_t> output;
    for (int frame = 0; frame < 200; frame++)
    {
        model->RunFrame(output);
    }
    Check(output == table.output && std::ranges::any_of(output, [](int16_t sample) { return sample != 0; }),
          "it plays as the table's sound");
    Check(model->engine->StartStreamSound(test.archive.Sounds()[1], file) == nullptr,
          "a sequence sound's entry doesn't start as a stream");
}

// A stream of 9,600 samples of 16-bit PCM without a loop plays for 100 frames. The sound finishes in the frame after
// its voice ends. A stream that loops plays on, and each pass of its loop after the first gives the first pass's output
// again: the voice loops to the loop's block with the loop's state.
void CheckEnds()
{
    streams::StreamSpec once;
    once.encoding = rsar::WaveEncoding::kPcm16;
    once.channels = {Tone(9600, 0), Tone(9600, 1)};
    once.tracks = {{127, 64, {0, 1}}};
    const TestArchive test({});
    Playing playing(test, k2007, streams::MakeStream(once));
    int finished = -1;
    for (int frame = 0; frame < 150 && finished < 0; frame++)
    {
        playing.Run(1);
        if (playing.player->IsFinished())
        {
            finished = frame;
        }
    }
    Check(finished >= 100 && finished <= 103,
          "the stream ends after its samples (frame " + std::to_string(finished) + ")");
    Check(!playing.model->engine->IsBusy(), "the engine is idle after the stream");

    streams::StreamSpec looped;
    looped.loop_start = 14336;
    looped.channels = {Tone(14336 + 14400, 0)};
    looped.tracks = {{127, 64, {0}}};
    for (const SndBuild build : {k2007, k2008})
    {
        Playing loop(test, build, streams::MakeStream(looped));
        loop.Run(800);
        const std::size_t length = 14400;
        const std::size_t from = 14336 + 3;
        bool repeats = !loop.player->IsFinished();
        for (std::size_t s = from; s + length < loop.output.size() / 2; s++)
        {
            repeats = repeats && loop.output[s * 2] == loop.output[(s + length) * 2];
        }
        Check(repeats && loop.output.size() / 2 > from + 2 * length,
              std::string(build.SinceMarch2008() ? "2008" : "2007") + ": each pass of the loop repeats the first");
    }
}

// A snapshot keeps the stream's player. A restore gives the same output again. The game's sends reach the voices. A
// restore after a later start is refused.
void CheckSnapshots()
{
    streams::StreamSpec spec;
    spec.loop_start = 0;
    spec.channels = {Tone(20000, 0), Tone(20000, 1)};
    spec.tracks = {{127, 64, {0, 1}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const TestArchive test({});
    auto model = std::make_unique<snd::ArchiveModel>();
    model->StartWithRenderer(test.archive);
    model->engine->SetEffects({}, {{0, 1, 0.25f}}, 0);
    snd::StrmPlayer* player = model->engine->StartStreamSound(0, file);
    Check(player != nullptr && model->engine->SoundSends(player->Id())[1] == 0.25f, "the game's sends");
    std::vector<int16_t> before;
    for (int frame = 0; frame < 10; frame++)
    {
        model->RunFrame(before);
    }
    const std::vector<uint32_t> indices = model->engine->Voices().At(player->TrackVoice(0)).AxVoiceIndices();
    const ax::Pb pb = model->ax.ReadPb(indices.at(0));
    Check(pb.mix.aux_b_l != 0 && pb.mix.aux_a_l == 0, "the send reaches bus B");

    const snd::ArchiveModel::Snapshot snapshot = model->Save();
    std::vector<int16_t> first;
    for (int frame = 0; frame < 120; frame++)
    {
        model->RunFrame(first);
    }

    model->Restore(snapshot);
    std::vector<int16_t> second;
    for (int frame = 0; frame < 120; frame++)
    {
        model->RunFrame(second);
    }

    Check(first == second && player->IsActive(), "a restore gives the same output again");
    model->engine->StartStreamSound(0, file);
    bool refused = false;
    try
    {
        model->Restore(snapshot);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }

    Check(refused, "a restore after another start is refused");
}

} // namespace

int main()
{
    const auto tests = []
    {
        CheckVoiceOf2007();
        CheckTracksOf2008();
        CheckRefusals();
        CheckEntriesOutsideTable();
        CheckEnds();
        CheckSnapshots();
    };

    return xsflib::testing::RunTest("rvl_stream_sound", tests);
}
