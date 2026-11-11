// SPDX-License-Identifier: MIT

// Tests of the Wii's banks and waves as files of the 3DS, the Wii U and the Switch, and of theirs as the Wii's
// (ntdlib/rsar/convert.h), on the banks and waves of ntdlib/testing/rsar_files.h. A converted wave decodes to the
// samples that it held. A converted bank plays the region that it played at every key and velocity, read back by the
// other console's reader. The later consoles' wave archives and wave sound files come from
// ntdlib/testing/sar_files.h. A converted wave archive holds the waves' samples, and a converted wave sound file the
// values that the Wii plays.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace rsar_files = ntdlib::testing::rsar_files;
namespace sar_files = ntdlib::testing::sar_files;
using ntdlib::SndBuild;
using ntdlib::rsar::Bank;
using ntdlib::rsar::Wave;
using ntdlib::sar::Flavor;
using xsflib::testing::ErrorOf;
using Bytes = std::vector<uint8_t>;

constexpr ntdlib::rsar::ReadOptions kPlayback{.playback_fields = true};
constexpr SndBuild kBuild2007{.date = 20070608};
constexpr SndBuild kBuild2008{.date = 20080620};
constexpr uint32_t kWaveArchive = ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, 7);

// A console's flavor, and a version of its wave files.
struct WaveTarget
{
    Flavor flavor;
    uint32_t version;
};

constexpr WaveTarget kWaveTargets[] = {
    {Flavor::kCtr, 0x02010000}, {Flavor::kCafe, 0x00010100}, {Flavor::kNx, 0x00010200}};

// The samples that the later consoles' decoder gives for `file`, a wave of `flavor`.
ntdlib::wave::Pcm Decoded(const ntdlib::sar::WaveFile& file, Flavor flavor)
{
    const Bytes bytes = file.Write();
    return ntdlib::sar::DecodeWave(bytes, ntdlib::sar::Options{.flavor = flavor});
}

// Whether `a` and `b` hold the same samples, rate and loop.
bool SamePcm(const ntdlib::wave::Pcm& a, const ntdlib::wave::Pcm& b)
{
    return a.sample_rate == b.sample_rate && a.loop == b.loop && a.loop_start == b.loop_start &&
           a.loop_end == b.loop_end && a.channels == b.channels;
}

// A DSP-ADPCM wave keeps its settings and its state, and decodes to the Wii's samples on each console.
void TestAdpcmWave()
{
    const Bytes rwav = rsar_files::MakeRwav(0x31, true, 22050);
    const Wave wave = Wave::ParseFile(rwav, kPlayback);
    for (const auto [flavor, version] : kWaveTargets)
    {
        const ntdlib::sar::WaveFile file = ntdlib::rsar::WaveFileOf(wave, flavor, version);
        XSFLIB_CHECK(file.flavor == flavor && file.version == version);
        XSFLIB_CHECK(file.encoding == ntdlib::sar::WaveEncoding::kDspAdpcm && file.loop && file.sample_rate == 22050);
        XSFLIB_CHECK(file.loop_start == wave.loop_start && file.loop_end == wave.loop_end);
        XSFLIB_CHECK(file.original_loop_start == wave.loop_start);
        XSFLIB_CHECK(file.channels.size() == 1 && file.channels[0].adpcm.has_value());
        XSFLIB_CHECK(file.channels[0].samples.size() == (wave.loop_end + 13) / 14 * 8);
        XSFLIB_CHECK(file.channels[0].samples[0] == 0x31);
        XSFLIB_CHECK(file.channels[0].adpcm->coefs == wave.channels[0].adpcm->coefs);
        XSFLIB_CHECK(file.channels[0].adpcm->pred_scale == wave.channels[0].adpcm->pred_scale);
        XSFLIB_CHECK(SamePcm(Decoded(file, flavor), ntdlib::rsar::DecodeWave(wave)));
    }
}

