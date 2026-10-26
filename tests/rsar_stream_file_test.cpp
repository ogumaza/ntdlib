// SPDX-License-Identifier: MIT

// Tests of the reader of the Wii's stream files (ntdlib/rsar/stream_file.h), on files of ntdlib/testing/rsar_streams.h.
// A stereo stream with one track lays HEAD out as the tools lay it out: the settings at 0x60 and the tracks' table at
// 0x94. With an extended track, the channels' table lies at 0xAC, and the DSP-ADPCM infos at 0xC8 and 0x100. With a
// simple track, they lie at 0xA4, 0xC0 and 0xF8. HEAD takes 0x100 bytes either way. The other offsets and sizes follow
// from the layout that the header describes. The encoder's decoding of each channel gives the samples that the
// decoder has to give, and the encoder's input gives ADPC's entries. An archive that ntdlib's model lays out names
// files beside it. The streams go to the later consoles with their blocks, and the later consoles' streams to the Wii.
// The writer gives the builder's files back, and a stream made from samples decodes as the encoder's frames do.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/tables.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/testing/rsar_streams.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"

namespace
{

namespace streams = ntdlib::testing::rsar_streams;
using ntdlib::rsar::StreamFile;
using ntdlib::rsar::WaveEncoding;
using xsflib::testing::ErrorOf;

// `count` samples of a tone, a different one for each channel.
std::vector<int16_t> Tone(std::size_t count, int channel)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        samples[i] = static_cast<int16_t>(static_cast<int>((i * (7 + channel)) % 200) * 100 - 10000);
    }

    return samples;
}

// The decoding of `channel`, `count` samples of the encoder's frames as one run, with the Wii's sums.
std::vector<int16_t> Decoded(const ntdlib::wave::DspAdpcmChannel& channel, uint32_t count)
{
    ntdlib::wave::EncodedWave wave;
    wave.encoding = ntdlib::wave::Encoding::kDspAdpcm;
    wave.sum = ntdlib::wave::DspAdpcmSum::kWide;
    wave.loop_end = count;
    ntdlib::wave::EncodedChannel& one = wave.channels.emplace_back();
    one.data = channel.data;
    one.coefs = &channel.coefs;

    return ntdlib::wave::DecodeWave(wave).channels.front();
}

