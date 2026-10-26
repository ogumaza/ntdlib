// SPDX-License-Identifier: MIT

// Tests of the models of the Wii's waves and wave archives, and of the raw wave data of the Wii's older banks
// (ntdlib/rsar/wave_file.h). The offsets and sizes that the tests expect follow from the layout that the header
// describes. The readers of formats.h read the written files back.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace rsar = ntdlib::rsar;
using rsar::DspAdpcmInfo;
using rsar::WaveArchiveFile;
using rsar::WaveEncoding;
using rsar::WaveFile;

// The readers of formats.h with every field of a DSP-ADPCM info and the channels' volumes.
rsar::ReadOptions PlaybackOptions()
{
    rsar::ReadOptions options;
    options.playback_fields = true;

    return options;
}

// A DSP-ADPCM info whose values count up from `first`, with the contexts' samples negative.
DspAdpcmInfo Adpcm(int first)
{
    DspAdpcmInfo info;
    for (std::size_t k = 0; k < info.coefs.size(); ++k)
    {
        info.coefs[k] = static_cast<int16_t>(first + static_cast<int>(k));
    }
    info.gain = static_cast<uint16_t>(first + 16);
    info.pred_scale = static_cast<uint16_t>(first + 17);
    info.yn1 = static_cast<int16_t>(-first - 18);
    info.yn2 = static_cast<int16_t>(-first - 19);
    info.loop_pred_scale = static_cast<uint16_t>(first + 20);
    info.loop_yn1 = static_cast<int16_t>(-first - 21);
    info.loop_yn2 = static_cast<int16_t>(-first - 22);

    return info;
}

// `count` bytes that count up from `first`.
std::vector<uint8_t> Counting(std::size_t count, uint8_t first)
{
    std::vector<uint8_t> bytes;
    for (std::size_t i = 0; i < count; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(first + i));
    }

    return bytes;
}

// Whether `bytes` holds `value` from `start` up to `end`.
bool Filled(const std::vector<uint8_t>& bytes, std::size_t start, std::size_t end, uint8_t value)
{
    return end <= bytes.size() &&
           std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                       bytes.begin() + static_cast<std::ptrdiff_t>(end), [value](uint8_t b) { return b == value; });
}

// A stereo DSP-ADPCM wave of 30 samples at 22,050 Hz: 3 frames of 8 bytes in each channel, with a loop from sample 16.
WaveFile StereoAdpcm()
{
    WaveFile wave;
    wave.loop = true;
    wave.sample_rate = 22050;
    wave.loop_start = 16;
    wave.loop_end = 30;
    wave.channels.push_back({Counting(24, 1), Adpcm(1), {WaveFile::kUnity, 0, 0x00800000, 0}});
    wave.channels.push_back({Counting(24, 0x41), Adpcm(100), {0, WaveFile::kUnity, 0, 0x00800000}});

    return wave;
}

// A mono PCM16 wave of 10 samples without a loop.
WaveFile MonoPcm16()
{
    WaveFile wave;
    wave.encoding = WaveEncoding::kPcm16;
    wave.loop_end = 10;
    wave.channels.push_back({Counting(20, 0x80), std::nullopt});

    return wave;
}