// A wave of 16-bit PCM takes each console's byte order. NW4R SND plays two channels, and the file keeps two. A channel
// that ends before the wave's end can't convert.
void TestPcmWave()
{
    const std::vector<int16_t> samples = {1, -2, 300, -400, 5, 0x1234};
    const Bytes stereo_file = rsar_files::MakePcm16Rwav(6, samples, 2);
    const Wave stereo = Wave::ParseFile(stereo_file, kPlayback);
    const ntdlib::sar::WaveFile cafe = ntdlib::rsar::WaveFileOf(stereo, Flavor::kCafe, 0x00010100);
    const ntdlib::sar::WaveFile nx = ntdlib::rsar::WaveFileOf(stereo, Flavor::kNx, 0x00010200);
    XSFLIB_CHECK(cafe.channels.size() == 2 && nx.channels.size() == 2 && !cafe.channels[0].adpcm);
    XSFLIB_CHECK(cafe.channels[1].samples.size() == 12 && cafe.channels[1].samples[10] == 0x12);
    XSFLIB_CHECK(nx.channels[1].samples.size() == 12 && nx.channels[1].samples[10] == 0x34);
    XSFLIB_CHECK(SamePcm(Decoded(cafe, Flavor::kCafe), ntdlib::rsar::DecodeWave(stereo)));
    XSFLIB_CHECK(SamePcm(Decoded(nx, Flavor::kNx), ntdlib::rsar::DecodeWave(stereo)));

    const Bytes three_file = rsar_files::MakePcm16Rwav(6, samples, 3);
    const Wave three = Wave::ParseFile(three_file, kPlayback);
    XSFLIB_CHECK(ntdlib::rsar::WaveFileOf(three, Flavor::kNx, 0x00010200).channels.size() == 2);

    const Bytes short_file = rsar_files::MakePcm16Rwav(10, samples, 2);
    const Wave short_channel = Wave::ParseFile(short_file, kPlayback);
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WaveFileOf(short_channel, Flavor::kNx, 0x00010200); })
             .empty());
    Wave silent = stereo;
    silent.channels.clear();
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WaveFileOf(silent, Flavor::kNx, 0x00010200); }).empty());
}

// Whether the later consoles' reader of `file` plays the region of `bank` at every key and velocity of each program,
// with the values that `build` reads.
bool PlaysAsTheWii(const Bank& bank, const SndBuild& build, const ntdlib::sar::BankFile& file, Flavor flavor)
{
    const Bytes bytes = file.Write();
    const ntdlib::sar::Bank read = ntdlib::sar::Bank::Parse(bytes, ntdlib::sar::Options{.flavor = flavor});
    for (uint32_t program = 0; program < bank.InstrumentCount(); ++program)
    {
        for (int key = 0; key < 128; ++key)
        {
            for (int velocity = 0; velocity < 128; ++velocity)
            {
                const ntdlib::rsar::Region* region = bank.Find(program, key, velocity);
                if (region != nullptr && static_cast<std::size_t>(region->wave_index) >= bank.Waves().size())
                {
                    region = nullptr;
                }
                const ntdlib::sar::VelocityRegion* converted =
                    program < read.instruments.size() && read.instruments[program]
                        ? read.instruments[program]->Find(key, velocity)
                        : nullptr;
                if ((region == nullptr) != (converted == nullptr))
                {
                    return false;
                }
                if (region == nullptr)
                {
                    continue;
                }

                const bool march_2008 = build.SinceMarch2008();
                if (converted->wave_id_index != static_cast<uint32_t>(region->wave_index) ||
                    converted->original_key != region->original_key || converted->volume != region->volume ||
                    converted->pan != region->pan || converted->pitch != region->tune ||
                    converted->adshr.attack != region->attack || converted->adshr.decay != region->decay ||
                    converted->adshr.sustain != region->sustain || converted->adshr.release != region->release ||
                    converted->adshr.hold != (march_2008 ? region->hold : 0) ||
                    converted->ignore_note_off != (march_2008 && region->note_off_type == 1) ||
                    converted->key_group != (march_2008 ? region->key_group : 0) || converted->interpolation != 0)
                {
                    return false;
                }
            }
        }
    }

    return read.waves == file.waves;
}