// A stereo DSP-ADPCM stream of 20,000 samples whose loop starts at 14,336, the second block's start. The last block
// holds 5,664 samples in 3,237 bytes: 404 frames and a frame of 8 samples, its header and 3 bytes. Its pieces take
// 3,264 bytes. ADPC holds an entry for each block.
void TestDspAdpcm()
{
    streams::StreamSpec spec;
    spec.loop_start = 14336;
    spec.channels = {Tone(20000, 0), Tone(20000, 1)};
    spec.tracks = {{100, 40, {0, 1}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const xsflib::Reader r(file, true);
    XSFLIB_CHECK(r.U32(0x10) == 0x40 && r.U32(0x14) == 0x100 && r.U32(0x18) == 0x140 && r.U32(0x1C) == 0x20);
    XSFLIB_CHECK(r.U32(0x50 + 4) == 0x4C && r.U32(0x58 + 4) == 0x64 && r.U8(0x94) == 1 && r.U8(0x95) == 1);
    XSFLIB_CHECK(r.U32(0xC0 + 4) == 0x80 && r.U32(0xF8 + 4) == 0xB8 && r.U32(0x20) == 0x160);

    const StreamFile stream = StreamFile::Read(file);
    XSFLIB_CHECK(stream.encoding == WaveEncoding::kDspAdpcm && stream.loop && stream.loop_start == 14336);
    XSFLIB_CHECK(stream.sample_count == 20000 && stream.sample_rate == 32000 && stream.block_samples == 14336);
    XSFLIB_CHECK(stream.BlockCount() == 2 && stream.BlockSize() == 0x2000 && stream.LastBlockSamples() == 5664);
    XSFLIB_CHECK(stream.LastBlockSize() == 3237 && stream.history_interval == 14336 && stream.extended_tracks);
    XSFLIB_CHECK(stream.tracks.size() == 1 && stream.tracks[0] == (StreamFile::Track{100, 40, {0, 1}}));
    XSFLIB_CHECK(stream.channels.size() == 2);

    ntdlib::wave::Pcm pcm = ntdlib::rsar::DecodeStream(stream);
    XSFLIB_CHECK(pcm.loop && pcm.loop_start == 14336 && pcm.loop_end == 20000 && pcm.channels.size() == 2);
    for (std::size_t c = 0; c < 2; ++c)
    {
        const StreamFile::Channel& channel = stream.channels[c];
        const ntdlib::wave::DspAdpcmChannel encoded =
            ntdlib::wave::EncodeDspAdpcm(spec.channels[c], 14336, ntdlib::wave::DspAdpcmSum::kWide);
        XSFLIB_CHECK(channel.samples.size() == 0x2000 + 3264);
        XSFLIB_CHECK(
            std::equal(channel.samples.begin(), channel.samples.begin() + 0x2000 + 3237, encoded.data.begin()));
        XSFLIB_CHECK(channel.adpcm && channel.adpcm->coefs == encoded.coefs);
        XSFLIB_CHECK(channel.adpcm->pred_scale == encoded.pred_scale && channel.adpcm->yn1 == 0);
        XSFLIB_CHECK(channel.adpcm->loop_pred_scale == encoded.loop_pred_scale);
        XSFLIB_CHECK(channel.adpcm->loop_yn1 == encoded.loop_yn1 && channel.adpcm->loop_yn2 == encoded.loop_yn2);
        XSFLIB_CHECK(channel.history.size() == 2 && channel.history[0] == (std::array<int16_t, 2>{0, 0}));
        XSFLIB_CHECK(channel.history[1] == (std::array<int16_t, 2>{spec.channels[c][14335], spec.channels[c][14334]}));

        // The decoder plays the blocks as one run. The state at the loop's start holds the decoded samples there.
        const std::vector<int16_t> decoded = Decoded(encoded, 20000);
        XSFLIB_CHECK(pcm.channels[c] == decoded);
        XSFLIB_CHECK(decoded[14335] == channel.adpcm->loop_yn1 && decoded[14334] == channel.adpcm->loop_yn2);
    }
}

// A stereo DSP-ADPCM stream of 30,000 samples without a loop, with a simple track and an interval of 1,024 samples in
// ADPC. Each full block's 8 KiB count 8 entries, and the last block's 759 bytes count 1: 17 entries, the last of them
// for sample 16,384.
void TestOldInterval()
{
    streams::StreamSpec spec;
    spec.channels = {Tone(30000, 2), Tone(30000, 3)};
    spec.history_interval = 1024;
    spec.extended_tracks = false;
    spec.tracks = {{127, 64, {0, 1}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const xsflib::Reader r(file, true);
    XSFLIB_CHECK(r.U32(0x14) == 0x100 && r.U8(0x94) == 1 && r.U8(0x95) == 0 && r.U32(0x58 + 4) == 0x5C);
    XSFLIB_CHECK(r.U32(0xB8 + 4) == 0x78 && r.U32(0xF0 + 4) == 0xB0 && r.U32(0x1C) == 0xA0);

    const StreamFile stream = StreamFile::Read(file);
    XSFLIB_CHECK(!stream.loop && stream.BlockCount() == 3 && stream.LastBlockSamples() == 1328);
    XSFLIB_CHECK(stream.LastBlockSize() == 759 && stream.history_interval == 1024 && !stream.extended_tracks);
    XSFLIB_CHECK(stream.tracks.size() == 1 && stream.tracks[0] == (StreamFile::Track{127, 64, {0, 1}}));
    for (std::size_t c = 0; c < 2; ++c)
    {
        const std::vector<std::array<int16_t, 2>>& history = stream.channels[c].history;
        XSFLIB_CHECK(history.size() == 17);
        XSFLIB_CHECK(history[1] == (std::array<int16_t, 2>{spec.channels[c][1023], spec.channels[c][1022]}));
        XSFLIB_CHECK(history[16] == (std::array<int16_t, 2>{spec.channels[c][16383], spec.channels[c][16382]}));
    }

    const ntdlib::wave::Pcm pcm = ntdlib::rsar::DecodeStream(stream);
    XSFLIB_CHECK(!pcm.loop && pcm.loop_end == 30000);
    for (std::size_t c = 0; c < 2; ++c)
    {
        const ntdlib::wave::DspAdpcmChannel encoded =
            ntdlib::wave::EncodeDspAdpcm(spec.channels[c], 0, ntdlib::wave::DspAdpcmSum::kWide);
        XSFLIB_CHECK(pcm.channels[c] == Decoded(encoded, 30000));
    }
}

// A stereo stream of 5,000 samples in 16-bit PCM, without ADPC. A block holds 4,096 samples of each channel. The last
// block holds 904 samples in 1,808 bytes, and its pieces take 1,824. The samples are big-endian.
void TestPcm()
{
    streams::StreamSpec spec;
    spec.encoding = WaveEncoding::kPcm16;
    spec.sample_rate = 44100;
    spec.loop_start = 4096;
    spec.channels = {Tone(5000, 4), Tone(5000, 5)};
    spec.tracks = {{127, 64, {0, 1}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    const xsflib::Reader r(file, true);
    XSFLIB_CHECK(r.U32(0x18) == 0 && r.U32(0x1C) == 0 && r.U32(0x14) == 0xA0 && r.U32(0x20) == 0xE0);

    const StreamFile stream = StreamFile::Read(file);
    XSFLIB_CHECK(stream.encoding == WaveEncoding::kPcm16 && stream.sample_rate == 44100 &&
                 stream.block_samples == 4096);
    XSFLIB_CHECK(stream.BlockCount() == 2 && stream.LastBlockSamples() == 904 && stream.LastBlockSize() == 1808);
    XSFLIB_CHECK(stream.history_interval == 0 && stream.channels[1].samples.size() == 0x2000 + 1824);
    XSFLIB_CHECK(!stream.channels[0].adpcm && stream.channels[0].history.empty());
    XSFLIB_CHECK(xsflib::LoadBe16(&stream.channels[1].samples[2]) == static_cast<uint16_t>(spec.channels[1][1]));

    const ntdlib::wave::Pcm pcm = ntdlib::rsar::DecodeStream(stream);
    XSFLIB_CHECK(pcm.loop && pcm.loop_start == 4096 && pcm.channels == spec.channels);
}

// The reader refuses other files, settings that don't add up and parts that run past their blocks.
void TestRefusals()
{
    streams::StreamSpec spec;
    spec.channels = {Tone(20000, 0), Tone(20000, 1)};
    spec.tracks = {{127, 64, {0, 1}}};
    const std::vector<uint8_t> file = streams::MakeStream(spec);
    XSFLIB_CHECK(StreamFile::Read(file).channels.size() == 2);

    const auto refused = [](const std::vector<uint8_t>& other)
    {
        return !ErrorOf<xsflib::FormatError>([&] { StreamFile::Read(other); }).empty();
    };
    std::vector<uint8_t> other = file;
    other[1] = 'W';
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe16(&other[6], 0x0101);
    XSFLIB_CHECK(refused(other));
    other = file;
    other[0x60] = 3;
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe16(&other[0x66], 0x20);
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe32(&other[0x74], 3);
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe32(&other[0x80], 3000);
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe32(&other[0x90], 0);
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe32(&other[0x1C], 8);
    XSFLIB_CHECK(refused(other));
    other = file;
    xsflib::StoreBe32(&other[0x24], xsflib::LoadBe32(&other[0x24]) - 0x20);
    XSFLIB_CHECK(refused(other));
    other = file;
    other[0xA9] = 2;
    XSFLIB_CHECK(refused(other));
    other = file;
    other[0xAC] = 3;
    XSFLIB_CHECK(refused(other));
}

// An archive whose file table names files beside it: the reader gives each one's path, from the archive's folder or
// from the disc's root, and no path for a file of a group. The table holds three files. The writer of their sizes gives
// the archive that the model lays out with the new sizes.
void TestExternalPaths()
{
    ntdlib::rsar::ArchiveModel model;
    model.players = {{"PLAYER_BGM", 1, 0}};
    model.files = {
        {.path = "stream/BGM.brstm", .path_size = 0x1234},
        {.bytes = std::vector<uint8_t>(32, 1), .wave_data = std::vector<uint8_t>(32, 2)},
        {.path = "/strm/JINGLE.brstm", .path_size = 0x40},
    };
    ntdlib::rsar::SoundModel bgm;
    bgm.name = "STRM_BGM";
    bgm.type = ntdlib::rsar::SoundType::kStream;
    ntdlib::rsar::SoundModel jingle = bgm;
    jingle.name = "STRM_JINGLE";
    jingle.file_id = 2;
    model.sounds = {bgm, jingle};

    ntdlib::rsar::ReadOptions options;
    options.playback_fields = true;
    const ntdlib::rsar::SoundArchive read =
        ntdlib::rsar::SoundArchive::Load(ntdlib::rsar::BuildArchive(model), options);
    XSFLIB_CHECK(read.ExternalPath(0) == "stream/BGM.brstm" && read.ExternalPath(2) == "/strm/JINGLE.brstm");
    XSFLIB_CHECK(!read.ExternalPath(1) && !read.ExternalPath(3));
    XSFLIB_CHECK(read.FileCount() == 3);
    XSFLIB_CHECK(read.Sounds().size() == 2 && read.Sounds()[1].type == ntdlib::rsar::SoundType::kStream);

    // The entry of a file beside the archive takes a new size. The entry of a file in a group refuses one.
    std::vector<uint8_t> archive = ntdlib::rsar::BuildArchive(model);
    const std::vector<std::pair<uint32_t, uint32_t>> sizes = {{2, 0x5678}, {0, 0x9ABC}};
    ntdlib::rsar::WriteExternalFileSizes(archive, sizes);
    ntdlib::rsar::ArchiveModel sized = model;
    sized.files[0].path_size = 0x9ABC;
    sized.files[2].path_size = 0x5678;
    XSFLIB_CHECK(archive == ntdlib::rsar::BuildArchive(sized) && ntdlib::rsar::ReadArchiveModel(archive) == sized);
    const std::vector<std::pair<uint32_t, uint32_t>> inside = {{0, 1}, {1, 0x40}};
    const std::vector<std::pair<uint32_t, uint32_t>> past = {{3, 0x40}};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WriteExternalFileSizes(archive, inside); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WriteExternalFileSizes(archive, past); }).empty());
    XSFLIB_CHECK(archive == ntdlib::rsar::BuildArchive(sized));
}

// The streams go to the Switch and the Wii U with their blocks. SEEK takes ADPC's entries where ADPC holds one for a
// block's start, and the decoded samples elsewhere. The later consoles' readers read the files back, and decode the
// Wii's samples.
void TestConversions()
{
    streams::StreamSpec spec;
    spec.loop_start = 14336;
    spec.channels = {Tone(20000, 0), Tone(20000, 1)};
    spec.tracks = {{100, 40, {0, 1}}};
    const StreamFile stream = StreamFile::Read(streams::MakeStream(spec));
    const ntdlib::wave::Pcm pcm = ntdlib::rsar::DecodeStream(stream);
    for (const ntdlib::sar::Flavor flavor : {ntdlib::sar::Flavor::kNx, ntdlib::sar::Flavor::kCafe})
    {
        const ntdlib::sar::StreamFile later = ntdlib::rsar::StreamFileOf(stream, flavor, 0x00040000);
        XSFLIB_CHECK(later.flavor == flavor && later.version == 0x00040000 && later.loop && later.loop_start == 14336);
        XSFLIB_CHECK(later.sample_count == 20000 && later.sample_rate == 32000 && later.block_samples == 14336);
        XSFLIB_CHECK(later.original_loop_start == 14336 && later.original_loop_end == 20000);
        XSFLIB_CHECK(later.tracks && later.tracks->size() == 1 && (*later.tracks)[0].volume == 100);
        XSFLIB_CHECK((*later.tracks)[0].pan == 40 && (*later.tracks)[0].channels == (std::vector<uint8_t>{0, 1}));
        for (std::size_t c = 0; c < 2; ++c)
        {
            const ntdlib::sar::StreamFile::Channel& channel = later.channels[c];
            XSFLIB_CHECK(channel.samples == stream.channels[c].samples &&
                         channel.history == stream.channels[c].history);
            XSFLIB_CHECK(channel.adpcm && channel.adpcm->coefs == stream.channels[c].adpcm->coefs);
            XSFLIB_CHECK(channel.adpcm->loop_yn1 == stream.channels[c].adpcm->loop_yn1);
        }
        const ntdlib::sar::StreamFile read = ntdlib::sar::StreamFile::Read(later.Write());
        XSFLIB_CHECK(read == later && ntdlib::sar::DecodeStream(read).channels == pcm.channels);
    }

    // An interval of 1,024 samples holds entries for the starts of the first two blocks, but not the third's.
    spec = {};
    spec.channels = {Tone(30000, 2)};
    spec.history_interval = 1024;
    spec.tracks = {{127, 64, {0}}};
    const StreamFile old = StreamFile::Read(streams::MakeStream(spec));
    const std::vector<int16_t> decoded = ntdlib::rsar::DecodeStream(old).channels[0];
    const ntdlib::sar::StreamFile later = ntdlib::rsar::StreamFileOf(old, ntdlib::sar::Flavor::kNx, 0x00040000);
    XSFLIB_CHECK(!later.loop && later.original_loop_start == 0 && later.original_loop_end == 30000);
    XSFLIB_CHECK(later.channels[0].history.size() == 3 && later.channels[0].history[1] == old.channels[0].history[14]);
    XSFLIB_CHECK(later.channels[0].history[0] == (std::array<int16_t, 2>{0, 0}));
    XSFLIB_CHECK(later.channels[0].history[2] == (std::array<int16_t, 2>{decoded[28671], decoded[28670]}));
    XSFLIB_CHECK(later.channels[0].history[1] != (std::array<int16_t, 2>{decoded[14335], decoded[14334]}));

    // 16-bit PCM takes the Switch's byte order. SEEK takes the samples before each block.
    spec = {};
    spec.encoding = WaveEncoding::kPcm16;
    spec.channels = {Tone(5000, 4)};
    spec.tracks = {{127, 64, {0}}};
    const StreamFile pcm16 = StreamFile::Read(streams::MakeStream(spec));
    const ntdlib::sar::StreamFile switched = ntdlib::rsar::StreamFileOf(pcm16, ntdlib::sar::Flavor::kNx, 0x00040000);
    XSFLIB_CHECK(switched.encoding == ntdlib::sar::WaveEncoding::kPcm16 && switched.block_samples == 4096);
    XSFLIB_CHECK(xsflib::LoadLe16(&switched.channels[0].samples[2]) == static_cast<uint16_t>(spec.channels[0][1]));
    const std::vector<std::array<int16_t, 2>>& seek = switched.channels[0].history;
    XSFLIB_CHECK(seek.size() == 2 && seek[0] == (std::array<int16_t, 2>{0, 0}));
    XSFLIB_CHECK(seek[1] == (std::array<int16_t, 2>{spec.channels[0][4095], spec.channels[0][4094]}));
    const ntdlib::sar::StreamFile kept = ntdlib::rsar::StreamFileOf(pcm16, ntdlib::sar::Flavor::kCafe, 0x00030000);
    XSFLIB_CHECK(ntdlib::sar::StreamFile::Read(kept.Write()).channels[0].history == seek);
    XSFLIB_CHECK(ntdlib::sar::DecodeStream(ntdlib::sar::StreamFile::Read(switched.Write())).channels == spec.channels);

    // A loop inside a block has no place in the later consoles' streams.
    StreamFile inside = pcm16;
    inside.loop = true;
    inside.loop_start = 100;
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>(
                      [&] { ntdlib::rsar::StreamFileOf(inside, ntdlib::sar::Flavor::kNx, 0x00040000); })
                      .empty());
}

// The writer gives back the builder's files byte for byte: in DSP-ADPCM with an extended track, with an interval of
// 1,024 samples and a simple track, and in 16-bit PCM. A channel's gain and history at the start go back into its
// DSP-ADPCM info.
void TestWrite()
{
    streams::StreamSpec adpcm;
    adpcm.loop_start = 14336;
    adpcm.channels = {Tone(20000, 0), Tone(20000, 1)};
    adpcm.tracks = {{100, 40, {0, 1}}};
    streams::StreamSpec old;
    old.channels = {Tone(30000, 2), Tone(30000, 3), Tone(30000, 4)};
    old.history_interval = 1024;
    old.extended_tracks = false;
    old.tracks = {{127, 64, {0, 1}}, {127, 64, {2}}};
    streams::StreamSpec pcm;
    pcm.encoding = WaveEncoding::kPcm16;
    pcm.sample_rate = 44100;
    pcm.loop_start = 0;
    pcm.channels = {Tone(5000, 4), Tone(5000, 5)};
    pcm.tracks = {{90, 20, {1}}, {127, 64, {0}}};

    for (const streams::StreamSpec& spec : {adpcm, old, pcm})
    {
        const std::vector<uint8_t> file = streams::MakeStream(spec);
        const std::vector<uint8_t> written = StreamFile::Read(file).Write();
        XSFLIB_CHECK(written == file);
    }

    StreamFile stream = StreamFile::Read(streams::MakeStream(adpcm));
    stream.channels[1].adpcm->gain = 0x1234;
    stream.channels[1].adpcm->yn1 = -5;
    stream.channels[1].adpcm->yn2 = 7;
    const std::vector<uint8_t> file = stream.Write();
    const xsflib::Reader r(file, true);
    XSFLIB_CHECK(r.U16(0x100 + 0x20) == 0x1234 && r.S16(0x100 + 0x24) == -5 && r.S16(0x100 + 0x26) == 7);
    XSFLIB_CHECK(StreamFile::Read(file) == stream);
}

// The writer refuses a stream that the settings can't describe.
void TestWriteRefusals()
{
    streams::StreamSpec spec;
    spec.loop_start = 14336;
    spec.channels = {Tone(20000, 0), Tone(20000, 1)};
    spec.tracks = {{127, 64, {0, 1}}};
    const StreamFile stream = StreamFile::Read(streams::MakeStream(spec));
    const auto refused = [&stream](const auto& change)
    {
        StreamFile other = stream;
        change(other);
        return !ErrorOf<std::invalid_argument>([&] { other.Write(); }).empty();
    };
    XSFLIB_CHECK(!refused([](StreamFile&) {}));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.encoding = static_cast<WaveEncoding>(3); }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.channels.clear(); }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.block_samples = 4096; }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.sample_rate = 0; }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.sample_rate = 0x1000000; }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.loop_start = 20000; }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.channels[1].adpcm.reset(); }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.channels[0].samples.pop_back(); }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.channels[1].history.pop_back(); }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.history_interval = 0; }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.tracks[0].channels.push_back(2); }));
    XSFLIB_CHECK(refused([](StreamFile& s) { s.tracks.resize(256, s.tracks[0]); }));
    XSFLIB_CHECK(!refused([](StreamFile& s) { s.sample_rate = 0xFFFFFF; }));
    XSFLIB_CHECK(!refused([](StreamFile& s) { s.tracks.clear(); }));
}

