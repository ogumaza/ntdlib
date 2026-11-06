// SPDX-License-Identifier: MIT

// Tests of the host's player of the Nintendo 64's sound engines, with a model made by hand. Each of the model's two
// voices adds a constant to the dry bus. The expected values follow from the microcode's clear, load, mixer and
// interleave commands, as the SDK's public abi.h gives them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/audio_rsp.h"
#include "ntdlib/n64/frame_player.h"

namespace
{

namespace n64 = ntdlib::n64;

// The list's addresses of a chunk's four buses, and the voice's buffer.
constexpr uint32_t kLeft = 0x440;
constexpr uint32_t kRight = 0x580;
constexpr uint32_t kWetLeft = 0x6C0;
constexpr uint32_t kWetRight = 0x800;
constexpr uint32_t kVoiceBuffer = 0x100;
constexpr uint32_t kChunkBytes = 0x140;
constexpr int kChunkSamples = 160;

constexpr int16_t kFirstLevel = 0x1000;
constexpr int16_t kSecondLevel = 0x0200;
constexpr int kFirstTrack = 3;
constexpr int kSecondTrack = 17;

class TestModel final : public n64::FrameModel
{
public:
    explicit TestModel(n64::AudioMemory& memory)
    {
        format_.microcode = n64::AudioMicrocode::kStandard;
        format_.output_rate = 32000;
        format_.frame_step = kChunkSamples;
        format_.areas = {{0x5C0 + kLeft, kChunkBytes, 0},
                         {0x5C0 + kRight, kChunkBytes, 1},
                         {0x5C0 + kWetLeft, kChunkBytes, 0},
                         {0x5C0 + kWetRight, kChunkBytes, 1}};
        first_ = Level(memory, kFirstLevel);
        second_ = Level(memory, kSecondLevel);
    }

    std::unique_ptr<n64::FrameModel> Clone() const override
    {
        return std::make_unique<TestModel>(*this);
    }

    void TakeState(const n64::FrameModel& other) override
    {
        frames_ = dynamic_cast<const TestModel&>(other).frames_;
    }

    void BuildFrame(n64::AudioList& list, n64::AudioMemory& /*memory*/, uint32_t output, int samples) override
    {
        frames_++;
        uint32_t place = output;
        for (int left = samples; left > 0; left -= kChunkSamples)
        {
            list.ClearBuffer(kLeft, kChunkBytes);
            list.ClearBuffer(kRight, kChunkBytes);
            list.ClearBuffer(kWetLeft, kChunkBytes);
            list.ClearBuffer(kWetRight, kChunkBytes);
            list.MarkVoice(kFirstTrack);
            AddVoice(list, first_, kLeft);
            list.MarkVoice(kSecondTrack);
            AddVoice(list, second_, kRight);
            AddVoice(list, second_, kWetRight);
            list.MarkVoicesEnd();
            list.MarkChunkEnd();
            list.SetBuffer(0, 0, 0, kChunkBytes);
            list.Mixer(0, 0x7FFF, kWetLeft, kLeft);
            list.Mixer(0, 0x7FFF, kWetRight, kRight);
            list.SetBuffer(0, 0, 0, kChunkBytes);
            list.Interleave(kLeft, kRight);
            list.SetBuffer(0, 0, 0, kChunkBytes * 2);
            list.SaveBuffer(place);
            place += kChunkBytes * 2;
        }
    }

    bool Busy() const override
    {
        return frames_ < 3;
    }

    const n64::FrameFormat& Format() const override
    {
        return format_;
    }

    int Frames() const
    {
        return frames_;
    }

private:
    static uint32_t Level(n64::AudioMemory& memory, int16_t level)
    {
        const uint32_t place = memory.Allocate(kChunkBytes);
        std::array<uint8_t, 2> bytes = {};
        xsflib::StoreBe16(bytes.data(), static_cast<uint16_t>(level));
        for (uint32_t i = 0; i < kChunkBytes; i += 2)
        {
            memory.Write(place + i, bytes.data(), bytes.size());
        }

        return place;
    }

    static void AddVoice(n64::AudioList& list, uint32_t level, uint32_t bus)
    {
        list.SetBuffer(0, kVoiceBuffer, 0, kChunkBytes);
        list.LoadBuffer(level);
        list.SetBuffer(0, 0, 0, kChunkBytes);
        list.Mixer(0, 0x7FFF, kVoiceBuffer, bus);
    }

    n64::FrameFormat format_;
    uint32_t first_ = 0;
    uint32_t second_ = 0;
    int frames_ = 0;
};

struct Bench
{
    Bench() : memory({}, 0x8000), player(std::make_unique<TestModel>(memory), memory, kChunkSamples * 2.0)
    {
    }

    int Frames()
    {
        return dynamic_cast<TestModel&>(player.Model()).Frames();
    }