// A bank of version 1.2 with an RWAR plays as the Wii plays it, on each console and by each build's rules.
void TestBank()
{
    const Bytes file = rsar_files::MakeNewBank();
    const Bytes waves = rsar_files::MakeRwar();
    const Bank bank = Bank::Parse(file, waves, kPlayback);
    const ntdlib::sar::BankFile cafe =
        ntdlib::rsar::BankFileOf(bank, kBuild2008, Flavor::kCafe, 0x00010000, kWaveArchive);
    XSFLIB_CHECK(cafe.flavor == Flavor::kCafe && cafe.version == 0x00010000 && cafe.order.empty());
    XSFLIB_CHECK(cafe.waves.size() == 2 && cafe.waves[1].wave_archive_item == kWaveArchive && cafe.waves[1].index == 1);
    XSFLIB_CHECK(cafe.instruments.size() == 70);
    XSFLIB_CHECK(!cafe.instruments[3] && !cafe.instruments[4] && !cafe.instruments[5]);

    // Instrument 1 splits its keys at 63, and instrument 2 its velocities at 63 on keys 60 to 62.
    XSFLIB_CHECK(cafe.instruments[0] && cafe.instruments[0]->key_regions.size() == 1);
    XSFLIB_CHECK(cafe.instruments[1] && cafe.instruments[1]->key_regions.size() == 2);
    XSFLIB_CHECK(cafe.instruments[1]->key_regions[0].last_key == 63);
    XSFLIB_CHECK(cafe.instruments[1]->key_regions[1].regions[0].values.original_key == 80);
    XSFLIB_CHECK(cafe.instruments[2] && cafe.instruments[2]->key_regions.size() == 1);
    XSFLIB_CHECK(cafe.instruments[2]->key_regions[0].first_key == 60 &&
                 cafe.instruments[2]->key_regions[0].last_key == 62);
    XSFLIB_CHECK(cafe.instruments[2]->key_regions[0].regions.size() == 2);
    XSFLIB_CHECK(cafe.instruments[2]->key_regions[0].regions[1].first_velocity == 64);
    XSFLIB_CHECK(cafe.instruments[2]->key_regions[0].regions[1].values.wave_id_index == 1);

    // The note-off type and the key group come in March 2008.
    XSFLIB_CHECK(cafe.instruments[6]->key_regions[0].regions[0].values.ignore_note_off);
    XSFLIB_CHECK(cafe.instruments[7]->key_regions[0].regions[0].values.key_group == 1);
    const ntdlib::sar::BankFile old =
        ntdlib::rsar::BankFileOf(bank, kBuild2007, Flavor::kCafe, 0x00010000, kWaveArchive);
    XSFLIB_CHECK(!old.instruments[6]->key_regions[0].regions[0].values.ignore_note_off);
    XSFLIB_CHECK(old.instruments[7]->key_regions[0].regions[0].values.key_group == 0);

    XSFLIB_CHECK(PlaysAsTheWii(bank, kBuild2008, cafe, Flavor::kCafe));
    XSFLIB_CHECK(PlaysAsTheWii(bank, kBuild2007, old, Flavor::kCafe));
    for (const Flavor flavor : {Flavor::kNx, Flavor::kCtr})
    {
        const uint32_t version = flavor == Flavor::kCtr ? 0x01000100 : 0x00010000;
        XSFLIB_CHECK(PlaysAsTheWii(bank, kBuild2008,
                                   ntdlib::rsar::BankFileOf(bank, kBuild2008, flavor, version, kWaveArchive), flavor));
    }
}

// The count of velocity regions of `later`, BankFileOf's conversion of `bank` for `build`, or 0 where one of them holds
// other values than ValuesOf gives for the region that `bank` plays at its first key and velocity.
std::size_t ValuesAsBankFileOf(const Bank& bank, const SndBuild& build, const ntdlib::sar::BankFile& later)
{
    std::size_t count = 0;
    for (uint32_t program = 0; program < later.instruments.size(); ++program)
    {
        if (!later.instruments[program])
        {
            continue;
        }

        for (const ntdlib::sar::BankFile::KeyRegion& keys : later.instruments[program]->key_regions)
        {
            for (const ntdlib::sar::BankFile::Region& region : keys.regions)
            {
                const ntdlib::rsar::Region* played = bank.Find(program, keys.first_key, region.first_velocity);
                if (played == nullptr || !(ntdlib::rsar::ValuesOf(*played, build) == region.values))
                {
                    return 0;
                }
                ++count;
            }
        }
    }

    return count;
}