// A stream made from samples. A loop from sample 10,000 moves to the second block's start, 14,336, and the samples
// from 10,000 to 14,336 follow the end: 24,336 samples. The last block holds 10,000 of them. ADPC holds the samples
// before each block's start, and the DSP-ADPCM info the encoder's state at the loop's start. The stream takes a track
// for each pair of channels, and three channels take two tracks.
void TestMakeStream()
{
    const std::vector<std::vector<int16_t>> channels = {Tone(20000, 0), Tone(20000, 1), Tone(20000, 2)};
    const StreamFile stream = ntdlib::rsar::MakeStream(WaveEncoding::kDspAdpcm, 32000, channels, 10000);
    XSFLIB_CHECK(stream.loop && stream.loop_start == 14336 && stream.sample_count == 24336);
    XSFLIB_CHECK(stream.BlockCount() == 2 && stream.LastBlockSamples() == 10000 && stream.history_interval == 14336);
    XSFLIB_CHECK(stream.extended_tracks && stream.tracks.size() == 2);
    XSFLIB_CHECK(stream.tracks[0] == (StreamFile::Track{127, 64, {0, 1}}));
    XSFLIB_CHECK(stream.tracks[1] == (StreamFile::Track{127, 64, {2}}));
    const ntdlib::wave::Pcm pcm = ntdlib::rsar::DecodeStream(stream);
    for (std::size_t c = 0; c < channels.size(); ++c)
    {
        std::vector<int16_t> input = channels[c];
        input.insert(input.end(), channels[c].begin() + 10000, channels[c].begin() + 14336);
        const ntdlib::wave::DspAdpcmChannel encoded =
            ntdlib::wave::EncodeDspAdpcm(input, 14336, ntdlib::wave::DspAdpcmSum::kWide);
        const StreamFile::Channel& channel = stream.channels[c];
        XSFLIB_CHECK(channel.samples.size() == 0x2000 + 5728 && channel.adpcm->coefs == encoded.coefs);
        XSFLIB_CHECK(channel.adpcm->loop_pred_scale == encoded.loop_pred_scale);
        XSFLIB_CHECK(channel.history.size() == 2 &&
                     channel.history[1] == (std::array<int16_t, 2>{input[14335], input[14334]}));
        XSFLIB_CHECK(pcm.channels[c] == Decoded(encoded, 24336));
        XSFLIB_CHECK(pcm.channels[c][14335] == channel.adpcm->loop_yn1);
    }
    XSFLIB_CHECK(StreamFile::Read(stream.Write()) == stream);

    // 16-bit PCM keeps its samples. A loop at a block's start stays there.
    const std::vector<std::vector<int16_t>> pair = {Tone(5000, 4), Tone(5000, 5)};
    const StreamFile pcm16 = ntdlib::rsar::MakeStream(WaveEncoding::kPcm16, 48000, pair, 4096);
    XSFLIB_CHECK(pcm16.loop_start == 4096 && pcm16.sample_count == 5000 && pcm16.history_interval == 0);
    XSFLIB_CHECK(pcm16.channels[0].history.empty() && !pcm16.channels[0].adpcm);
    XSFLIB_CHECK(ntdlib::rsar::DecodeStream(StreamFile::Read(pcm16.Write())).channels == pair);

    // A loop in the last block moves past the samples' end. The samples after the end repeat the loop.
    const std::vector<std::vector<int16_t>> one = {Tone(5000, 6)};
    const StreamFile late = ntdlib::rsar::MakeStream(WaveEncoding::kPcm16, 32000, one, 4500);
    XSFLIB_CHECK(late.loop_start == 8192 && late.sample_count == 8692 && late.tracks.size() == 1);
    const std::vector<int16_t> decoded = ntdlib::rsar::DecodeStream(late).channels[0];
    bool repeats = true;
    for (std::size_t i = 5000; i < decoded.size(); ++i)
    {
        repeats = repeats && decoded[i] == one[0][4500 + (i - 4500) % 500];
    }
    XSFLIB_CHECK(repeats);

    // A stream without a loop holds the state at the first sample there. MakeStream refuses what a stream can't hold.
    const StreamFile plain = ntdlib::rsar::MakeStream(WaveEncoding::kDspAdpcm, 32000, one, std::nullopt);
    XSFLIB_CHECK(!plain.loop && plain.loop_start == 0 && plain.sample_count == 5000);
    XSFLIB_CHECK(plain.channels[0].adpcm->loop_pred_scale == plain.channels[0].adpcm->pred_scale);
    const auto refused = [](WaveEncoding encoding, uint32_t rate, const std::vector<std::vector<int16_t>>& input,
                            std::optional<uint32_t> loop)
    {
        return !ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::MakeStream(encoding, rate, input, loop); }).empty();
    };
    XSFLIB_CHECK(refused(WaveEncoding::kPcm8, 32000, one, std::nullopt));
    XSFLIB_CHECK(refused(WaveEncoding::kPcm16, 32000, {}, std::nullopt));
    XSFLIB_CHECK(refused(WaveEncoding::kPcm16, 32000, {Tone(10, 0), Tone(11, 1)}, std::nullopt));
    XSFLIB_CHECK(refused(WaveEncoding::kPcm16, 32000, {std::vector<int16_t>{}}, std::nullopt));
    XSFLIB_CHECK(refused(WaveEncoding::kPcm16, 32000, one, 5000));
    XSFLIB_CHECK(refused(WaveEncoding::kPcm16, 0, one, std::nullopt));
    XSFLIB_CHECK(refused(WaveEncoding::kPcm16, 0x1000000, one, std::nullopt));
    XSFLIB_CHECK(!refused(WaveEncoding::kPcm16, 0xFFFFFF, one, 4999));
}

