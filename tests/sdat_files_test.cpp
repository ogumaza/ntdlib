// SPDX-License-Identifier: MIT

// Tests of the writers of the DS's sound files (ntdlib/sdat/files.h). The readers of formats.h read what they write.
// The expected bytes follow the layouts in files.h and formats.h, worked out by hand. The expected samples of IMA-ADPCM
// follow from the encoder's rule: the DS's decoder (sdat::DecodeWave) plays them close to the input.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/wave/encode_ima.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace sdat = ntdlib::sdat;
using Bytes = std::vector<uint8_t>;

constexpr double kPi = 3.14159265358979323846;

uint32_t U32At(const Bytes& bytes, std::size_t at)
{
    return xsflib::LoadLe32(bytes.data() + at);
}

// A region of `type` with wave `wave`, and the other parameters from `seed`.
sdat::Region MakeRegion(sdat::InstrumentType type, uint16_t wave, uint8_t seed)
{
    return {.type = type,
            .wave = wave,
            .wave_archive = static_cast<uint16_t>(seed % 4),
            .root_key = static_cast<uint8_t>(60 + seed),
            .attack = static_cast<uint8_t>(120 - seed),
            .decay = static_cast<uint8_t>(100 - seed),
            .sustain = static_cast<uint8_t>(90 - seed),
            .release = static_cast<uint8_t>(80 - seed),
            .pan = static_cast<uint8_t>(30 + seed)};
}

// Whether two banks hold the same instruments, wherever their data lies.
bool SameInstruments(const sdat::Bank& a, const sdat::Bank& b)
{
    if (a.instruments.size() != b.instruments.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.instruments.size(); i++)
    {
        const sdat::Instrument& x = a.instruments[i];
        const sdat::Instrument& y = b.instruments[i];
        if (x.type != y.type || !(x.region == y.region) || x.min_key != y.min_key || x.max_key != y.max_key ||
            x.split_keys != y.split_keys || x.regions != y.regions)
        {
            return false;
        }
    }

    return true;
}

// A sequence file: the header, the DATA block with the bytecode's offset, the bytecode, and zeros up to 4 bytes.
void TestSequenceFile()
{
    const Bytes bytecode = {0x3C, 0x7F, 0x30, 0xFF, 0xFF};

    const Bytes file = sdat::SequenceFile(bytecode);

    const Bytes expected = {'S',  'S', 'E',  'Q',  0xFF, 0xFE, 0x00, 0x01, 0x24, 0x00, 0x00, 0x00,
                            0x10, 0,   0x01, 0x00, 'D',  'A',  'T',  'A',  0x14, 0x00, 0x00, 0x00,
                            0x1C, 0,   0,    0,    0x3C, 0x7F, 0x30, 0xFF, 0xFF, 0,    0,    0};
    XSFLIB_CHECK(file == expected);
    const sdat::Sequence sequence = sdat::Sequence::Parse(file);
    XSFLIB_CHECK(sequence.data_offset == 0x1C && sequence.bytecode.size() == 8 && sequence.bytecode[4] == 0xFF);
}

// A sequence archive: the bytecode's offset and the count, a record of 12 bytes for each sequence, then the bytecode.
void TestSequenceArchiveFile()
{
    const std::vector<sdat::ArchivedSequence> sequences = {{0, 3, 100, 64, 32, 1, 0}, {4, 5, 90, 60, 30, 2, 0x1234}};
    const Bytes bytecode = {0x3C, 0x7F, 0x30, 0xFF, 0x40, 0x7F, 0x30, 0xFF};

    const Bytes file = sdat::SequenceArchiveFile(sequences, bytecode);

    XSFLIB_CHECK(file.size() == 0x40 && U32At(file, 8) == 0x40 && U32At(file, 0x14) == 0x30);
    XSFLIB_CHECK(U32At(file, 0x18) == 0x38 && U32At(file, 0x1C) == 2);
    XSFLIB_CHECK(U32At(file, 0x2C) == 4 && file[0x30] == 5 && file[0x32] == 90 && file[0x33] == 60);
    XSFLIB_CHECK(file[0x34] == 30 && file[0x35] == 2 && file[0x36] == 0x34 && file[0x37] == 0x12);
    const sdat::SequenceArchive archive = sdat::SequenceArchive::Parse(file);
    XSFLIB_CHECK(archive.sequences == sequences);
    XSFLIB_CHECK(Bytes(archive.bytecode.begin(), archive.bytecode.end()) == bytecode);
}