// A region's values follow the build. Without a build, they take every field of the region. Before March 2008, they
// take no hold, note-off type or key group. Only a note-off type of 1 ignores the note-off. A region made from the
// values gives the fields back, with the wave that the caller names and without the interpolation. The values agree
// with every velocity region of BankFileOf's conversion of the test bank, for each build.
void TestRegionValues()
{
    ntdlib::rsar::Region region;
    region.wave_index = 3;
    region.attack = 10;
    region.decay = 20;
    region.sustain = 30;
    region.release = 40;
    region.hold = 5;
    region.note_off_type = 1;
    region.key_group = 2;
    region.original_key = 72;
    region.volume = 90;
    region.pan = 30;
    region.tune = 1.5f;

    const ntdlib::sar::VelocityRegion values = ntdlib::rsar::ValuesOf(region);
    const ntdlib::sar::VelocityRegion old = ntdlib::rsar::ValuesOf(region, kBuild2007);

    XSFLIB_CHECK(values.wave_id_index == 3 && values.original_key == 72 && values.volume == 90 && values.pan == 30);
    XSFLIB_CHECK(values.pitch == 1.5f && values.interpolation == 0);
    XSFLIB_CHECK(values.ignore_note_off && values.key_group == 2);
    XSFLIB_CHECK(values.adshr.attack == 10 && values.adshr.decay == 20 && values.adshr.sustain == 30);
    XSFLIB_CHECK(values.adshr.hold == 5 && values.adshr.release == 40);
    XSFLIB_CHECK(ntdlib::rsar::ValuesOf(region, kBuild2008) == values);
    XSFLIB_CHECK(!old.ignore_note_off && old.key_group == 0 && old.adshr.hold == 0);
    XSFLIB_CHECK(old.adshr.attack == 10 && old.adshr.release == 40 && old.original_key == 72 && old.pitch == 1.5f);
    ntdlib::rsar::Region other_type = region;
    other_type.note_off_type = 2;
    XSFLIB_CHECK(!ntdlib::rsar::ValuesOf(other_type).ignore_note_off);

    ntdlib::sar::VelocityRegion linear = values;
    linear.interpolation = 1;
    XSFLIB_CHECK(ntdlib::rsar::RegionOf(values, 3) == region);
    XSFLIB_CHECK(ntdlib::rsar::RegionOf(linear, 3) == region);
    XSFLIB_CHECK(ntdlib::rsar::RegionOf(values, -1).wave_index == -1);
    XSFLIB_CHECK(ntdlib::rsar::RegionOf(old, 3).note_off_type == 0 && ntdlib::rsar::RegionOf(old, 3).hold == 0);

    const Bytes file = rsar_files::MakeNewBank();
    const Bytes waves = rsar_files::MakeRwar();
    const Bank bank = Bank::Parse(file, waves, kPlayback);
    for (const SndBuild& build : {kBuild2007, kBuild2008})
    {
        const ntdlib::sar::BankFile later =
            ntdlib::rsar::BankFileOf(bank, build, Flavor::kCafe, 0x00010000, kWaveArchive);
        XSFLIB_CHECK(ValuesAsBankFileOf(bank, build, later) > 0);
    }
}

// A bank of version 1.1 describes its waves in its WAVE block. Its waves convert too.
void TestOldBank()
{
    const Bytes file = rsar_files::MakeOldBank();
    const Bytes data = rsar_files::MakeOldWaveData();
    const Bank bank = Bank::Parse(file, data, kPlayback);
    const ntdlib::sar::BankFile cafe =
        ntdlib::rsar::BankFileOf(bank, kBuild2007, Flavor::kCafe, 0x00010000, kWaveArchive);
    XSFLIB_CHECK(bank.HasWaveBlock() && cafe.waves.size() == 2);
    XSFLIB_CHECK(PlaysAsTheWii(bank, kBuild2007, cafe, Flavor::kCafe));
    for (const Wave& wave : bank.Waves())
    {
        const ntdlib::sar::WaveFile converted = ntdlib::rsar::WaveFileOf(wave, Flavor::kCafe, 0x00010100);
        XSFLIB_CHECK(SamePcm(Decoded(converted, Flavor::kCafe), ntdlib::rsar::DecodeWave(wave)));
    }
}

