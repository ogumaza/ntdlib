// SPDX-License-Identifier: MIT

// The Wii's part of the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h). The model is archive mode's
// ArchiveModel of NW4R SND and AX, and its frames last 3 ms: 96 stereo samples at 32 kHz. Each frame mixes AX's frame,
// on the DSP or with ax::Renderer, and then runs the sound frame. A frame's commands therefore sound from the next
// frame on, as the analysis's rules take them (nwsnd::LengthRulesFor). An open of a track keeps the track's wait
// (SeqTrack::Open, 0x8003ffcc). A measure that reaches the limit inside counted loops runs again with each track's
// outermost counted loop endless. With an effect on an aux bus, the effects ring until a second has passed without a
// sound louder than ±8, as on the Wii U (cafe::snd::Engine::EffectsRinging). ArrangedLengthConsole also measures a
// model started without the DSP, and a sound of which only some tracks play.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/rip/length.h"

#include "ntdlib/first_sample.h"
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

// Archive mode's model for a length analysis that may run without the DSP and that may play only some tracks. A model
// started with ArchiveModel::StartWithoutDsp sets `without_dsp`. Its frames then mix nothing. A frame has sound if any
// channel plays, and the first frame in which a note plays counts from its start. `tracks` gives the tracks that play,
// a bit for each, as the start's Arrangement gives them. The other tracks play at a volume of 0 and don't hold a loop's
// fade back.
struct ArrangedLengthModel : LengthArchiveModel
{
    bool without_dsp = false; // whether the model was started without the DSP
    uint32_t tracks = ~0u;    // the tracks that play, a bit for each
};

// The Wii's part of the length analysis for ArrangedLengthModel. With the DSP, a frame runs, sounds and gives its first
// sample as in LengthConsole.
struct ArrangedLengthConsole : LengthConsole
{
    using Model = ArrangedLengthModel;

    // Returns false if the microcode stopped responding.
    static bool RunFrame(Model& model)
    {
        if (!model.without_dsp)
        {
            return LengthConsole::RunFrame(model);
        }

        model.RunFrameWithoutDsp();

        return true;
    }

    static bool Sounded(const Model& model)
    {
        return model.without_dsp ? model.engine->Channels().ActiveCount() > 0 : xsflib::rip::Audible(model.output);
    }

    // A pair of samples lasts 1/32 ms at the output's 32 kHz.
    static std::optional<double> FirstSampleMs(const Model& model)
    {
        if (model.without_dsp)
        {
            return Sounded(model) ? std::optional<double>(0.0) : std::nullopt;
        }

        return ntdlib::FirstSampleMs(model.output, 1000.0 / ax::kSampleRate);
    }

    static uint32_t Tracks(const Model& model)
    {
        return model.tracks;
    }
};

} // namespace ntdlib::rvl::snd
