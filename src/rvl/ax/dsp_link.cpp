// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/ax/dsp_link.h"

#include <cstdint>
#include <span>

#include "ntdlib/dsp/dsp.h"

namespace ntdlib::rvl::ax
{
namespace
{

// The address in IRAM where AX starts its microcode as a DSP task: the task's init vector (__AXOutInitDSP).
constexpr uint16_t kInitVector = 0x0010;

// The mails between AX and its microcode (DSPUCode in the SDK's DSP library).
constexpr uint32_t kDspInit = 0xdcd10000;
constexpr uint32_t kDspYield = 0xdcd10002;
constexpr uint32_t kDspRequest = 0xdcd10004;
constexpr uint32_t kMailContinue = 0xcdd10003;
constexpr uint32_t kCommandListMail = 0xbabe0000;

// The most DSP steps to wait for a mail. A frame takes about 30,000 at most.
constexpr uint64_t kStepLimit = 20'000'000;

// The DSP runs in batches. The mailboxes are checked between batches. Steps that run after a mail only spin in the
// microcode loop that waits for the next mail. They change no state.
constexpr uint64_t kBatchSteps = 256;

// Bit 31 of a mailbox is set while the mailbox holds a mail that hasn't been read.
constexpr uint32_t kMailboxFull = 0x80000000;

// Runs `dsp` in batches until `done` returns true. Returns false if the DSP halts first or runs kStepLimit steps.
template <typename Done>
bool RunUntil(ntdlib::dsp::Dsp<ntdlib::dsp::WiiMemory>& dsp, Done done)
{
    for (uint64_t steps = 0; !done(); steps += kBatchSteps)
    {
        if (steps >= kStepLimit || dsp.Halted())
        {
            return false;
        }

        dsp.Run(kBatchSteps);
    }

    return true;
}

} // namespace

bool DspLink::Start(std::span<const uint16_t> microcode, uint32_t address)
{
    dsp_->StartMicrocode(microcode, address, kInitVector);

    uint32_t mail = 0;
    return WaitForMail(mail) && mail == kDspInit;
}

bool DspLink::RunFrame(uint32_t command_list, uint32_t size)
{
    if (!SendMail(kCommandListMail | size) || !SendMail(command_list))
    {
        return false;
    }

    // The DSP library passes a request to AX's request callback (__AXDSPRequestCallback, 0x80105f74). That callback
    // does nothing.
    uint32_t mail = 0;
    do
    {
        if (!WaitForMail(mail))
        {
            return false;
        }
    } while (mail == kDspRequest);

    return mail == kDspYield && SendMail(kMailContinue);
}

bool DspLink::SendMail(uint32_t mail)
{
    dsp_->CpuWriteMailboxHigh(static_cast<uint16_t>(mail >> 16));
    dsp_->CpuWriteMailboxLow(static_cast<uint16_t>(mail));

    return RunUntil(*dsp_, [this] { return (dsp_->MailboxToDsp() & kMailboxFull) == 0; });
}

bool DspLink::WaitForMail(uint32_t& mail)
{
    if (!RunUntil(*dsp_, [this] { return (dsp_->MailboxToCpu() & kMailboxFull) != 0; }))
    {
        return false;
    }

    // This code takes the place of the DSP library's interrupt handler. The microcode raises that interrupt when it
    // sends a mail. The handler reads the high half of the mailbox before the low half. Reading the low half empties
    // the mailbox.
    const uint16_t high = dsp_->CpuReadMailboxHigh(false);
    const uint16_t low = dsp_->CpuReadMailboxLow(false);
    mail = static_cast<uint32_t>(high) << 16 | low;
    dsp_->TakeInterruptRequest();

    return true;
}

} // namespace ntdlib::rvl::ax
