// SPDX-License-Identifier: MIT

// Tests of the reader and the writer of stream files (ntdlib/sar/stream_file.h), in each flavor's format: the formats
// of the 3DS, the Wii U and the Switch. The offsets and sizes that the tests expect follow from the layout that the
// header describes. A stereo stream of the Wii U without tracks lays out INFO as the tools lay it out: the settings at
// 0x60, the table of channels at 0xAC, the channels' infos at 0xC0 and the DSP-ADPCM infos at 0xD0. The 3DS's samples
// past the end follow the rules that the header gives for the 3DS's tools.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"

namespace
{

using ntdlib::sar::Flavor;
using ntdlib::sar::StreamFile;
using ntdlib::sar::WaveEncoding;
using xsflib::testing::ErrorOf;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};

// The newest version of a flavor's streams that the writer lays out.
uint32_t Newest(Flavor flavor)
{
    return flavor == Flavor::kCtr ? 0x02030100 : 0x00040000;
}

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

// A stereo DSP-ADPCM stream of 20,000 samples whose loop starts at sample 100. The loop moves to 14,336, the start of
// the second block, and the stream ends 14,236 samples later: 34,236 samples in 3 blocks. The last block holds 5,564
// samples in 3,180 bytes: 397 frames and a frame of 6 samples, its header and 3 bytes. The last pieces take 3,200
// bytes. INFO ends at 0x12C and takes 0x100 bytes. SEEK starts at 0x140, and DATA at 0x160. The samples start at
// 0x180.
void TestDspAdpcmStream(Flavor flavor)
{
    const std::vector<std::vector<int16_t>> input = {Tone(20000, 0), Tone(20000, 1)};
    const StreamFile stream = ntdlib::sar::MakeStream(flavor, 0x00040000, WaveEncoding::kDspAdpcm, 44100, input, 100);
    XSFLIB_CHECK(stream.loop && stream.loop_start == 14336 && stream.sample_count == 34236);
    XSFLIB_CHECK(stream.original_loop_start == 100 && stream.original_loop_end == 20000);
    XSFLIB_CHECK(stream.BlockCount() == 3 && stream.BlockSize() == 0x2000 && stream.LastBlockSamples() == 5564);
    XSFLIB_CHECK(stream.LastBlockSize() == 3180 && stream.channels.size() == 2);
    XSFLIB_CHECK(stream.channels[0].samples.size() == 2 * 0x2000 + 3200);
    XSFLIB_CHECK(stream.channels[1].history[1] == (std::array<int16_t, 2>{input[1][14335], input[1][14334]}));
    XSFLIB_CHECK(stream.channels[0].history[0] == (std::array<int16_t, 2>{0, 0}));

    const std::vector<uint8_t> file = stream.Write();
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    XSFLIB_CHECK(r.Magic(0, flavor == Flavor::kCtr ? "CSTM" : "FSTM") && r.U16(4) == 0xFEFF && r.U16(6) == 0x40);
    XSFLIB_CHECK(r.U32(8) == 0x00040000 && r.U32(12) == file.size() && r.U16(0x10) == 3);
    XSFLIB_CHECK(r.U16(0x14) == 0x4000 && r.U32(0x18) == 0x40 && r.U32(0x1C) == 0x100);
    XSFLIB_CHECK(r.U16(0x20) == 0x4001 && r.U32(0x24) == 0x140 && r.U32(0x28) == 0x20);
    XSFLIB_CHECK(r.U16(0x2C) == 0x4002 && r.U32(0x30) == 0x160 && r.U32(0x34) == 0x20 + 2 * 2 * 0x2000 + 2 * 3200);
    XSFLIB_CHECK(file.size() == 0x160 + r.U32(0x34));
    XSFLIB_CHECK(r.Magic(0x40, "INFO") && r.U16(0x48) == 0x4100 && r.U32(0x4C) == 0x18);
    XSFLIB_CHECK(r.U16(0x50) == 0 && r.U32(0x54) == 0xFFFFFFFF && r.U16(0x58) == 0x0101 && r.U32(0x5C) == 0x64);
    XSFLIB_CHECK(r.U8(0x60) == 2 && r.U8(0x61) == 1 && r.U8(0x62) == 2 && r.U8(0x63) == 0 && r.U32(0x64) == 44100);
    XSFLIB_CHECK(r.U32(0x68) == 14336 && r.U32(0x6C) == 34236 && r.U32(0x70) == 3 && r.U32(0x74) == 0x2000);
    XSFLIB_CHECK(r.U32(0x78) == 14336 && r.U32(0x7C) == 3180 && r.U32(0x80) == 5564 && r.U32(0x84) == 3200);
    XSFLIB_CHECK(r.U32(0x88) == 4 && r.U32(0x8C) == 14336 && r.U16(0x90) == 0x1F00 && r.U32(0x94) == 0x18);
    XSFLIB_CHECK(r.U16(0x98) == 0x100 && r.U16(0x9C) == 0 && r.U32(0xA0) == 0xFFFFFFFF);
    XSFLIB_CHECK(r.U32(0xA4) == 100 && r.U32(0xA8) == 20000 && r.U32(0xAC) == 2);
    XSFLIB_CHECK(r.U16(0xB0) == 0x4102 && r.U32(0xB4) == 0x14 && r.U16(0xB8) == 0x4102 && r.U32(0xBC) == 0x1C);
    XSFLIB_CHECK(r.U16(0xC0) == 0x0300 && r.U32(0xC4) == 0x10 && r.U16(0xC8) == 0x0300 && r.U32(0xCC) == 0x36);
    XSFLIB_CHECK(r.S16(0xD0) == stream.channels[0].adpcm->coefs[0] &&
                 r.S16(0xFE) == stream.channels[1].adpcm->coefs[0]);
    XSFLIB_CHECK(r.U16(0xF6) == stream.channels[0].adpcm->loop_pred_scale && r.Magic(0x140, "SEEK"));

    // SEEK holds the samples before each block little-endian, in DATA's order: the second block's entries follow the
    // first block's.
    const std::size_t kSecond = 0x148 + 2 * 4;
    XSFLIB_CHECK(static_cast<int16_t>(xsflib::LoadLe16(&file[kSecond])) == input[0][14335]);
    XSFLIB_CHECK(static_cast<int16_t>(xsflib::LoadLe16(&file[kSecond + 2])) == input[0][14334]);
    XSFLIB_CHECK(static_cast<int16_t>(xsflib::LoadLe16(&file[kSecond + 4])) == input[1][14335]);
    XSFLIB_CHECK(r.Magic(0x160, "DATA") && file[0x180] == stream.channels[0].samples[0]);
    XSFLIB_CHECK(file[0x180 + 0x2000] == stream.channels[1].samples[0]);
    XSFLIB_CHECK(file[0x180 + 4 * 0x2000 + 3200] == stream.channels[1].samples[2 * 0x2000]);

    // The file reads back, and decodes as the encoder's frames decode. The loop's samples follow the loop's end.
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read == stream && read.Write() == file);
    const ntdlib::wave::Pcm pcm = ntdlib::sar::DecodeStream(read);
    XSFLIB_CHECK(pcm.sample_rate == 44100 && pcm.loop && pcm.loop_start == 14336 && pcm.loop_end == 34236);
    XSFLIB_CHECK(pcm.channels.size() == 2 && pcm.channels[1].size() == 34236);
    const ntdlib::wave::DspAdpcmSum sum =
        flavor == Flavor::kCtr ? ntdlib::wave::DspAdpcmSum::kWide : ntdlib::wave::DspAdpcmSum::kWrap;
    XSFLIB_CHECK(pcm.channels[1] == ntdlib::wave::DecodeDspAdpcm(stream.channels[1].samples, 34236,
                                                                 stream.channels[1].adpcm->coefs, 0, 0, sum));
    XSFLIB_CHECK(pcm.channels[0][14335] == stream.channels[0].adpcm->loop_yn1 &&
                 pcm.channels[0][14334] == stream.channels[0].adpcm->loop_yn2);
}

