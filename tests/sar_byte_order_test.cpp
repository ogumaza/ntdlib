// SPDX-License-Identifier: MIT

// Tests of the files of one console as files of another (ntdlib/sar/byte_order.h). The builders of sar_files.h write
// each bank and wave sound file for each of the three consoles from one spec. A file of one console has to become the
// file of another, byte for byte, but for the version. The readers of formats.h read the files back.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/testing/sar_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
using ntdlib::sar::ConvertWave;
using ntdlib::sar::ConvertWaveArchive;
using ntdlib::sar::Flavor;
using ntdlib::sar::WaveArchiveFile;
using ntdlib::sar::WaveEncoding;
using ntdlib::sar::WaveFile;
using ntdlib::sar::WithConsole;
using ntdlib::sar::WithOtherByteOrder;

constexpr uint32_t kBankVersion = 0x00010000;
constexpr uint32_t kWaveSoundVersion = 0x00010100;

// The versions that sar_files.h gives the 3DS's banks and wave sound files.
constexpr uint32_t kCtrBankVersion = 0x01000100;
constexpr uint32_t kCtrWaveSoundVersion = 0x01000000;

template <typename Error, typename Call>
bool Throws(Call call)
{
    return !ErrorOf<Error>(call).empty();
}

ntdlib::sar::Options Options(Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

    return options;
}

// A region without parameters.
sar_files::Region PlainRegion(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;

    return region;
}

// A region with every parameter that the walk knows.
sar_files::Region FullRegion(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;
    region.original_key = 60 + static_cast<int>(wave);
    region.volume = 100;
    region.pan = 20;
    region.pitch = 1.5f;
    region.flags = 0x00020301;
    region.adshr = std::array<uint8_t, 5>{127, 100, 90, 80, 70};

    return region;
}

// A bank with each kind of region table: a direct key table over a range velocity table of five entries, one empty, an
// index key table with an empty key, and an empty instrument.
sar_files::BankSpec Spec()
{
    sar_files::VelocityTable range;
    range.kind = sar_files::VelocityTable::kRange;
    range.entries = {
        {20, FullRegion(0)}, {40, std::nullopt}, {60, FullRegion(1)}, {100, PlainRegion(0)}, {127, FullRegion(2)}};
    sar_files::KeyTable direct;
    direct.entries = {{127, range}};

    sar_files::VelocityTable single;
    single.entries = {{127, FullRegion(1)}};
    sar_files::KeyTable index;
    index.kind = sar_files::KeyTable::kIndex;
    index.first = 36;
    index.entries = {{36, single}, {37, std::nullopt}, {38, single}};

    sar_files::BankSpec spec;
    spec.waves = {{0x05000001, 3}, {0x05000002, 0}, {0x05000001, 7}};
    spec.instruments = {direct, std::nullopt, index};

    return spec;
}

// A bank file of one console becomes the other's, byte for byte, and reads with the same regions.
void TestBank()
{
    const std::vector<uint8_t> wii_u = sar_files::Bank(Flavor::kCafe, Spec());
    const std::vector<uint8_t> switch_bank = sar_files::Bank(Flavor::kNx, Spec());

    const std::vector<uint8_t> to_switch = WithOtherByteOrder(wii_u, kBankVersion);
    const std::vector<uint8_t> to_wii_u = WithOtherByteOrder(switch_bank, kBankVersion);

    XSFLIB_CHECK(to_switch == switch_bank);
    XSFLIB_CHECK(to_wii_u == wii_u);
    const ntdlib::sar::Bank read = ntdlib::sar::Bank::Parse(to_switch, Options(Flavor::kNx));
    XSFLIB_CHECK(read.instruments.size() == 3 && read.waves.size() == 3 && read.waves[2].index == 7);
    XSFLIB_CHECK(read.instruments[0].has_value() && !read.instruments[1].has_value());
    const ntdlib::sar::VelocityRegion* region = read.instruments[0]->Find(60, 50);
    XSFLIB_CHECK(region && region->wave_id_index == 1 && region->original_key == 61 && region->pitch == 1.5f);
    XSFLIB_CHECK(region && region->key_group == 3 && region->interpolation == 2 && region->adshr.release == 70);
    XSFLIB_CHECK(read.instruments[2]->Find(38, 64) && !read.instruments[2]->Find(37, 64));
}