// A bank. The table holds 6 records, up to 0x54. The data follows in the order of the offsets: the PSG's (0x54), the
// PCM instrument's (0x70, shared by the second PCM record), the drum set's (0x90), then the new key split's. The gaps
// between the old offsets go. Each record gives the type in its low byte and the offset above it.
void TestBankFile()
{
    sdat::Bank bank;
    bank.instruments.resize(6);
    bank.instruments[0] = {
        .type = sdat::InstrumentType::kPcm, .offset = 0x70, .region = MakeRegion(sdat::InstrumentType::kPcm, 7, 2)};
    bank.instruments[2] = {
        .type = sdat::InstrumentType::kPsg, .offset = 0x54, .region = MakeRegion(sdat::InstrumentType::kPsg, 3, 9)};
    bank.instruments[3] = {.type = sdat::InstrumentType::kDrumSet, .offset = 0x90, .min_key = 36, .max_key = 37};
    bank.instruments[3].regions = {MakeRegion(sdat::InstrumentType::kPcm, 1, 1),
                                   MakeRegion(sdat::InstrumentType::kNoise, 0, 4)};
    bank.instruments[3].regions[0].padding = 5;
    bank.instruments[4] = bank.instruments[0];
    bank.instruments[5] = {.type = sdat::InstrumentType::kKeySplit, .split_keys = {40, 127}};
    bank.instruments[5].regions = {MakeRegion(sdat::InstrumentType::kPcm, 4, 3),
                                   MakeRegion(sdat::InstrumentType::kPsg, 5, 6)};

    const Bytes file = sdat::BankFile(bank);

    XSFLIB_CHECK(file.size() == 0xA4 && U32At(file, 8) == 0xA4 && U32At(file, 0x14) == 0x94);
    XSFLIB_CHECK(U32At(file, 0x18) == 0 && U32At(file, 0x34) == 0 && U32At(file, 0x38) == 6);
    XSFLIB_CHECK(U32At(file, 0x3C) == 0x5E01 && U32At(file, 0x40) == 0 && U32At(file, 0x44) == 0x5402);
    XSFLIB_CHECK(U32At(file, 0x48) == 0x6810 && U32At(file, 0x4C) == 0x5E01 && U32At(file, 0x50) == 0x8211);
    const Bytes psg = {0x03, 0x00, 0x01, 0x00, 69, 111, 91, 81, 71, 39};
    XSFLIB_CHECK(Bytes(file.begin() + 0x54, file.begin() + 0x5E) == psg);
    XSFLIB_CHECK(file[0x68] == 36 && file[0x69] == 37 && file[0x6A] == 1 && file[0x6B] == 5 && file[0x76] == 3);
    XSFLIB_CHECK(file[0x82] == 40 && file[0x83] == 127 && file[0x84] == 0 && file[0x8A] == 1 && file[0x96] == 2);
    XSFLIB_CHECK(file[0xA2] == 0 && file[0xA3] == 0);
    const sdat::Bank parsed = sdat::Bank::Parse(file);
    XSFLIB_CHECK(SameInstruments(parsed, bank));
    XSFLIB_CHECK(sdat::BankFile(parsed) == file);
}

