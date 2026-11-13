// SPDX-License-Identifier: MIT

// Archive mode's stack for Skyward Sword HD: main memory, AX's voices on the game's layer (Layer), the Switch's
// renderer, NW4R SND's engine, the reverbs on the aux buses (Effects) and the system downmix of the renderer's 6
// channels to stereo (downmix.h). The game's AX timer runs every 3 ms (0x100e62c): AX's frame callback, then NW4R SND's
// sound frame. The render thread runs a frame of the renderer every 5 ms. The model puts AX's tick k at 3k ms and the
// renderer's frame j at 5j ms. The ticks at or before a frame run before it. That order is the model's choice.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/nx/audio/downmix.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/nx/effects.h"
#include "ntdlib/rvl/nx/layer.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rvl::nx
{

struct ArchiveModel
{
    // The renderer's output rate, and its samples for each frame of each channel.
    static constexpr int32_t kOutputRate = 48000;
    static constexpr int kOutputFrameSamples = 240;

    // A snapshot of the model between frames. It can only be restored to this model.
    struct Snapshot
    {
        ntdlib::dsp::WiiMemory::Snapshot memory;
        audio::Renderer::Snapshot renderer;
        ax::System ax;
        Layer::Snapshot layer;
        Effects effects;
        snd::Engine::Snapshot engine;
        uint64_t ticks = 0;
        uint64_t frames = 0;
    };

    ArchiveModel();
    ArchiveModel(const ArchiveModel&) = delete;
    ArchiveModel& operator=(const ArchiveModel&) = delete;

    // Starts the engine on `archive`, with the pools of voices and channels that the game sets up, in the output mode
    // that the layer sets, Dolby Pro Logic II (0x1026ec0). Then puts the game's reverbs on the aux buses: bus B's, as
    // the game's start-up does (0xf32878), and bus A's for a scene that gives no type of room (0xf761c0). The archive
    // must outlive the model. Set `build` first.
    void Start(const ntdlib::rsar::SoundArchive& archive);

    // One frame of the renderer, after the AX ticks due by then. Appends the frame's 240 stereo samples at 48 kHz to
    // `output`, left then right: the device sink's 6 channels through `downmix`.
    void RunFrame(std::vector<int16_t>& output);

    Snapshot Save(const Snapshot* previous = nullptr) const;
    void Restore(const Snapshot& snapshot);

    ntdlib::dsp::WiiMemory memory;
    ax::System ax{memory};
    audio::Renderer renderer{Layer::Parameter()};
    Layer layer{ax, memory, renderer};
    Effects effects;
    std::optional<snd::Engine> engine;
    SndBuild build{.date = 20090924};                 // the build of NW4R SND that the engine follows, set before Start
    audio::Downmix downmix = audio::Downmix::kStereo; // the system's downmix for a stereo output
    uint64_t ticks = 0;                               // the AX ticks run so far
    uint64_t frames = 0;                              // the renderer's frames run so far
};

} // namespace ntdlib::rvl::nx