// The later consoles' streams go to the Wii with their blocks. A Wii stream that goes to the Switch and comes back is
// the stream again. A stream of the Switch or the 3DS takes the Wii's byte order in 16-bit PCM, and one of the 3DS
// drops the samples past its end. ADPC takes SEEK's entries. A stream without tracks takes a track for each pair of
// channels.
void TestWiiStreamOf()
{
    streams::StreamSpec spec;
    spec.loop_start = 14336;
    spec.channels = {Tone(20000, 0), Tone(20000, 1)};
    spec.tracks = {{100, 40, {0, 1}}};
    const StreamFile stream = StreamFile::Read(streams::MakeStream(spec));
    for (const ntdlib::sar::Flavor flavor :
         {ntdlib::sar::Flavor::kNx, ntdlib::sar::Flavor::kCafe, ntdlib::sar::Flavor::kCtr})
    {
        const ntdlib::sar::StreamFile later = ntdlib::rsar::StreamFileOf(stream, flavor, 0x00040000);
        XSFLIB_CHECK(ntdlib::rsar::WiiStreamOf(later) == stream);
    }

    // A 3DS stream in 16-bit PCM, without tracks and with samples past its end.
    const std::vector<std::vector<int16_t>> channels = {Tone(9000, 1), Tone(9000, 2), Tone(9000, 3)};
    const ntdlib::sar::StreamFile ctr = ntdlib::sar::MakeStream(
        ntdlib::sar::Flavor::kCtr, 0x02030100, ntdlib::sar::WaveEncoding::kPcm16, 32728, channels, 100);
    XSFLIB_CHECK(ctr.samples_past_end != 0 && ctr.loop_start == 4096 && !ctr.tracks);
    const StreamFile wii = ntdlib::rsar::WiiStreamOf(ctr);
    XSFLIB_CHECK(wii.encoding == WaveEncoding::kPcm16 && wii.sample_rate == 32728 && wii.loop);
    XSFLIB_CHECK(wii.loop_start == 4096 && wii.sample_count == ctr.sample_count && wii.history_interval == 0);
    XSFLIB_CHECK(wii.extended_tracks && wii.tracks.size() == 2 && wii.tracks[1].channels == std::vector<uint8_t>{2});
    XSFLIB_CHECK(xsflib::LoadBe16(&wii.channels[2].samples[2]) == static_cast<uint16_t>(channels[2][1]));
    const StreamFile read = StreamFile::Read(wii.Write());
    const ntdlib::wave::Pcm decoded = ntdlib::rsar::DecodeStream(read);
    const ntdlib::wave::Pcm later = ntdlib::sar::DecodeStream(ctr);
    for (std::size_t c = 0; c < channels.size(); ++c)
    {
        XSFLIB_CHECK(std::equal(decoded.channels[c].begin(), decoded.channels[c].end(), later.channels[c].begin()));
    }

    // A Switch stream in DSP-ADPCM with a track: the blocks keep their bytes, and ADPC takes SEEK's entries.
    const std::vector<std::vector<int16_t>> longer = {Tone(30000, 1), Tone(30000, 2), Tone(30000, 3)};
    ntdlib::sar::StreamFile nx = ntdlib::sar::MakeStream(ntdlib::sar::Flavor::kNx, 0x00040000,
                                                         ntdlib::sar::WaveEncoding::kDspAdpcm, 48000, longer, 14336);
    nx.tracks = std::vector<ntdlib::sar::StreamFile::Track>{{90, 30, 0, 0, {2, 0}}};
    const StreamFile from_nx = ntdlib::rsar::WiiStreamOf(nx);
    XSFLIB_CHECK(from_nx.tracks.size() == 1 && from_nx.tracks[0] == (StreamFile::Track{90, 30, {2, 0}}));
    XSFLIB_CHECK(from_nx.history_interval == 14336 && from_nx.channels.size() == 3 && from_nx.BlockCount() == 3);
    for (std::size_t c = 0; c < longer.size(); ++c)
    {
        XSFLIB_CHECK(from_nx.channels[c].samples == nx.channels[c].samples);
        XSFLIB_CHECK(from_nx.channels[c].history == nx.channels[c].history);
        XSFLIB_CHECK(from_nx.channels[c].adpcm->loop_yn1 == nx.channels[c].adpcm->loop_yn1);
    }
    XSFLIB_CHECK(StreamFile::Read(from_nx.Write()) == from_nx);

    // NW4R SND loops a stream from the start of the block that holds the loop's start.
    ntdlib::sar::StreamFile inside = nx;
    inside.loop_start = 100;
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiStreamOf(inside); }).empty());
    ntdlib::sar::StreamFile fast = nx;
    fast.sample_rate = 0x1000000;
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiStreamOf(fast); }).empty());
}

} // namespace

int main()
{
    TestDspAdpcm();
    TestOldInterval();
    TestPcm();
    TestRefusals();
    TestExternalPaths();
    TestConversions();
    TestWrite();
    TestWriteRefusals();
    TestMakeStream();
    TestWiiStreamOf();

    return xsflib::testing::Summary("rsar stream file");
}
