// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/sound_players.h"

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/stream_sound_player_impl.h"
#include "ntdlib/nwsnd/wave_sound_player.h"
#include "ntdlib/nwsnd/wave_sound_player_impl.h"

template class ntdlib::nwsnd::WaveSoundPlayer<ntdlib::cafe::snd::SequencePolicy>;
template class ntdlib::nwsnd::StreamSoundPlayer<ntdlib::cafe::snd::SequencePolicy>;
