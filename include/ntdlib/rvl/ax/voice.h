// SPDX-License-Identifier: MIT

// An AX voice (AXVPB): the CPU's copy of a voice's parameter block, plus a record of which parts changed. AX copies the
// changes into the PB in main memory once per frame (see System::SyncPbs).

#pragma once

#include <cstdint>
#include <functional>

#include "ntdlib/ax/lpf.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/types.h"

namespace ntdlib::rvl::ax
{

struct Voice
{
    // Called when AX takes the voice away. AX does this in two cases: to free the voice for one of higher priority
    // (AXAcquireVoice), and when the frame would require more DSP cycles than the microcode is allowed
    // (System::SyncPbs).
    using Callback = std::function<void(Voice* voice)>;

    // AXSetVoiceState (main.dol 0x80107374). Stopping a running voice fades out the last samples it added to the mix.
    void SetState(VoiceState state);

    // AXSetVoiceAddr (0x801073e4). For PCM it also sets the ADPCM block to the identity: no coefficients and a gain of
    // 0x800 (PCM16) or 0x100 (PCM8).
    void SetAddr(const PbAddr& address);

    bool IsRunning() const
    {
        return pb.state == static_cast<uint16_t>(VoiceState::kRun);
    }

    // The priority stacks' links (AXVPB + 0x0, + 0x4) and the voice's priority (+ 0xc) are in System's
    // ntdlib::ax::Voices. The stack of voices with callbacks due uses `next1` (+ 0x8).
    Voice* next1 = nullptr;

    Callback callback;            // + 0x10
    uint32_t index = 0;           // + 0x18
    uint32_t sync = 0;            // + 0x1c
    bool depop = false;           // + 0x20
    ntdlib::PAddr itd_buffer = 0; // + 0x24
    Pb pb;                        // + 0x28
};

using ntdlib::ax::LpfCoefs;

// The coefficients a0 and b0 of PbLpf for a cutoff of `freq` Hz (AXGetLpfCoefs, 0x801074dc). The game computes cos and
// sqrt in double precision (MSL's cos and sqrt) and rounds each result to single precision.
inline LpfCoefs CalcLpfCoefs(uint16_t freq)
{
    return ntdlib::ax::ComputeLpfCoefs(freq, {.rate = static_cast<float>(kSampleRate), .double_precision = true});
}

} // namespace ntdlib::rvl::ax
