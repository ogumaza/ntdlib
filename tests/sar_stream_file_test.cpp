// SPDX-License-Identifier: MIT

// Tests of the reader and the writer of stream files (ntdlib/sar/stream_file.h), in each flavor's format: the formats
// of the 3DS, the Wii U and the Switch. The offsets and sizes that the tests expect follow from the layout that the
// header describes. A stereo stream of the Wii U without tracks lays out INFO as the tools lay it out: the settings at
// 0x60, the table of channels at 0xAC, the channels' infos at 0xC0 and the DSP-ADPCM infos at 0xD0.

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
    const StreamFile stream = ntdlib::sar::MakeStream(flavor, 0x00040000, WaveEncoding::kPcm16, 32000, input, {});
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
    const StreamFile looped = ntdlib::sar::MakeStream(flavor, 0x00040000, WaveEncoding::kPcm16, 32000, input, 100);
    std::vector<int16_t> expected = input[0];
    expected.insert(expected.end(), input[0].begin() + 100, input[0].begin() + 4096);
    XSFLIB_CHECK(looped.loop_start == 4096 && looped.sample_count == 8996 && looped.original_loop_end == 5000);
    XSFLIB_CHECK(ntdlib::sar::DecodeStream(StreamFile::Read(looped.Write())).channels[0] == expected);

    // A last frame of DSP-ADPCM with an odd number of samples takes its header and a byte for each two of them, the
    // last byte half used: 1,009 samples take 72 frames and 2 bytes.
    const std::vector<std::vector<int16_t>> odd = {Tone(1009, 0)};
    XSFLIB_CHECK(ntdlib::sar::MakeStream(flavor, 0x00040000, WaveEncoding::kDspAdpcm, 32000, odd, {}).LastBlockSize() ==
                 578);
}

// A stream's tracks: each one's volume, pan, span, flags and channels read back.
void TestTracks(Flavor flavor)
{
    const std::vector<std::vector<int16_t>> input = {Tone(300, 0), Tone(300, 1), Tone(300, 2)};
    StreamFile stream = ntdlib::sar::MakeStream(flavor, 0x00040000, WaveEncoding::kDspAdpcm, 48000, input, {});
    stream.tracks = std::vector<StreamFile::Track>{{100, 20, 3, 1, {0, 1}}, {127, 64, 0, 0, {2}}};
    const std::vector<uint8_t> file = stream.Write();
    const StreamFile read = StreamFile::Read(file);
    XSFLIB_CHECK(read.tracks == stream.tracks && read == stream && read.Write() == file);

    stream.tracks->back().channels = {3};
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { stream.Write(); }).empty());
}

// Whether `make` throws an exception of type E.
template <typename E, typename Make>
bool Throws(Make make)
{
    return !ErrorOf<E>(make).empty();
}

// A stream of the Wii U becomes the Switch's stream of the same samples, and the Switch's the Wii U's: 16-bit PCM
// takes the other byte order, and the rest stays. The 3DS's streams don't convert.
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

    const StreamFile ctr = ntdlib::sar::MakeStream(Flavor::kCtr, 0x00040000, WaveEncoding::kPcm16, 32000, input, {});
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { ntdlib::sar::ConvertStream(ctr, Flavor::kNx, 0x00040000); }));
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
        TestDspAdpcmStream(flavor);
        TestVersion3(flavor);
        TestPcmStream(flavor);
        TestTracks(flavor);
    }
    TestConversions();
    TestRefusals();

    return xsflib::testing::Summary("stream file");
}
