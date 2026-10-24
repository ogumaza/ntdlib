// SPDX-License-Identifier: MIT

// The 3DS's part of the length analysis of a sequence sound (ntdlib/nwsnd/length_model.h): the model is an
// ArchiveModel on the model of the DSP's audio firmware, and its frames last 160 samples at the DSP's rate, about
// 4.9 ms. The output is the frame that the DSP mixed last (dsp::Device::MixedFrame), a frame behind the engine. Archive
// mode runs no effects on the aux buses, and nothing rings.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/rip/length.h"

#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/nnsnd/sound_system.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/nwsnd/length_model.h"

namespace ntdlib::ctr::snd
{

struct LengthConsole
{
    using Model = ArchiveModel;

    static constexpr double kFrameMs = 1000.0 * 160 / dsp::kSampleRate;

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.dsp->MixedFrame();
    }

    static bool Ringing(const Model& /*model*/)
    {
        return false;
    }

    static std::size_t VoiceCount(const Model& model)
    {
        return static_cast<std::size_t>(model.snd.VoiceCount());
    }

    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        const nnsnd::SoundSystem::VoicePlace place = model.snd.PlaceOf(static_cast<int>(index));
        return {place.playing, place.loops, static_cast<int64_t>(place.position)};
    }
};

} // namespace ntdlib::ctr::snd
