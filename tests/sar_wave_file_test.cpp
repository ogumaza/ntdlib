// SPDX-License-Identifier: MIT

// Tests of the readers and writers of wave files and wave archives (ntdlib/sar/wave_file.h), in each flavor's format:
// the formats of the 3DS, the Wii U and the Switch. The offsets and sizes that the tests expect follow from the layout
// that the header describes. The readers of formats.h read the written files back.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/testing/sar_files.h"

using xsflib::testing::failures;

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
using ntdlib::sar::DspAdpcmInfo;
using ntdlib::sar::Flavor;
using ntdlib::sar::WaveArchiveFile;
using ntdlib::sar::WaveEncoding;
using ntdlib::sar::WaveFile;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};

// The readers of formats.h, stopping at the first problem: they then read every field of a DSP-ADPCM info.
ntdlib::sar::Options StrictOptions(Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

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
    info.pred_scale = static_cast<uint16_t>(first + 16);
    info.yn1 = static_cast<int16_t>(-first - 17);
    info.yn2 = static_cast<int16_t>(-first - 18);
    info.loop_pred_scale = static_cast<uint16_t>(first + 19);
    info.loop_yn1 = static_cast<int16_t>(-first - 20);
    info.loop_yn2 = static_cast<int16_t>(-first - 21);

    return info;
}

bool SameAdpcm(const DspAdpcmInfo& a, const DspAdpcmInfo& b)
{
    return a.coefs == b.coefs && a.pred_scale == b.pred_scale && a.yn1 == b.yn1 && a.yn2 == b.yn2 &&
           a.loop_pred_scale == b.loop_pred_scale && a.loop_yn1 == b.loop_yn1 && a.loop_yn2 == b.loop_yn2;
}

// Whether `bytes` holds `value` from `start` up to `end`.
bool Filled(const std::vector<uint8_t>& bytes, std::size_t start, std::size_t end, uint8_t value)
{
    return end <= bytes.size() &&
           std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                       bytes.begin() + static_cast<std::ptrdiff_t>(end), [value](uint8_t b) { return b == value; });
}

// A stereo DSP-ADPCM wave of 2 frames a channel. INFO holds the settings at 0x48, the table at 0x5C, the channels'
// infos at 0x70 and 0x84, and the DSP-ADPCM infos at 0x98 and 0xC6. Zeros pad it to 0x100. DATA's samples start at
// 0x120, on the first 32-byte boundary after DATA's 8 bytes. On the Switch, the 64-byte boundary puts them at 0x140.
// The second channel starts on the next boundary after the first channel's 16 bytes.
void TestDspAdpcmWave(Flavor flavor)
{
    WaveFile wave;
    wave.flavor = flavor;
    wave.version = flavor == Flavor::kCtr ? 0x02010100 : 0x00010100;
    wave.encoding = WaveEncoding::kDspAdpcm;
    wave.sample_rate = 32000;
    wave.loop_end = 28;
    wave.channels = {{std::vector<uint8_t>(16, 0x11), Adpcm(1)}, {std::vector<uint8_t>(16, 0x22), Adpcm(100)}};
    const bool wide = flavor == Flavor::kNx;
    const std::size_t first = wide ? 0x140 : 0x120;
    const std::size_t second = wide ? 0x180 : 0x140;

    const std::vector<uint8_t> file = wave.Write();
    const ntdlib::sar::Wave parsed = ntdlib::sar::Wave::Parse(file, StrictOptions(flavor));
    const WaveFile read = WaveFile::Read(file);

    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    XSFLIB_CHECK(r.Magic(0, flavor == Flavor::kCtr ? "CWAV" : "FWAV") && r.U16(4) == 0xFEFF && r.U16(6) == 0x40);
    XSFLIB_CHECK(r.U32(8) == wave.version && r.U32(12) == file.size() && file.size() == second + 16);
    XSFLIB_CHECK(r.U16(0x10) == 2 && r.U16(0x14) == 0x7000 && r.U32(0x18) == 0x40 && r.U32(0x1C) == 0xC0);
    XSFLIB_CHECK(r.U16(0x20) == 0x7001 && r.U32(0x24) == 0x100 && r.U32(0x28) == file.size() - 0x100);
    XSFLIB_CHECK(r.Magic(0x40, "INFO") && r.U32(0x44) == 0xC0 && r.Magic(0x100, "DATA"));
    XSFLIB_CHECK(r.U32(0x5C) == 2 && r.U16(0x60) == 0x7100 && r.U32(0x64) == 0x14 && r.U32(0x6C) == 0x28);
    XSFLIB_CHECK(r.U16(0x70) == 0x1F00 && r.U32(0x74) == first - 0x108 && r.U32(0x88) == second - 0x108);
    XSFLIB_CHECK(r.U16(0x78) == 0x0300 && r.U32(0x7C) == 0x28 && r.U16(0x8C) == 0x0300 && r.U32(0x90) == 0x42);
    XSFLIB_CHECK(r.S16(0x98) == 1 && r.S16(0xC2) == -22 && r.S16(0xC6) == 100 && r.S16(0xF0) == -121);
    XSFLIB_CHECK(Filled(file, 0xF2, 0x100, 0) && Filled(file, 0x108, first, 0) &&
                 Filled(file, first, first + 16, 0x11));
    XSFLIB_CHECK(Filled(file, first + 16, second, 0) && Filled(file, second, second + 16, 0x22));
    XSFLIB_CHECK(parsed.encoding == WaveEncoding::kDspAdpcm && parsed.sample_rate == 32000 && parsed.loop_end == 28);
    XSFLIB_CHECK(parsed.channels.size() == 2 && parsed.channels[0].data.data() == file.data() + first);
    XSFLIB_CHECK(parsed.channels[1].data.data() == file.data() + second && parsed.channels[1].data.size() == 16);
    XSFLIB_CHECK(SameAdpcm(*parsed.channels[0].adpcm, Adpcm(1)) && SameAdpcm(*parsed.channels[1].adpcm, Adpcm(100)));
    XSFLIB_CHECK(read.flavor == flavor && read.version == wave.version && read.channels.size() == 2);
    XSFLIB_CHECK(read.channels[0].samples.size() == second - first &&
                 read.channels[1].samples == wave.channels[1].samples);
    XSFLIB_CHECK(SameAdpcm(*read.channels[1].adpcm, Adpcm(100)) && read.Write() == file);
}

