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

#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/machine.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/tables.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/types.h"

namespace ntdlib::rvl::ax
{
namespace
{

// AX allows the microcode 1/667 of the bus clock per frame.
constexpr uint32_t kMaxDspCycles = ntdlib::flipper::kWiiBusClockHz / 667;

// The parts of a voice's PB that __AXSetPBDefault (0x8010707c) marks as changed.
constexpr uint32_t kDefaultSync =
    sync::kState | sync::kItd | sync::kLpf | sync::kBiquad | sync::kRemote | sync::kRmtSrc | sync::kRmtIir;

// The DSP cycles that the microcode spends on a voice's mixes (__AXMixCycles, __AXRmtMixCycles). kMixCycles is indexed
// by 5 bits of Pb::MixerCtrl(): left, right, delta, surround and surround delta for one bus. kRemoteMixCycles is
// indexed by 2 bits of Pb::rmt_mixer_ctrl.
constexpr std::array<uint32_t, 32> kMixCycles = {2,    408,  408,  810,  1404, 1404, 1404, 1404, 408,  816,  816,
                                                 1218, 1812, 1812, 1812, 1812, 707,  1115, 1115, 1517, 2111, 2111,
                                                 2111, 2111, 707,  1115, 1115, 1517, 2111, 2111, 2111, 2111};
constexpr std::array<uint32_t, 4> kRemoteMixCycles = {4, 86, 151, 151};

// The same for the build of October 2006 (at 0x80530348 in Smooth Moves' main.dol). That build counts no cycles for the
// Wii Remotes.
constexpr std::array<uint32_t, 32> kMixCyclesOctober2006 = {
    0, 760, 760, 1470, 0, 1265, 1265, 2470, 760,  1520, 1520, 2230, 760,  2025, 2025, 3230,
    0, 760, 760, 1470, 0, 1265, 1265, 2470, 1265, 2025, 2025, 2735, 1265, 2530, 2530, 3735};

// The commands in the microcode's command list that every build numbers alike. BuildTraits numbers the others.
enum Command : uint16_t
{
    kCommandSetup = 0,
    kCommandAddToLr = 1,
    kCommandProcess = 4,
};

// The compressor's threshold and release time, in frames.
constexpr uint16_t kCompressorThreshold = 32768;
constexpr uint16_t kCompressorRelease = 10;

// The entries of the depop block, in order: 12 for the main mix and its aux buses, then 8 for the Wii Remotes.
enum DepopEntry
{
    kDepopL,
    kDepopR,
    kDepopS,
    kDepopAuxAL,
    kDepopAuxAR,
    kDepopAuxAS,
    kDepopAuxBL,
    kDepopAuxBR,
    kDepopAuxBS,
    kDepopAuxCL,
    kDepopAuxCR,
    kDepopAuxCS,
    kDepopMain0,
    kDepopAux0,
    kDepopMain1,
    kDepopAux1,
    kDepopMain2,
    kDepopAux2,
    kDepopMain3,
    kDepopAux3,
};

constexpr uint32_t kStudioEntrySize = 6;

// The samples in one Wii Remote ring buffer.
constexpr int32_t kRemoteBufferLength = layout::kRemoteBufferSamples;

// The addresses in the Wii Remotes' rings that a frame's command list gives the microcode: each ring at `position`, in
// samples from its start.
std::array<ntdlib::PAddr, kRemoteCount> RemoteBuffersAt(int32_t position)
{
    std::array<ntdlib::PAddr, kRemoteCount> remote;
    for (int i = 0; i < kRemoteCount; i++)
    {
        remote[i] = layout::kRemoteBuffers + static_cast<ntdlib::PAddr>((i * kRemoteBufferLength + position) * 2);
    }

    return remote;
}

uint16_t Hi(uint32_t value)
{
    return static_cast<uint16_t>(value >> 16);
}

uint16_t Lo(uint32_t value)
{
    return static_cast<uint16_t>(value);
}

// The DSP cycles for the voice's sample rate conversion (__AXGetSrcCycles, inlined in __AXSyncPBs).
uint32_t SrcCycles(const Pb& pb)
{
    const uint32_t ratio = Join(pb.src.ratio_hi, pb.src.ratio_lo);
    if (pb.src_select == static_cast<uint16_t>(SrcMode::kFourTap))
    {
        return (ratio * 512 + 32768) / 65536 + 1561;
    }
    if (pb.src_select == static_cast<uint16_t>(SrcMode::kLinear))
    {
        return (ratio * 512 + 32768) / 65536 + 1466;
    }

    return 605;
}

// The DSP cycles that a running voice takes in the build of December 2006 (__AXSyncPBs, 0x80106d64).
uint32_t RunningCyclesDecember2006(const Pb& pb)
{
    uint32_t cycles = 387;
    if (pb.lpf.on != 0)
    {
        cycles += 309;
    }

    if (pb.biquad.on != 0)
    {
        cycles += 1024;
    }

    if (pb.itd.flag == 1)
    {
        cycles += 27;
    }

    const uint32_t ctrl = pb.MixerCtrl();
    cycles += SrcCycles(pb);
    cycles +=
        kMixCycles[ctrl & 31] + kMixCycles[ctrl >> 16 & 31] + kMixCycles[ctrl >> 21 & 31] + kMixCycles[ctrl >> 26 & 31];

    if (pb.remote == 1)
    {
        cycles += 613;
        if (pb.rmt_iir.on == kLpfOn)
        {
            cycles += 118;
        }
        else if (pb.rmt_iir.on == kBiquadOn)
        {
            cycles += 834;
        }

        for (int shift = 0; shift < 16; shift += 2)
        {
            cycles += kRemoteMixCycles[pb.rmt_mixer_ctrl >> shift & 3];
        }
    }

    return cycles;
}

// The same in the build of October 2006 (0x80030af0). Both sample rate converters take the same cycles, and no
// conversion takes none. The low-pass filter and the second filter take 555 each. The model never turns the second
// filter on.
uint32_t RunningCyclesOctober2006(const Pb& pb)
{
    const uint32_t ctrl = pb.MixerCtrl();
    uint32_t cycles = kMixCyclesOctober2006[ctrl & 31] + kMixCyclesOctober2006[ctrl >> 16 & 31] +
                      kMixCyclesOctober2006[ctrl >> 21 & 31] + kMixCyclesOctober2006[ctrl >> 26 & 31] + 140;
    if (pb.src_select != static_cast<uint16_t>(SrcMode::kNone))
    {
        const uint32_t ratio = Join(pb.src.ratio_hi, pb.src.ratio_lo);
        cycles += ((ratio * 132 + 0x8000) / 65536 + 728) * 6;
    }

    if (pb.lpf.on != 0)
    {
        cycles += 555;
    }

    return cycles;
}

// What the builds of AX's CPU side differ in, apart from the PB's layout (see LoadPbOctober2006).
struct BuildTraits
{
    uint32_t voice_count;

