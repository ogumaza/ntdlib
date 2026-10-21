// SPDX-License-Identifier: MIT

// Tests of ArchiveFiles (nwsnd/archive_files.h) on a made-up sound archive in the byte orders of the Wii U and the
// Switch: the banks and wave archives by item, each read once, the waves' info by encoding, and the notes that a bank's
// programs play.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

namespace
{

namespace build = ntdlib::testing::sar_files;
namespace sar = ntdlib::sar;
using ntdlib::nwsnd::LoadedWaveArchive;
using xsflib::testing::Check;

enum class Format : uint32_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
};

struct Context
{
    uint16_t header = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
};

// A wave's info with both places of a channel's samples: the bytes, and their address in a console's memory.
struct TestWaveInfo
{
    struct Channel
    {
        std::span<const uint8_t> data;
        uint32_t address = 0;
        std::array<int16_t, 16> coefs{};
        Context context;
        Context loop_context;
    };

    Format format = Format::kDspAdpcm;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    int64_t loop_start = 0;
    int64_t loop_end = 0;
    int64_t original_loop_start = 0;
    std::array<Channel, 2> channels;
};

struct Policy
{
    using WaveInfo = TestWaveInfo;

    static void PlaceSamples(TestWaveInfo::Channel& channel, std::span<const uint8_t> samples,
                             const LoadedWaveArchive& archive)
    {
        channel.data = samples;
        channel.address = archive.address + static_cast<uint32_t>(samples.data() - archive.file.data());
    }
};

using Files = ntdlib::nwsnd::ArchiveFiles<Policy>;

constexpr uint32_t kBank = 0x03000000;
constexpr uint32_t kWaveArchive = 0x05000000;
constexpr uint32_t kAddress = 0x10000000;

// A program whose one region plays the bank's wave `wave` at every key and velocity.
build::KeyTable OneRegion(uint32_t wave)
{
    build::Region region;
    region.wave = wave;
    build::VelocityTable velocities;
    velocities.entries = {{0, region}};
    build::KeyTable keys;
    keys.entries = {{0, velocities}};

    return keys;
}

