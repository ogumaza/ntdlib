// SPDX-License-Identifier: MIT

// The CPU side of the mail exchange between AX and its microcode, for archive mode. DspLink does what the SDK's DSP
// library and AX do around each frame (__AXOutNewFrame at main.dol 0x80105c00 and __DSPHandler at 0x80138d8c in Wii
// Sports).
//
// The microcode starts in the state that the boot ROM leaves it in (ntdlib::dsp::Dsp::StartMicrocode). It mails
// 0xDCD10000 when it's ready. Each frame, AX mails 0xBABE0000 with the size of the command list, then the address of
// the list. The microcode mixes the list. Along the way it sends a request mail (0xDCD10004) that AX ignores. At the
// end of the list the microcode mails 0xDCD10002 (yield). The DSP library's interrupt handler has no other task to run.
// It answers 0xCDD10003 (continue).

#pragma once

#include <cstdint>
#include <span>

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"

namespace ntdlib::rvl::ax
{

class DspLink
{
public:
    explicit DspLink(ntdlib::dsp::Dsp<ntdlib::dsp::WiiMemory>& dsp) : dsp_(&dsp)
    {
    }

    // Starts `microcode` (loaded from `address` in the game) and waits for its first mail. Returns false if the
    // microcode sends no mail.
    bool Start(std::span<const uint16_t> microcode, uint32_t address);

    // Sends the microcode the `size`-byte command list at `command_list` (a cached address) and runs the DSP while the
    // microcode mixes it. Answers the yield mail at the end. Returns false if the microcode stops sending mail.
    bool RunFrame(uint32_t command_list, uint32_t size);

private:
    // Sends a mail and runs the DSP until the DSP reads it.
    bool SendMail(uint32_t mail);

    // Runs the DSP until it sends a mail. Reads the mail into `mail`.
    bool WaitForMail(uint32_t& mail);

    ntdlib::dsp::Dsp<ntdlib::dsp::WiiMemory>* dsp_;
};

} // namespace ntdlib::rvl::ax