// The samples that the Wii's decoder gives for `wave`, a wave of the Wii, written and read back.
ntdlib::wave::Pcm DecodedOnTheWii(const ntdlib::rsar::WaveFile& wave)
{
    const Bytes bytes = wave.Write();
    return ntdlib::rsar::DecodeWave(Wave::ParseFile(bytes, kPlayback));
}

// A wave of each later console goes to the Wii with its settings and samples, in the Wii's byte order. The Wii keeps
// two channels. A wave in IMA-ADPCM, a channel that ends before the wave's end and a wave without channels can't go.
void TestWiiWaves()
{
    const Bytes adpcm_file = rsar_files::MakeRwav(0x31, true, 22050);
    const Wave adpcm = Wave::ParseFile(adpcm_file, kPlayback);
    const std::vector<int16_t> samples = {1, -2, 300, -400, 5, 0x1234};
    const Bytes three_file = rsar_files::MakePcm16Rwav(6, samples, 3);
    const Wave three = Wave::ParseFile(three_file, kPlayback);
    for (const auto [flavor, version] : kWaveTargets)
    {
        const ntdlib::sar::WaveFile later = ntdlib::rsar::WaveFileOf(adpcm, flavor, version);
        const ntdlib::rsar::WaveFile wii = ntdlib::rsar::WiiWaveOf(later);
        XSFLIB_CHECK(wii.encoding == ntdlib::rsar::WaveEncoding::kDspAdpcm && wii.loop && wii.sample_rate == 22050);
        XSFLIB_CHECK(wii.loop_start == adpcm.loop_start && wii.loop_end == adpcm.loop_end && wii.channels.size() == 1);
        XSFLIB_CHECK(wii.channels[0].adpcm->coefs == adpcm.channels[0].adpcm->coefs);
        XSFLIB_CHECK(wii.channels[0].adpcm->loop_yn1 == adpcm.channels[0].adpcm->loop_yn1);
        XSFLIB_CHECK(SamePcm(DecodedOnTheWii(wii), Decoded(later, flavor)));

        // 16-bit PCM takes the Wii's byte order.
        const ntdlib::rsar::WaveFile pcm = ntdlib::rsar::WiiWaveOf(ntdlib::rsar::WaveFileOf(three, flavor, version));
        XSFLIB_CHECK(pcm.channels.size() == 2 && pcm.channels[1].samples.size() == 12 && !pcm.channels[1].adpcm);
        XSFLIB_CHECK(pcm.channels[1].samples[10] == 0x12 && pcm.channels[1].samples[11] == 0x34);
        XSFLIB_CHECK(SamePcm(DecodedOnTheWii(pcm), ntdlib::rsar::DecodeWave(three)));
    }

    ntdlib::sar::WaveFile ima = ntdlib::rsar::WaveFileOf(three, Flavor::kCtr, 0x02010000);
    ima.encoding = ntdlib::sar::WaveEncoding::kImaAdpcm;
    ntdlib::sar::WaveFile short_channel = ntdlib::rsar::WaveFileOf(three, Flavor::kNx, 0x00010200);
    short_channel.channels[1].samples.resize(10);
    ntdlib::sar::WaveFile silent = short_channel;
    silent.channels.clear();
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiWaveOf(ima); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiWaveOf(short_channel); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiWaveOf(silent); }).empty());
}

