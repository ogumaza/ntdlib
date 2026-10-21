// SPDX-License-Identifier: MIT

#include "ntdlib/nx/atk/sound_players.h"

#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nwsnd/stream_sound_player_impl.h"
#include "ntdlib/nwsnd/wave_sound_player.h"
#include "ntdlib/nwsnd/wave_sound_player_impl.h"
#include "ntdlib/nx/atk/engine.h"

template class ntdlib::nwsnd::WaveSoundPlayer<ntdlib::nx::atk::SequencePolicy>;
template class ntdlib::nwsnd::StreamSoundPlayer<ntdlib::nx::atk::SequencePolicy>;
