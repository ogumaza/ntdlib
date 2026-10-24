// SPDX-License-Identifier: MIT

// Tests of the model of the Wii's AX microcode (rvl/ax/render.h): silence without voices, a 16-bit voice at full
// volume that comes out as it is, the same voice at half volume, a voice on the right channel only, and a voice whose
// end without a loop stops it. A voice of AX's build of October 2006 comes out as it is too. The expected values follow
// from the microcode's arithmetic: at a volume of 0x8000, a sample passes the envelope, the mix and the output
// unchanged, and the compressor leaves a frame alone while every sample of the main bus stays below full scale. A
// host's meters measure each voice's mix into the main bus and what an aux bus returns, and leave the output as it is.
// At a return volume of 0x8000, the build of October 2006 returns half an aux bus, and the build of December 2006 the
// whole bus.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/render.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/types.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

using xsflib::testing::Check;

namespace
{

using namespace ntdlib::rvl;

// Where the tests put a voice's samples in MEM1, and how many there are.
constexpr ntdlib::PAddr kSamples = 0x00600000;
constexpr int kSampleCount = 1000;

int16_t SampleAt(int i)
{
    return static_cast<int16_t>(i * 61 - 30000);
}

// A frame's output: left and right for each of its 96 samples.
struct Output
{
    std::vector<int16_t> left;
    std::vector<int16_t> right;
};

// A model of AX and its microcode of `build`, with the test's samples in memory.
struct Model
{
    explicit Model(ax::Build build = ax::Build::kDecember2006) : renderer(build)
    {
        ax.Init(build);
        for (int i = 0; i < kSampleCount; i++)
        {
            xsflib::StoreBe16(memory.At(kSamples + static_cast<ntdlib::PAddr>(i) * 2, 2),
                              static_cast<uint16_t>(SampleAt(i)));
        }
    }

    // Starts a voice that plays the samples once at the sample rate, without a converter, into the channels that
    // `mixer` gives at `volume`.
    ax::Voice* StartVoice(uint32_t mixer, uint16_t volume)
    {
        ax::Voice* voice = ax.AcquireVoice(31, nullptr);
        ax::PbAddr address;
        address.format = static_cast<uint16_t>(ntdlib::ax::SampleFormat::kPcm16);
        const uint32_t start = kSamples / 2;
        const uint32_t end = start + kSampleCount - 1;
        address.loop_address_hi = static_cast<uint16_t>(start >> 16);
        address.loop_address_lo = static_cast<uint16_t>(start);
        address.end_address_hi = static_cast<uint16_t>(end >> 16);
        address.end_address_lo = static_cast<uint16_t>(end);
        address.current_address_hi = static_cast<uint16_t>(start >> 16);
        address.current_address_lo = static_cast<uint16_t>(start);
        voice->SetAddr(address);
        voice->pb.src_select = static_cast<uint16_t>(ntdlib::ax::SrcMode::kNone);
        voice->pb.SetMixerCtrl(mixer);
        voice->pb.mix.l = volume;
        voice->pb.mix.r = volume;
        voice->pb.ve.current_volume = 0x8000;
        voice->sync |= ntdlib::ax::sync::kAll;
        voice->SetState(ntdlib::ax::VoiceState::kRun);
        return voice;
    }

    Output RunFrame(ax::Meters* meters = nullptr)
    {
        const ax::Frame frame = ax.StartFrame();
        Check(renderer.RunFrame(memory, frame.command_list, ax::layout::kCommandListSize, meters),
              "the frame's commands run");
        Output output;
        const uint8_t* samples = memory.At(frame.output, ax::layout::kOutputBufferSize);
        for (int i = 0; i < ax::kSamplesPerFrame; i++)
        {
            output.right.push_back(static_cast<int16_t>(xsflib::LoadBe16(samples + i * 4)));
            output.left.push_back(static_cast<int16_t>(xsflib::LoadBe16(samples + i * 4 + 2)));
        }
        ax.FinishFrame();
        return output;
    }