void TestArchive(bool big_endian)
{
    const std::string order = big_endian ? " (big-endian)" : " (little-endian)";
    const auto check = [&order](bool ok, const char* what)
    {
        Check(ok, (what + order).c_str());
    };

    // The wave archive: a looping stereo DSP-ADPCM wave, PCM16, PCM8 and IMA-ADPCM waves, and a wave with no channels.
    const build::WaveSpec kAdpcm{2,  true, 48000,
                                 14, 28,   {std::vector<uint8_t>(16, 0x17), std::vector<uint8_t>(16, 0x25)}};
    const build::WaveSpec kPcm16{1, false, 32000, 0, 8, {std::vector<uint8_t>(16, 1)}};
    const build::WaveSpec kPcm8{0, false, 22050, 0, 8, {std::vector<uint8_t>(8, 2)}};
    const build::WaveSpec kIma{3, false, 32000, 0, 8, {std::vector<uint8_t>(8, 3)}};
    const build::WaveSpec kNone{2, false, 32000, 0, 0, {}};
    const std::vector<uint8_t> waves = build::WaveArchive(
        big_endian, {build::Wave(big_endian, kAdpcm), build::Wave(big_endian, kPcm16), build::Wave(big_endian, kPcm8),
                     build::Wave(big_endian, kIma), build::Wave(big_endian, kNone)});

    // The bank: program 0 plays wave 0. Program 1 is empty. Program 2's region names a wave past the bank's list, and
    // program 3's region names a wave past the wave archive's end.
    build::BankSpec bank;
    bank.waves = {{kWaveArchive, 0}, {kWaveArchive, 9}};
    bank.instruments = {OneRegion(0), std::nullopt, OneRegion(5), OneRegion(1)};

    build::ArchiveSpec spec;
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 0}};
    spec.files.resize(2);
    spec.files[0].bytes = waves;
    spec.files[1].bytes = build::Bank(big_endian, bank);

    sar::Options options;
    options.flavor = big_endian ? sar::Flavor::kCafe : sar::Flavor::kNx;
    // The rules that stop at the first problem. Under them the reader reads the whole DSP-ADPCM state.
    options.rules = sar::Rules::kStopAtFirstProblem;
    const sar::SoundArchive archive = sar::SoundArchive::Load(build::Archive(big_endian, spec), options);

    int loads = 0;
    const auto load = [&loads](LoadedWaveArchive& loaded)
    {
        loaded.address = kAddress;
        ++loads;
    };
    Files files(archive, options, load);

    // Banks and wave archives by item, each read once.
    const sar::Bank* first_bank = files.Bank(kBank);
    check(first_bank != nullptr && first_bank->instruments.size() == 4, "the bank");
    check(files.Bank(kBank) == first_bank, "the bank is read once");
    check(files.Bank(kBank + 1) == nullptr, "a bank past the archive's list");
    check(files.Bank(kWaveArchive) == nullptr, "a wave archive's item as a bank");
    check(files.WaveArchive(kWaveArchive + 1) == nullptr && files.WaveArchive(kBank) == nullptr,
          "a wave archive past the list, and a bank's item as a wave archive");

    check(loads == 0, "no wave archive is loaded before the first call for one");
    const LoadedWaveArchive* loaded = files.WaveArchive(kWaveArchive);
    check(loaded != nullptr && loaded->archive.waves.size() == 5 && loaded->address == kAddress && loads == 1,
          "the wave archive, loaded with its address");
    check(files.WaveArchive(kWaveArchive) == loaded && loads == 1, "the wave archive is read once");
    if (loaded == nullptr)
    {
        return;
    }

    // The DSP-ADPCM wave: the builder gives each channel coefficients of 1 to 16, a frame header of its first byte, and
    // the values 100 to 104 for yn1, yn2 and the loop's context.
    const std::optional<TestWaveInfo> wave = files.Wave(kWaveArchive, 0);
    check(wave && wave->format == Format::kDspAdpcm && wave->loop && wave->channel_count == 2 &&
              wave->sample_rate == 48000 && wave->loop_start == 14 && wave->loop_end == 28 &&
              wave->original_loop_start == 14,
          "the DSP-ADPCM wave's format, loop, channels, rate and loop points");
    if (wave)
    {
        const TestWaveInfo::Channel& left = wave->channels[0];
        const TestWaveInfo::Channel& right = wave->channels[1];
        check(!left.data.empty() && left.data[0] == 0x17 && !right.data.empty() && right.data[0] == 0x25,
              "each channel's samples");
        check(left.address == kAddress + static_cast<uint32_t>(left.data.data() - loaded->file.data()) &&
                  right.address > left.address,
              "each channel's samples at their address");
        check(left.coefs[0] == 1 && left.coefs[15] == 16 && right.coefs[7] == 8, "the coefficients");
        check(left.context.header == 0x17 && left.context.yn1 == 100 && left.context.yn2 == 101 &&
                  right.context.header == 0x25,
              "the context at the start");
        check(left.loop_context.header == 102 && left.loop_context.yn1 == 103 && left.loop_context.yn2 == 104,
              "the context at the loop");
    }

    const std::optional<TestWaveInfo> pcm16_wave = files.Wave(kWaveArchive, 1);
    check(pcm16_wave && pcm16_wave->format == Format::kPcm16 && !pcm16_wave->loop && pcm16_wave->channel_count == 1 &&
              pcm16_wave->loop_end == 8 && pcm16_wave->channels[0].coefs[0] == 0 &&
              pcm16_wave->channels[0].context.header == 0,
          "a PCM16 wave, with no DSP-ADPCM state");
    const std::optional<TestWaveInfo> pcm8_wave = files.Wave(kWaveArchive, 2);
    check(pcm8_wave && pcm8_wave->format == Format::kPcm8 && pcm8_wave->sample_rate == 22050, "a PCM8 wave");
    const std::optional<TestWaveInfo> ima_wave = files.Wave(kWaveArchive, 3);
    check(ima_wave && ima_wave->format == Format::kDspAdpcm && ima_wave->channels[0].coefs[0] == 0,
          "an IMA-ADPCM wave reads as DSP-ADPCM, with no coefficients");

    check(!files.Wave(kWaveArchive, 4), "a wave with no channels");
    check(!files.Wave(kWaveArchive, 5), "a wave past the archive's end");
    check(!files.Wave(kWaveArchive + 1, 0), "a wave of a missing wave archive");

    const std::optional<TestWaveInfo> again = files.Wave(kWaveArchive, 0);
    check(again && wave && again->channels[1].data.data() == wave->channels[1].data.data() && loads == 1,
          "a wave read again");

    // The notes that the bank's programs play.
    const std::vector<uint32_t> kBanks = {kBank};

    const std::optional<Files::Note> note = files.FindNote(kBanks, 0, 0, 60, 100);
    check(note && note->region != nullptr && note->region->wave_id_index == 0 && note->wave.channel_count == 2 &&
              note->wave.format == Format::kDspAdpcm,
          "program 0's note");
    check(!files.FindNote(kBanks, 1, 0, 60, 100) && !files.FindNote(kBanks, -1, 0, 60, 100), "a slot past the list");
    check(!files.FindNote({kBank + 1}, 0, 0, 60, 100), "a slot whose bank is missing");
    check(!files.FindNote(kBanks, 0, 1, 60, 100), "an empty program");
    check(!files.FindNote(kBanks, 0, 2, 60, 100), "a region whose wave is past the bank's list");
    check(!files.FindNote(kBanks, 0, 3, 60, 100), "a region whose wave is past the wave archive's end");
    check(!files.FindNote(kBanks, 0, 4, 60, 100), "a program past the bank's list");
}

} // namespace

int main()
{
    TestArchive(true);
    TestArchive(false);

    return xsflib::testing::Summary("nwsnd archive files");
}