// A mono PCM16 wave that loops. A PCM channel's reference to a DSP-ADPCM info is a reference to nothing. INFO ends at
// 0x7C, and zeros pad it to 0x80. The samples start at 0xA0, or at 0xC0 on the Switch, and end the file.
void TestPcmWave(Flavor flavor)
{
    const bool big_endian = flavor == Flavor::kCafe;
    WaveFile wave;
    wave.flavor = flavor;
    wave.version = flavor == Flavor::kCtr ? 0x02010000 : 0x00010200;
    wave.encoding = WaveEncoding::kPcm16;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_start = 1;
    wave.loop_end = 3;
    wave.original_loop_start = 7;
    const std::vector<uint8_t> samples = big_endian ? std::vector<uint8_t>{0x00, 0x01, 0xFF, 0xFE, 0x12, 0x34}
                                                    : std::vector<uint8_t>{0x01, 0x00, 0xFE, 0xFF, 0x34, 0x12};
    wave.channels = {{samples, std::nullopt}};
    const std::size_t start = flavor == Flavor::kNx ? 0xC0 : 0xA0;

    const std::vector<uint8_t> file = wave.Write();
    const ntdlib::sar::Wave parsed = ntdlib::sar::Wave::Parse(file, StrictOptions(flavor));

    const xsflib::Reader r(file, big_endian);
    XSFLIB_CHECK(file.size() == start + 6 && r.U32(0x1C) == 0x40 && r.U32(0x24) == 0x80 && r.U32(0x28) == start - 0x7A);
    XSFLIB_CHECK(r.U8(0x48) == 1 && r.U8(0x49) == 1 && r.U32(0x50) == 1 && r.U32(0x54) == 3 && r.U32(0x58) == 7);
    XSFLIB_CHECK(r.U32(0x5C) == 1 && r.U32(0x64) == 0x0C && r.U32(0x6C) == start - 0x88);
    XSFLIB_CHECK(r.U16(0x70) == 0 && r.U32(0x74) == 0xFFFFFFFF && Filled(file, 0x78, 0x80, 0));
    XSFLIB_CHECK(parsed.encoding == WaveEncoding::kPcm16 && parsed.big_endian == big_endian && parsed.loop);
    XSFLIB_CHECK(parsed.loop_start == 1 && parsed.loop_end == 3 && !parsed.channels[0].adpcm);
    XSFLIB_CHECK(
        std::equal(samples.begin(), samples.end(), parsed.channels[0].data.begin(), parsed.channels[0].data.end()));
    XSFLIB_CHECK(WaveFile::Read(file).original_loop_start == 7 && WaveFile::Read(file).Write() == file);
}