// Whether `back`, a bank of the Wii, plays the region of `bank` at every key and velocity of each program, with the
// values that a build of 2008 reads. The later consoles' banks hold no surround pan. With `missing`, `back` lacks that
// wave of `bank`: its regions play nothing, and the later waves move down by one.
bool PlaysTheSame(const Bank& bank, const Bank& back, std::optional<int32_t> missing = std::nullopt)
{
    if (back.InstrumentCount() != bank.InstrumentCount() ||
        back.Waves().size() != bank.Waves().size() - (missing ? 1 : 0))
    {
        return false;
    }
    for (uint32_t program = 0; program < bank.InstrumentCount(); ++program)
    {
        for (int key = 0; key < 128; ++key)
        {
            for (int velocity = 0; velocity < 128; ++velocity)
            {
                const ntdlib::rsar::Region* region = bank.Find(program, key, velocity);
                if (region != nullptr && (static_cast<std::size_t>(region->wave_index) >= bank.Waves().size() ||
                                          region->wave_index == missing))
                {
                    region = nullptr;
                }
                const ntdlib::rsar::Region* other = back.Find(program, key, velocity);
                if ((region == nullptr) != (other == nullptr))
                {
                    return false;
                }
                if (region == nullptr)
                {
                    continue;
                }
                ntdlib::rsar::Region expected = *region;
                expected.surround_pan = 0;
                expected.wave_data_location = 0;
                if (missing && expected.wave_index > *missing)
                {
                    --expected.wave_index;
                }
                if (!(*other == expected))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Whether a key and velocity of a program of `bank` plays wave `index`.
bool PlaysWave(const Bank& bank, int32_t index)
{
    for (uint32_t program = 0; program < bank.InstrumentCount(); ++program)
    {
        for (int key = 0; key < 128; ++key)
        {
            for (int velocity = 0; velocity < 128; ++velocity)
            {
                const ntdlib::rsar::Region* region = bank.Find(program, key, velocity);
                if (region != nullptr && region->wave_index == index)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

// A bank of the Wii that goes to a later console and comes back plays as it did, with its waves' samples. Without its
// first wave, the bank plays nothing where that wave played. A region whose wave lies past the waves can't go to the
// Wii.
void TestWiiBank()
{
    const Bytes file = rsar_files::MakeNewBank();
    const Bytes data = rsar_files::MakeRwar();
    const Bank bank = Bank::Parse(file, data, kPlayback);
    for (const auto [flavor, version] : kWaveTargets)
    {
        const uint32_t bank_version = flavor == Flavor::kCtr ? 0x01000100 : 0x00010000;
        const ntdlib::sar::BankFile later =
            ntdlib::rsar::BankFileOf(bank, kBuild2008, flavor, bank_version, kWaveArchive);
        std::vector<std::optional<ntdlib::rsar::WaveFile>> waves;
        for (const Wave& wave : bank.Waves())
        {
            waves.push_back(ntdlib::rsar::WiiWaveOf(ntdlib::rsar::WaveFileOf(wave, flavor, version)));
        }
        const ntdlib::rsar::BankFile wii = ntdlib::rsar::WiiBankOf(later, 0x0102, waves);
        const ntdlib::rsar::BankFile::Files files = wii.Write();
        const Bank back = Bank::Parse(files.file, files.wave_data, kPlayback);
        XSFLIB_CHECK(wii.version == 0x0102 && PlaysTheSame(bank, back));
        for (std::size_t i = 0; i < bank.Waves().size(); ++i)
        {
            XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(back.Waves()[i]), ntdlib::rsar::DecodeWave(bank.Waves()[i])));
        }

        std::vector<std::optional<ntdlib::rsar::WaveFile>> without_first = waves;
        without_first[0].reset();
        const ntdlib::rsar::BankFile::Files lacking = ntdlib::rsar::WiiBankOf(later, 0x0102, without_first).Write();
        const Bank lacking_back = Bank::Parse(lacking.file, lacking.wave_data, kPlayback);
        XSFLIB_CHECK(PlaysWave(bank, 0) && PlaysWave(bank, 1) && PlaysTheSame(bank, lacking_back, 0));
        XSFLIB_CHECK(
            !ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiBankOf(later, 0x0102, {waves[0]}); }).empty());
    }
}

// A wave archive of each later console goes to the Wii with each wave's samples, in the archive's order. A wave that
// the reader can't read, such as one in IMA-ADPCM, stops the conversion.
void TestWiiWaveArchive()
{
    const Bytes adpcm_file = rsar_files::MakeRwav(0x31, true, 22050);
    const Wave adpcm = Wave::ParseFile(adpcm_file, kPlayback);
    const Bytes pcm_file = rsar_files::MakePcm16Rwav(6, {1, -2, 300, -400, 5, 0x1234}, 2);
    const Wave pcm = Wave::ParseFile(pcm_file, kPlayback);
    for (const auto [flavor, version] : kWaveTargets)
    {
        ntdlib::sar::WaveArchiveFile later;
        later.flavor = flavor;
        for (const Wave& wave : {adpcm, pcm})
        {
            later.waves.push_back(ntdlib::rsar::WaveFileOf(wave, flavor, version).Write());
        }
        const ntdlib::rsar::WaveArchiveFile wii = ntdlib::rsar::WiiWaveArchiveOf(later);
        const Bytes bytes = wii.Write();
        const ntdlib::rsar::WaveArchive read = ntdlib::rsar::WaveArchive::Parse(bytes);
        XSFLIB_CHECK(wii.version == 0x0100 && read.waves.size() == 2);
        XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(Wave::ParseFile(read.waves[0], kPlayback)),
                             ntdlib::rsar::DecodeWave(adpcm)));
        XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(Wave::ParseFile(read.waves[1], kPlayback)),
                             ntdlib::rsar::DecodeWave(pcm)));
    }

    ntdlib::sar::WaveArchiveFile ima;
    ima.waves.push_back(sar_files::Wave(Flavor::kNx, {.encoding = 3, .loop_end = 16, .channels = {Bytes(8)}}));
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { ntdlib::rsar::WiiWaveArchiveOf(ima); }).empty());
}

