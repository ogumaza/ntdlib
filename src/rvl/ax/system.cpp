// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/ax/system.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "xsflib/bytes.h"

#include "ntdlib/ax/voices.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/tables.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/types.h"
#include "rvl/ax/system_internal.h"

namespace ntdlib::rvl::ax
{

System::System(ntdlib::dsp::WiiMemory& memory) : memory_(&memory)
{
}

void System::Init(Build build)
{
    build_ = build;
    const BuildTraits& traits = TraitsOf(build_);
    voice_count_ = traits.voice_count;

    uint8_t* table = memory_->At(layout::kCompressorTable, layout::kCompressorTableSize);
    for (std::size_t i = 0; i < tables::kCompressor.size(); i++)
    {
        xsflib::StoreBe16(table + i * 2, tables::kCompressor[i]);
    }

    // __AXAllocInit (0x80104564), and __AXVPBInitCommon's frees: every voice goes onto the free stack, in index order.
    allocation_ = ntdlib::ax::Voices(static_cast<int>(voice_count_));
    callback_stack_ = nullptr;

    // __AXVPBInitCommon (0x801070ec): frees every voice and links every PB to the next one in the microcode's list. The
    // build of October 2006 (__AXVPBInit, 0x80030de4) also gives each PB the address of a buffer for its updates.
    max_dsp_cycles_ = kMaxDspCycles;
    recorded_dsp_cycles_ = 0;
    std::memset(memory_->At(layout::kItdBuffers, voice_count_ * layout::kItdBufferSize), 0,
                voice_count_ * layout::kItdBufferSize);
    std::memset(memory_->At(layout::kPbs, voice_count_ * traits.pb_size), 0, voice_count_ * traits.pb_size);
    if (traits.update_buffers)
    {
        std::memset(memory_->At(layout::kUpdateBuffers, voice_count_ * layout::kUpdateBufferSize), 0,
                    voice_count_ * layout::kUpdateBufferSize);
    }

    for (uint32_t i = 0; i < voice_count_; i++)
    {
        InitVoice(i, traits.update_buffers);
    }

    // __AXSPBInit (0x80106648), __AXAuxInit (0x8010495c) and __AXClInit (0x80105b6c). __AXAuxInit clears the first
    // frame of each ring. Main memory starts cleared.
    depop_ = {};
    aux_callbacks_ = {};
    aux_volume_ = {kMaxVolume, kMaxVolume, kMaxVolume};
    aux_clear_ = {};
    aux_dsp_write_ = 0;
    aux_dsp_read_ = 1;
    aux_cpu_ = 2;
    command_list_position_ = 0;
    command_list_write_ = layout::kCommandLists;
    master_volume_ = kMaxVolume;

    // __AXOutInit (0x80106068) in AX's default mode. That mode uses two output buffers. The first command list writes
    // to the second one.
    output_frame_ = 0;
    std::memset(memory_->At(layout::kOutputBuffers, 3 * layout::kOutputBufferSize), 0, 3 * layout::kOutputBufferSize);
    std::memset(memory_->At(layout::kSurroundBuffer, layout::kSurroundBufferSize), 0, layout::kSurroundBufferSize);
    std::memset(memory_->At(layout::kRemoteBuffers, kRemoteCount * kRemoteBufferLength * 2), 0,
                kRemoteCount * kRemoteBufferLength * 2);

    // The first command list writes the rings from their start.
    remote_position_ = kRemoteSamplesPerFrame;
    NextFrame(layout::kSurroundBuffer, layout::kOutputBuffers + layout::kOutputBufferSize, RemoteBuffersAt(0));
}

void System::InitVoice(uint32_t index, bool update_buffers)
{
    Voice& voice = voices_[index];
    Pb& dsp = dsp_pbs_[index];
    voice = Voice{};
    dsp = Pb{};
    voice.index = index;
    voice.itd_buffer = layout::kItdBuffers + index * layout::kItdBufferSize;
    SetPbDefault(&voice);

    const uint32_t next = index == voice_count_ - 1 ? 0 : ntdlib::dsp::ToCached(PbAddress(index + 1));
    const uint32_t curr = ntdlib::dsp::ToCached(PbAddress(index));
    const uint32_t itd = ntdlib::dsp::ToCached(voice.itd_buffer);
    voice.pb.next_hi = dsp.next_hi = Hi(next);
    voice.pb.next_lo = dsp.next_lo = Lo(next);
    voice.pb.curr_hi = dsp.curr_hi = Hi(curr);
    voice.pb.curr_lo = dsp.curr_lo = Lo(curr);
    voice.pb.itd.buffer_hi = dsp.itd.buffer_hi = Hi(itd);
    voice.pb.itd.buffer_lo = dsp.itd.buffer_lo = Lo(itd);
    WritePb(index, dsp);

    if (update_buffers)
    {
        // The update block's three counters stay 0. Its last two words give the buffer's address.
        const uint32_t updates = ntdlib::dsp::ToCached(layout::kUpdateBuffers + index * layout::kUpdateBufferSize);
        uint8_t* block = memory_->At(PbAddress(index) + kUpdateBlockOctober2006, 10);
        xsflib::StoreBe16(block + 6, Hi(updates));
        xsflib::StoreBe16(block + 8, Lo(updates));
    }
}

Pb System::ReadPb(uint32_t index) const
{
    const BuildTraits& traits = TraitsOf(build_);
    return traits.load_pb(memory_->At(PbAddress(index), traits.pb_size));
}

void System::WritePb(uint32_t index, const Pb& pb)
{
    const BuildTraits& traits = TraitsOf(build_);
    traits.store_pb(pb, memory_->At(PbAddress(index), traits.pb_size));
}

void System::UseLayer(VoiceLayer& layer, uint32_t count)
{
    // The layer's aux buses return nothing until the game sets their volumes. Its master volume starts at 1.
    layer_ = &layer;
    voice_count_ = std::min<uint32_t>(count, kVoiceCount);
    aux_volume_ = {0, 0, 0};
    master_volume_ = kMaxVolume;
    for (uint32_t i = 0; i < voice_count_; i++)
    {
        voices_[i] = Voice{};
        voices_[i].index = i;
        voices_[i].layer = &layer;
    }
}

Voice* System::AcquireVoice(uint32_t priority, Voice::Callback callback)
{
    if (layer_ != nullptr)
    {
        return layer_->AcquireVoice(priority, std::move(callback));
    }

    // With no free voice, steal the oldest voice of the lowest priority below this one. Call its owner's callback
    // first.
    const std::optional<ntdlib::ax::Voices::Taken> taken = allocation_.Take(priority);
    if (!taken)
    {
        return nullptr;
    }

    Voice* voice = &voices_[taken->voice];
    if (taken->stolen)
    {
        if (voice->IsRunning())
        {
            voice->depop = true;
        }

        if (voice->callback)
        {
            const Voice::Callback owner = voice->callback;
            owner(voice);
        }
    }

    allocation_.Push(voice->index, priority);
    voice->callback = std::move(callback);
    SetPbDefault(voice);

    return voice;
}

void System::FreeVoice(Voice* voice)
{
    if (layer_ != nullptr)
    {
        layer_->FreeVoice(*voice);

        return;
    }

    allocation_.Remove(voice->index);
    if (voice->IsRunning())
    {
        voice->depop = true;
    }

    SetPbDefault(voice);
    allocation_.Push(voice->index, kPriorityFree);
}

void System::SetVoicePriority(Voice* voice, uint32_t priority)
{
    if (layer_ != nullptr)
    {
        layer_->SetVoicePriority(*voice, priority);

        return;
    }

    allocation_.Remove(voice->index);
    allocation_.Push(voice->index, priority);
}

void System::SetMasterVolume(uint16_t volume)
{
    master_volume_ = std::min(volume, kMaxVolume);
}

Frame System::StartFrame()
{
    SyncPbs(TraitsOf(build_).headroom);
    PrintStudio();

    const uint32_t list = CommandListAddress();
    return {list, list_output_[(ntdlib::dsp::ToPhysical(list) - layout::kCommandLists) / layout::kCommandListSize]};
}

void System::FinishFrame()
{
    ServiceCallbackStack();
    ProcessAux();
    if (frame_callback_)
    {
        frame_callback_();
    }

    // Advance the Wii Remote rings by one frame. Then build the next command list.
    const std::array<ntdlib::PAddr, kRemoteCount> remote = RemoteBuffersAt(remote_position_);
    remote_position_ += kRemoteSamplesPerFrame;
    if (remote_position_ >= kRemoteBufferLength)
    {
        remote_position_ = 0;
    }

    NextFrame(layout::kSurroundBuffer, layout::kOutputBuffers + output_frame_ * layout::kOutputBufferSize, remote);
    output_frame_ = (output_frame_ + 1) & 1;
}

void System::RegisterAuxCallback(int bus, AuxCallback callback)
{
    if (!callback && TraitsOf(build_).clears_aux_rings)
    {
        aux_clear_[bus].fill(true);
    }

    aux_callbacks_[bus] = std::move(callback);
}

void System::ProcessAux()
{
    // __AXProcessAux (0x80104c60; October 2006, 0x8002eba4). Each bus with a callback processes the frame that the DSP
    // wrote two frames ago. The DSP mixes that frame back in the next frame. Without a callback, a frame marked for
    // clearing is cleared. A callback registered later then starts from silence. Then the three places move on.
    for (int bus = 0; bus < kAuxBusCount; bus++)
    {
        const ntdlib::PAddr address = AuxFrameAddress(bus, aux_cpu_);
        uint8_t* bytes = memory_->At(address, layout::kAuxFrameSize);
        if (aux_callbacks_[bus])
        {
            axfx::AuxFrame frame;
            for (int channel = 0; channel < axfx::kChannelCount; channel++)
            {
                for (int i = 0; i < kSamplesPerFrame; i++)
                {
                    frame.channels[channel][i] = static_cast<int32_t>(
                        xsflib::LoadBe32(bytes + channel * layout::kAuxChannelSize + static_cast<uint32_t>(i) * 4));
                }
            }

            aux_callbacks_[bus](frame);
            for (int channel = 0; channel < axfx::kChannelCount; channel++)
            {
                for (int i = 0; i < kSamplesPerFrame; i++)
                {
                    xsflib::StoreBe32(bytes + channel * layout::kAuxChannelSize + static_cast<uint32_t>(i) * 4,
                                      static_cast<uint32_t>(frame.channels[channel][i]));
                }
            }
        }
        else if (aux_clear_[bus][aux_cpu_])
        {
            std::memset(memory_->At(address, layout::kAuxFrameStride[bus]), 0, layout::kAuxFrameStride[bus]);
            aux_clear_[bus][aux_cpu_] = false;
        }
    }

    aux_dsp_write_ = (aux_dsp_write_ + 1) % layout::kAuxRingFrames;
    aux_dsp_read_ = (aux_dsp_read_ + 1) % layout::kAuxRingFrames;
    aux_cpu_ = (aux_cpu_ + 1) % layout::kAuxRingFrames;
}

ntdlib::PAddr System::AuxFrameAddress(int bus, uint32_t position) const
{
    return layout::kAuxRings[bus] + position * layout::kAuxFrameStride[bus];
}

void System::WriteAuxCommands(uint16_t first_command, uint32_t cycles)
{
    // __AXNextFrame's commands for the aux buses in stereo: for each bus with a callback, the return volume, the frame
    // that the DSP writes the bus to, and the processed frame that it mixes back.
    for (int bus = 0; bus < kAuxBusCount; bus++)
    {
        if (!aux_callbacks_[bus])
        {
            continue;
        }

        WriteCommand(static_cast<uint16_t>(first_command + bus));
        WriteCommand(aux_volume_[bus]);
        WriteCommand32(ntdlib::dsp::ToCached(AuxFrameAddress(bus, aux_dsp_write_)));
        WriteCommand32(ntdlib::dsp::ToCached(AuxFrameAddress(bus, aux_dsp_read_)));
        command_list_cycles_ += cycles;
    }
}

void System::PushCallbackStack(Voice* voice)
{
    voice->next1 = callback_stack_;
    callback_stack_ = voice;
}

Voice* System::PopCallbackStack()
{
    Voice* head = callback_stack_;
    if (head != nullptr)
    {
        callback_stack_ = head->next1;
    }

    return head;
}

ntdlib::PAddr System::PbAddress(uint32_t index) const
{
    return layout::kPbs + index * static_cast<ntdlib::PAddr>(TraitsOf(build_).pb_size);
}

void System::SetPbDefault(Voice* voice)
{
    // __AXSetPBDefault (0x8010707c): a stopped voice with no filters, no ITD and no Wii Remote output.
    voice->pb.state = static_cast<uint16_t>(VoiceState::kStop);
    voice->pb.itd.flag = 0;
    voice->sync = kDefaultSync;
    voice->pb.lpf.on = 0;
    voice->pb.biquad.on = 0;
    voice->pb.remote = 0;
    voice->pb.rmt_iir.on = 0;
    voice->pb.rmt_src = PbRmtSrc{};
}

} // namespace ntdlib::rvl::ax