// A DSP-ADPCM wave's info gives the loop in nibbles, past the two of each frame's header. The channels' samples follow
// one another in DATA, and INFO and DATA end on 32 bytes.
void TestDspAdpcmWave()
{
    const WaveFile wave = StereoAdpcm();
    const std::vector<uint8_t> file = wave.Write();
    const xsflib::Reader r(file, true);
    const rsar::Wave parsed = rsar::Wave::ParseFile(file, PlaybackOptions());
    const WaveFile read = WaveFile::Read(file);

    XSFLIB_CHECK(file.size() == 0x140 && r.Magic(0, "RWAV") && r.U16(4) == 0xFEFF && r.U16(6) == 0x0102);
    XSFLIB_CHECK(r.U32(8) == 0x140 && r.U16(0xC) == 0x20 && r.U16(0xE) == 2);
    XSFLIB_CHECK(r.U32(0x10) == 0x20 && r.U32(0x14) == 0xE0 && r.U32(0x18) == 0x100 && r.U32(0x1C) == 0x40);
    XSFLIB_CHECK(r.Magic(0x20, "INFO") && r.U32(0x24) == 0xE0 && r.Magic(0x100, "DATA") && r.U32(0x104) == 0x40);
    XSFLIB_CHECK(r.U8(0x28) == 2 && r.U8(0x29) == 1 && r.U8(0x2A) == 2 && r.U8(0x2B) == 0 && r.U16(0x2C) == 22050);
    XSFLIB_CHECK(r.U32(0x30) == 20 && r.U32(0x34) == 35 && r.U32(0x38) == 0x1C && r.U32(0x3C) == 0xD8);
    XSFLIB_CHECK(r.U32(0x44) == 0x24 && r.U32(0x48) == 0x40);
    XSFLIB_CHECK(r.U32(0x4C) == 0 && r.U32(0x50) == 0x5C && r.U32(0x54) == WaveFile::kUnity);
    XSFLIB_CHECK(r.U32(0x5C) == 0x00800000 && r.U32(0x68) == 24 && r.U32(0x6C) == 0x8C && r.U32(0x74) == 0x01000000);
    XSFLIB_CHECK(r.S16(0x84) == 1 && r.U16(0xA4) == 17 && r.S16(0xA8) == -19 && r.S16(0xB0) == -23);
    XSFLIB_CHECK(r.S16(0xB4) == 100 && r.U16(0xD6) == 117 && r.S16(0xE0) == -122);
    XSFLIB_CHECK(Filled(file, 0xE4, 0x100, 0) && file[0x108] == 1 && file[0x120] == 0x41 && file[0x137] == 0x58);
    XSFLIB_CHECK(Filled(file, 0x138, 0x140, 0));

    XSFLIB_CHECK(parsed.encoding == WaveEncoding::kDspAdpcm && parsed.loop && parsed.sample_rate == 22050);
    XSFLIB_CHECK(parsed.loop_start == 16 && parsed.loop_end == 30 && parsed.channels.size() == 2);
    XSFLIB_CHECK(parsed.channels[0].data.data() == file.data() + 0x108 && parsed.channels[0].adpcm == Adpcm(1));
    XSFLIB_CHECK(parsed.channels[1].data.data() == file.data() + 0x120 && parsed.channels[1].adpcm == Adpcm(100));
    XSFLIB_CHECK(parsed.channels[1].volumes == wave.channels[1].volumes);
    XSFLIB_CHECK(read == wave && read.Write() == file);
}