// The stream of TestDspAdpcmStream in version 0x00030000. Its settings end at 0xA4, without the loop's original start
// and end. The rest of INFO starts 8 bytes earlier: the table of channels at 0xA4. INFO still takes 0x100 bytes, and
// SEEK and DATA keep their places and bytes. The file reads back with the loop's moved start as its original start.
void TestVersion3(Flavor flavor)
{
    const std::vector<std::vector<int16_t>> input = {Tone(20000, 0), Tone(20000, 1)};
    StreamFile stream = ntdlib::sar::MakeStream(flavor, 0x00030000, WaveEncoding::kDspAdpcm, 44100, input, 100);
    XSFLIB_CHECK(stream.original_loop_start == 100 && stream.original_loop_end == 20000);
    const std::vector<uint8_t> file = stream.Write();
    stream.version = 0x00040000;
    const std::vector<uint8_t> newer = stream.Write();
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    XSFLIB_CHECK(file.size() == newer.size() && r.U32(8) == 0x00030000 && r.U32(0x1C) == 0x100);
    XSFLIB_CHECK(r.U16(0x58) == 0x0101 && r.U32(0x5C) == 0x5C && r.U32(0xA4) == 2 && r.U16(0xA8) == 0x4102);
    XSFLIB_CHECK(std::equal(file.begin() + 12, file.begin() + 0x5C, newer.begin() + 12));
    XSFLIB_CHECK(std::equal(file.begin() + 0x60, file.begin() + 0xA4, newer.begin() + 0x60));
    XSFLIB_CHECK(std::equal(file.begin() + 0xA4, file.begin() + 0x124, newer.begin() + 0xAC));
    XSFLIB_CHECK(std::all_of(file.begin() + 0x124, file.begin() + 0x140, [](uint8_t b) { return b == 0; }));
    XSFLIB_CHECK(std::equal(file.begin() + 0x140, file.end(), newer.begin() + 0x140));

    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read.version == 0x00030000 && read.original_loop_start == 14336 && read.original_loop_end == 34236);
    XSFLIB_CHECK(read.Write() == file && read.channels == stream.channels && read.loop_start == stream.loop_start);
}

