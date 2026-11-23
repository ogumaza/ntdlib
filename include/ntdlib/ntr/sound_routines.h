// SPDX-License-Identifier: MIT

// A search of a DS game's ARM9 binary for the routines of the SDK's sound library (NITRO-System's sound archive player)
// that a program calls to play one sequence on the game's code: the update that the game calls once a frame, the
// start of a sequence, the load of its data, the wait for the vertical blank, the routine that queues one command of
// the driver, and the start-up decompressor (code.h). The decompressor's body is free once the binary is stored
// decompressed, and only if no overlay that the game loads is compressed: the SDK's overlay loader decompresses an
// overlay with the same routine.
//
// The rules work on the routines' steps (ntdlib/arm/routine.h). They therefore hold for the compilers' variants of the
// same code, in ARM and in Thumb code. A routine in Thumb code has bit 0 set, and so does a call's target in Thumb
// code. Pokémon Black 2 (USA) builds the SDK's sound library in Thumb code. Each list holds every routine that matches.
// A caller takes a routine only where its list holds exactly one, and confirms the result by a run.

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
//
// With `ex`, the start is NNS_SndArcPlayerStartSeqEx(handle, player, bank, priority, sequence), with the sequence on
// the stack. It calls `get_info` with the sequence, and reads each of the player, the bank and the priority from the
// record where its argument is negative. Called with -1, -1 and -1, it is the plain start. A game whose linker left the
// plain start out has only this one: Pokémon HeartGold (USA).
struct SequenceStart
{
    friend bool operator==(const SequenceStart&, const SequenceStart&) = default;

    uint32_t address = 0;
    uint32_t get_info = 0;
    uint32_t start = 0;
    bool ex = false;
};

// The load of a sequence's data. `load` takes the sequence's number, the kinds of data to load (0xFF for every kind),
// the heap, 1 and a null pointer. Among its first three calls, it calls a SequenceStart's get_info with the sequence. A
// later call passes its second to fourth arguments on: the load of the bank. `wrapper` is NNS_SndArcLoadSeq(sequence,
// heap). That routine calls `load` with 0xFF. A game that links only NNS_SndArcLoadSeqEx or NNS_SndArcLoadGroup has no
// wrapper: then each routine of that shape is a load.
struct SequenceLoad
{
    friend bool operator==(const SequenceLoad&, const SequenceLoad&) = default;

    std::optional<uint32_t> wrapper;
    uint32_t load = 0;
};

// The wait for a frame, and the constants that a caller passes it.
struct SoundWait
{
    friend bool operator==(const SoundWait&, const SoundWait&) = default;

    uint32_t address = 0;
    std::vector<uint32_t> arguments;
};

struct SoundRoutines
{
    std::vector<SoundUpdate> updates;

    // The plain starts. Where none matches, the starts of the `ex` form.
    std::vector<SequenceStart> starts;

    std::vector<SequenceLoad> loads;

    // The wait for the vertical blank, OS_WaitVBlankIntr: a routine of two calls and nothing else, the first with 1
    // and the second with 1 and 1, within ten instructions. It takes no arguments. Where none matches, the waits are
    // OS_WaitIrq, called with 1 and 1: a routine that compares its first argument with 0, then loads the word at
    // DTCM + 0x3FF8 (the interrupt check word, at an address that ends in 0x3FF8 after a multiple of 0x4000) and
    // stores it back, the bits of its second argument cleared, then loads it again and calls a routine (the sleep of
    // OS_SleepThread). Mario Kart DS (USA) calls OS_WaitIrq(1, 1) itself and links no OS_WaitVBlankIntr.
    std::vector<SoundWait> waits;

    // The routine that queues one command of the driver, (id, argument 0 to 3), with the fourth argument on the stack:
    // it calls a routine with 1 (the allocation of a command), stores its five arguments at 4, 8, 12, 16 and 20 of the
    // command that the call returns, then calls a routine with the command (the queue).
    std::vector<uint32_t> commands;

    std::optional<StartupDecompressor> decompressor;
};

// The routines of `arm9`, the ARM9 binary decompressed at its address (Arm9Code), whose entry code starts at `entry`.
SoundRoutines FindSoundRoutines(const LoadedCode& arm9, uint32_t entry);

} // namespace ntdlib::ntr