// The version goes into the header, and only there.
void TestVersion()
{
    const std::vector<uint8_t> wii_u = sar_files::Bank(Flavor::kCafe, Spec());

    const std::vector<uint8_t> to_switch = WithOtherByteOrder(wii_u, 0x00010203);

    XSFLIB_CHECK(xsflib::LoadLe32(&to_switch[8]) == 0x00010203);
    std::vector<uint8_t> expected = sar_files::Bank(Flavor::kNx, Spec());
    xsflib::StoreLe32(&expected[8], 0x00010203);
    XSFLIB_CHECK(to_switch == expected);
}

// Two instruments can share their table of key regions. The walk writes the shared fields once.
void TestSharedTable()
{
    // The second instrument's reference names the first instrument. Its old structures, at the end of INFO, become
    // zeros.
    const auto shared = [](Flavor flavor)
    {
        sar_files::BankSpec spec = Spec();
        spec.instruments = {spec.instruments[0], spec.instruments[0]};
        std::vector<uint8_t> file = sar_files::Bank(flavor, spec);
        const bool big_endian = flavor == Flavor::kCafe;
        const xsflib::Reader r(file, big_endian);
        const std::size_t info = r.U32(0x18) + 8;
        const std::size_t table = info + r.U32(info + 12);
        const std::size_t first = table + r.U32(table + 8);
        const std::size_t second = table + r.U32(table + 16);
        std::fill(file.begin() + static_cast<std::ptrdiff_t>(second),
                  file.begin() + static_cast<std::ptrdiff_t>(r.U32(0x18) + r.U32(0x1C)), uint8_t{0});
        big_endian ? xsflib::StoreBe32(&file[table + 16], static_cast<uint32_t>(first - table))
                   : xsflib::StoreLe32(&file[table + 16], static_cast<uint32_t>(first - table));

        return file;
    };
    const std::vector<uint8_t> wii_u = shared(Flavor::kCafe);

    const std::vector<uint8_t> to_switch = WithOtherByteOrder(wii_u, kBankVersion);

    XSFLIB_CHECK(to_switch == shared(Flavor::kNx));
    const ntdlib::sar::Bank read = ntdlib::sar::Bank::Parse(to_switch, Options(Flavor::kNx));
    XSFLIB_CHECK(read.instruments.size() == 2 && read.instruments[1]->Find(60, 50) &&
                 read.instruments[1]->Find(60, 50)->wave_id_index == 1);
}

sar_files::WaveSoundFileSpec WaveSoundSpec()
{
    sar_files::WaveSoundSpec sound;
    sound.pan = 30;
    sound.pitch = 0.75f;
    sound.filter = 0x40;
    sound.sends = {127, 3, 10, 20, 30};
    sound.adshr = std::array<uint8_t, 5>{127, 127, 127, 127, 60};
    sound.tracks = {{{0.0f, 1.5f, 0}, {2.0f, 0.5f, 1}}, {{1.0f, 3.0f, 1}}};
    sound.notes = {PlainRegion(0), FullRegion(1)};

    sar_files::WaveSoundSpec plain;
    plain.tracks = {{{0.0f, 1.0f, 0}}};
    plain.notes = {PlainRegion(1)};

    sar_files::WaveSoundFileSpec spec;
    spec.waves = {{0x05000003, 1}, {0x05000003, 2}};
    spec.sounds = {sound, plain};

    return spec;
}

// A wave sound file of one console becomes the other's, byte for byte, and reads with the same wave sounds.
void TestWaveSoundFile()
{
    const std::vector<uint8_t> wii_u = sar_files::WaveSoundFile(Flavor::kCafe, WaveSoundSpec());
    const std::vector<uint8_t> switch_file = sar_files::WaveSoundFile(Flavor::kNx, WaveSoundSpec());

    const std::vector<uint8_t> to_switch = WithOtherByteOrder(wii_u, kWaveSoundVersion);
    const std::vector<uint8_t> to_wii_u = WithOtherByteOrder(switch_file, kWaveSoundVersion);

    XSFLIB_CHECK(to_switch == switch_file);
    XSFLIB_CHECK(to_wii_u == wii_u);
    const ntdlib::sar::WaveSoundFile read = ntdlib::sar::WaveSoundFile::Parse(to_switch, Options(Flavor::kNx));
    XSFLIB_CHECK(read.sounds.size() == 2 && read.sounds[0].pan == 30 && read.sounds[0].pitch == 0.75f);
    XSFLIB_CHECK(read.sounds[0].tracks.size() == 2 && read.sounds[0].tracks[0][1].position == 2.0f);
    XSFLIB_CHECK(read.sounds[0].notes.size() == 2 && read.sounds[0].notes[1].volume == 100);
    XSFLIB_CHECK(read.sounds[0].adshr.release == 60 && read.waves[1].index == 2);
}