// An archive of waves of 0x50, 0x21 and 0x40 bytes. INFO's table ends at 0x70, and zeros pad it to 0x80. FILE's waves
// start at 0xA0, on the first 32-byte boundary after FILE's 8 bytes, or at 0xC0 on the Switch. Each later wave starts
// on the next boundary. An archive without waves ends 32 bytes after FILE's start.
void TestWaveArchive(Flavor flavor)
{
    WaveArchiveFile archive;
    archive.flavor = flavor;
    archive.version = flavor == Flavor::kCtr ? 0x01000000 : 0x00010000;
    archive.waves = {std::vector<uint8_t>(0x50, 0x11), std::vector<uint8_t>(0x21, 0x22),
                     std::vector<uint8_t>(0x40, 0x33)};
    const std::vector<std::size_t> starts = flavor == Flavor::kNx ? std::vector<std::size_t>{0xC0, 0x140, 0x180}
                                                                  : std::vector<std::size_t>{0xA0, 0x100, 0x140};
    WaveArchiveFile empty = archive;
    empty.waves.clear();

    const std::vector<uint8_t> file = archive.Write();
    const std::vector<uint8_t> none = empty.Write();
    const ntdlib::sar::WaveArchive parsed = ntdlib::sar::WaveArchive::Parse(file, StrictOptions(flavor));

    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    XSFLIB_CHECK(r.Magic(0, flavor == Flavor::kCtr ? "CWAR" : "FWAR") && r.U32(8) == archive.version);
    XSFLIB_CHECK(file.size() == starts[2] + 0x40 && r.U32(12) == file.size() && r.U16(0x14) == 0x6800);
    XSFLIB_CHECK(r.U32(0x1C) == 0x40 && r.U16(0x20) == 0x6801 && r.U32(0x24) == 0x80 &&
                 r.U32(0x28) == file.size() - 0x80);
    XSFLIB_CHECK(r.U32(0x48) == 3 && Filled(file, 0x70, 0x80, 0) && Filled(file, 0x88, starts[0], 0));
    for (std::size_t i = 0; i < 3; ++i)
    {
        XSFLIB_CHECK(r.U16(0x4C + i * 12) == 0x1F00 && r.U32(0x50 + i * 12) == starts[i] - 0x88);
        XSFLIB_CHECK(r.U32(0x54 + i * 12) == archive.waves[i].size());
        XSFLIB_CHECK(Filled(file, starts[i], starts[i] + archive.waves[i].size(), archive.waves[i][0]));
        XSFLIB_CHECK(parsed.waves[i].data() == file.data() + starts[i] &&
                     parsed.waves[i].size() == archive.waves[i].size());
    }
    XSFLIB_CHECK(Filled(file, starts[0] + 0x50, starts[1], 0) && Filled(file, starts[1] + 0x21, starts[2], 0));
    XSFLIB_CHECK(WaveArchiveFile::Read(file).waves == archive.waves && WaveArchiveFile::Read(file).Write() == file);
    XSFLIB_CHECK(none.size() == 0x80 && xsflib::Reader(none, flavor == Flavor::kCafe).U32(0x24) == 0x60);
    XSFLIB_CHECK(WaveArchiveFile::Read(none).waves.empty() && WaveArchiveFile::Read(none).Write() == none);
}