// A wave sound file of each later console goes to the Wii in versions 1.2 and 1.3. Each wave sound keeps its pitch,
// pan, surround pan, sends and tracks. The 3DS's reader reads no send to bus C. A wave sound's first note takes the
// wave sound's envelope, as the Wii plays a note's, and the other notes keep theirs. A wave that the archive lacks
// leaves the notes that name it without a wave, and the later waves move down by one. A note whose wave lies past the
// waves can't go.
void TestWiiWaveSounds()
{
    const Bytes adpcm_file = rsar_files::MakeRwav(0x31, true, 22050);
    const Wave adpcm = Wave::ParseFile(adpcm_file, kPlayback);
    const Bytes pcm_file = rsar_files::MakePcm16Rwav(6, {1, -2, 300, -400, 5, 0x1234}, 2);
    const Wave pcm = Wave::ParseFile(pcm_file, kPlayback);
    for (const auto [flavor, version] : kWaveTargets)
    {
        sar_files::WaveSoundFileSpec spec;
        spec.waves = {{kWaveArchive, 4}, {kWaveArchive, 9}};
        sar_files::WaveSoundSpec& first = spec.sounds.emplace_back();
        first.pan = 20;
        first.span = 30;
        first.pitch = 1.5f;
        first.sends = {100, 3, 40, 50, 60};
        first.adshr = std::array<uint8_t, 5>{10, 11, 12, 13, 14};
        first.tracks = {{{.position = 0.0f, .length = 2.0f, .note = 0}, {.position = 2.0f, .length = 1.0f, .note = 1}}};
        first.notes = {{.wave = 1, .original_key = 62, .volume = 90, .adshr = std::array<uint8_t, 5>{1, 2, 3, 4, 5}},
                       {.wave = 0, .pitch = 2.0f, .adshr = std::array<uint8_t, 5>{6, 7, 8, 9, 10}}};
        sar_files::WaveSoundSpec& second = spec.sounds.emplace_back();
        second.notes = {{.wave = 0}};
        const Bytes bytes = sar_files::WaveSoundFile(flavor, spec);
        const ntdlib::sar::WaveSoundFile later =
            ntdlib::sar::WaveSoundFile::Parse(bytes, ntdlib::sar::Options{.flavor = flavor});
        std::vector<std::optional<ntdlib::rsar::WaveFile>> waves;
        for (const Wave& wave : {adpcm, pcm})
        {
            waves.push_back(ntdlib::rsar::WiiWaveOf(ntdlib::rsar::WaveFileOf(wave, flavor, version)));
        }

        for (const uint16_t wsd_version : {uint16_t{0x0102}, uint16_t{0x0103}})
        {
            const ntdlib::rsar::WsdFile wii = ntdlib::rsar::WiiWsdOf(later, wsd_version, waves);
            const ntdlib::rsar::WsdFile::Files files = wii.Write();
            XSFLIB_CHECK(ntdlib::rsar::WsdFile::Read(files.file, files.wave_data) == wii);
            const ntdlib::rsar::WaveSoundFile read =
                ntdlib::rsar::WaveSoundFile::Parse(files.file, files.wave_data, kPlayback);
            XSFLIB_CHECK(read.Version() == wsd_version && read.Sounds().size() == 2 && read.Waves().size() == 2);
            const ntdlib::rsar::WaveSoundFile::WaveSound& sound = read.Sounds()[0];
            XSFLIB_CHECK(sound.pan == 20 && sound.surround_pan == 30 && sound.pitch == 1.5f);
            XSFLIB_CHECK(sound.main_send == 100 && sound.fx_sends[0] == 40 && sound.fx_sends[1] == 50);
            XSFLIB_CHECK(sound.fx_sends[2] == (flavor == Flavor::kCtr ? 0 : 60));
            XSFLIB_CHECK(wii.sounds[0].tracks.size() == 1 && wii.sounds[0].tracks[0].events.size() == 2);
            XSFLIB_CHECK(wii.sounds[0].tracks[0].events[1].position == 2.0f &&
                         wii.sounds[0].tracks[0].events[1].note == 1);
            XSFLIB_CHECK(sound.notes.size() == 2 && sound.notes[0].wave_index == 1 && sound.notes[1].wave_index == 0);
            XSFLIB_CHECK(sound.notes[0].attack == 10 && sound.notes[0].decay == 11 && sound.notes[0].sustain == 12);
            XSFLIB_CHECK(sound.notes[0].hold == 13 && sound.notes[0].release == 14);
            XSFLIB_CHECK(sound.notes[0].original_key == 62 && sound.notes[0].volume == 90);
            XSFLIB_CHECK(sound.notes[1].attack == 6 && sound.notes[1].hold == 9 && sound.notes[1].release == 10);
            XSFLIB_CHECK(sound.notes[1].pitch == 2.0f);
            const ntdlib::rsar::WaveSoundFile::WaveSound& plain = read.Sounds()[1];
            XSFLIB_CHECK(plain.pan == 64 && plain.pitch == 1.0f && plain.main_send == 127 && plain.notes.size() == 1);
            XSFLIB_CHECK(plain.notes[0].attack == 127 && plain.notes[0].hold == 127);
            XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(read.Waves()[0]), ntdlib::rsar::DecodeWave(adpcm)));
            XSFLIB_CHECK(SamePcm(ntdlib::rsar::DecodeWave(read.Waves()[1]), ntdlib::rsar::DecodeWave(pcm)));
        }

        std::vector<std::optional<ntdlib::rsar::WaveFile>> without_first = waves;
        without_first[0].reset();
        const ntdlib::rsar::WsdFile lacking = ntdlib::rsar::WiiWsdOf(later, 0x0103, without_first);
        XSFLIB_CHECK(lacking.waves.size() == 1 && lacking.sounds[0].notes[0].wave_index == 0);
        XSFLIB_CHECK(lacking.sounds[0].notes[1].wave_index == -1 && lacking.sounds[1].notes[0].wave_index == -1);
        XSFLIB_CHECK(
            !ErrorOf<std::invalid_argument>([&] { ntdlib::rsar::WiiWsdOf(later, 0x0103, {waves[0]}); }).empty());
    }
}

} // namespace

int main()
{
    TestAdpcmWave();
    TestPcmWave();
    TestBank();
    TestRegionValues();
    TestOldBank();
    TestWiiWaves();
    TestWiiBank();
    TestWiiWaveArchive();
    TestWiiWaveSounds();

    return xsflib::testing::Summary("RSAR conversion");
}