// Where a reference at `at` of `file` points, with its offset from `base`: from the reference itself unless `base`
// gives another place.
std::size_t Target(const std::vector<uint8_t>& file, Flavor flavor, std::size_t at,
                   std::optional<std::size_t> base = {})
{
    const xsflib::Reader r(file, flavor == Flavor::kCafe);

    return base.value_or(at) + static_cast<std::size_t>(r.S32(at + 4));
}

// Writes `value` at `at` of `file` in `flavor`'s byte order.
void Store(std::vector<uint8_t>& file, Flavor flavor, std::size_t at, uint32_t value)
{
    flavor == Flavor::kCafe ? xsflib::StoreBe32(&file[at], value) : xsflib::StoreLe32(&file[at], value);
}

// Where INFO's body starts and the block ends, and where the table of key regions of instrument `instrument` lies.
// INFO's references count from the body's start, and a table's references from the table's start.
struct Places
{
    std::size_t info = 0;
    std::size_t end = 0;
    std::size_t keys = 0;
};

Places PlacesOf(const std::vector<uint8_t>& file, Flavor flavor, std::size_t instrument)
{
    const xsflib::Reader r(file, flavor == Flavor::kCafe);
    Places places;
    places.info = r.U32(0x18) + 8;
    places.end = r.U32(0x18) + r.U32(0x1C);
    const std::size_t table = Target(file, flavor, places.info + 8, places.info);
    places.keys = Target(file, flavor, Target(file, flavor, table + 4 + instrument * 8, table));

    return places;
}

// Two key regions can share their table of velocity regions. The walk writes the shared fields again, unchanged.
void TestSharedVelocityTable()
{
    // The second key region names the first key region's table. Its own table and region, at the end of INFO, become
    // zeros.
    const auto shared = [](Flavor flavor)
    {
        sar_files::VelocityTable first;
        first.entries = {{127, FullRegion(0)}};
        sar_files::VelocityTable second;
        second.entries = {{127, FullRegion(1)}};
        sar_files::KeyTable keys;
        keys.kind = sar_files::KeyTable::kRange;
        keys.entries = {{60, first}, {127, second}};
        sar_files::BankSpec spec;
        spec.waves = {{0x05000001, 0}, {0x05000001, 1}};
        spec.instruments = {keys};
        std::vector<uint8_t> file = sar_files::Bank(flavor, spec);

        const Places places = PlacesOf(file, flavor, 0);
        const std::size_t first_key = Target(file, flavor, places.keys + 8, places.keys);
        const std::size_t second_key = Target(file, flavor, places.keys + 16, places.keys);
        const std::size_t first_table = Target(file, flavor, first_key);
        const std::size_t second_table = Target(file, flavor, second_key);
        std::fill(file.begin() + static_cast<std::ptrdiff_t>(second_table),
                  file.begin() + static_cast<std::ptrdiff_t>(places.end), uint8_t{0});
        Store(file, flavor, second_key + 4, static_cast<uint32_t>(first_table - second_key));

        return file;
    };
    const std::vector<uint8_t> wii_u = shared(Flavor::kCafe);

    const std::vector<uint8_t> to_switch = WithOtherByteOrder(wii_u, kBankVersion);

    XSFLIB_CHECK(to_switch == shared(Flavor::kNx));
    const ntdlib::sar::Bank read = ntdlib::sar::Bank::Parse(to_switch, Options(Flavor::kNx));
    XSFLIB_CHECK(read.instruments[0] && read.instruments[0]->Find(100, 64) &&
                 read.instruments[0]->Find(100, 64)->wave_id_index == 0);
}

