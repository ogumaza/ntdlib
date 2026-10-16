// SPDX-License-Identifier: MIT

// The places where Cemu's AX does otherwise than the console's. Each switch makes the model of AX do what Cemu does
// there. The model's output can then be compared with Cemu's sample by sample. All of them are off unless the caller
// turns them on.

#pragma once

namespace ntdlib::cafe::ax
{

struct CemuDepartures
{
    // Every switch on.
    static CemuDepartures All()
    {
        return {true, true, true, true, true};
    }

    // A voice keeps the ADPCM history of its last use: the sync copies the game's ADPCM block to the renderer without
    // its yn1 and yn2. The console copies them.
    bool stale_history = false;

    // An ADPCM loop goes on with the history that the loop's end left. The console loads the loop's history.
    bool loop_keeps_history = false;

    // The three 4-tap types interpolate linearly.
    bool linear_four_tap = false;

    // Linear interpolation of ADPCM and PCM8 goes from the second newest sample to the newest. The microcode goes from
    // the oldest of the four last samples to the second oldest for every format. Cemu does too for PCM16.
    bool newest_samples = false;

    // Every device converts a 32 kHz renderer's output to 48 kHz by linear interpolation. The console uses its FIR
    // filter unless the game selects linear interpolation.
    bool linear_upsampler = false;
};

} // namespace ntdlib::cafe::ax