    // The PB: its size, how it's read and written, and whether it has a buffer for the changes that the microcode
    // makes within a frame. The model never queues such changes.
    std::size_t pb_size;
    Pb (*load_pb)(const uint8_t* bytes);
    void (*store_pb)(const Pb& pb, uint8_t* bytes);
    bool update_buffers;

    // The DSP cycles: the headroom that __AXOutNewFrame passes to __AXSyncPBs, what __AXSyncPBs counts besides the
    // command list and the voices, a voice's ITD, and a running voice.
    uint32_t headroom;
    uint32_t sync_cycles;
    uint32_t itd_cycles;
    uint32_t (*running_cycles)(const Pb& pb);

    // Whether an aux bus whose callback goes marks the three frames of its ring to be cleared (__clearAuxA and so on).
    bool clears_aux_rings;

    // The command list (__AXNextFrame): the numbers of the commands after PROCESS, and AX's estimates of the cycles of
    // SETUP, of each aux bus and of OUTPUT.
    std::optional<uint16_t> after_process; // a command without arguments
    uint16_t mix_aux_a;                    // B and C follow
    uint16_t compressor;
    uint16_t wm_output;
    uint16_t output;
    bool output_volume; // whether OUTPUT takes the master volume
    uint16_t end;
    uint32_t setup_cycles;
    uint32_t aux_cycles;
    uint32_t output_cycles;
};

// The build that Wii Sports links, and the later ones. In its default mode, AX passes 24,400 cycles of headroom
// (__AXOutNewFrame, 0x80105c00).
constexpr BuildTraits kDecember2006Traits = {
    .voice_count = kVoiceCount,
    .pb_size = kPbSize,
    .load_pb = LoadPb,
    .store_pb = StorePb,
    .update_buffers = false,
    .headroom = 24400,
    .sync_cycles = 32 + kVoiceCount * 600,
    .itd_cycles = 129,
    .running_cycles = RunningCyclesDecember2006,
    .clears_aux_rings = true,
    .after_process = std::nullopt,
    .mix_aux_a = 5,
    .compressor = 10,
    .wm_output = 13,
    .output = 11,
    .output_volume = true,
    .end = 14,
    .setup_cycles = 4126,
    .aux_cycles = 2235,
    .output_cycles = 1172,
};

// The build of October 2006. Its __AXOutNewFrame (0x8002fb70) passes 0 cycles of headroom when the DSP has finished the
// last frame by the audio interface's interrupt. Otherwise it passes the time it waited for the DSP. Here the DSP is
// always done in time. Its __AXSyncPBs counts no cycles for the ITD. Command 5 always follows PROCESS. The commands
// after it are numbered one higher than in later builds.
constexpr BuildTraits kOctober2006Traits = {
    .voice_count = kVoiceCountOctober2006,
    .pb_size = kPbSizeOctober2006,
    .load_pb = LoadPbOctober2006,
    .store_pb = StorePbOctober2006,
    .update_buffers = true,
    .headroom = 0,
    .sync_cycles = 43536,
    .itd_cycles = 0,
    .running_cycles = RunningCyclesOctober2006,
    .clears_aux_rings = false,
    .after_process = 5,
    .mix_aux_a = 6,
    .compressor = 11,
    .wm_output = 14,
    .output = 12,
    .output_volume = false,
    .end = 15,
    .setup_cycles = 4246,
    .aux_cycles = 1249,
    .output_cycles = 665,
};

const BuildTraits& TraitsOf(Build build)
{
    return build == Build::kOctober2006 ? kOctober2006Traits : kDecember2006Traits;
}

// Fills one entry of the depop block from its accumulator (__AXDepopFadeMain 0x801063f8, __AXDepopFadeRmt 0x80106464).
// The accumulator falls by at most 20 per sample. The entry gives the microcode the accumulator value and the fall per
// sample.
void DepopFade(int32_t& sum, int32_t& value, int16_t& delta, int32_t samples)
{
    if (sum / samples == 0)
    {
        sum = 0;
        value = 0;
        delta = 0;

        return;
    }

    const int32_t step = std::clamp(sum / samples, -20, 20);
    value = sum;
    sum -= step * samples;
    delta = static_cast<int16_t>(-step);
}

} // namespace

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
        Voice& voice = voices_[i];
        Pb& dsp = dsp_pbs_[i];
        voice = Voice{};
        dsp = Pb{};
        voice.index = i;
        voice.itd_buffer = layout::kItdBuffers + i * layout::kItdBufferSize;
        SetPbDefault(&voice);

