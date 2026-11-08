// SPDX-License-Identifier: MIT

// Skyward Sword HD's part of the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h). The model is
// rvl::nx::ArchiveModel: NW4R SND and AX on the layer in the game's code, mixed by the Switch renderer. The model's
// frames are the renderer's frames of 5 ms: 240 stereo samples at 48 kHz. The AX ticks due by then run before each
// frame. A frame's commands therefore sound in the same frame. As for nn::atk, the analysis's rules still count them
// from the next frame on. The difference is one frame of 5 ms. As on the Wii, an open of a track keeps the track's
// wait (SeqTrack::Open). As on the Wii (rvl::snd::LengthConsole), the reverbs on the aux buses ring until a second
// has passed without a sound louder than ±8.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/rip/length.h"

#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/nx/archive_model.h"

namespace ntdlib::rvl::nx
{

// The model for the length analysis, with the output of the frame that it ran last.
struct LengthArchiveModel : ArchiveModel
{
    std::vector<int16_t> output; // the last frame's output, left then right
    int ringing_frames = 0;      // the frames until the reverbs stop ringing
};

struct LengthConsole
{
    using Model = LengthArchiveModel;

    static constexpr double kFrameMs = 5.0;

    static constexpr bool kOpenKeepsWait = true;

    // The model throws for what it doesn't model. The analysis then gives no timing (xsflib::rip::TryMeasure).
    static bool RunFrame(Model& model)
    {
        model.output.clear();
        model.RunFrame(model.output);
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
        return model.effects.Any() && model.ringing_frames > 0;
    }

    static std::size_t VoiceCount(const Model&)
    {
        return Layer::kVoiceCount;
    }

    // The voice's PB, where the layer leaves the record's state and the wave's current address after each frame.
    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        const ax::Pb& pb = model.ax.VoiceAt(static_cast<uint32_t>(index)).pb;
        constexpr uint16_t kPlaying = 1;
        return {pb.state == kPlaying, pb.addr.loop_flag != 0,
                static_cast<int64_t>(ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo))};
    }

private:
    static constexpr int kRingingFrames = 200; // a second of frames
};

} // namespace ntdlib::rvl::nx
