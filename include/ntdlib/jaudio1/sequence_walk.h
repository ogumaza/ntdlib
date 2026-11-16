// SPDX-License-Identifier: MIT

// The static walk of a JAudio1 sequence (jaudio/sequence_walk.h's paths): the banks and programs that its notes can
// start with, as the model of this module reads its commands.

#pragma once

#include <cstdint>
#include <span>

#include "ntdlib/jaudio/sequence_walk.h"

namespace ntdlib::jaudio1
{

// Walks a sequence of JAudio1, as the model of this module reads it (Tracks::ParseSeq), from `start` in `data`.
ntdlib::jaudio::SequenceInstruments WalkSequence(std::span<const uint8_t> data, uint32_t start = 0);

} // namespace ntdlib::jaudio1