// A mono 16-bit PCM stream of 5,000 samples without a loop: one block of 4,096 samples a piece, then a last block of
// 904 samples in 1,808 bytes, 1,824 with the padding. The samples take the file's byte order, and decode exactly. The
// loop's original end is the stream's length.
void TestPcmStream(Flavor flavor)
{
    const std::vector<std::vector<int16_t>> input = {Tone(5000, 2)};
    const StreamFile stream = ntdlib::sar::MakeStream(flavor, Newest(flavor), WaveEncoding::kPcm16, 32000, input, {});
    XSFLIB_CHECK(!stream.loop && stream.loop_start == 0 && stream.sample_count == 5000);
    XSFLIB_CHECK(stream.original_loop_start == 0 && stream.original_loop_end == 5000);
    XSFLIB_CHECK(stream.block_samples == 4096 && stream.BlockCount() == 2 && stream.LastBlockSize() == 1808);
    XSFLIB_CHECK(stream.channels[0].samples.size() == 0x2000 + 1824 && !stream.channels[0].adpcm);

    const std::vector<uint8_t> file = stream.Write();
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    const std::size_t samples = r.U32(0x30) + 0x20;
    XSFLIB_CHECK(r.U8(r.U32(0x18) + 0x20) == 1 && r.U32(r.U32(0x18) + 0x20 + 0x24) == 1824);
    XSFLIB_CHECK(r.S16(samples) == input[0][0] && r.S16(samples + 2 * 4999) == input[0][4999]);
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read == stream && read.Write() == file && ntdlib::sar::DecodeStream(read).channels[0] == input[0]);

    // With a loop from sample 100, the loop moves to 4,096, and the samples from 100 up to 4,096 follow the end.
    const StreamFile looped = ntdlib::sar::MakeStream(flavor, Newest(flavor), WaveEncoding::kPcm16, 32000, input, 100);
    std::vector<int16_t> expected = input[0];
    expected.insert(expected.end(), input[0].begin() + 100, input[0].begin() + 4096);
    XSFLIB_CHECK(looped.loop_start == 4096 && looped.sample_count == 8996 && looped.original_loop_end == 5000);
    XSFLIB_CHECK(ntdlib::sar::DecodeStream(StreamFile::Read(looped.Write())).channels[0] == expected);

    // A last frame of DSP-ADPCM with an odd number of samples takes its header and a byte for each two of them, the
    // last byte half used: 1,009 samples take 72 frames and 2 bytes.
    const std::vector<std::vector<int16_t>> odd = {Tone(1009, 0)};
    XSFLIB_CHECK(ntdlib::sar::MakeStream(flavor, Newest(flavor), WaveEncoding::kDspAdpcm, 32000, odd, {})
                     .LastBlockSize() == 578);
}

