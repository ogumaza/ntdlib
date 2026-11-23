// SPDX-License-Identifier: MIT

// System's work in each frame (see system.h): the sync of the voices' PBs with what the microcode wrote, the service of
// each voice's changes, the depop, the callbacks, and the command list that the microcode runs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "xsflib/bytes.h"

#include "ntdlib/ax/ax.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/types.h"
#include "rvl/ax/system_internal.h"

namespace ntdlib::rvl::ax
{

void System::SyncPbs(uint32_t base_cycles)
{
    // __AXSyncPBs (0x80106d64; October 2006, 0x80030af0). Read back what the microcode wrote. Then go through the
    // voices from the highest priority and add up the cycles that each running voice will take. A voice that would push
    // the frame over the limit stops. ServiceCallbackStack calls its owner's callback later.
    for (uint32_t i = 0; i < voice_count_; i++)
    {
        dsp_pbs_[i] = ReadPb(i);
    }

    const BuildTraits& traits = TraitsOf(build_);
    uint32_t cycles = command_list_cycles_ + base_cycles + traits.sync_cycles;
    const auto sync_voice = [&](uint32_t index)
    {
        Voice* voice = &voices_[index];
        if (voice->pb.itd.flag == 1)
        {
            cycles += traits.itd_cycles;
        }

        if (voice->depop)
        {
            DepopVoice(dsp_pbs_[voice->index]);
        }

        if (voice->IsRunning())
        {
            cycles += traits.running_cycles(voice->pb);
            if (max_dsp_cycles_ > cycles)
            {
                ServiceVpb(voice);
            }
            else
            {
                DumpVpb(voice);
            }
        }
        else
        {
            ServiceVpb(voice);
        }

        voice->sync = 0;
        voice->depop = false;
    };
    for (uint32_t priority = kPriorityMax; priority > kPriorityFree; priority--)
    {
        allocation_.ForEachNewestFirst(priority, sync_voice);
    }

    recorded_dsp_cycles_ = cycles;

    // A free voice only stops. A voice that was running when it was freed also fades out.
    const auto stop_free_voice = [&](uint32_t index)
    {
        Voice* voice = &voices_[index];
        if (voice->depop)
        {
            DepopVoice(dsp_pbs_[voice->index]);
        }

        voice->depop = false;
        dsp_pbs_[voice->index].state = static_cast<uint16_t>(VoiceState::kStop);
    };
    allocation_.ForEachNewestFirst(kPriorityFree, stop_free_voice);

    for (uint32_t i = 0; i < voice_count_; i++)
    {
        WritePb(i, dsp_pbs_[i]);
    }
}

void System::ServiceVpb(Voice* voice)
{
    // __AXServiceVPB (0x80106814). Copy the CPU's changes to the microcode's PB. For fields that the CPU hasn't
    // changed, copy back the microcode's changes.
    Pb& dst = dsp_pbs_[voice->index];
    Pb& src = voice->pb;
    const uint32_t sync = voice->sync;
    if (sync == 0)
    {
        src.state = dst.state;
        src.ve.current_volume = dst.ve.current_volume;
        src.addr.current_address_hi = dst.addr.current_address_hi;
        src.addr.current_address_lo = dst.addr.current_address_lo;

        return;
    }
    if (sync & sync::kAll)
    {
        dst = src;

        return;
    }

    ServiceSettings(dst, src, sync);

    if (sync & sync::kItdShift)
    {
        dst.itd.target_shift_l = src.itd.target_shift_l;
        dst.itd.target_shift_r = src.itd.target_shift_r;
    }
    else if (sync & sync::kItd)
    {
        dst.itd = src.itd;
        std::memset(memory_->At(voice->itd_buffer, layout::kItdBufferSize), 0, layout::kItdBufferSize);
    }

    if (sync & sync::kDpop)
    {
        dst.dpop = src.dpop;
    }

    if (sync & sync::kVeDelta)
    {
        src.ve.current_volume = dst.ve.current_volume;
        dst.ve.current_delta = src.ve.current_delta;
    }
    else if (sync & sync::kVe)
    {
        dst.ve = src.ve;
    }

    ServiceAddresses(dst, src, sync);
    ServiceDecoder(dst, src, sync);
    ServiceFilters(dst, src, sync);
    ServiceRemote(dst, src, sync);
}

void System::DumpVpb(Voice* voice)
{
    // __AXDumpVPB (inlined in __AXSyncPBs). Stop a voice that doesn't fit in the frame's cycles and queue its callback.
    Pb& dsp = dsp_pbs_[voice->index];
    if (dsp.state == static_cast<uint16_t>(VoiceState::kRun))
    {
        DepopVoice(dsp);
    }

    voice->pb.state = static_cast<uint16_t>(VoiceState::kStop);
    dsp.state = static_cast<uint16_t>(VoiceState::kStop);
    PushCallbackStack(voice);
}

void System::DepopVoice(const Pb& pb)
{
    // __AXDepopVoice (0x801066a0): adds the last samples that the voice added to each mix to the depop accumulators.
    const auto add = [this](DepopEntry entry, uint16_t sample)
    {
        depop_[entry].sum += static_cast<int16_t>(sample);
    };

    add(kDepopL, pb.dpop.l);
    add(kDepopAuxAL, pb.dpop.aux_a_l);
    add(kDepopAuxBL, pb.dpop.aux_b_l);
    add(kDepopAuxCL, pb.dpop.aux_c_l);
    add(kDepopR, pb.dpop.r);
    add(kDepopAuxAR, pb.dpop.aux_a_r);
    add(kDepopAuxBR, pb.dpop.aux_b_r);
    add(kDepopAuxCR, pb.dpop.aux_c_r);
    add(kDepopS, pb.dpop.s);
    add(kDepopAuxAS, pb.dpop.aux_a_s);
    add(kDepopAuxBS, pb.dpop.aux_b_s);
    add(kDepopAuxCS, pb.dpop.aux_c_s);
    add(kDepopMain0, pb.rmt_dpop.main0);
    add(kDepopMain1, pb.rmt_dpop.main1);
    add(kDepopMain2, pb.rmt_dpop.main2);
    add(kDepopMain3, pb.rmt_dpop.main3);
    add(kDepopAux0, pb.rmt_dpop.aux0);
    add(kDepopAux1, pb.rmt_dpop.aux1);
    add(kDepopAux2, pb.rmt_dpop.aux2);
    add(kDepopAux3, pb.rmt_dpop.aux3);
}

void System::PrintStudio()
{
    // __AXPrintStudio (0x801064d0): writes the depop block. The SETUP command in the command list gives the microcode
    // its address. Each entry packs a 32-bit value and a 16-bit fall per sample.
    uint8_t* studio = memory_->At(layout::kStudio, depop_.size() * kStudioEntrySize);
    for (std::size_t i = 0; i < depop_.size(); i++)
    {
        Depop& entry = depop_[i];
        const int32_t samples = i < kDepopMain0 ? kSamplesPerFrame : kRemoteSamplesPerFrame;
        DepopFade(entry.sum, entry.value, entry.delta, samples);
        xsflib::StoreBe32(studio + i * kStudioEntrySize, static_cast<uint32_t>(entry.value));
        xsflib::StoreBe16(studio + i * kStudioEntrySize + 4, static_cast<uint16_t>(entry.delta));
    }
}

void System::ServiceCallbackStack()
{
    // __AXServiceCallbackStack (0x801044b8). Run the callbacks of the voices that SyncPbs dropped. Then free those
    // voices.
    for (Voice* voice = PopCallbackStack(); voice != nullptr; voice = PopCallbackStack())
    {
        if (allocation_.Priority(voice->index) > kPriorityFree)
        {
            if (voice->callback)
            {
                const Voice::Callback callback = voice->callback;
                callback(voice);
            }

            allocation_.Remove(voice->index);
            allocation_.Push(voice->index, kPriorityFree);
        }
    }
}

uint32_t System::CommandListAddress()
{
    // __AXGetCommandListAddress (0x8010517c). Returns the list built last and switches to the other list for writing.
    const ntdlib::PAddr list = layout::kCommandLists + command_list_position_ * layout::kCommandListSize;
    command_list_position_ = (command_list_position_ + 1) % 2;
    command_list_write_ = layout::kCommandLists + command_list_position_ * layout::kCommandListSize;

    return ntdlib::dsp::ToCached(list);
}

void System::WriteCommand(uint16_t word)
{
    xsflib::StoreBe16(memory_->At(command_list_write_, 2), word);
    command_list_write_ += 2;
}

void System::WriteCommand32(uint32_t value)
{
    WriteCommand(Hi(value));
    WriteCommand(Lo(value));
}

void System::NextFrame(ntdlib::PAddr surround, ntdlib::PAddr output,
                       const std::array<ntdlib::PAddr, kRemoteCount>& remote)
{
    // __AXNextFrame (0x801051ac; October 2006, 0x8002ef20) in stereo, with the compressor on. Both builds write the
    // commands in the same order. The cycle counts are AX's estimates. SyncPbs uses them.
    const BuildTraits& traits = TraitsOf(build_);
    list_output_[(command_list_write_ - layout::kCommandLists) / layout::kCommandListSize] = output;
    command_list_cycles_ = 7811;

    WriteCommand(kCommandSetup);
    WriteCommand32(ntdlib::dsp::ToCached(layout::kStudio));
    command_list_cycles_ += traits.setup_cycles;

    WriteCommand(kCommandAddToLr);
    WriteCommand32(ntdlib::dsp::ToCached(surround));
    command_list_cycles_ += 733;

    WriteCommand(kCommandProcess);
    WriteCommand32(ntdlib::dsp::ToCached(layout::kPbs));
    if (traits.after_process)
    {
        WriteCommand(*traits.after_process);
    }

    WriteAuxCommands(traits.mix_aux_a, traits.aux_cycles);

    WriteCommand(traits.compressor);
    WriteCommand(kCompressorThreshold);
    WriteCommand(kCompressorRelease);
    WriteCommand32(ntdlib::dsp::ToCached(layout::kCompressorTable));
    command_list_cycles_ += 1850;

    WriteCommand(traits.wm_output);
    for (const ntdlib::PAddr buffer : remote)
    {
        WriteCommand32(ntdlib::dsp::ToCached(buffer));
    }
    command_list_cycles_ += 409;

    WriteCommand(traits.output);
    if (traits.output_volume)
    {
        WriteCommand(master_volume_);
    }
    WriteCommand32(ntdlib::dsp::ToCached(surround));
    WriteCommand32(ntdlib::dsp::ToCached(output));
    command_list_cycles_ += traits.output_cycles;

    WriteCommand(traits.end);
    command_list_cycles_ += 30;
}

} // namespace ntdlib::rvl::ax