// A PCM16 wave's info gives the loop in samples, and a PCM8 wave's in bytes. A loop can hold no sample.
void TestPcmWave()
{
    const WaveFile pcm16 = MonoPcm16();
    const std::vector<uint8_t> file16 = pcm16.Write();
    const xsflib::Reader r16(file16, true);
    WaveFile pcm8;
    pcm8.encoding = WaveEncoding::kPcm8;
    pcm8.loop = true;
    pcm8.sample_rate = 0x123456;
    pcm8.loop_start = 7;
    pcm8.loop_end = 7;
    pcm8.channels.push_back({Counting(7, 0x10), std::nullopt});
    const std::vector<uint8_t> file8 = pcm8.Write();
    const xsflib::Reader r8(file8, true);
    const rsar::Wave parsed = rsar::Wave::ParseFile(file8, PlaybackOptions());

    XSFLIB_CHECK(file16.size() == 0xA0 && r16.U32(0x14) == 0x60 && r16.U32(0x18) == 0x80 && r16.U32(0x1C) == 0x20);
    XSFLIB_CHECK(r16.U8(0x28) == 1 && r16.U8(0x29) == 0 && r16.U32(0x30) == 0 && r16.U32(0x34) == 9);
    XSFLIB_CHECK(r16.U32(0x3C) == 0x58 && r16.U32(0x44) == 0x20 && r16.U32(0x48) == 0 && r16.U32(0x4C) == 0);
    XSFLIB_CHECK(r16.U32(0x50) == WaveFile::kUnity && Filled(file16, 0x64, 0x80, 0) && file16[0x88] == 0x80);
    XSFLIB_CHECK(Filled(file16, 0x9C, 0xA0, 0) && WaveFile::Read(file16) == pcm16);
    XSFLIB_CHECK(r8.U8(0x28) == 0 && r8.U8(0x2B) == 0x12 && r8.U16(0x2C) == 0x3456);
    XSFLIB_CHECK(r8.U32(0x30) == 7 && r8.U32(0x34) == 6);
    XSFLIB_CHECK(parsed.loop_start == 7 && parsed.loop_end == 7 && parsed.sample_rate == 0x123456);
    XSFLIB_CHECK(WaveFile::Read(file8) == pcm8 && WaveFile::Read(file8).Write() == file8);
}

// A file laid out another way reads too. Its channels' samples run up to the next channel's, and the last channel's to
// DATA's end. Write then lays the samples out as the tools do, and zeros pad INFO.
void TestOtherLayout()
{
    std::vector<uint8_t> file = StereoAdpcm().Write();
    xsflib::StoreBe32(&file[0x68], 32);
    file[0xF0] = 0xAA;
    const WaveFile read = WaveFile::Read(file);
    std::vector<uint8_t> written = read.Write();

    XSFLIB_CHECK(read.channels[0].samples.size() == 32 && read.channels[1].samples.size() == 24);
    XSFLIB_CHECK(read.channels[0].samples[24] == 0x41 && written.size() == file.size() && written[0xF0] == 0);
    written[0xF0] = 0xAA;
    XSFLIB_CHECK(written == file);
}

// A wave archive's table names each wave from DATA's start, and the waves start 32 bytes into DATA.
void TestWaveArchive()
{
    WaveArchiveFile archive;
    archive.waves = {StereoAdpcm().Write(), MonoPcm16().Write()};
    const std::vector<uint8_t> file = archive.Write();
    const xsflib::Reader r(file, true);
    const rsar::WaveArchive parsed = rsar::WaveArchive::Parse(file, PlaybackOptions());

    XSFLIB_CHECK(file.size() == 0x260 && r.Magic(0, "RWAR") && r.U16(6) == 0x0100 && r.U32(8) == 0x260);
    XSFLIB_CHECK(r.U32(0x10) == 0x20 && r.U32(0x14) == 0x40 && r.U32(0x18) == 0x60 && r.U32(0x1C) == 0x200);
    XSFLIB_CHECK(r.Magic(0x20, "TABL") && r.U32(0x24) == 0x40 && r.U32(0x28) == 2);
    XSFLIB_CHECK(r.U32(0x2C) == 0x01000000 && r.U32(0x30) == 0x20 && r.U32(0x34) == 0x140);
    XSFLIB_CHECK(r.U32(0x38) == 0x01000000 && r.U32(0x3C) == 0x160 && r.U32(0x40) == 0xA0);
    XSFLIB_CHECK(Filled(file, 0x44, 0x60, 0) && r.Magic(0x60, "DATA") && r.U32(0x64) == 0x200);
    XSFLIB_CHECK(Filled(file, 0x68, 0x80, 0) && r.Magic(0x80, "RWAV") && r.Magic(0x1C0, "RWAV"));
    XSFLIB_CHECK(parsed.waves.size() == 2 && parsed.waves[1].data() == file.data() + 0x1C0);
    XSFLIB_CHECK(parsed.waves[1].size() == 0xA0 && WaveArchiveFile::Read(file) == archive);

    const WaveArchiveFile empty;
    XSFLIB_CHECK(empty.Write().size() == 0x60 && WaveArchiveFile::Read(empty.Write()) == empty);
}

