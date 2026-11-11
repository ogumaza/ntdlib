// SPDX-License-Identifier: MIT

// The hold of a gain that NW4R SND's AX voices and voices both apply.

#pragma once

#include <algorithm>

#include "ntdlib/rvl/snd/util.h"

namespace ntdlib::rvl::snd
{

// Voice::SetVolume (0x8004c830), SetVeVolume and SetMainOutVolume hold a gain from the channel to 0 and 1. Builds from
// September 2009 on hold it only to 0. The gain can then go above full level.
inline float HeldGain(float gain, const SndBuild& build)
{
    return build.SinceSeptember2009() ? std::max(gain, 0.0f) : std::clamp(gain, 0.0f, 1.0f);
}

} // namespace ntdlib::rvl::snd