    n64::AudioMemory memory;
    n64::FramePlayer player;
};

// A gain of 0x7FFF adds a sample as it is.
constexpr int16_t kBothRight = kSecondLevel * 2;

void CheckSamples(const std::vector<int16_t>& out, int16_t left, int16_t right)
{
    XSFLIB_CHECK(!out.empty());
    for (std::size_t i = 0; i < out.size(); i += 2)
    {
        XSFLIB_CHECK(out[i] == left);
        XSFLIB_CHECK(out[i + 1] == right);
    }
}

void TestRunsWholeFramesInParts()
{
    Bench bench;
    std::vector<int16_t> out;

    // The first 100 samples take a frame of 320. The next 300 take a second one.
    bench.player.Run(100, out);
    XSFLIB_CHECK(out.size() == 200);
    XSFLIB_CHECK(bench.Frames() == 1);
    bench.player.Run(300, out);
    XSFLIB_CHECK(out.size() == 800);
    XSFLIB_CHECK(bench.Frames() == 2);
    XSFLIB_CHECK(bench.player.Position() == 400);
    CheckSamples(out, kFirstLevel, kBothRight);
}

void TestFramesKeepToTheMean()
{
    // A mean of 240 samples and a step of 160: frames of 160 and 320 in turn.
    n64::AudioMemory memory({}, 0x8000);
    n64::FramePlayer player(std::make_unique<TestModel>(memory), memory, 240.0);
    std::vector<int16_t> out;
    player.Run(160, out);
    XSFLIB_CHECK(dynamic_cast<TestModel&>(player.Model()).Frames() == 1);
    player.Run(320, out);
    XSFLIB_CHECK(dynamic_cast<TestModel&>(player.Model()).Frames() == 2);
    player.Run(160, out);
    XSFLIB_CHECK(dynamic_cast<TestModel&>(player.Model()).Frames() == 3);
    CheckSamples(out, kFirstLevel, kBothRight);
}

void TestMutes()
{
    Bench bench;
    std::vector<int16_t> out;
    bench.player.MuteTrack(kFirstTrack, true);
    XSFLIB_CHECK(bench.player.IsTrackMuted(kFirstTrack));
    XSFLIB_CHECK(!bench.player.IsTrackMuted(kSecondTrack));
    bench.player.Run(320, out);
    CheckSamples(out, 0, kBothRight);

    // A mute takes effect with the next frame.
    out.clear();
    bench.player.MuteTrack(kFirstTrack, false);
    bench.player.MuteTrack(kSecondTrack, true);
    bench.player.Run(320, out);
    CheckSamples(out, kFirstLevel, 0);

    bool threw = false;
    try
    {
        bench.player.MuteTrack(n64::FramePlayer::kHostTracks, true);
    }
    catch (const std::out_of_range&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

void TestMeters()
{
    Bench bench;
    std::vector<int16_t> out;
    bench.player.Run(320, out);
    XSFLIB_CHECK((bench.player.TrackPeaks(kFirstTrack) == std::array<int32_t, 2>{0, 0}));

    bench.player.MeterTracks(true);
    out.clear();
    bench.player.Run(320, out);
    CheckSamples(out, kFirstLevel, kBothRight);
    XSFLIB_CHECK((bench.player.TrackPeaks(kFirstTrack) == std::array<int32_t, 2>{kFirstLevel, 0}));
    XSFLIB_CHECK((bench.player.TrackPeaks(kSecondTrack) == std::array<int32_t, 2>{0, kBothRight}));
    XSFLIB_CHECK((bench.player.TrackPeaks(0) == std::array<int32_t, 2>{0, 0}));
    XSFLIB_CHECK((bench.player.TrackPeaks(n64::FramePlayer::kHostTracks) == std::array<int32_t, 2>{0, 0}));

    // A muted track's meter still moves.
    bench.player.MuteTrack(kFirstTrack, true);
    out.clear();
    bench.player.Run(320, out);
    CheckSamples(out, 0, kBothRight);
    XSFLIB_CHECK((bench.player.TrackPeaks(kFirstTrack) == std::array<int32_t, 2>{kFirstLevel, 0}));

    // A run without mixing gives 0.
    bench.player.Run(320);
    XSFLIB_CHECK((bench.player.TrackPeaks(kFirstTrack) == std::array<int32_t, 2>{0, 0}));
}

void TestSnapshots()
{
    Bench bench;
    std::vector<int16_t> out;
    bench.player.Run(100, out);
    const n64::FramePlayer::Snapshot snapshot = bench.player.Save();
    bench.player.MuteTrack(kFirstTrack, true);
    std::vector<int16_t> muted;
    bench.player.Run(540, muted);
    XSFLIB_CHECK(bench.Frames() == 2);
    XSFLIB_CHECK(muted[0] == kFirstLevel);
    XSFLIB_CHECK(muted.back() == kBothRight);
    XSFLIB_CHECK(muted[muted.size() - 2] == 0);

    bench.player.Restore(snapshot);
    XSFLIB_CHECK(bench.Frames() == 1);
    XSFLIB_CHECK(bench.player.Position() == 100);

    // The mute stayed, and the frame before the snapshot's end was already mixed.
    XSFLIB_CHECK(bench.player.IsTrackMuted(kFirstTrack));
    std::vector<int16_t> again;
    bench.player.Run(540, again);
    XSFLIB_CHECK(again == muted);

    Bench other;
    bool threw = false;
    try
    {
        other.player.Restore(snapshot);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

void TestRunWithoutMixing()
{
    Bench bench;
    bench.player.Run(320);
    XSFLIB_CHECK(bench.Frames() == 1);
    XSFLIB_CHECK(bench.player.Position() == 320);
    std::vector<int16_t> out;
    bench.player.Run(320, out);
    CheckSamples(out, kFirstLevel, kBothRight);
    XSFLIB_CHECK(bench.player.Busy());
    bench.player.Run(320);
    XSFLIB_CHECK(!bench.player.Busy());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_frame_player",
                                    []
                                    {
                                        TestRunsWholeFramesInParts();
                                        TestFramesKeepToTheMean();
                                        TestMutes();
                                        TestMeters();
                                        TestSnapshots();
                                        TestRunWithoutMixing();
                                    });
}
