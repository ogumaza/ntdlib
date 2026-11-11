// SPDX-License-Identifier: MIT

// The definitions of sequence.h's templates. The code that uses the model includes this file in one source file, with
// the policy's types complete. That file builds the templates for the policy:
//
//     template class ntdlib::nwsnd::SequenceTrack<Policy>;
//     template class ntdlib::nwsnd::SequenceSoundPlayer<Policy>;

#pragma once

#include "ntdlib/nwsnd/sequence_commands_impl.h"
#include "ntdlib/nwsnd/sequence_player_impl.h"
#include "ntdlib/nwsnd/sequence_track_impl.h"