// Two records with one offset and other data don't share it. Nor do two new instruments with the same data. The data
// goes in the table's order, from 0x4C.
void TestBankFileOfEditedInstruments()
{
    const sdat::Instrument added{.type = sdat::InstrumentType::kPcm,
                                 .region = MakeRegion(sdat::InstrumentType::kPcm, 3, 0)};
    sdat::Bank bank;
    bank.instruments = {
        {.type = sdat::InstrumentType::kPcm, .offset = 0x44, .region = MakeRegion(sdat::InstrumentType::kPcm, 1, 0)},
        {.type = sdat::InstrumentType::kPcm, .offset = 0x44, .region = MakeRegion(sdat::InstrumentType::kPcm, 2, 0)},
        added,
        added};

    const Bytes file = sdat::BankFile(bank);

    XSFLIB_CHECK(U32At(file, 0x3C) == 0x4C01 && U32At(file, 0x40) == 0x5601);
    XSFLIB_CHECK(U32At(file, 0x44) == 0x6001 && U32At(file, 0x48) == 0x6A01);
    XSFLIB_CHECK(file[0x4C] == 1 && file[0x56] == 2 && file[0x60] == 3 && file[0x6A] == 3 && file.size() == 0x74);
}

void TestBankRefusals()
{
    sdat::Bank drums;
    drums.instruments = {{.type = sdat::InstrumentType::kDrumSet, .min_key = 36, .max_key = 38}};
    drums.instruments[0].regions.resize(2);
    sdat::Bank backwards;
    backwards.instruments = {{.type = sdat::InstrumentType::kDrumSet, .min_key = 38, .max_key = 36}};
    sdat::Bank split;
    split.instruments = {{.type = sdat::InstrumentType::kKeySplit, .split_keys = {40, 0, 127}}};
    split.instruments[0].regions.resize(3);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::BankFile(drums); }) ==
                 "a drum set's regions don't cover its keys");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::BankFile(backwards); }) ==
                 "a drum set's regions don't cover its keys");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::BankFile(split); }) ==
                 "a key split's regions don't match its keys");
}

// A wave's bytes, a wave file and a wave archive. The archive's waves follow its table of 2 offsets, at 0x44 and 0x54.
// The first wave ends with zeros up to 4 bytes.
void TestWaveFiles()
{
    const Bytes samples = {0x34, 0x12, 0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00};
    const sdat::Wave wave{.encoding = sdat::WaveEncoding::kPcm16,
                          .loop = 1,
                          .sample_rate = 32768,
                          .timer = 511,
                          .loop_start = 1,
                          .loop_length = 1,
                          .data = samples};
    const Bytes short_samples = {0x10, 0x20};
    const sdat::Wave short_wave{.sample_rate = 8000, .timer = 2094, .data = short_samples};

    const Bytes bytes = sdat::WaveBytes(wave);
    const Bytes swav = sdat::WaveFile(bytes);
    const Bytes swar = sdat::WaveArchiveFile(std::vector<Bytes>{sdat::WaveBytes(short_wave), bytes});

    const Bytes expected = {0x01, 0x01, 0x00, 0x80, 0xFF, 0x01, 0x01, 0x00, 0x01, 0x00,
                            0x00, 0x00, 0x34, 0x12, 0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00};
    XSFLIB_CHECK(bytes == expected);
    XSFLIB_CHECK(swav.size() == 0x2C && U32At(swav, 8) == 0x2C && U32At(swav, 0x14) == 0x1C);
    const sdat::Wave single = sdat::Wave::ParseFile(swav);
    XSFLIB_CHECK(single.loop == 1 && single.timer == 511 && single.loop_start == 1 && single.loop_length == 1);
    XSFLIB_CHECK(Bytes(single.data.begin(), single.data.end()) == samples);
    XSFLIB_CHECK(swar.size() == 0x68 && U32At(swar, 0x38) == 2 && U32At(swar, 0x3C) == 0x44);
    XSFLIB_CHECK(U32At(swar, 0x40) == 0x54 && swar[0x50] == 0x10 && swar[0x52] == 0 && swar[0x53] == 0);
    XSFLIB_CHECK(swar[0x54] == 0x01 && swar[0x67] == 0x00 && swar[0x66] == 0x01);
    const sdat::WaveArchive archive = sdat::WaveArchive::Parse(swar);
    XSFLIB_CHECK(archive.waves.size() == 2 && archive.waves[0].sample_rate == 8000 && archive.waves[0].timer == 2094);
    XSFLIB_CHECK(archive.waves[1].loop_length == 1 && sdat::DecodeWave(archive.waves[1])[1] == -0x8000);
}

