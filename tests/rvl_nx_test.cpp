// SPDX-License-Identifier: MIT

// Tests of Skyward Sword HD's layer in place of AX (rvl::nx::Layer). AX's voices go to the layer: an acquisition takes
// the first free record, or the oldest of the lowest priority below the one asked for, whose callback runs first. A
// freed voice that played waits for the next frame. The PB's state word holds the record's state. A PCM16 voice plays
// its big-endian wave through the renderer at 48 kHz. Its current address goes back into the PB, and the loop address
// replaces it once the wave has ended. A snapshot of the layer, the renderer and AX plays the same frames again. A
// stream voice is refused. The renderer's priorities follow the game's formula (0x103ee88). The game's reverbs
// (rvl::nx::Effects) give a bus silence for two frames after its first reverb. Then the reverb runs on the bus's frames
// and rings after its input stops. The reverb class gives nn::audio the parameters of the game's reverbs for buses A
// and B, and holds other values to their ranges.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/nx/audio/reverb.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/nx/effects.h"
#include "ntdlib/rvl/nx/layer.h"

namespace
{

using namespace ntdlib;
namespace ax = ntdlib::rvl::ax;
namespace audio = ntdlib::nx::audio;
using ntdlib::rvl::nx::Layer;
using xsflib::testing::Check;

constexpr std::size_t kChannels = 6;
constexpr std::size_t kFrameSamples = 240;

struct Fixture
{
    dsp::WiiMemory memory;
    ax::System ax{memory};
    audio::Renderer renderer{Layer::Parameter()};
    Layer layer{ax, memory, renderer};