        const uint32_t next = i == voice_count_ - 1 ? 0 : ntdlib::dsp::ToCached(PbAddress(i + 1));
        const uint32_t curr = ntdlib::dsp::ToCached(PbAddress(i));
        const uint32_t itd = ntdlib::dsp::ToCached(voice.itd_buffer);
        voice.pb.next_hi = dsp.next_hi = Hi(next);
        voice.pb.next_lo = dsp.next_lo = Lo(next);
        voice.pb.curr_hi = dsp.curr_hi = Hi(curr);
        voice.pb.curr_lo = dsp.curr_lo = Lo(curr);
        voice.pb.itd.buffer_hi = dsp.itd.buffer_hi = Hi(itd);
        voice.pb.itd.buffer_lo = dsp.itd.buffer_lo = Lo(itd);
        WritePb(i, dsp);

        if (traits.update_buffers)
        {
            // The update block's three counters stay 0. Its last two words give the buffer's address.
            const uint32_t updates = ntdlib::dsp::ToCached(layout::kUpdateBuffers + i * layout::kUpdateBufferSize);
            uint8_t* block = memory_->At(PbAddress(i) + kUpdateBlockOctober2006, 10);
            xsflib::StoreBe16(block + 6, Hi(updates));
            xsflib::StoreBe16(block + 8, Lo(updates));
        }
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

Voice* System::AcquireVoice(uint32_t priority, Voice::Callback callback)
{
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

    if (sync & sync::kSelect)
    {
        dst.src_select = src.src_select;
        dst.coef_select = src.coef_select;
    }

    if (sync & sync::kMixerCtrl)
    {
        dst.mixer_ctrl_hi = src.mixer_ctrl_hi;
        dst.mixer_ctrl_lo = src.mixer_ctrl_lo;
    }

    if (sync & sync::kState)
    {
        dst.state = src.state;
    }
    else
    {
        src.state = dst.state;
    }

    if (sync & sync::kType)
    {
        dst.type = src.type;
    }

    if (sync & sync::kMix)
    {
        dst.mix = src.mix;
    }

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

    if (sync & (sync::kLoopFlag | sync::kLoopAddr | sync::kEndAddr | sync::kCurrAddr))
    {
        if (sync & sync::kLoopFlag)
        {
            dst.addr.loop_flag = src.addr.loop_flag;
        }

        if (sync & sync::kLoopAddr)
        {
            dst.addr.loop_address_hi = src.addr.loop_address_hi;
            dst.addr.loop_address_lo = src.addr.loop_address_lo;
        }

        if (sync & sync::kEndAddr)
        {
            dst.addr.end_address_hi = src.addr.end_address_hi;
            dst.addr.end_address_lo = src.addr.end_address_lo;
        }

        if (sync & sync::kCurrAddr)
        {
            dst.addr.current_address_hi = src.addr.current_address_hi;
            dst.addr.current_address_lo = src.addr.current_address_lo;
        }
        else
        {
            src.addr.current_address_hi = dst.addr.current_address_hi;
            src.addr.current_address_lo = dst.addr.current_address_lo;
        }
    }
    else if (sync & sync::kAddr)
    {
        dst.addr = src.addr;
    }
    else
    {
        src.addr.current_address_hi = dst.addr.current_address_hi;
        src.addr.current_address_lo = dst.addr.current_address_lo;
    }

    if (sync & sync::kAdpcm)
    {
        dst.adpcm = src.adpcm;
    }

    if (sync & sync::kSrcRatio)
    {
        dst.src.ratio_hi = src.src.ratio_hi;
        dst.src.ratio_lo = src.src.ratio_lo;
    }
    else if (sync & sync::kSrc)
    {
        dst.src = src.src;
    }

    if (sync & sync::kAdpcmLoop)
    {
        dst.adpcm_loop = src.adpcm_loop;
    }

    if (sync & sync::kLpfCoefs)
    {
        dst.lpf.a0 = src.lpf.a0;
        dst.lpf.b0 = src.lpf.b0;
    }
    else if (sync & sync::kLpf)
    {
        dst.lpf = src.lpf;
    }

    if (sync & sync::kBiquadCoefs)
    {
        dst.biquad.b0 = src.biquad.b0;
        dst.biquad.b1 = src.biquad.b1;
        dst.biquad.b2 = src.biquad.b2;
        dst.biquad.a1 = src.biquad.a1;
        dst.biquad.a2 = src.biquad.a2;
    }
    else if (sync & sync::kBiquad)
    {
        dst.biquad = src.biquad;
    }

    if (sync & sync::kRemote)
    {
        dst.remote = src.remote;
    }

    if (sync & sync::kRmtMixerCtrl)
    {
        dst.rmt_mixer_ctrl = src.rmt_mixer_ctrl;
    }

    if (sync & sync::kRmtMix)
    {
        dst.rmt_mix = src.rmt_mix;
    }

    if (sync & sync::kRmtDpop)
    {
        dst.rmt_dpop = src.rmt_dpop;
    }

    if (sync & sync::kRmtSrc)
    {
        dst.rmt_src = src.rmt_src;
    }

    // When the remote filter is a low-pass filter, its third and fourth words are a0 and b0.
    if (sync & sync::kRmtIirLpfCoefs)
    {
        dst.rmt_iir.xn2 = src.rmt_iir.xn2;
        dst.rmt_iir.yn1 = src.rmt_iir.yn1;
    }
    else if (sync & sync::kRmtIirBiquadCoefs)
    {
        dst.rmt_iir.b0 = src.rmt_iir.b0;
        dst.rmt_iir.b1 = src.rmt_iir.b1;
        dst.rmt_iir.b2 = src.rmt_iir.b2;
        dst.rmt_iir.a1 = src.rmt_iir.a1;
        dst.rmt_iir.a2 = src.rmt_iir.a2;
    }
    else if (sync & sync::kRmtIir)
    {
        dst.rmt_iir = src.rmt_iir;
    }
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