// PCM8 without a loop: each sample's high byte, rounded to the nearest, then zeros up to a word. The timer is
// 16,756,991 / 22,050, rounded down: 759.
void TestEncodePcm8()
{
    const std::vector<int16_t> samples = {0x7FFF, -0x8000, 0x0080, 0x007F, -0x0081};

    const Bytes wave = sdat::EncodeWave(samples, 22050, sdat::WaveEncoding::kPcm8, std::nullopt);

    const Bytes expected = {0x00, 0x00, 0x22, 0x56, 0xF7, 0x02, 0x00, 0x00, 0x02, 0x00,
                            0x00, 0x00, 0x7F, 0x80, 0x01, 0x00, 0xFF, 0x00, 0x00, 0x00};
    XSFLIB_CHECK(wave == expected);
}

// PCM16 with a loop from sample 1 of 4. The loop's start moves to sample 2, the next word. Its length of 3 samples
// repeats to 6, 3 words. The samples from sample 4 on repeat the loop from its old start.
void TestEncodePcm16Loop()
{
    const std::vector<int16_t> samples = {100, 200, 300, 400};

    const Bytes bytes = sdat::EncodeWave(samples, 32768, sdat::WaveEncoding::kPcm16, 1);

    const Bytes params = {0x01, 0x01, 0x00, 0x80, 0xFF, 0x01, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00};
    XSFLIB_CHECK(bytes.size() == 12 + 16 && Bytes(bytes.begin(), bytes.begin() + 12) == params);
    const xsflib::Reader r(bytes);
    const sdat::Wave wave = sdat::Wave::Parse(r, 0, bytes.size());
    XSFLIB_CHECK(wave.LoopStartSample() == 2 && wave.Samples() == 8);
    XSFLIB_CHECK(sdat::DecodeWave(wave) == (std::vector<int16_t>{100, 200, 300, 400, 200, 300, 400, 200}));
}

// IMA-ADPCM. A wave without a loop starts with the decoder's state, and its loop starts at that word. A loop from
// sample 3 of 20 moves to sample 8. Its length of 17 repeats 8 times, to 136 samples, 17 words. The header's sample is
// the first sample's, and each decoded sample lies close to the input.
void TestEncodeImaAdpcm()
{
    std::vector<int16_t> sine(20);
    for (std::size_t i = 0; i < sine.size(); i++)
    {
        sine[i] =
            static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * kPi * (static_cast<double>(i) + 0.5) / 17.0)));
    }

    const Bytes once = sdat::EncodeWave(std::vector<int16_t>(5, 0), 32768, sdat::WaveEncoding::kImaAdpcm, std::nullopt);
    const Bytes looped = sdat::EncodeWave(sine, 32768, sdat::WaveEncoding::kImaAdpcm, 3);

    const Bytes params = {0x02, 0x00, 0x00, 0x80, 0xFF, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
    XSFLIB_CHECK(once.size() == 12 + 4 + 4 && Bytes(once.begin(), once.begin() + 12) == params);
    XSFLIB_CHECK(looped.size() == 12 + 4 + 72 && looped[1] == 1 && looped[6] == 2 && U32At(looped, 8) == 17);
    XSFLIB_CHECK(static_cast<int16_t>(U32At(looped, 12) & 0xFFFF) == sine[0] && looped[14] <= 88 && looped[15] == 0);
    const xsflib::Reader r(looped);
    const sdat::Wave wave = sdat::Wave::Parse(r, 0, looped.size());
    const std::vector<int16_t> decoded = sdat::DecodeWave(wave);
    XSFLIB_CHECK(wave.LoopStartSample() == 8 && decoded.size() == 144);
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < decoded.size(); i++)
    {
        const double expected = i < sine.size() ? sine[i] : sine[3 + (i - 3) % 17];
        signal += expected * expected;
        noise += (expected - decoded[i]) * (expected - decoded[i]);
    }
    XSFLIB_CHECK(10.0 * std::log10(signal / noise) > 20.0);
}