// A stream's tracks: each one's volume, pan, span, flags and channels read back.
void TestTracks(Flavor flavor)
{
    const std::vector<std::vector<int16_t>> input = {Tone(300, 0), Tone(300, 1), Tone(300, 2)};
    StreamFile stream = ntdlib::sar::MakeStream(flavor, Newest(flavor), WaveEncoding::kDspAdpcm, 48000, input, {});
    stream.tracks = std::vector<StreamFile::Track>{{100, 20, 3, 1, {0, 1}}, {127, 64, 0, 0, {2}}};
    const std::vector<uint8_t> file = stream.Write();
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read.tracks == stream.tracks && read == stream && read.Write() == file);

    // The table of channels follows the table of tracks, and the first track's info follows both.
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    XSFLIB_CHECK(r.U32(0x5C) == r.U32(0x54) + 4 + 2 * 8 && r.U32(0x48 + r.U32(0x54) + 8) == 4 + 2 * 8 + 4 + 3 * 8);

    stream.tracks->back().channels = {3};
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { stream.Write(); }).empty());
}

// The 16-bit sample at sample `index` of a channel's bytes, little-endian.
int16_t SampleAt(const StreamFile::Channel& channel, std::size_t index)
{
    return static_cast<int16_t>(xsflib::LoadLe16(&channel.samples[index * 2]));
}

// A looped stereo 16-bit PCM stream of the 3DS of 5,000 samples, with a loop from sample 100. The loop moves to 4,096,
// and the stream ends 804 samples into its third block. The loop's first 128 samples follow the end in the last block.
// The last block's size counts 932 samples: 1,864 bytes, 1,888 with the padding. SEEK holds an entry for each block.
void TestCtrPastEnd()
{
    const std::vector<std::vector<int16_t>> input = {Tone(5000, 0), Tone(5000, 1)};
    const StreamFile stream =
        ntdlib::sar::MakeStream(Flavor::kCtr, 0x02030100, WaveEncoding::kPcm16, 32000, input, 100);
    XSFLIB_CHECK(stream.loop_start == 4096 && stream.sample_count == 8996 && stream.samples_past_end == 128);
    XSFLIB_CHECK(stream.uncounted_samples == 0 && stream.BlockCount() == 3 && stream.LastBlockSamples() == 804);
    XSFLIB_CHECK(stream.LastBlockSize() == 1864 && stream.OverflowSamples() == 0 && stream.OverflowSize() == 0);
    for (int c = 0; c < 2; ++c)
    {
        const StreamFile::Channel& channel = stream.channels[c];
        XSFLIB_CHECK(channel.samples.size() == 2 * 0x2000 + 1888 && channel.history.size() == 3);
        std::size_t differ = 0;
        for (std::size_t i = 0; i < 128; ++i)
        {
            differ += SampleAt(channel, 8996 + i) == input[c][4096 + i] ? 0 : 1;
        }
        XSFLIB_CHECK(differ == 0 && SampleAt(channel, 8996 + 128) == 0);
    }

    const std::vector<uint8_t> file = stream.Write();
    const xsflib::Reader r(file, false);
    XSFLIB_CHECK(r.Magic(0, "CSTM") && r.U32(8) == 0x02030100 && r.U32(0x7C) == 1864 && r.U32(0x80) == 804);
    XSFLIB_CHECK(r.U32(0x84) == 1888 && r.U32(0xA4) == 100 && r.U32(0xA8) == 5000);
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read == stream && read.Write() == file);
    XSFLIB_CHECK(ntdlib::sar::DecodeStream(read).channels[1].size() == 8996);

    // DSP-ADPCM takes the same layout. The last block's size counts the bytes up to the 128th sample past the end.
    const std::vector<std::vector<int16_t>> longer = {Tone(20000, 0), Tone(20000, 1)};
    const StreamFile adpcm =
        ntdlib::sar::MakeStream(Flavor::kCtr, 0x02030100, WaveEncoding::kDspAdpcm, 32000, longer, 100);
    XSFLIB_CHECK(adpcm.loop_start == 14336 && adpcm.samples_past_end == 128 && adpcm.LastBlockSamples() == 5564);
    XSFLIB_CHECK(adpcm.LastBlockSize() == 3253 && StreamFile::Read(adpcm.Write()) == adpcm);
}

