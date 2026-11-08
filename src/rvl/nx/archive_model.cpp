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

} // namespace

ArchiveModel::ArchiveModel() = default;

void ArchiveModel::Start(const ntdlib::rsar::SoundArchive& archive)
{
    engine.emplace(memory, ax, archive, build);
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

    for (std::size_t i = 0; i < kOutputFrameSamples; i++)
    {
        output.push_back(samples[i * kSinkChannels]);
        output.push_back(samples[i * kSinkChannels + 1]);
    }
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
