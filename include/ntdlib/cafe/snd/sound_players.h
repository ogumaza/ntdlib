// SPDX-License-Identifier: MIT

// nw::snd's players of wave sounds and of stream sounds in NintendoWare for Cafe (driver::WaveSoundPlayer and
// driver::StreamSoundPlayer): ntdlib's model of them (see nwsnd/wave_sound_player.h and nwsnd/stream_sound_player.h),
// with the Wii U's policy (see sequence.h).

#pragma once

#include <cstdint>
#include <string_view>

#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/wave_sound_player.h"

namespace ntdlib::cafe::snd
{

using WaveSoundPlayer = ntdlib::nwsnd::WaveSoundPlayer<SequencePolicy>;
using StreamSoundPlayer = ntdlib::nwsnd::StreamSoundPlayer<SequencePolicy>;

// The tracks of stream sound `sound` that a known game plays when the sound starts, bit N for track N. The list finds
// the sound by its name. The game's code sets the other tracks to a volume of 0 through the sound's handle.
// `archive_sha1` is the SHA-1 of the sound archive that holds the sound, in 40 lowercase hex digits. The lookup gives
// every track for a sound that the list leaves out, and for every sound of an unknown archive. Engine::StartStreamSound
// takes the mask.
uint16_t StartingStreamTracks(std::string_view archive_sha1, std::string_view sound);

} // namespace ntdlib::cafe::snd
