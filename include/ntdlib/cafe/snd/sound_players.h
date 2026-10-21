// SPDX-License-Identifier: MIT

// nw::snd's players of wave sounds and of stream sounds in NintendoWare for Cafe (driver::WaveSoundPlayer and
// driver::StreamSoundPlayer): ntdlib's model of them (see nwsnd/wave_sound_player.h and nwsnd/stream_sound_player.h),
// with the Wii U's policy (see sequence.h).

#pragma once

#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/wave_sound_player.h"

namespace ntdlib::cafe::snd
{

using WaveSoundPlayer = ntdlib::nwsnd::WaveSoundPlayer<SequencePolicy>;
using StreamSoundPlayer = ntdlib::nwsnd::StreamSoundPlayer<SequencePolicy>;

} // namespace ntdlib::cafe::snd