// The raw samples of the waves of a bank of version 1.1 start each wave on 32 bytes, with each channel after the one
// before, and end on 32 bytes. A bank of version 1.1 holds the same wave data.
void TestRawWaveData()
{
    const std::vector<WaveFile> waves = {StereoAdpcm(), MonoPcm16()};
    const std::vector<uint8_t> raw = rsar::RawWaveData(waves);
    XSFLIB_CHECK(raw.size() == 0x60 && std::equal(raw.begin(), raw.begin() + 24, Counting(24, 1).begin()));
    XSFLIB_CHECK(std::equal(raw.begin() + 24, raw.begin() + 48, Counting(24, 0x41).begin()));
    XSFLIB_CHECK(Filled(raw, 48, 0x40, 0) &&
                 std::equal(raw.begin() + 0x40, raw.begin() + 0x54, Counting(20, 0x80).begin()));
    XSFLIB_CHECK(Filled(raw, 0x54, 0x60, 0) && rsar::RawWaveData({}).empty());
    XSFLIB_CHECK(rsar::BankFile::FromInstruments(0x0101, {}, waves).Write().wave_data == raw);
}

template <class Error, class Call>
bool Throws(Call call, const std::string& part)
{
    const std::string message = ErrorOf<Error>(call);

    return !message.empty() && message.find(part) != std::string::npos;
}

// The writers refuse a wave without channels or with more than 255, a rate past 24 bits, a wave without samples, a
// loop that starts past the wave's end, and a DSP-ADPCM info that doesn't match the encoding. An archive's wave has to
// take a multiple of 32 bytes. The readers refuse other files and missing blocks.
void TestRefusals()
{
    WaveFile none = MonoPcm16();
    none.channels.clear();
    WaveFile many = MonoPcm16();
    many.channels.resize(256, many.channels[0]);
    WaveFile fast = MonoPcm16();
    fast.sample_rate = 0x1000000;
    WaveFile empty = MonoPcm16();
    empty.loop_end = 0;
    WaveFile backwards = MonoPcm16();
    backwards.loop_start = 11;
    WaveFile bare = StereoAdpcm();
    bare.channels[1].adpcm.reset();
    WaveFile pcm = MonoPcm16();
    pcm.channels[0].adpcm = Adpcm(1);
    WaveArchiveFile odd;
    odd.waves = {std::vector<uint8_t>(33)};
    const std::vector<uint8_t> file = StereoAdpcm().Write();
    std::vector<uint8_t> no_data = file;
    xsflib::StoreBe16(&no_data[0xE], 1);
    std::vector<uint8_t> moved = file;
    moved[0x100] = 'X';

    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { none.Write(); }, "holds 0 channels"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { many.Write(); }, "holds 256 channels"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { fast.Write(); }, "passes 24 bits"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { empty.Write(); }, "loop ends before it starts"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { backwards.Write(); }, "loop ends before it starts"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { bare.Write(); }, "has no DSP-ADPCM info"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { pcm.Write(); }, "a PCM channel has a DSP-ADPCM info"));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { odd.Write(); }, "isn't a multiple of 32 bytes"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WaveFile::Read(odd.waves[0]); }, "not an RWAV file"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WaveFile::Read(no_data); }, "has no DATA block"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WaveFile::Read(moved); }, "has no DATA block"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WaveArchiveFile::Read(file); }, "not an RWAR file"));
}

} // namespace

int main()
{
    TestDspAdpcmWave();
    TestPcmWave();
    TestOtherLayout();
    TestWaveArchive();
    TestRawWaveData();
    TestRefusals();

    return xsflib::testing::Summary("Wii wave file");
}