// A reference of kNoRegionReference names no region, whatever its offset.
void TestNoRegionWithOffset()
{
    // The empty second entry of the first instrument's velocity table points at the table's start.
    const auto pointing = [](Flavor flavor)
    {
        std::vector<uint8_t> file = sar_files::Bank(flavor, Spec());
        const Places places = PlacesOf(file, flavor, 0);
        const std::size_t velocities = Target(file, flavor, Target(file, flavor, places.keys));
        Store(file, flavor, velocities + 12 + 8 + 4, 0);

        return file;
    };

    const std::vector<uint8_t> to_switch = WithOtherByteOrder(pointing(Flavor::kCafe), kBankVersion);

    XSFLIB_CHECK(to_switch == pointing(Flavor::kNx));
}

// A bank file and a wave sound file of the 3DS become the Wii U's and the Switch's, byte for byte but for the version,
// and the other way round. The magic takes the console's letter, and the fields its byte order.
void TestConsoles()
{
    const Flavor consoles[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};
    const auto bank = [](Flavor flavor)
    {
        return sar_files::Bank(flavor, Spec());
    };
    const auto wave_sound = [](Flavor flavor)
    {
        return sar_files::WaveSoundFile(flavor, WaveSoundSpec());
    };

    for (const Flavor from : consoles)
    {
        for (const Flavor to : consoles)
        {
            const uint32_t bank_version = to == Flavor::kCtr ? kCtrBankVersion : kBankVersion;
            const uint32_t wave_sound_version = to == Flavor::kCtr ? kCtrWaveSoundVersion : kWaveSoundVersion;
            XSFLIB_CHECK(WithConsole(bank(from), to, bank_version) == bank(to));
            XSFLIB_CHECK(WithConsole(wave_sound(from), to, wave_sound_version) == wave_sound(to));
        }
    }
    const ntdlib::sar::Bank read = ntdlib::sar::Bank::Parse(
        WithConsole(bank(Flavor::kCafe), Flavor::kCtr, kCtrBankVersion), Options(Flavor::kCtr));
    XSFLIB_CHECK(read.instruments.size() == 3 && read.instruments[0]->Find(60, 50)->original_key == 61);

    // A big-endian file of the 3DS doesn't exist. WithOtherByteOrder takes only the Wii U's and the Switch's files.
    std::vector<uint8_t> big = bank(Flavor::kCafe);
    big[0] = 'C';
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithConsole(big, Flavor::kNx, kBankVersion); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithOtherByteOrder(bank(Flavor::kCtr), kBankVersion); }));
}

// What the walk refuses: a 3DS file, another kind of file, an option that it doesn't know, a byte that no structure
// holds, and structures that overlap.
void TestRefusals()
{
    using xsflib::FormatError;

    XSFLIB_CHECK(Throws<FormatError>([&] { WithOtherByteOrder(sar_files::Bank(Flavor::kCtr, Spec()), 1); }));
    XSFLIB_CHECK(Throws<FormatError>([&] { WithOtherByteOrder(sar_files::WaveArchive(Flavor::kCafe, {}), 1); }));

    // Bit 5 in place of bit 4 of a region without an envelope.
    sar_files::Region plain = FullRegion(0);
    plain.adshr.reset();
    sar_files::VelocityTable velocities;
    velocities.entries = {{127, plain}};
    sar_files::KeyTable keys;
    keys.entries = {{127, velocities}};
    sar_files::BankSpec spec;
    spec.waves = {{0x05000001, 0}};
    spec.instruments = {keys};
    std::vector<uint8_t> option = sar_files::Bank(Flavor::kCafe, spec);
    const Places places = PlacesOf(option, Flavor::kCafe, 0);
    const std::size_t table = Target(option, Flavor::kCafe, Target(option, Flavor::kCafe, places.keys));
    const std::size_t region = Target(option, Flavor::kCafe, table);
    XSFLIB_CHECK(xsflib::LoadBe32(&option[region + 4]) == 0x1F);
    XSFLIB_CHECK(WithOtherByteOrder(option, kBankVersion) == sar_files::Bank(Flavor::kNx, spec));
    xsflib::StoreBe32(&option[region + 4], 0x2F);
    XSFLIB_CHECK(Throws<FormatError>([&] { WithOtherByteOrder(option, kBankVersion); }));

    // A wave sound's sends in a region: bit 8 in place of bit 4, with a value that names the zeros after the region.
    constexpr uint32_t kRegionBytes = 4 + 4 + 4 * 5;
    xsflib::StoreBe32(&option[region + 4], 0x10F);
    xsflib::StoreBe32(&option[region + 24], kRegionBytes);
    XSFLIB_CHECK(Throws<FormatError>([&] { WithOtherByteOrder(option, kBankVersion); }));

    // A byte in the padding at INFO's end.
    std::vector<uint8_t> stray = sar_files::Bank(Flavor::kCafe, Spec());
    stray.back() = 1;
    XSFLIB_CHECK(Throws<FormatError>([&] { WithOtherByteOrder(stray, kBankVersion); }));

    // The reference to the table of waves names the table of instruments. A bank without waves leaves no other bytes.
    sar_files::BankSpec no_waves = Spec();
    no_waves.waves.clear();
    std::vector<uint8_t> overlap = sar_files::Bank(Flavor::kCafe, no_waves);
    const std::size_t info = PlacesOf(overlap, Flavor::kCafe, 0).info;
    xsflib::StoreBe32(&overlap[info + 4], xsflib::LoadBe32(&overlap[info + 12]));
    XSFLIB_CHECK(Throws<FormatError>([&] { WithOtherByteOrder(overlap, kBankVersion); }));
}

