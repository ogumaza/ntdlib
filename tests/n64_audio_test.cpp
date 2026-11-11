// SPDX-License-Identifier: MIT

// Tests of the audio microcode's model (ntdlib/n64/audio_rsp.h) on the commands of the SDK's public abi.h. The
// expected values come by hand from what each command does: a gain is a 16-bit fraction, a sum saturates, and a VADPCM
// frame with a book of zeros gives each nibble shifted left by the frame's scale.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/audio_rsp.h"

namespace
{

namespace n64 = ntdlib::n64;

constexpr uint32_t kFirstBuffer = 0x100;
constexpr uint32_t kSecondBuffer = 0x200;
constexpr uint32_t kBytes = 32;

struct Bench
{
    Bench() : memory({}, 0x1000)
    {
        input = memory.Allocate(kBytes * 2);
        second_input = memory.Allocate(kBytes * 2);
        output = memory.Allocate(kBytes * 2);
        state = memory.Allocate(kBytes * 2);
    }

    // Puts `count` samples of `value` at `address`.
    void Fill(uint32_t address, int count, int16_t value)
    {
        for (int i = 0; i < count; i++)
        {
            std::array<uint8_t, 2> bytes = {};
            xsflib::StoreBe16(bytes.data(), static_cast<uint16_t>(value));
            memory.Write(address + static_cast<uint32_t>(i) * 2, bytes.data(), bytes.size());
        }
    }

    int16_t Sample(uint32_t address, int index) const
    {
        std::array<uint8_t, 2> bytes = {};
        memory.Read(address + static_cast<uint32_t>(index) * 2, bytes.data(), bytes.size());

        return static_cast<int16_t>(xsflib::LoadBe16(bytes.data()));
    }

    void Run(n64::AudioMicrocode microcode = n64::AudioMicrocode::kStandard)
    {
        n64::AudioRsp rsp(microcode);
        rsp.StartTask();
        rsp.Run(list.commands, memory);
        list.Clear();
    }

    // Loads `count` bytes at `from` to the buffer `dmem`.
    void Load(uint32_t dmem, uint32_t from, uint32_t count)
    {
        list.SetBuffer(0, dmem, 0, count);
        list.LoadBuffer(from);
    }

    // Saves `count` bytes of the buffer `dmem`.
    void Save(uint32_t dmem, uint32_t to, uint32_t count)
    {
        list.SetBuffer(0, 0, dmem, count);
        list.SaveBuffer(to);
    }

