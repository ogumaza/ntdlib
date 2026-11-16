// SPDX-License-Identifier: MIT

// A search of a DS game's ARM9 binary for the routines of the SDK's sound library (NITRO-System's sound archive player)
// that a program calls to play one sequence on the game's code: the update that the game calls once a frame, the
// start of a sequence, the load of its data, the wait for the vertical blank, and the start-up decompressor (code.h).
// The decompressor's body is free once the binary is stored decompressed, and only if no overlay that the game loads
// is compressed: the SDK's overlay loader decompresses an overlay with the same routine.
//
// The rules work on the routines' steps (ntdlib/arm/routine.h), so they hold for the compilers' variants of the same
// code. They read ARM code only: a build in Thumb code gives no match. Each list holds every routine that matches. A
// caller takes a routine only where its list holds exactly one, and confirms the result by a run.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/ntr/code.h"

namespace ntdlib::ntr
{

// The update, NNS_SndMain. It calls `receive` with 0 until that returns 0 (SND_RecvCommandReply), then the routines
// of `calls` in order, then `flush` with 0 (SND_FlushCommand), and returns. A routine in Thumb code has bit 0 set.
struct SoundUpdate
{
    friend bool operator==(const SoundUpdate&, const SoundUpdate&) = default;

    uint32_t address = 0;
    uint32_t receive = 0;
    std::vector<uint32_t> calls; // two to four
    uint32_t flush = 0;
};

// The start of a sequence, NNS_SndArcPlayerStartSeq(handle, sequence). It calls `get_info` with the sequence's number
// (NNS_SndArcGetSeqInfo), reads the bank (a half at 4), the priority (a byte at 8) and the player (a byte at 9) of the
// record that it returns, and calls `start` with the handle.
struct SequenceStart
{
    friend bool operator==(const SequenceStart&, const SequenceStart&) = default;

    uint32_t address = 0;
    uint32_t get_info = 0;
    uint32_t start = 0;
};

// The load of a sequence's data. `load` takes the sequence's number, the kinds of data to load (0xFF for every kind),
// the heap, 1 and a null pointer. Among its first three calls, it calls a SequenceStart's get_info with the sequence.
// A later call passes its second to fourth arguments on: the load of the bank. `wrapper` is NNS_SndArcLoadSeq(sequence,
// heap), which calls `load` with 0xFF. A game that links only NNS_SndArcLoadSeqEx or NNS_SndArcLoadGroup has no
// wrapper: then each routine of that shape is a load.
struct SequenceLoad
{
    friend bool operator==(const SequenceLoad&, const SequenceLoad&) = default;

    std::optional<uint32_t> wrapper;
    uint32_t load = 0;
};

struct SoundRoutines
{
    std::vector<SoundUpdate> updates;
    std::vector<SequenceStart> starts;
    std::vector<SequenceLoad> loads;

    // The wait for the vertical blank, OS_WaitVBlankIntr: a routine of two calls and nothing else, the first with 1
    // and the second with 1 and 1, within ten instructions.
    std::vector<uint32_t> waits;

    std::optional<StartupDecompressor> decompressor;
};

// The routines of `arm9`, the ARM9 binary decompressed at its address (Arm9Code), whose entry code starts at `entry`.
SoundRoutines FindSoundRoutines(const LoadedCode& arm9, uint32_t entry);

} // namespace ntdlib::ntr
