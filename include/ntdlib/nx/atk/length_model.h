// SPDX-License-Identifier: MIT

// The Switch's part of the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h): the model is an
// ArchiveModel, and its frames last 5 ms. The output is what the renderer rendered in the frame. The effects ring until
// the output is silence.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/rip/length.h"

#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/low_level_voice.h"

namespace ntdlib::nx::atk
{

struct LengthConsole
{
    using Model = ArchiveModel;

    static constexpr double kFrameMs = 1000.0 * Model::kFrameSamples / Model::kSampleRate;

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.renderer.DeviceOutput();
    }

    static bool Ringing(const Model& model)
    {
        const std::span<const int16_t> output = Output(model);
        return std::any_of(output.begin(), output.end(), [](int16_t sample) { return sample != 0; });
    }

    static std::size_t VoiceCount(const Model& model)
    {
        return model.engine->LowLevelVoices().All().size();
    }

    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        const LowLevelVoice& voice = model.engine->LowLevelVoices().All()[index];
        return {voice.IsPlaying(), voice.Loops(), voice.PlayPosition()};
    }
};

} // namespace ntdlib::nx::atk