    ntdlib::dsp::WiiMemory memory;
    ax::System ax{memory};
    ax::Renderer renderer;
};

bool Silent(const std::vector<int16_t>& samples)
{
    for (const int16_t sample : samples)
    {
        if (sample != 0)
        {
            return false;
        }
    }

    return true;
}

void TestSilence()
{
    Model model;
    for (int frame = 0; frame < 3; frame++)
    {
        const Output output = model.RunFrame();
        Check(Silent(output.left) && Silent(output.right), "a frame without voices is silent");
    }
}

void TestFullVolume()
{
    Model model;
    model.StartVoice(ax::mixer::kL, 0x8000);
    for (int frame = 0; frame < 3; frame++)
    {
        const Output output = model.RunFrame();
        bool same = Silent(output.right);
        for (int i = 0; i < ax::kSamplesPerFrame; i++)
        {
            same = same && output.left[static_cast<std::size_t>(i)] == SampleAt(frame * ax::kSamplesPerFrame + i);
        }
        Check(same, "a voice at full volume on the left comes out as it is");
    }
}

void TestHalfVolume()
{
    Model model;
    model.StartVoice(ax::mixer::kL | ax::mixer::kR, 0x4000);
    const Output output = model.RunFrame();
    bool halved = true;
    for (int i = 0; i < ax::kSamplesPerFrame; i++)
    {
        // The product, doubled, loses its 16 fractional bits: the half rounds down.
        const int expected = SampleAt(i) >> 1;
        halved = halved && output.left[static_cast<std::size_t>(i)] == expected &&
                 output.right[static_cast<std::size_t>(i)] == expected;
    }
    Check(halved, "a voice at half volume comes out halved on both sides");
}

void TestEnd()
{
    // The voice ends without a loop after its 1,000th sample: in the 11th frame. The PB keeps the stop, and the
    // frame's later samples are silent.
    Model model;
    ax::Voice* voice = model.StartVoice(ax::mixer::kR, 0x8000);
    Output last;
    for (int frame = 0; frame < 11; frame++)
    {
        last = model.RunFrame();
    }
    const int left_over = kSampleCount - 10 * ax::kSamplesPerFrame;
    bool ended = Silent(last.left);
    for (int i = 0; i < ax::kSamplesPerFrame; i++)
    {
        const int expected = i < left_over ? SampleAt(10 * ax::kSamplesPerFrame + i) : 0;
        ended = ended && last.right[static_cast<std::size_t>(i)] == expected;
    }
    Check(ended, "a voice that ends without a loop goes silent after its last sample");
    Check(model.ax.ReadPb(voice->index).state == static_cast<uint16_t>(ntdlib::ax::VoiceState::kStop),
          "the voice's PB keeps the stop");
}

void TestOctober2006()
{
    // The build of October 2006 splits PROCESS in two, and keeps its PBs in another layout. A voice at full volume on
    // the left comes out as it is, and stops after its last sample in the 11th frame. The PB's update block keeps its
    // bytes.
    Model model(ax::Build::kOctober2006);
    ax::Voice* voice = model.StartVoice(ax::mixer::kL, 0x8000);
    const uint8_t* block = model.memory.At(model.ax.PbAddress(voice->index) + ax::kUpdateBlockOctober2006, 10);
    const std::vector<uint8_t> update(block, block + 10);
    bool same = true;
    Output last;
    for (int frame = 0; frame < 11; frame++)
    {
        last = model.RunFrame();
        same = same && Silent(last.right);
        for (int i = 0; i < ax::kSamplesPerFrame; i++)
        {
            const int at = frame * ax::kSamplesPerFrame + i;
            same = same && last.left[static_cast<std::size_t>(i)] == (at < kSampleCount ? SampleAt(at) : 0);
        }
    }
    Check(same, "a voice of the build of October 2006 at full volume comes out as it is, and goes silent at its end");
    Check(model.ax.ReadPb(voice->index).state == static_cast<uint16_t>(ntdlib::ax::VoiceState::kStop),
          "the October 2006 PB keeps the stop");
    Check(std::equal(update.begin(), update.end(), block), "the October 2006 PB's update block keeps its bytes");
}

// The largest magnitude of `samples`.
int Peak(const std::vector<int16_t>& samples)
{
    int peak = 0;
    for (const int16_t sample : samples)
    {
        peak = std::max(peak, std::abs(int{sample}));
    }

    return peak;
}

void TestMeters()
{
    // Meter 0 measures a voice on the left at full volume, and meter 1 a voice on both sides at half volume. A voice on
    // the right without a meter adds to neither. The frame's first sample is the loudest: -30,000, and -15,000 at half
    // volume. The output stays as it is without the meters.
    Model metered;
    Model plain;
    std::vector<uint32_t> indices;
    for (Model* model : {&metered, &plain})
    {
        indices.push_back(model->StartVoice(ax::mixer::kL, 0x8000)->index);
        indices.push_back(model->StartVoice(ax::mixer::kL | ax::mixer::kR, 0x4000)->index);
        model->StartVoice(ax::mixer::kR, 0x8000);
    }
    ax::Meters meters;
    meters.pbs = {{metered.ax.PbAddress(indices[0]), 0}, {metered.ax.PbAddress(indices[1]), 1}};
    meters.peaks.assign(2, {7, 7});
    const Output output = metered.RunFrame(&meters);
    const Output expected = plain.RunFrame();

    Check(meters.peaks[0][0] == 30000 && meters.peaks[0][1] == 0, "a meter measures its voice's mix on each side");
    Check(meters.peaks[1][0] == 15000 && meters.peaks[1][1] == 15000, "a meter measures a voice at half volume");
    Check(output.left == expected.left && output.right == expected.right, "the meters leave the output as it is");
    Check(meters.aux_peaks[0][0] == 0 && meters.aux_peaks[2][1] == 0, "an aux bus that nothing returns measures 0");
}

void TestAuxReturnPeaks()
{
    // A voice sends to aux bus A's left only. The bus's callback leaves each frame as it is, and the frame comes back
    // into the main bus at the return volume of 0x8000. The return's peak is then the output's on the left.
    Model model;
    ax::Voice* voice = model.StartVoice(ax::mixer::kAuxAL, 0);
    voice->pb.mix.aux_a_l = 0x8000;
    voice->sync |= ntdlib::ax::sync::kAll;
    model.ax.RegisterAuxCallback(0, [](ntdlib::rvl::axfx::AuxFrame&) {});
    ax::Meters meters;
    bool returned = false;
    bool measured = true;
    for (int frame = 0; frame < 8; frame++)
    {
        const Output output = model.RunFrame(&meters);
        returned = returned || Peak(output.left) > 0;
        measured = measured && meters.aux_peaks[0][0] == Peak(output.left) && meters.aux_peaks[0][1] == 0 &&
                   Peak(output.right) == 0;
    }

    Check(returned, "the aux bus's frames come back into the main bus");
    Check(measured, "the return's peaks are those of what it adds to each side");
}

void TestAuxSurroundPeaks()
{
    // The bus's callback moves each frame's left into its surround channel. The surround channel comes back into the
    // main bus, and the next frame's ADD_TO_LR adds it to the left and right. The return's peaks are then the output's
    // on each side, in every frame, and a meter of the voice measures nothing.
    Model model;
    ax::Voice* voice = model.StartVoice(ax::mixer::kAuxAL, 0);
    voice->pb.mix.aux_a_l = 0x8000;
    voice->sync |= ntdlib::ax::sync::kAll;
    model.ax.RegisterAuxCallback(0,
                                 [](ntdlib::rvl::axfx::AuxFrame& frame)
                                 {
                                     frame.channels[ntdlib::rvl::axfx::kSurround] =
                                         frame.channels[ntdlib::rvl::axfx::kLeft];
                                     frame.channels[ntdlib::rvl::axfx::kLeft].fill(0);
                                 });
    ax::Meters meters;
    meters.pbs.emplace_back(model.ax.PbAddress(voice->index), 0);
    meters.peaks.resize(1);
    bool both = false;
    bool measured = true;
    for (int frame = 0; frame < 8; frame++)
    {
        const Output output = model.RunFrame(&meters);
        both = both || (Peak(output.left) > 0 && Peak(output.right) > 0);
        measured = measured && meters.aux_peaks[0][0] == Peak(output.left) &&
                   meters.aux_peaks[0][1] == Peak(output.right) && meters.peaks[0][0] == 0 && meters.peaks[0][1] == 0;
    }

    Check(both, "the surround channel comes out on both sides a frame later");
    Check(measured, "the return's peaks hold the surround channel that reaches each side");
}

void TestOctober2006AuxReturn()
{
    // A voice sends to aux bus A's left only, and the bus's callback leaves each frame as it is. The microcode of
    // October 2006 doesn't double the products of the return volume. At 0x8000, its return is then half that of the
    // microcode of December 2006. The product loses its 16 fractional bits: the half rounds down.
    Model december;
    Model october(ax::Build::kOctober2006);
    for (Model* model : {&december, &october})
    {
        ax::Voice* voice = model->StartVoice(ax::mixer::kAuxAL, 0);
        voice->pb.mix.aux_a_l = 0x8000;
        voice->sync |= ntdlib::ax::sync::kAll;
        model->ax.RegisterAuxCallback(0, [](ntdlib::rvl::axfx::AuxFrame&) {});
    }
    bool returned = false;
    bool halved = true;
    for (int frame = 0; frame < 8; frame++)
    {
        const Output whole = december.RunFrame();
        const Output half = october.RunFrame();
        returned = returned || Peak(whole.left) > 0;
        for (int i = 0; i < ax::kSamplesPerFrame; i++)
        {
            const auto at = static_cast<std::size_t>(i);
            halved = halved && half.left[at] == whole.left[at] >> 1 && half.right[at] == 0;
        }
    }

    Check(returned, "the aux bus's frames come back into the main bus");
    Check(halved, "the build of October 2006 returns half the bus at a return volume of 0x8000");
}

} // namespace

int main()
{
    TestSilence();
    TestFullVolume();
    TestHalfVolume();
    TestEnd();
    TestOctober2006();
    TestMeters();
    TestAuxReturnPeaks();
    TestAuxSurroundPeaks();
    TestOctober2006AuxReturn();

    return xsflib::testing::Summary("Wii AX renderer");
}