// A looped stereo 16-bit PCM stream of the 3DS of 8,142 samples whose loop starts at its first sample. The loop moves
// to the second block: the samples up to 4,096 follow the end, and the stream ends 4,046 samples into its third block.
// The last block has room for 50 of the 128 samples past the end. The block ends full, and a piece of each channel
// after it holds the other 78: 156 bytes, 160 with the padding. SEEK holds an entry for each piece.
void TestCtrOverflow()
{
    const std::vector<std::vector<int16_t>> input = {Tone(8142, 0), Tone(8142, 1)};
    const StreamFile stream = ntdlib::sar::MakeStream(Flavor::kCtr, 0x02030100, WaveEncoding::kPcm16, 32000, input, 0);
    XSFLIB_CHECK(stream.loop_start == 4096 && stream.sample_count == 12238 && stream.LastBlockSamples() == 4046);
    XSFLIB_CHECK(stream.original_loop_start == 0 && stream.original_loop_end == 8142);
    XSFLIB_CHECK(stream.LastBlockSize() == 0x2000 && stream.OverflowSamples() == 78 && stream.OverflowSize() == 160);
    for (int c = 0; c < 2; ++c)
    {
        const StreamFile::Channel& channel = stream.channels[c];
        XSFLIB_CHECK(channel.samples.size() == 3 * 0x2000 + 160 && channel.history.size() == 4);
        std::size_t differ = 0;
        for (std::size_t i = 0; i < 128; ++i)
        {
            differ += SampleAt(channel, 12238 + i) == input[c][4096 + i] ? 0 : 1;
        }
        XSFLIB_CHECK(differ == 0 && channel.history[3] == (std::array<int16_t, 2>{input[c][4145], input[c][4144]}));
    }

    // DATA holds the three blocks' pieces, then each channel's piece after the last block. SEEK holds 4 entries for
    // each channel.
    const std::vector<uint8_t> file = stream.Write();
    const xsflib::Reader r(file, false);
    const std::size_t data = r.U32(0x30);
    XSFLIB_CHECK(r.U32(0x34) == 0x20 + 6 * 0x2000 + 2 * 160);
    XSFLIB_CHECK(static_cast<int16_t>(r.U16(data + 0x20 + 6 * 0x2000)) == input[0][4146]);
    XSFLIB_CHECK(static_cast<int16_t>(r.U16(data + 0x20 + 6 * 0x2000 + 160)) == input[1][4146]);
    XSFLIB_CHECK(static_cast<int16_t>(r.U16(r.U32(0x24) + 8 + 7 * 4)) == input[1][4145]);
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read == stream && read.Write() == file);
}