// The squared error of the samples that the nibbles closest to `samples` decode to with the DS's step, from `state`.
double GreedyError(ntdlib::wave::ImaAdpcmState state, const std::vector<int16_t>& samples)
{
    double error = 0.0;
    for (const int16_t sample : samples)
    {
        ntdlib::wave::ImaAdpcmState best = ntdlib::ntr::AdpcmStep(state, 0);
        for (uint8_t nibble = 1; nibble < 16; nibble++)
        {
            const ntdlib::wave::ImaAdpcmState next = ntdlib::ntr::AdpcmStep(state, nibble);
            if (std::abs(next.sample - sample) < std::abs(best.sample - sample))
            {
                best = next;
            }
        }
        error += static_cast<double>(best.sample - sample) * (best.sample - sample);
        state = best;
    }

    return error;
}

// IMA-ADPCM decodes with the DS's step. It keeps a step down from going below -0x7FFF. IMA's step stops at -0x8000.
// Near -0x8000, the two steps lead to other choices. Each decoded sample is the closest that a nibble reaches from the
// state before it, with the DS's step. The header's index gives the opening the least error.
void TestEncodeImaAdpcmWithTheDsStep()
{
    const std::vector<std::vector<int16_t>> inputs = {{-32768, -32768, -32744, -32768, -32768, -32768, -32744, -32768},
                                                      {-32768, -32639, -32761, -32708, -32768, -32768, -31468, -32768},
                                                      {-32721, -32135, -32768, -32761, -32768, -32768, -32768, -32768}};

    for (const std::vector<int16_t>& samples : inputs)
    {
        const Bytes bytes = sdat::EncodeWave(samples, 32768, sdat::WaveEncoding::kImaAdpcm, std::nullopt);

        const ntdlib::wave::ImaAdpcmState start = ntdlib::ntr::AdpcmHeader(U32At(bytes, 12));
        XSFLIB_CHECK(start.sample == samples[0]);
        for (int index = 0; index <= 88; index++)
        {
            XSFLIB_CHECK(GreedyError(start, samples) <=
                         GreedyError({samples[0], static_cast<uint8_t>(index)}, samples));
        }
        ntdlib::wave::ImaAdpcmState state = start;
        for (std::size_t i = 0; i < samples.size(); i++)
        {
            const uint8_t nibble = i % 2 == 0 ? bytes[16 + i / 2] & 0x0F : bytes[16 + i / 2] >> 4;
            const ntdlib::wave::ImaAdpcmState next = ntdlib::ntr::AdpcmStep(state, nibble);
            for (uint8_t other = 0; other < 16; other++)
            {
                XSFLIB_CHECK(std::abs(next.sample - samples[i]) <=
                             std::abs(ntdlib::ntr::AdpcmStep(state, other).sample - samples[i]));
            }
            state = next;
        }
    }
}