// A wave of 16-bit PCM takes the other byte order. Its loop takes its original start from the version. The samples
// after its length go.
void TestPcmWave()
{
    WaveFile wave;
    wave.flavor = Flavor::kCafe;
    wave.version = 0x00010100;
    wave.encoding = WaveEncoding::kPcm16;
    wave.loop = true;
    wave.sample_rate = 32000;
    wave.loop_start = 1;
    wave.loop_end = 3;
    wave.channels = {{{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xFF, 0xFF}, std::nullopt}};

    const WaveFile to_switch = ConvertWave(wave, Flavor::kNx, 0x00010200);
    const WaveFile back = ConvertWave(to_switch, Flavor::kCafe, 0x00010100);

    XSFLIB_CHECK(to_switch.flavor == Flavor::kNx && to_switch.version == 0x00010200);
    XSFLIB_CHECK(to_switch.channels[0].samples == std::vector<uint8_t>({0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A}));
    XSFLIB_CHECK(to_switch.original_loop_start == 1 && to_switch.loop_start == 1 && to_switch.loop_end == 3);
    XSFLIB_CHECK(back.channels[0].samples == std::vector<uint8_t>({0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}));
    XSFLIB_CHECK(back.original_loop_start == 0 && back.flavor == Flavor::kCafe && back.version == 0x00010100);
    const WaveFile read = WaveFile::Read(to_switch.Write());
    XSFLIB_CHECK(read.channels[0].samples == to_switch.channels[0].samples && read.original_loop_start == 1);
}

// A DSP-ADPCM wave keeps its frames and its infos. A wave of 0x00010200 keeps its loop's original start.
void TestAdpcmWave()
{
    WaveFile wave;
    wave.flavor = Flavor::kNx;
    wave.version = 0x00010200;
    wave.encoding = WaveEncoding::kDspAdpcm;
    wave.loop = true;
    wave.loop_start = 14;
    wave.loop_end = 30;
    wave.original_loop_start = 12;
    ntdlib::sar::DspAdpcmInfo info;
    info.coefs[0] = -2;
    info.pred_scale = 0x17;
    info.loop_yn1 = -300;
    std::vector<uint8_t> frames(24 + 40);
    for (std::size_t k = 0; k < frames.size(); ++k)
    {
        frames[k] = static_cast<uint8_t>(k + 1);
    }
    wave.channels = {{frames, info}, {frames, info}};

    const WaveFile to_wii_u = ConvertWave(wave, Flavor::kCafe, 0x00010200);
    const WaveFile older = ConvertWave(wave, Flavor::kCafe, 0x00010100);

    XSFLIB_CHECK(to_wii_u.original_loop_start == 12 && older.original_loop_start == 0);
    XSFLIB_CHECK(to_wii_u.channels[1].samples == std::vector<uint8_t>(frames.begin(), frames.begin() + 24));
    XSFLIB_CHECK(to_wii_u.channels[1].adpcm && to_wii_u.channels[1].adpcm->loop_yn1 == -300);
    const WaveFile read = WaveFile::Read(to_wii_u.Write());
    XSFLIB_CHECK(read.flavor == Flavor::kCafe && read.channels[0].samples.size() == 32);
    XSFLIB_CHECK(read.channels[1].samples.size() == 24 && read.channels[1].adpcm->coefs[0] == -2);
}