// The 3DS's oldest version: settings of 0x38 bytes, without the regions. A stream without a loop holds a sample of 0
// past its end, and its last block's count leaves out the last 127 samples. A last block of fewer samples counts one.
void TestCtrOldest()
{
    const std::vector<std::vector<int16_t>> input = {Tone(20000, 0)};
    const StreamFile stream =
        ntdlib::sar::MakeStream(Flavor::kCtr, 0x02000000, WaveEncoding::kDspAdpcm, 32000, input, {});
    XSFLIB_CHECK(stream.samples_past_end == 1 && stream.uncounted_samples == 127);
    XSFLIB_CHECK(stream.LastBlockSamples() == 5664 - 127 && stream.LastBlockSize() == 3238);
    const std::vector<uint8_t> file = stream.Write();
    const xsflib::Reader r(file, false);
    XSFLIB_CHECK(r.U32(8) == 0x02000000 && r.U32(0x54) == 0xFFFFFFFF && r.U32(0x5C) == 0x18 + 0x38);
    XSFLIB_CHECK(r.U32(0x80) == 5664 - 127 && r.U32(0x98) == 1);
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read.Write() == file && ntdlib::sar::DecodeStream(read).channels[0].size() == 20000);
    XSFLIB_CHECK(read.original_loop_start == 0 && read.original_loop_end == 20000);

    const std::vector<std::vector<int16_t>> short_last = {Tone(3 * 4096 + 60, 0)};
    const StreamFile one =
        ntdlib::sar::MakeStream(Flavor::kCtr, 0x02000000, WaveEncoding::kPcm16, 32000, short_last, {});
    XSFLIB_CHECK(one.BlockCount() == 4 && one.uncounted_samples == 59 && one.LastBlockSamples() == 1);
    XSFLIB_CHECK(StreamFile::Read(one.Write()).Write() == one.Write());

    // A looped stream takes the loop's first samples past its end in this version too.
    const StreamFile looped =
        ntdlib::sar::MakeStream(Flavor::kCtr, 0x02000000, WaveEncoding::kPcm16, 32000, input, 0);
    XSFLIB_CHECK(looped.samples_past_end == 128 && looped.uncounted_samples == 0);
    XSFLIB_CHECK(StreamFile::Read(looped.Write()).Write() == looped.Write());
}

// Whether `make` throws an exception of type E.
template <typename E, typename Make>
bool Throws(Make make)
{
    return !ErrorOf<E>(make).empty();
}

// A stream of the Wii U becomes the Switch's stream of the same samples, and the Switch's the Wii U's: 16-bit PCM
// takes the other byte order, and the rest stays. A stream of the 3DS loses its samples past the end on the way to the
// other consoles, and holds none when it comes back.
void TestConversions()
{
    const std::vector<std::vector<int16_t>> input = {Tone(9000, 0), Tone(9000, 1)};
    for (const WaveEncoding encoding : {WaveEncoding::kPcm16, WaveEncoding::kDspAdpcm})
    {
        const StreamFile cafe = ntdlib::sar::MakeStream(Flavor::kCafe, 0x00040000, encoding, 32000, input, 50);
        const StreamFile nx = ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, encoding, 32000, input, 50);
        XSFLIB_CHECK(ntdlib::sar::ConvertStream(cafe, Flavor::kNx, 0x00040000) == nx);
        XSFLIB_CHECK(ntdlib::sar::ConvertStream(nx, Flavor::kCafe, 0x00040000).Write() == cafe.Write());
        const StreamFile old = ntdlib::sar::MakeStream(Flavor::kCafe, 0x00030000, encoding, 32000, input, 50);
        XSFLIB_CHECK(ntdlib::sar::ConvertStream(nx, Flavor::kCafe, 0x00030000).Write() == old.Write());
    }

    // 16-bit PCM of the 3DS: the samples up to the end stay, and zeros follow them.
    const StreamFile ctr = ntdlib::sar::MakeStream(Flavor::kCtr, 0x02030100, WaveEncoding::kPcm16, 32000, input, 50);
    const StreamFile nx = ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, WaveEncoding::kPcm16, 32000, input, 50);
    XSFLIB_CHECK(ctr.samples_past_end == 128 && ntdlib::sar::ConvertStream(ctr, Flavor::kNx, 0x00040000) == nx);
    const StreamFile back = ntdlib::sar::ConvertStream(nx, Flavor::kCtr, 0x02030100);
    XSFLIB_CHECK(back == ntdlib::sar::WithoutSamplesPastEnd(ctr) && back.samples_past_end == 0);
    XSFLIB_CHECK(StreamFile::Read(back.Write()) == back);

    // DSP-ADPCM keeps the frame of the last sample whole, and zeros follow it.
    const StreamFile adpcm =
        ntdlib::sar::MakeStream(Flavor::kCtr, 0x02000000, WaveEncoding::kDspAdpcm, 32000, input, {});
    const StreamFile cafe = ntdlib::sar::ConvertStream(adpcm, Flavor::kCafe, 0x00030000);
    XSFLIB_CHECK(cafe.uncounted_samples == 0 && cafe.LastBlockSamples() == 9000 && cafe.LastBlockSize() == 5143);
    XSFLIB_CHECK(std::equal(cafe.channels[0].samples.begin(), cafe.channels[0].samples.begin() + 5144,
                            adpcm.channels[0].samples.begin()));
    XSFLIB_CHECK(std::all_of(cafe.channels[0].samples.begin() + 5144, cafe.channels[0].samples.end(),
                             [](uint8_t b) { return b == 0; }));
    const StreamFile cafe_read = StreamFile::Read(cafe.Write());
    XSFLIB_CHECK(ntdlib::sar::DecodeStream(cafe_read).channels == ntdlib::sar::DecodeStream(adpcm).channels);

    // A track of the 3DS has no span or flags.
    StreamFile tracked = nx;
    tracked.tracks = std::vector<StreamFile::Track>{{100, 20, 3, 1, {0, 1}}};
    const StreamFile ctr_tracked = ntdlib::sar::ConvertStream(tracked, Flavor::kCtr, 0x02010000);
    XSFLIB_CHECK(ctr_tracked.tracks->front() == (StreamFile::Track{100, 20, 0, 0, {0, 1}}));
    XSFLIB_CHECK(StreamFile::Read(ctr_tracked.Write()).Write() == ctr_tracked.Write());
}

