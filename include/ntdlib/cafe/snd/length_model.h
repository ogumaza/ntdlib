// SPDX-License-Identifier: MIT

// The Wii U's part of the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h): the model is an
// ArchiveModel, and its frames last 3 ms. The output is AX's output in the frame. The engine follows whether the
// effects still ring (Engine::EffectsRinging).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/rip/length.h"

#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nwsnd/length_model.h"

namespace ntdlib::cafe::snd
{

struct LengthConsole
{
    using Model = ArchiveModel;

    static constexpr double kFrameMs = 1000.0 * Model::kFrameSamples / Model::kSampleRate;

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.system.Output();
    }

    static bool Ringing(const Model& model)
    {
        return model.engine->EffectsRinging();
    }

    static std::size_t VoiceCount(const Model& model)
    {
        return model.system.Voices().size();
    }

    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        const ax::Pb& pb = model.system.Voices()[index].renderer;
        return {pb.state == ax::VoiceState::kRun, pb.loop_flag != 0, static_cast<int64_t>(pb.current_address)};
    }
};

} // namespace ntdlib::cafe::snd
