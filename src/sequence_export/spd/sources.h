// SPDX-License-Identifier: MIT

// The sound data in the game's memory, and the samples that the hardware channels play from it, for the performer and
// the SoundFont and SFZ builders of Nintendo SPD's sounds.

#pragma once

#include <cstdint>
#include <span>

#include "seqlib/pcm.h"

#include "ntdlib/sequence_export/spd/convert.h"
#include "ntdlib/spd/player.h"
#include "ntdlib/spd/song.h"

namespace ntdlib::sequence_export::spd
{

// The driver's timer of a square wave or noise at key 64 (0x037FA1F8).
constexpr int kToneKey = 64;
constexpr int kToneTimer = 0x18DA;

// The driver gives the hardware a sample's header's address plus 10 as the data's start (ntdlib/spd/song.h).
constexpr uint32_t kHeaderToData = 10;

// The sound data at the game's layout.
struct Memory
{
    explicit Memory(const SoundData& data);

    uint8_t Read8(uint32_t address) const;
    uint16_t Read16(uint32_t address) const;

    SoundData data;
    ntdlib::spd::Layout layout;
};

// A source's samples, the rate at which they play at `root`, and the key `root`.
struct SourceWave
{
    seqlib::Pcm pcm;
    uint32_t rate = 0;
    int root = 60;
};

// The samples that a hardware channel plays of `source`: a sample's data from the word that the hardware starts at to
// the loop's end, decoded as the hardware decodes it, or the sound unit's square wave or noise.
SourceWave WaveOf(const Memory& memory, const Source& source);

} // namespace ntdlib::sequence_export::spd