// The reader refuses other files and settings that don't add up. The writer and MakeStream refuse what the format
// can't hold.
void TestRefusals()
{
    const std::vector<std::vector<int16_t>> input = {Tone(1000, 0)};
    const StreamFile stream =
        ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, WaveEncoding::kDspAdpcm, 48000, input, 14);
    const std::vector<uint8_t> file = stream.Write();
    XSFLIB_CHECK(StreamFile::Read(file) == stream);

    std::vector<uint8_t> other = file;
    other[1] = 'W';
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { StreamFile::Read(other); }));
    other = file;
    other[0x63] = 1;
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { StreamFile::Read(other); }));
    other = file;
    xsflib::StoreLe32(&other[0x70], 5);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { StreamFile::Read(other); }));
    other = file;
    other.resize(other.size() - 0x20);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { StreamFile::Read(other); }));

    StreamFile wrong = stream;
    wrong.version = 0x00050000;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong.version = 0x00020000;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong.version = 0x02030100;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong.flavor = Flavor::kCtr;
    XSFLIB_CHECK(StreamFile::Read(wrong.Write()).flavor == Flavor::kCtr);
    wrong.version = 0x02020000;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong.version = 0x00040000;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong = stream;
    wrong.uncounted_samples = 1000;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong = stream;
    wrong.channels[0].adpcm.reset();
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong = stream;
    wrong.channels[0].history.pop_back();
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));
    wrong = stream;
    wrong.sample_count += 14336;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { wrong.Write(); }));

    const std::vector<std::vector<int16_t>> uneven = {Tone(10, 0), Tone(11, 1)};
    const std::vector<std::vector<int16_t>> kEmpty = {std::vector<int16_t>()};
    XSFLIB_CHECK(Throws<std::invalid_argument>(
        [&] { ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, WaveEncoding::kPcm8, 48000, input, {}); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>(
        [&] { ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, WaveEncoding::kPcm16, 48000, uneven, {}); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>(
        [&] { ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, WaveEncoding::kPcm16, 48000, kEmpty, {}); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>(
        [&] { ntdlib::sar::MakeStream(Flavor::kNx, 0x00040000, WaveEncoding::kPcm16, 48000, input, 1000); }));
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        if (flavor != Flavor::kCtr)
        {
            TestDspAdpcmStream(flavor);
            TestVersion3(flavor);
        }
        TestPcmStream(flavor);
        TestTracks(flavor);
    }
    TestCtrPastEnd();
    TestCtrOverflow();
    TestCtrOldest();
    TestConversions();
    TestRefusals();

    return xsflib::testing::Summary("stream file");
}