// A wave file that the test builder lays out another way reads, with the same settings and samples.
void TestOtherLayout(Flavor flavor)
{
    sar_files::WaveSpec spec;
    spec.loop = true;
    spec.loop_start = 14;
    spec.loop_end = 42;
    spec.channels = {std::vector<uint8_t>(24, 0x80), std::vector<uint8_t>(24, 0x90)};
    const std::vector<uint8_t> built = sar_files::Wave(flavor, spec);

    const WaveFile read = WaveFile::Read(built);
    const std::vector<uint8_t> written = read.Write();
    const ntdlib::sar::Wave rewritten = ntdlib::sar::Wave::Parse(written, StrictOptions(flavor));

    XSFLIB_CHECK(read.loop && read.loop_start == 14 && read.loop_end == 42 && read.channels.size() == 2);
    XSFLIB_CHECK(read.channels[0].adpcm->coefs[15] == 16 && read.channels[0].adpcm->pred_scale == 0x80);
    XSFLIB_CHECK(rewritten.loop && rewritten.loop_end == 42 && rewritten.channels[1].adpcm->pred_scale == 0x90);
    XSFLIB_CHECK(rewritten.channels[1].data.size() >= 24 && rewritten.channels[1].data[23] == 0x90);
}

template <class Error, class Call>
bool Throws(Call call)
{
    try
    {
        call();
    }
    catch (const Error&)
    {
        return true;
    }

    return false;
}

// Whether WaveFile::Read refuses `bytes` with a message that holds `part`.
bool Refuses(const std::vector<uint8_t>& bytes, const std::string& part)
{
    try
    {
        WaveFile::Read(bytes);
    }
    catch (const xsflib::FormatError& e)
    {
        return std::string(e.what()).find(part) != std::string::npos;
    }

    return false;
}

// The writer refuses an IMA-ADPCM wave, and a DSP-ADPCM info that doesn't match the encoding. The readers refuse
// another file, a file too short for its header, a file without a byte order mark, a big-endian 3DS file, a missing
// block, an IMA-ADPCM wave, a DSP-ADPCM channel without its info, and channels whose samples don't follow one another.
void TestRefusals()
{
    WaveFile ima;
    ima.encoding = WaveEncoding::kImaAdpcm;
    ima.channels = {{{0, 0}, std::nullopt}};
    WaveFile bare;
    bare.encoding = WaveEncoding::kDspAdpcm;
    bare.channels = {{{0, 0}, std::nullopt}};
    WaveFile pcm;
    pcm.channels = {{{0, 0}, Adpcm(1)}};
    WaveFile stereo;
    stereo.encoding = WaveEncoding::kDspAdpcm;
    stereo.channels = {{std::vector<uint8_t>(8, 1), Adpcm(1)}, {std::vector<uint8_t>(8, 2), Adpcm(2)}};
    const std::vector<uint8_t> file = stereo.Write();
    std::vector<uint8_t> other_magic = file;
    other_magic[0] = 'X';
    std::vector<uint8_t> no_mark = file;
    no_mark[4] = 0;
    WaveFile cafe = stereo;
    cafe.flavor = Flavor::kCafe;
    std::vector<uint8_t> big_ctr = cafe.Write();
    big_ctr[0] = 'C';
    std::vector<uint8_t> no_data = file;
    no_data[0x20] = 0x02;
    std::vector<uint8_t> ima_file = file;
    ima_file[0x48] = 3;
    std::vector<uint8_t> no_info = file;
    no_info[0x78] = 0;
    no_info[0x79] = 0;
    std::vector<uint8_t> swapped = file;
    std::swap_ranges(swapped.begin() + 0x74, swapped.begin() + 0x78, swapped.begin() + 0x88);
    WaveArchiveFile archive;
    std::vector<uint8_t> no_file = archive.Write();
    no_file[0x20] = 0x02;

    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { ima.Write(); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { bare.Write(); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { pcm.Write(); }));
    XSFLIB_CHECK(!Throws<xsflib::FormatError>([&] { WaveFile::Read(file); }));
    XSFLIB_CHECK(Refuses(sar_files::Sequence(Flavor::kNx, {}, {}), "not a wave file") &&
                 Refuses(other_magic, "not a wave"));
    XSFLIB_CHECK(Refuses(std::vector<uint8_t>(file.begin(), file.begin() + 0x13), "not a wave file"));
    XSFLIB_CHECK(Refuses(no_mark, "without a byte order mark") && Refuses(big_ctr, "a big-endian CWAV"));
    XSFLIB_CHECK(Refuses(no_data, "no DATA block") && Refuses(ima_file, "encoding 3"));
    XSFLIB_CHECK(Refuses(no_info, "without its DSP-ADPCM info") && Refuses(swapped, "don't follow one another"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WaveArchiveFile::Read(file); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WaveArchiveFile::Read(no_file); }));
}