void TestEncodeRefusals()
{
    const std::vector<int16_t> samples(4);
    const std::vector<int16_t> long_wave(262144 + 4);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::EncodeWave(samples, 0, {}, std::nullopt); }) ==
                 "a wave's sample rate is below 256 Hz");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::EncodeWave(samples, 255, {}, std::nullopt); }) ==
                 "a wave's sample rate is below 256 Hz");
    XSFLIB_CHECK(sdat::EncodeWave(samples, 256, {}, std::nullopt)[4] == 0xB0);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::EncodeWave(samples, 32768, {}, 4); }) ==
                 "a wave's loop starts at or past its end");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { sdat::EncodeWave(samples, 32768, static_cast<sdat::WaveEncoding>(3), 0); }) ==
                 "unknown wave encoding");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { sdat::EncodeWave(long_wave, 32768, {}, 262141); }) ==
                 "a wave's loop starts past the 65,535 words of its field");
    XSFLIB_CHECK(sdat::EncodeWave(long_wave, 32768, {}, 262140)[6] == 0xFF);
}

// The timer's clock is half the ARM7's clock of 33,513,982 Hz. A word holds 4 samples of PCM8, 2 of PCM16 and 8 of
// IMA-ADPCM. A file's ID takes the low 24 bits of its field.
void TestHardwareConstants()
{
    const auto unknown = []
    {
        sdat::SamplesPerWord(static_cast<sdat::WaveEncoding>(3));
    };

    XSFLIB_CHECK(sdat::kTimerClockHz == 16756991);
    XSFLIB_CHECK(sdat::SamplesPerWord(sdat::WaveEncoding::kPcm8) == 4);
    XSFLIB_CHECK(sdat::SamplesPerWord(sdat::WaveEncoding::kPcm16) == 2);
    XSFLIB_CHECK(sdat::SamplesPerWord(sdat::WaveEncoding::kImaAdpcm) == 8);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(unknown) == "unknown wave encoding");
    XSFLIB_CHECK(sdat::kFileIdBits == 0xFFFFFF);
}

// A sine and a slower one, `samples` of each.
std::vector<std::vector<int16_t>> Stereo(std::size_t samples)
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(samples));
    for (std::size_t i = 0; i < samples; i++)
    {
        channels[0][i] = static_cast<int16_t>(9000.0 * std::sin(static_cast<double>(i) * 0.05));
        channels[1][i] = static_cast<int16_t>(9000.0 * std::sin(static_cast<double>(i) * 0.013));
    }

    return channels;
}

// The samples that a channel of IMA-ADPCM decodes to, when the whole channel is one block: its state's word, then its
// nibbles.
std::vector<int16_t> WholeImaChannel(std::span<const int16_t> samples)
{
    const ntdlib::wave::ImaAdpcmChannel channel = ntdlib::wave::EncodeImaAdpcm(samples, 0, ntdlib::ntr::AdpcmStep);
    Bytes block;
    xsflib::AppendLe16(block, static_cast<uint16_t>(channel.start.sample));
    xsflib::AppendLe16(block, channel.start.index);
    block.insert(block.end(), channel.data.begin(), channel.data.end());

    return sdat::DecodeAdpcm(block, static_cast<uint32_t>(samples.size()));
}

