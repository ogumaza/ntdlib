// SPDX-License-Identifier: MIT

// The modes of a track's mute in NW4R SND, nw::snd and nn::snd (SeqTrack::SetMute). The sequence's mute command (0xDD)
// and the sound's handle (SequenceSoundHandle::SetTrackMute) take them.

#pragma once

namespace ntdlib::nwsnd
{

// SEQ_MUTE_OFF plays the track again. SEQ_MUTE_NO_STOP skips its notes from the next one on, and its channels sound
// on. SEQ_MUTE_RELEASE also releases its channels, and SEQ_MUTE_STOP also stops them at once. NITRO-Composer's driver
// has the same four modes (ntr/snd/driver.h).
inline constexpr int kMuteOff = 0;
inline constexpr int kMuteNoStop = 1;
inline constexpr int kMuteRelease = 2;
inline constexpr int kMuteStop = 3;

} // namespace ntdlib::nwsnd
