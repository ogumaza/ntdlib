// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/sequence.h"

#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/rvl/snd/engine.h"

template class ntdlib::nwsnd::SequenceTrack<ntdlib::rvl::snd::SequencePolicy>;
template class ntdlib::nwsnd::SequenceSoundPlayer<ntdlib::rvl::snd::SequencePolicy>;