// The STRM reader reads back each field that EncodeStream writes. A stream of 32,728 Hz takes a timer of
// 16,756,991 / 32,728 / 32 = 16. A part of IMA-ADPCM starts with 4 bytes of state, so a block of 512 bytes holds
// 1,016 samples. Two channels of 2 blocks of 512 bytes and one of 91 end on a multiple of 4 bytes after 2 bytes of
// padding.
void TestStreamLayout()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(1016 * 2 + 173);

    const Bytes ima = sdat::EncodeStream(channels, 32728, sdat::WaveEncoding::kImaAdpcm, std::nullopt);

    const sdat::Stream stream = sdat::Stream::Parse(ima);
    XSFLIB_CHECK(stream.header.version == 0x0100 && stream.header.size == ima.size());
    XSFLIB_CHECK(stream.header.header_size == 0x10 && stream.header.blocks == 2);
    XSFLIB_CHECK(stream.encoding == sdat::WaveEncoding::kImaAdpcm && stream.loop == 0 && stream.channels == 2);
    XSFLIB_CHECK(stream.head_padding == 0 && stream.sample_rate == 32728 && stream.timer == 16);
    XSFLIB_CHECK(stream.samples == 1016 * 2 + 173 && stream.loop_start == 0);
    XSFLIB_CHECK(stream.blocks == 3 && stream.block_bytes == 0x200 && stream.block_samples == 1016);
    XSFLIB_CHECK(stream.last_block_bytes == 91 && stream.last_block_samples == 173);
    XSFLIB_CHECK(stream.data_offset == 0x68 && stream.data.size() == 2 * (2 * 512 + 91) + 2);
    XSFLIB_CHECK(std::ranges::all_of(stream.reserved, [](uint8_t byte) { return byte == 0; }));
    XSFLIB_CHECK(ima.size() == 0x68 + 2 * (2 * 512 + 91) + 2);
    XSFLIB_CHECK(U32At(ima, 0x14) == 0x50 && U32At(ima, 0x64) == ima.size() - 0x60);
    XSFLIB_CHECK(ima[0x1C] == 0xD8 && ima[0x1D] == 0x7F && ima[0x1E] == 16 && ima[0x1F] == 0);
    XSFLIB_CHECK(sdat::StreamFile(stream) == ima);

    // Each block starts with the state where the whole channel's nibbles reach it.
    for (uint32_t channel = 0; channel < 2; channel++)
    {
        XSFLIB_CHECK(sdat::DecodeStream(stream, channel) == WholeImaChannel(channels[channel]));
    }

    // 16-bit PCM takes blocks of 256 samples and gives the samples back. PCM8 takes blocks of 512 samples.
    const Bytes pcm16 = sdat::EncodeStream(channels, 32728, sdat::WaveEncoding::kPcm16, std::nullopt);
    const sdat::Stream pcm16_stream = sdat::Stream::Parse(pcm16);
    XSFLIB_CHECK(pcm16_stream.block_samples == 256 && pcm16_stream.block_bytes == 512 && pcm16_stream.blocks == 9);
    XSFLIB_CHECK(pcm16_stream.last_block_samples == 2205 - 8 * 256 && pcm16_stream.last_block_bytes == 2 * 157);
    XSFLIB_CHECK(sdat::DecodeStream(pcm16_stream, 0) == channels[0]);
    XSFLIB_CHECK(sdat::DecodeStream(pcm16_stream, 1) == channels[1]);
    XSFLIB_CHECK(sdat::StreamFile(pcm16_stream) == pcm16);
    const Bytes pcm8 = sdat::EncodeStream(channels, 32728, sdat::WaveEncoding::kPcm8, std::nullopt);
    const sdat::Stream pcm8_stream = sdat::Stream::Parse(pcm8);
    XSFLIB_CHECK(pcm8_stream.block_samples == 512 && pcm8_stream.block_bytes == 512 && pcm8_stream.blocks == 5);
    XSFLIB_CHECK(sdat::DecodeStream(pcm8_stream, 0).size() == channels[0].size());
    XSFLIB_CHECK(sdat::StreamFile(pcm8_stream) == pcm8);
}

// PCM8 takes each sample's high byte, rounded to the nearest, as a wave does. One channel of 5 samples.
void TestStreamPcm8()
{
    const std::vector<std::vector<int16_t>> channels = {{0x7FFF, -0x8000, 0x0080, 0x007F, -0x0081}};

    const Bytes bytes = sdat::EncodeStream(channels, 22050, sdat::WaveEncoding::kPcm8, std::nullopt);

    XSFLIB_CHECK(bytes.size() == 0x68 + 8 && bytes[0x1E] == 23);
    XSFLIB_CHECK(Bytes(bytes.begin() + 0x68, bytes.end()) == (Bytes{0x7F, 0x80, 0x01, 0x00, 0xFF, 0, 0, 0}));
    const sdat::Stream stream = sdat::Stream::Parse(bytes);
    XSFLIB_CHECK(stream.blocks == 1 && stream.last_block_bytes == 5 && stream.last_block_samples == 5);
}