// The wave archive and the index of each wave of a bank or a wave sound file.
std::vector<std::pair<uint32_t, uint32_t>> PairsOf(const std::vector<ntdlib::sar::WaveId>& waves)
{
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    for (const ntdlib::sar::WaveId& wave : waves)
    {
        pairs.emplace_back(wave.wave_archive_item, wave.index);
    }

    return pairs;
}

// A bank's and a wave sound file's waves take the wave archives that the map gives. The waves that the map leaves out
// keep theirs, and only the bytes of the IDs change.
void TestWaveArchives(Flavor flavor)
{
    const uint32_t war0 = ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, 0);
    const uint32_t war1 = ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, 1);
    const uint32_t war7 = ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, 7);
    const std::vector<std::pair<uint32_t, uint32_t>> waves = {{war0, 0}, {war1, 5}, {war0, 2}};
    sar_files::BankSpec bank_spec;
    bank_spec.waves = waves;
    bank_spec.instruments = {std::nullopt};
    sar_files::WaveSoundFileSpec sound_spec;
    sound_spec.waves = waves;
    const std::vector<uint8_t> bank = sar_files::Bank(flavor, bank_spec);
    const std::vector<uint8_t> sounds = sar_files::WaveSoundFile(flavor, sound_spec);

    const std::vector<uint8_t> new_bank = ntdlib::sar::WithWaveArchives(bank, flavor, {{war0, war7}});
    const std::vector<uint8_t> new_sounds = ntdlib::sar::WithWaveArchives(sounds, flavor, {{war0, war7}, {war7, war1}});

    const std::vector<std::pair<uint32_t, uint32_t>> expected = {{war7, 0}, {war1, 5}, {war7, 2}};
    XSFLIB_CHECK(PairsOf(ntdlib::sar::Bank::Parse(new_bank, StrictOptions(flavor)).waves) == expected);
    XSFLIB_CHECK(PairsOf(ntdlib::sar::WaveSoundFile::Parse(new_sounds, StrictOptions(flavor)).waves) == expected);
    for (const auto& [before, after] : {std::pair(&bank, &new_bank), std::pair(&sounds, &new_sounds)})
    {
        const auto differ = [](uint8_t a, uint8_t b)
        {
            return a != b;
        };
        std::size_t changed = 0;
        for (std::size_t i = 0; i < before->size() && i < after->size(); ++i)
        {
            changed += differ((*before)[i], (*after)[i]) ? 1 : 0;
        }
        XSFLIB_CHECK(before->size() == after->size() && changed == 2);
    }
}

// WithWaveArchives refuses another file, a file of another console, and a table of waves past the file's end: here
// the last wave's index lies past it.
void TestWaveArchiveRefusals()
{
    sar_files::BankSpec spec;
    spec.waves.assign(64, {ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, 0), 0});
    const std::vector<uint8_t> bank = sar_files::Bank(Flavor::kNx, spec);
    const auto info =
        static_cast<std::size_t>(std::ranges::search(bank, std::string_view("INFO")).begin() - bank.begin()) + 8;
    const std::size_t table = info + xsflib::LoadLe32(&bank[info + 4]);
    std::vector<uint8_t> cut = bank;
    cut.resize(table + 4 + 63 * 8 + 4);

    XSFLIB_CHECK(!Throws<xsflib::FormatError>([&] { ntdlib::sar::WithWaveArchives(bank, Flavor::kNx, {}); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { ntdlib::sar::WithWaveArchives(bank, Flavor::kCafe, {}); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { ntdlib::sar::WithWaveArchives(bank, Flavor::kCtr, {}); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>(
        [&] { ntdlib::sar::WithWaveArchives(WaveArchiveFile().Write(), Flavor::kNx, {}); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { ntdlib::sar::WithWaveArchives(cut, Flavor::kNx, {}); }));
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestDspAdpcmWave(flavor);
        TestPcmWave(flavor);
        TestWaveArchive(flavor);
        TestOtherLayout(flavor);
        TestWaveArchives(flavor);
    }
    TestRefusals();
    TestWaveArchiveRefusals();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("Wave file tests passed\n");

    return 0;
}