// A wave archive's waves become waves of the other console, as ConvertWave makes them.
void TestWaveArchive()
{
    sar_files::WaveSpec pcm;
    pcm.encoding = 1;
    pcm.loop_end = 2;
    pcm.channels = {{1, 2, 3, 4}};
    sar_files::WaveSpec adpcm;
    adpcm.loop = true;
    adpcm.loop_start = 0;
    adpcm.loop_end = 14;
    adpcm.channels = {{0x11, 1, 2, 3, 4, 5, 6, 7}};
    const std::vector<uint8_t> waves[] = {sar_files::Wave(Flavor::kCafe, pcm), sar_files::Wave(Flavor::kCafe, adpcm)};
    const WaveArchiveFile archive = WaveArchiveFile::Read(sar_files::WaveArchive(Flavor::kCafe, {waves[0], waves[1]}));

    const WaveArchiveFile converted = ConvertWaveArchive(archive, Flavor::kNx, 0x00010000, 0x00010200);

    XSFLIB_CHECK(converted.flavor == Flavor::kNx && converted.version == 0x00010000 && converted.waves.size() == 2);
    for (std::size_t i = 0; i < 2; ++i)
    {
        const WaveFile expected = ConvertWave(WaveFile::Read(waves[i]), Flavor::kNx, 0x00010200);
        XSFLIB_CHECK(converted.waves[i] == expected.Write());
    }
    XSFLIB_CHECK(WaveFile::Read(converted.waves[0]).channels[0].samples == std::vector<uint8_t>({2, 1, 4, 3}));

    // A wave archive of the 3DS and back, laid out as WaveFile writes a wave. 16-bit PCM is little-endian on the 3DS
    // too.
    const WaveArchiveFile ctr = ConvertWaveArchive(archive, Flavor::kCtr, 0x01000000, 0x02010000);
    const WaveArchiveFile back = ConvertWaveArchive(WaveArchiveFile::Read(ctr.Write()), Flavor::kCafe, 0x00010000,
                                                    WaveFile::Read(waves[0]).version);
    XSFLIB_CHECK(ctr.flavor == Flavor::kCtr && WaveFile::Read(ctr.waves[0]).flavor == Flavor::kCtr);
    XSFLIB_CHECK(WaveFile::Read(ctr.waves[0]).channels[0].samples == std::vector<uint8_t>({2, 1, 4, 3}));
    for (std::size_t i = 0; i < 2; ++i)
    {
        const WaveFile wave = WaveFile::Read(waves[i]);
        XSFLIB_CHECK(back.waves[i] == ConvertWave(wave, Flavor::kCafe, wave.version).Write());
    }
}

// The loop's original start follows each console's versions: 2.1.1.0 on the 3DS, and 1.2.0 on the Wii U and the
// Switch.
void TestCtrWave()
{
    WaveFile wave;
    wave.flavor = Flavor::kCtr;
    wave.version = 0x02010000;
    wave.encoding = WaveEncoding::kPcm16;
    wave.loop = true;
    wave.loop_start = 1;
    wave.loop_end = 3;
    wave.channels = {{{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}, std::nullopt}};

    const WaveFile to_switch = ConvertWave(wave, Flavor::kNx, 0x00010200);
    const WaveFile to_wii_u = ConvertWave(wave, Flavor::kCafe, 0x00010100);
    const WaveFile newer = ConvertWave(to_switch, Flavor::kCtr, 0x02010100);
    const WaveFile older = ConvertWave(to_switch, Flavor::kCtr, 0x02010000);

    XSFLIB_CHECK(to_switch.original_loop_start == 1 && to_switch.channels[0].samples == wave.channels[0].samples);
    XSFLIB_CHECK(to_wii_u.original_loop_start == 0);
    XSFLIB_CHECK(to_wii_u.channels[0].samples == std::vector<uint8_t>({0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A}));
    XSFLIB_CHECK(newer.original_loop_start == 1 && older.original_loop_start == 0);
    XSFLIB_CHECK(WaveFile::Read(newer.Write()).flavor == Flavor::kCtr);
}

} // namespace

int main()
{
    TestBank();
    TestVersion();
    TestSharedTable();
    TestSharedVelocityTable();
    TestNoRegionWithOffset();
    TestWaveSoundFile();
    TestConsoles();
    TestRefusals();
    TestPcmWave();
    TestAdpcmWave();
    TestWaveArchive();
    TestCtrWave();

    return xsflib::testing::Summary("Byte order");
}