// A loop moves forward to a block's start, and the samples between repeat after the end. The loop keeps its length.
void TestStreamLoop()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(2000);

    const Bytes bytes = sdat::EncodeStream(channels, 32728, sdat::WaveEncoding::kPcm16, 300);

    const sdat::Stream stream = sdat::Stream::Parse(bytes);
    XSFLIB_CHECK(stream.loop == 1 && stream.loop_start == 512 && stream.samples == 512 + 1700);
    XSFLIB_CHECK(bytes[0x19] == 1 && U32At(bytes, 0x20) == 512 && U32At(bytes, 0x24) == 512 + 1700);
    for (uint32_t channel = 0; channel < 2; channel++)
    {
        const std::vector<int16_t> decoded = sdat::DecodeStream(stream, channel);
        XSFLIB_CHECK(decoded.size() == 512 + 1700);
        for (std::size_t i = 0; i < decoded.size(); i++)
        {
            XSFLIB_CHECK(decoded[i] == channels[channel][i < 2000 ? i : 300 + (i - 2000)]);
        }
    }

    // A loop at a block stays, and takes no more samples.
    const sdat::Stream on_block =
        sdat::Stream::Parse(sdat::EncodeStream(channels, 32728, sdat::WaveEncoding::kImaAdpcm, 1016));
    XSFLIB_CHECK(on_block.loop == 1 && on_block.loop_start == 1016 && on_block.samples == 2000);
}

void TestStreamRefusals()
{
    const std::vector<std::vector<int16_t>> channels = Stereo(100);
    const std::vector<std::vector<int16_t>> three(3, channels[0]);
    const std::vector<std::vector<int16_t>> uneven = {channels[0], std::vector<int16_t>(99)};
    const std::vector<std::vector<int16_t>> empty(1);
    const auto encode = [](std::span<const std::vector<int16_t>> input, uint16_t rate, sdat::WaveEncoding encoding,
                           std::optional<uint32_t> loop_start)
    {
        return [=]
        {
            sdat::EncodeStream(input, rate, encoding, loop_start);
        };
    };
    const sdat::WaveEncoding pcm16 = sdat::WaveEncoding::kPcm16;

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode(channels, 255, pcm16, std::nullopt)) ==
                 "a stream's sample rate is below 256 Hz");
    XSFLIB_CHECK(sdat::Stream::Parse(sdat::EncodeStream(channels, 256, pcm16, std::nullopt)).timer == 2045);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode(three, 32728, pcm16, std::nullopt)) ==
                 "a stream has one or two channels");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode({}, 32728, pcm16, std::nullopt)) ==
                 "a stream has one or two channels");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode(uneven, 32728, pcm16, std::nullopt)) ==
                 "a stream's channels have other lengths or no samples");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode(empty, 32728, pcm16, std::nullopt)) ==
                 "a stream's channels have other lengths or no samples");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode(channels, 32728, static_cast<sdat::WaveEncoding>(3),
                                                       std::nullopt)) == "unknown wave encoding");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(encode(channels, 32728, pcm16, 100)) ==
                 "a stream's loop starts at or past its end");
    XSFLIB_CHECK(sdat::StreamBlockSamples(sdat::WaveEncoding::kImaAdpcm, 0x40) == 120);
}

} // namespace

int main()
{
    TestSequenceFile();
    TestSequenceArchiveFile();
    TestBankFile();
    TestBankFileOfEditedInstruments();
    TestBankRefusals();
    TestWaveFiles();
    TestEncodePcm8();
    TestEncodePcm16Loop();
    TestEncodeImaAdpcm();
    TestEncodeImaAdpcmWithTheDsStep();
    TestEncodeRefusals();
    TestHardwareConstants();
    TestStreamLayout();
    TestStreamPcm8();
    TestStreamLoop();
    TestStreamRefusals();

    return xsflib::testing::Summary("DS sound file writers");
}
