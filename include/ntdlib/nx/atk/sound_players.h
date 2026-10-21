// SPDX-License-Identifier: MIT

// nn::atk's players of wave sounds and of stream sounds (driver::WaveSoundPlayer and driver::StreamSoundPlayer):
// ntdlib's model of them (see nwsnd/wave_sound_player.h and nwsnd/stream_sound_player.h), with the Switch's policy (see
// sequence.h).

#pragma once

#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/wave_sound_player.h"
#include "ntdlib/nx/atk/sequence.h"

namespace ntdlib::nx::atk
{

using WaveSoundPlayer = ntdlib::nwsnd::WaveSoundPlayer<SequencePolicy>;
using StreamSoundPlayer = ntdlib::nwsnd::StreamSoundPlayer<SequencePolicy>;

} // namespace ntdlib::nx::atk
