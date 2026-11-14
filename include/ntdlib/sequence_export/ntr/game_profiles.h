// SPDX-License-Identifier: MIT

// The profiles of known DS games (ntdlib/ntr/game_profiles.h) as the options and arrangements of a converter of the
// DS's sound archives (archive_input.h). A converter that plays each sequence as the game's code starts it calls
// UseGameProfiles.
//
// The functions leave out the SDK's output effect (GameSettings::effect) and keep the plain stereo. A SoundFont or an
// SFZ file can't contain the surround.

#pragma once

#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/sequence_export/ntr/archive_input.h"
#include "ntdlib/sequence_export/ntr/performance.h"

namespace ntdlib::sequence_export::ntr
{

// Copies what `profile` sets before the sequence starts into `perform`: the channels of the archive's players and of
// some tracks, the reverb, and the variables. The profile's commands follow those that `perform` contains. A variable
// that `perform` sets already keeps its value: the command line's --var, then an arrangement's.
void ApplyProfile(const ntdlib::ntr::GameProfile& profile, PerformOptions& perform);

// The other states of the sequence in `profile`, as arrangements in the same order.
std::vector<Arrangement> ArrangementsOf(const ntdlib::ntr::GameProfile& profile);

// The profile of `sequence` of `input` (ntdlib::ntr::GameProfileOf). It looks the profile up by four keys: the game
// code from the header of the image, the path of the file in the image that contains the archive, the place of the
// archive among the archives in that file, and the label of the sequence.
ntdlib::ntr::GameProfile ProfileOf(const ArchiveInput& input, const ntdlib::ntr::SequenceRef& sequence);

// Sets ArchiveConverter::game_settings to ApplyProfile and ArchiveConverter::arrangements to ArrangementsOf, each with
// the profile of the sequence (ProfileOf).
void UseGameProfiles(ArchiveConverter& converter);

} // namespace ntdlib::sequence_export::ntr
