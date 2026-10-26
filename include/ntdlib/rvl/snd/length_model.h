// SPDX-License-Identifier: MIT

// The Wii's part of the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h). The model is archive mode's
// ArchiveModel of NW4R SND and AX, and its frames last 3 ms: 96 stereo samples at 32 kHz. Each frame mixes AX's frame,
// on the DSP or with ax::Renderer, and then runs the sound frame. A frame's commands therefore sound from the next
// frame on, as the analysis's rules take them (nwsnd::LengthRulesFor). An open of a track keeps the track's wait
// (SeqTrack::Open, 0x8003ffcc). A measure that reaches the limit inside counted loops runs again with each track's
// outermost counted loop endless. With an effect on an aux bus, the effects ring until a second has passed without a
// sound louder than ±8, as on the Wii U (cafe::snd::Engine::EffectsRinging).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/rip/length.h"

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"

namespace ntdlib::rvl::snd
{

// Archive mode's model for the length analysis, with the output of the frame that it ran last. A caller that starts
// the model sets up the effects on the engine (Engine::SetEffects) before the sound starts. Where `aux_returns` holds
// the return volumes of the aux buses, they go to AX before each frame (ax::System::SetAuxReturnVolume). NW4R SND sets
// a bus's return to full volume as it appends the bus's first effect (AxManager::AppendEffect), and a game sets its
// volumes after that.
struct LengthArchiveModel : ArchiveModel
{
    std::vector<int16_t> output;                                       // the last frame's output, left then right
    int ringing_frames = 0;                                            // the frames until the effects stop ringing
    std::optional<std::array<uint16_t, ax::kAuxBusCount>> aux_returns; // the buses' return volumes, in 1.15
};

struct LengthConsole
{
    using Model = LengthArchiveModel;

    static constexpr double kFrameMs = 1000.0 * ax::kSamplesPerFrame / ax::kSampleRate;

    static constexpr bool kOpenKeepsWait = true;

    // Returns false if the microcode stopped responding.
    static bool RunFrame(Model& model)
    {
        if (model.aux_returns)
        {
            for (std::size_t bus = 0; bus < model.aux_returns->size(); ++bus)
            {
                model.ax.SetAuxReturnVolume(static_cast<int>(bus), (*model.aux_returns)[bus]);
            }
        }
        model.output.clear();
        if (!model.RunFrame(model.output))
        {
            return false;
        }

        model.ringing_frames =
            xsflib::rip::Audible(model.output) ? kRingingFrames : std::max(model.ringing_frames - 1, 0);

        return true;
    }

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.output;
    }

    static bool Ringing(const Model& model)
    {
        return model.engine->HasEffects() && model.ringing_frames > 0;
    }

    static std::size_t VoiceCount(const Model& model)
    {
        return model.ax.VoiceCount();
    }

    // The voice's PB in main memory, where the mixer leaves its place after each frame.
    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        const ax::Pb pb = model.ax.ReadPb(static_cast<uint32_t>(index));
        return {pb.state == static_cast<uint16_t>(ax::VoiceState::kRun), pb.addr.loop_flag != 0,
                static_cast<int64_t>(ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo))};
    }

private:
    static constexpr int kRingingFrames = 334; // a second of frames
};

} // namespace ntdlib::rvl::snd