    n64::AudioMemory memory;
    n64::AudioList list;
    uint32_t input = 0;
    uint32_t second_input = 0;
    uint32_t output = 0;
    uint32_t state = 0;
};

void TestLoadAndSave()
{
    Bench bench;
    bench.Fill(bench.input, 16, 0x1234);
    bench.Load(kFirstBuffer, bench.input, kBytes);
    bench.Save(kFirstBuffer, bench.output, kBytes);
    bench.Run();
    for (int i = 0; i < 16; i++)
    {
        XSFLIB_CHECK(bench.Sample(bench.output, i) == 0x1234);
    }
}

void TestClear()
{
    Bench bench;
    bench.Fill(bench.input, 16, 0x1234);
    bench.Load(kFirstBuffer, bench.input, kBytes);
    bench.list.ClearBuffer(kFirstBuffer, 16);
    bench.Save(kFirstBuffer, bench.output, kBytes);
    bench.Run();
    XSFLIB_CHECK(bench.Sample(bench.output, 0) == 0);
    XSFLIB_CHECK(bench.Sample(bench.output, 7) == 0);
    XSFLIB_CHECK(bench.Sample(bench.output, 8) == 0x1234);
}

int16_t MixOne(int16_t in, int16_t gain, int16_t out)
{
    Bench bench;
    bench.Fill(bench.input, 16, in);
    bench.Fill(bench.second_input, 16, out);
    bench.Load(kFirstBuffer, bench.input, kBytes);
    bench.Load(kSecondBuffer, bench.second_input, kBytes);
    bench.list.SetBuffer(0, 0, 0, kBytes);
    bench.list.Mixer(0, static_cast<uint16_t>(gain), kFirstBuffer, kSecondBuffer);
    bench.Save(kSecondBuffer, bench.output, kBytes);
    bench.Run();
    XSFLIB_CHECK(bench.Sample(bench.output, 15) == bench.Sample(bench.output, 0));

    return bench.Sample(bench.output, 0);
}

void TestMixer()
{
    XSFLIB_CHECK(MixOne(0x4000, 0x4000, 0x1000) == 0x3000);
    XSFLIB_CHECK(MixOne(-0x4000, 0x4000, 0x1000) == -0x1000);
    XSFLIB_CHECK(MixOne(0x7FFF, 0x7FFF, 0x7000) == 0x7FFF);
    XSFLIB_CHECK(MixOne(-0x8000, 0x7FFF, -0x7000) == -0x8000);
    XSFLIB_CHECK(MixOne(0x4000, 0, 0x1000) == 0x1000);
}

void TestInterleave()
{
    Bench bench;
    bench.Fill(bench.input, 8, 0x1111);
    bench.Fill(bench.second_input, 8, 0x2222);
    bench.Load(kFirstBuffer, bench.input, 16);
    bench.Load(kSecondBuffer, bench.second_input, 16);
    constexpr uint32_t kOut = 0x300;
    bench.list.SetBuffer(0, 0, kOut, 16);
    bench.list.Interleave(kFirstBuffer, kSecondBuffer);
    bench.Save(kOut, bench.output, kBytes);
    bench.Run();
    for (int i = 0; i < 8; i++)
    {
        XSFLIB_CHECK(bench.Sample(bench.output, i * 2) == 0x1111);
        XSFLIB_CHECK(bench.Sample(bench.output, i * 2 + 1) == 0x2222);
    }
}

// GoldenEye 007's microcode adds its ramp where the others multiply. A gain of 0x1000 with a rate of 0x100 for each 8
// samples is at 0x1000 + 0x20 * (i + 1) at sample i. A dry gain of 0x7FFF on samples of 0x4000 halves it, within
// rounding.
void TestLinearRamp()
{
    Bench bench;
    bench.Fill(bench.input, kBytes / 2, 0x4000);
    bench.Load(kFirstBuffer, bench.input, kBytes);
    bench.list.SetBuffer(0, kFirstBuffer, kSecondBuffer, kBytes);
    bench.list.SetBuffer(n64::kAudioFlagAux, 0x400, 0x500, 0x600);
    bench.list.SetVolume(n64::kAudioFlagLeft | n64::kAudioFlagVolume, 0x1000, 0, 0);
    bench.list.SetVolume(n64::kAudioFlagVolume, 0x1000, 0, 0);
    bench.list.SetVolume(n64::kAudioFlagLeft, 0x7000, 0x100, 0);
    bench.list.SetVolume(0, 0x7000, 0x100, 0);
    bench.list.SetVolume(n64::kAudioFlagAux, 0x7FFF, 0, 0);
    bench.list.EnvMixer(n64::kAudioFlagInit | n64::kAudioFlagAux, bench.state);
    bench.Save(kSecondBuffer, bench.output, kBytes);
    bench.Run(n64::AudioMicrocode::kStandardLinear);

    const auto near = [](int value, int expected)
    {
        return std::abs(value - expected) <= 2;
    };

    XSFLIB_CHECK(near(bench.Sample(bench.output, 0), (0x1000 + 0x20) / 2));
    XSFLIB_CHECK(near(bench.Sample(bench.output, 7), (0x1000 + 0x100) / 2));
    XSFLIB_CHECK(near(bench.Sample(bench.output, 15), (0x1000 + 0x200) / 2));
}

void TestAdpcmWithBookOfZeros()
{
    Bench bench;

    // A frame of 9 bytes: scale 2 and predictor 0, then 16 nibbles.
    constexpr std::array<uint8_t, 9> kFrame = {0x20, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    bench.memory.Write(bench.input, kFrame.data(), kFrame.size());
    const std::vector<uint8_t> book(16, 0);
    const uint32_t book_place = bench.memory.Allocate(book.size());
    bench.memory.Write(book_place, book.data(), book.size());
    bench.list.LoadAdpcm(static_cast<uint32_t>(book.size()), book_place);
    bench.Load(kFirstBuffer, bench.input, kFrame.size());
    bench.list.SetBuffer(0, kFirstBuffer, kSecondBuffer, kBytes);
    bench.list.Adpcm(n64::kAudioFlagInit, bench.state);

    // The decoder puts the 16 samples before the frame at the start of its output, and the frame's after them.
    bench.Save(kSecondBuffer + kBytes, bench.output, kBytes);
    bench.Run();
    constexpr std::array<int16_t, 16> kNibbles = {1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1, 0};
    for (int i = 0; i < 16; i++)
    {
        XSFLIB_CHECK(bench.Sample(bench.output, i) == kNibbles[static_cast<std::size_t>(i)] * 4);
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_audio",
                                    []
                                    {
                                        TestLoadAndSave();
                                        TestClear();
                                        TestMixer();
                                        TestInterleave();
                                        TestLinearRamp();
                                        TestAdpcmWithBookOfZeros();
                                    });
}
