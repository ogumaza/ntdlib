// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/nx/archive_model.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ntdlib::rvl::nx
{

namespace
{

// The device sink's channels, and the AX timer's and the render thread's periods in milliseconds.
constexpr std::size_t kSinkChannels = 6;
constexpr uint64_t kTickPeriod = 3;
constexpr uint64_t kFramePeriod = 5;

// The pools that the game's start-up of NW4R SND sets up (0x1026d78). The Wii's pools are larger.
// - 64 AxVoices of 0x68 bytes: the count 0x40 and 0x1a00 bytes of work memory at 0x10270cc to 0x1027158, with the free
//   list at 0x2c861a0.
// - 64 voices of 0x140 bytes: the count 0x40 and 0x5000 bytes of work memory at 0x1027178 to 0x1027250, with the free
//   list at 0x2c8a6f0.
// - 65 channels of 0x100 bytes: 0x4100 bytes of work memory at 0x1027280 to 0x10272f0.
// With 64 voices, NW4R SND runs out of voices as the layer runs out of records. VoiceManager::AllocVoice then drops the
// voice of the lowest priority, before the layer would take the record with the oldest stamp (0x1027e1c).
//
// AllocVoice (0x103348c, with Voice::Acquire inlined) also drops a voice when AcquireAxVoice (0x1027cf4) finds no free
// AxVoice (0x10335b8 to 0x10336d8). It drops the first voice of the priority list (0x2c8a6e0) if that voice's priority
// is at or under the new voice's, and asks again at the same AX priority (0x10336e8 to 0x10336f8). A note on a wave
// with two channels takes two AxVoices, and the 64 AxVoices can run out while voices are free.
constexpr snd::EngineOptions kPools{.ax_voices = 64, .voices = 64, .channels = 65, .drop_voice_for_ax_voice = true};

} // namespace

ArchiveModel::ArchiveModel() = default;

void ArchiveModel::Start(const ntdlib::rsar::SoundArchive& archive)
{
    engine.emplace(memory, ax, archive, build, kPools);
    engine->SetOutputMode(ntdlib::nwsnd::OutputMode::kDpl2);
    effects.AppendReverb(ax, layer, 1, kBusBReverb);
    effects.AppendReverb(ax, layer, 0, kBusAReverb);
}

void ArchiveModel::RunFrame(std::vector<int16_t>& output)
{
    // The AX ticks at or before the frame's time: AX's frame callback, then the sound frame (0x100e62c).
    while (ticks * kTickPeriod <= frames * kFramePeriod)
    {
        ax.RunFrameCallback();
        engine->FrameProcess();
        ticks++;
    }

    layer.RenderFrame();
    frames++;
    const std::span<const int16_t> samples = renderer.DeviceOutput();
    if (samples.size() != kOutputFrameSamples * kSinkChannels)
    {
        throw std::logic_error("the layer's renderer gave a frame of the wrong size");
    }

    audio::DownmixFrame(samples, downmix, output);
}

ArchiveModel::Snapshot ArchiveModel::Save(const Snapshot* previous) const
{
    return {memory.Save(previous != nullptr ? &previous->memory : nullptr),
            renderer.Save(),
            ax,
            layer.Save(),
            effects,
            engine->Save(),
            ticks,
            frames};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    engine->Restore(snapshot.engine);
    memory.Restore(snapshot.memory);
    renderer.Restore(snapshot.renderer);
    ax = snapshot.ax;
    layer.Restore(snapshot.layer);
    effects = snapshot.effects;
    ticks = snapshot.ticks;
    frames = snapshot.frames;
}

} // namespace ntdlib::rvl::nx