    // The front channels of the next frame, left then right.
    std::array<std::vector<int16_t>, 2> Frame()
    {
        layer.RenderFrame();
        const std::span<const int16_t> output = renderer.DeviceOutput();
        std::array<std::vector<int16_t>, 2> samples;
        for (std::size_t i = 0; i < kFrameSamples; i++)
        {
            samples[0].push_back(output[i * kChannels]);
            samples[1].push_back(output[i * kChannels + 1]);
        }

        return samples;
    }
};

// A voice of `count` samples of 256 in PCM16, big-endian as on the Wii, that sends its full level to the left. The wave
// doesn't loop. Its loop address points at `zero`. NW4R SND points it at the zero buffer.
ax::Voice* StartVoice(Fixture& fixture, uint32_t count, PAddr& zero)
{
    std::vector<uint8_t> wave(static_cast<std::size_t>(count) * 2);
    for (std::size_t i = 0; i < wave.size(); i += 2)
    {
        wave[i] = 0x01;
        wave[i + 1] = 0x00;
    }

    const std::vector<uint8_t> silence(0x100);
    zero = fixture.memory.Store(silence.data(), silence.size());
    const PAddr address = fixture.memory.Store(wave.data(), wave.size());
    ax::Voice* voice = fixture.ax.AcquireVoice(16, nullptr);
    ax::PbAddr addr;
    addr.format = static_cast<uint16_t>(ax::SampleFormat::kPcm16);
    const uint32_t start = address / 2;
    const uint32_t end = start + count - 1;
    addr.loop_address_hi = static_cast<uint16_t>((zero / 2) >> 16);
    addr.loop_address_lo = static_cast<uint16_t>(zero / 2);
    addr.end_address_hi = static_cast<uint16_t>(end >> 16);
    addr.end_address_lo = static_cast<uint16_t>(end);
    addr.current_address_hi = static_cast<uint16_t>(start >> 16);
    addr.current_address_lo = static_cast<uint16_t>(start);
    voice->SetAddr(addr);
    voice->pb.src_select = static_cast<uint16_t>(ax::SrcMode::kFourTap);
    voice->pb.src.ratio_hi = 1;
    voice->pb.ve.current_volume = ax::kMaxVolume;
    voice->pb.mix.l = ax::kMaxVolume;
    voice->pb.SetMixerCtrl(ax::mixer::kL);
    voice->SetState(ax::VoiceState::kRun);

    return voice;
}

void CheckPriorities()
{
    Check(Layer::RendererPriority(0) == 255 && Layer::RendererPriority(31) == 0, "priorities 0 and 31");
    Check(Layer::RendererPriority(1) == 246 && Layer::RendererPriority(15) == 131 && Layer::RendererPriority(16) == 123,
          "priorities 1, 15 and 16");
    Check(Layer::RendererPriority(40) == 0, "a priority above 31 is held to 0");
}

void CheckAcquisition()
{
    Fixture fixture;
    std::vector<uint32_t> dropped;
    const auto callback = [&dropped](ax::Voice* voice)
    {
        dropped.push_back(voice->index);
    };
    bool all = true;
    for (uint32_t i = 0; i < Layer::kVoiceCount; i++)
    {
        const ax::Voice* voice = fixture.ax.AcquireVoice(i % 4 + 1, callback);
        all = all && voice != nullptr && voice->index == i && voice->pb.state == 8;
    }

    Check(all, "64 voices in order of their records, each acquired");
    Check(fixture.ax.AcquireVoice(1, callback) == nullptr, "no voice has a priority below 1");
    const ax::Voice* first = fixture.ax.AcquireVoice(2, callback);
    Check(first != nullptr && first->index == 0 && dropped == std::vector<uint32_t>{0},
          "priority 2 takes the oldest voice of priority 1, after its callback");
    const ax::Voice* second = fixture.ax.AcquireVoice(2, callback);
    Check(second != nullptr && second->index == 4 && dropped == std::vector<uint32_t>{0, 4},
          "the next takes the next oldest");
    Check(fixture.ax.VoicePriority(first) == 2 && static_cast<bool>(first->callback),
          "the taken voice's priority and callback");
}

void CheckPlay()
{
    Fixture fixture;
    PAddr zero = 0;
    ax::Voice* voice = StartVoice(fixture, 3200, zero);
    Check(voice->pb.state == 1, "a voice that runs plays");

    // The wave's 100 ms play at 256: its bytes are swapped for the renderer.
    bool level = true;
    bool right = true;
    for (int frame = 0; frame < 30; frame++)
    {
        const std::array<std::vector<int16_t>, 2> samples = fixture.Frame();
        if (frame >= 3 && frame < 18)
        {
            for (const int16_t sample : samples[0])
            {
                level = level && sample >= 250 && sample <= 262;
            }
        }

        for (const int16_t sample : samples[1])
        {
            right = right && sample == 0;
        }
    }

    Check(level, "the left channel plays the wave at 256");
    Check(right, "the right channel stays silent");
    const uint32_t current = ax::Join(voice->pb.addr.current_address_hi, voice->pb.addr.current_address_lo);
    Check(current == zero / 2, "after the wave, the PB's current address is the loop address");

    fixture.ax.FreeVoice(voice);
    Check(voice->pb.state == 0xfffe, "a freed voice that played waits for its release");
    fixture.Frame();
    Check(voice->pb.state == 0xffff, "the next frame frees it");
}

void CheckCurrentAddress()
{
    // While the wave plays, the current address runs from the start by the samples played.
    Fixture fixture;
    PAddr zero = 0;
    ax::Voice* voice = StartVoice(fixture, 32000, zero);
    const uint32_t start = ax::Join(voice->pb.addr.current_address_hi, voice->pb.addr.current_address_lo);
    std::vector<uint32_t> steps;
    uint32_t last = start;
    for (int frame = 0; frame < 8; frame++)
    {
        fixture.Frame();
        const uint32_t current = ax::Join(voice->pb.addr.current_address_hi, voice->pb.addr.current_address_lo);
        steps.push_back(current - last);
        last = current;
    }

    bool frames = true;
    for (std::size_t i = 3; i < steps.size(); i++)
    {
        frames = frames && steps[i] == 160;
    }

    Check(frames, "the current address moves on by a frame's 160 samples each frame");
}

void CheckSnapshot()
{
    Fixture fixture;
    PAddr zero = 0;
    StartVoice(fixture, 9600, zero);
    for (int frame = 0; frame < 4; frame++)
    {
        fixture.Frame();
    }

    const audio::Renderer::Snapshot renderer = fixture.renderer.Save();
    const ax::System ax = fixture.ax;
    const Layer::Snapshot layer = fixture.layer.Save();
    std::vector<int16_t> first;
    for (int frame = 0; frame < 40; frame++)
    {
        const std::vector<int16_t> samples = fixture.Frame()[0];
        first.insert(first.end(), samples.begin(), samples.end());
    }

    fixture.renderer.Restore(renderer);
    fixture.ax = ax;
    fixture.layer.Restore(layer);
    std::vector<int16_t> second;
    for (int frame = 0; frame < 40; frame++)
    {
        const std::vector<int16_t> samples = fixture.Frame()[0];
        second.insert(second.end(), samples.begin(), samples.end());
    }

    Check(first == second, "a restored snapshot plays the same frames");

    Fixture other;
    bool refused = false;
    try
    {
        other.layer.Restore(layer);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }

    Check(refused, "a snapshot of another layer is refused");
}

void CheckStream()
{
    Fixture fixture;
    ax::Voice* voice = fixture.ax.AcquireVoice(16, nullptr);
    voice->pb.type = static_cast<uint16_t>(ax::VoiceType::kStream);
    bool refused = false;
    try
    {
        voice->SetState(ax::VoiceState::kRun);
    }
    catch (const std::logic_error&)
    {
        refused = true;
    }

    Check(refused, "a stream voice is refused");
}

void CheckReverbParameters()
{
    namespace nx = ntdlib::rvl::nx;
    const audio::ReverbParameterSet b = nx::ReverbParameters(nx::kBusBReverb);
    Check(b.early_mode == 2 && b.late_mode == 1 && b.early_gain == 0.3f && b.late_gain == 1.0f && b.decay_time == 2.0f,
          "bus B's reverb: a hall's early reflections, a hall's late reverb, and the gains and decay time");
    Check(b.predelay == 0.1f * 1000.0f && b.high_frequency_decay_ratio == 0.7f && b.coloration == 0.2f / 0.6f &&
              b.reverb_gain == 1.0f && b.out_gain == 1.0f && b.dry_gain == 0.0f,
          "bus B's reverb: the pre-delay in ms, the damping, the coloration over 0.6, and no dry signal");
    const audio::ReverbParameterSet a = nx::ReverbParameters(nx::kBusAReverb);
    Check(a.early_mode == 3 && a.coloration == 1.0f && a.predelay == 0.02f * 1000.0f,
          "bus A's reverb: a cathedral's early reflections, and the coloration held to 1");

    nx::ReverbParam wild = nx::kBusBReverb;
    wild.predelay_time = 0.5f;
    wild.fused_time = -1.0f;
    wild.out_gain = 2.0f;
    wild.early_mode = -1;
    wild.fused_mode = 9;
    wild.early_gain = -0.5f;
    const audio::ReverbParameterSet held = nx::ReverbParameters(wild);
    Check(held.predelay == 0.15f * 1000.0f && held.decay_time == 0.0f && held.out_gain == 1.0f &&
              held.early_mode == 2 && held.late_mode == 2 && held.early_gain == 0.0f,
          "values outside their ranges are held to them, and unknown modes become a hall and a plate");
}

void CheckEffects()
{
    // The bus's frames: noise in 4 channels of 160 samples. A reverb that starts two frames later on the same
    // parameters gives what the bus gives from then on.
    namespace nx = ntdlib::rvl::nx;
    Fixture fixture;
    nx::Effects effects;
    effects.AppendReverb(fixture.ax, fixture.layer, 0, nx::kBusBReverb);
    Check(fixture.ax.AuxReturnVolume(0) == ax::kMaxVolume && effects.Any(),
          "an appended reverb turns the bus's return to full volume");

    audio::AuxReverb later(32000, 4);
    const std::array<int8_t, 4> blocks{0, 1, 2, 3};
    later.SetInputOutput(blocks, blocks);
    later.SetParameters(nx::ReverbParameters(nx::kBusBReverb));
    uint32_t state = 12345;
    bool waits = true;
    bool same = true;
    bool rings = false;
    for (int frame = 0; frame < 40; frame++)
    {
        std::array<int32_t, Layer::kAuxSamples> samples{};
        for (int32_t& sample : samples)
        {
            state = state * 1664525u + 1013904223u;
            sample = frame < 20 ? static_cast<int32_t>(state >> 16) - 0x8000 : 0;
        }
        std::array<int32_t, Layer::kAuxSamples> expected = samples;
        effects.AuxCallback(0, samples);
        if (frame < 2)
        {
            for (const int32_t sample : samples)
            {
                waits &= sample == 0;
            }
            continue;
        }

        later.Process(expected.data(), expected.data(), Layer::kAuxSamples / 4);
        same &= samples == expected;
        for (const int32_t sample : samples)
        {
            rings |= frame >= 30 && sample != 0;
        }
    }
    Check(waits, "the bus is silent for two frames after its first reverb");
    Check(same, "then the bus runs the reverb on its frames");
    Check(rings, "the reverb rings on after its input stops");

    std::array<int32_t, Layer::kAuxSamples> other{};
    other.fill(1 << 16);
    effects.AuxCallback(1, other);
    Check(other[0] == 0 && other[Layer::kAuxSamples - 1] == 0, "AxManager's callback silences a bus without effects");
}

} // namespace

int main()
{
    const auto tests = []
    {
        CheckPriorities();
        CheckAcquisition();
        CheckPlay();
        CheckCurrentAddress();
        CheckSnapshot();
        CheckStream();
        CheckReverbParameters();
        CheckEffects();
    };

    return xsflib::testing::RunTest("rvl_nx", tests);
}
