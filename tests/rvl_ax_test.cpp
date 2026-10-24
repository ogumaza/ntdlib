// SPDX-License-Identifier: MIT

// Tests of the model of the Wii's AX (rvl/ax): the first command lists and where they point, the PBs' links in main
// memory, the order in which AX hands out voices and steals them by priority, the copy of a voice's changes into its
// PB, and the voices that a frame drops when they would push it over its DSP cycles. The tests of the build of October
// 2006 check its command list, its PBs of 0x120 bytes, its 64 voices and the cycles that it counts.

#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/types.h"

using xsflib::testing::Check;

namespace
{

using namespace ntdlib::rvl;

uint32_t Cached(ntdlib::PAddr address)
{
    return ntdlib::dsp::ToCached(address);
}

// The words of the command list buffer at `address`: the list, and whatever follows its END in the buffer.
std::vector<uint16_t> ReadCommandList(const ntdlib::dsp::WiiMemory& memory, uint32_t address)
{
    std::vector<uint16_t> words;
    for (ntdlib::PAddr at = ntdlib::dsp::ToPhysical(address); words.size() < ax::layout::kCommandListSize / 2; at += 2)
    {
        words.push_back(xsflib::LoadBe16(memory.At(at, 2)));
    }

    return words;
}

void CheckCommandList(const ntdlib::dsp::WiiMemory& memory, uint32_t list, ntdlib::PAddr output,
                      ntdlib::PAddr remote_offset)
{
    const std::vector<uint16_t> words = ReadCommandList(memory, list);
    const auto word32 = [&words](std::size_t i)
    {
        return uint32_t{words[i]} << 16 | words[i + 1];
    };

    Check(words[0] == 0 && word32(1) == Cached(ax::layout::kStudio), "SETUP and the depop block");
    Check(words[3] == 1 && word32(4) == Cached(ax::layout::kSurroundBuffer), "ADD_TO_LR and the surround buffer");
    Check(words[6] == 4 && word32(7) == Cached(ax::layout::kPbs), "PROCESS and the PBs");
    Check(words[9] == 10 && words[10] == 32768 && words[11] == 10 && word32(12) == Cached(ax::layout::kCompressorTable),
          "COMPRESSOR, its threshold, release and table");
    Check(words[14] == 13, "WM_OUTPUT");
    for (int i = 0; i < 4; i++)
    {
        Check(word32(15 + i * 2) == Cached(ax::layout::kRemoteBuffers + i * 360 + remote_offset),
              "a Wii Remote's buffer");
    }
    Check(words[23] == 11 && words[24] == 0x8000 && word32(25) == Cached(ax::layout::kSurroundBuffer) &&
              word32(27) == Cached(output),
          "OUTPUT, the master volume and the buffers");
    Check(words[29] == 14, "END");
}

void TestInit()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);

    ax.Init();

    // The first command list fills the second output buffer. The microcode receives it with the first frame.
    CheckCommandList(memory, Cached(ax::layout::kCommandLists), ax::layout::kOutputBuffers + 384, 0);
    for (uint32_t i = 0; i < ax::kVoiceCount; i++)
    {
        const ax::Pb pb = ax::LoadPb(memory.At(ax::layout::kPbs + i * 0x140, ax::kPbSize));
        const uint32_t next = i == ax::kVoiceCount - 1 ? 0 : Cached(ax::layout::kPbs + (i + 1) * 0x140);
        Check(ax::Join(pb.next_hi, pb.next_lo) == next &&
                  ax::Join(pb.curr_hi, pb.curr_lo) == Cached(ax::layout::kPbs + i * 0x140),
              "a PB's links");
    }

    const ax::Frame first = ax.StartFrame();
    ax.FinishFrame();
    const ax::Frame second = ax.StartFrame();

    Check(first.command_list == Cached(ax::layout::kCommandLists) && first.output == ax::layout::kOutputBuffers + 384,
          "the first frame's command list");
    Check(
        second.command_list == Cached(ax::layout::kCommandLists + 0x80) && second.output == ax::layout::kOutputBuffers,
        "the second frame's command list");
    CheckCommandList(memory, second.command_list, ax::layout::kOutputBuffers, 18 * 2);
}

void TestAllocation()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);
    ax.Init();

    // Free voices come from the top of a stack that starts with the last voice.
    std::vector<ax::Voice*> voices;
    std::vector<uint32_t> dropped;
    const auto on_drop = [&dropped](ax::Voice* voice)
    {
        dropped.push_back(voice->index);
    };

    for (int i = 0; i < ax::kVoiceCount; i++)
    {
        voices.push_back(ax.AcquireVoice(16, on_drop));
    }

    Check(voices[0]->index == 95 && voices[1]->index == 94 && voices[95]->index == 0, "the order of free voices");
    Check(ax.AcquireVoice(16, on_drop) == nullptr && dropped.empty(), "no voice for an equal priority");

    // A higher priority steals the oldest voice of the lowest priority below it. The voice's owner is notified first.
    ax.SetVoicePriority(voices[5], 15);

    ax::Voice* first = ax.AcquireVoice(17, on_drop);
    const std::size_t drops_after_first = dropped.size();
    ax::Voice* second = ax.AcquireVoice(17, on_drop);

    Check(
        first == voices[5] && drops_after_first == 1 && dropped[0] == voices[5]->index && ax.VoicePriority(first) == 17,
        "a voice taken from a lower priority");
    Check(second == voices[0] && dropped.size() == 2, "the oldest voice taken first");

    // A freed voice is the next one handed out.
    ax.FreeVoice(voices[40]);
    Check(ax.AcquireVoice(1, on_drop) == voices[40], "a freed voice reused");
}

void TestSync()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);
    ax.Init();

    ax::Voice* voice = ax.AcquireVoice(16, nullptr);
    ax::PbAddr address;
    address.format = static_cast<uint16_t>(ax::SampleFormat::kPcm16);
    address.end_address_lo = 0x1234;
    address.current_address_hi = 0x0800;
    voice->SetAddr(address);
    voice->pb.mix.l = 0x4000;
    voice->pb.SetMixerCtrl(ax::mixer::kL);
    voice->sync |= ax::sync::kMix | ax::sync::kMixerCtrl;
    voice->SetState(ax::VoiceState::kRun);

    ax.StartFrame();

    const ax::Pb pb = ax::LoadPb(memory.At(ax::layout::kPbs + voice->index * 0x140, ax::kPbSize));
    Check(pb.state == 1 && pb.mix.l == 0x4000 && pb.MixerCtrl() == ax::mixer::kL && pb.addr.end_address_lo == 0x1234 &&
              pb.addr.current_address_hi == 0x0800 && pb.adpcm.gain == 0x800,
          "a voice's changes in its PB");
    Check(voice->sync == 0, "the changes cleared once copied");

    // Fields that the CPU hasn't changed get the microcode's values back in the CPU's copy.
    uint8_t* bytes = memory.At(ax::layout::kPbs + voice->index * 0x140, ax::kPbSize);
    xsflib::StoreBe16(bytes + 0x6e + 0xc, 0x0801);
    xsflib::StoreBe16(bytes + 0x10, 0);

    ax.FinishFrame();
    ax.StartFrame();

    Check(voice->pb.addr.current_address_hi == 0x0801 && !voice->IsRunning(), "the microcode's changes read back");
}

void TestCycleBudget()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);
    ax.Init();

    // Every voice runs with the polyphase converter at 2x and a full mix. AX drops the ones that don't fit.
    std::vector<uint32_t> dropped;
    const auto on_drop = [&dropped](ax::Voice* voice)
    {
        dropped.push_back(voice->index);
    };
    for (int i = 0; i < ax::kVoiceCount; i++)
    {
        ax::Voice* voice = ax.AcquireVoice(16, on_drop);
        voice->pb.src.ratio_hi = 2;
        voice->pb.SetMixerCtrl(0xffffffff);
        voice->SetState(ax::VoiceState::kRun);
    }

    ax.StartFrame();
    const bool dropped_in_frame = !dropped.empty();
    ax.FinishFrame();

    Check(!dropped_in_frame, "drop callbacks wait for the end of the frame");
    Check(!dropped.empty() && dropped.size() < ax::kVoiceCount, "voices dropped for want of cycles");
    Check(ax.DspCycles() > 364317, "the frame's cycles counted past the limit");
}

void TestOctober2006Layout()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);

    ax.Init(ax::Build::kOctober2006);

    // Command 5 follows PROCESS. The commands after it are numbered one higher, and OUTPUT takes no master volume.
    const std::vector<uint16_t> words = ReadCommandList(memory, Cached(ax::layout::kCommandLists));
    const auto word32 = [&words](std::size_t i)
    {
        return uint32_t{words[i]} << 16 | words[i + 1];
    };

    Check(words[0] == 0 && words[3] == 1 && words[6] == 4 && word32(7) == Cached(ax::layout::kPbs) && words[9] == 5,
          "SETUP, ADD_TO_LR, PROCESS and command 5");
    Check(
        words[10] == 11 && words[11] == 32768 && words[12] == 10 && word32(13) == Cached(ax::layout::kCompressorTable),
        "the October 2006 COMPRESSOR");
    Check(words[15] == 14 && word32(16) == Cached(ax::layout::kRemoteBuffers), "the October 2006 WM_OUTPUT");
    Check(words[24] == 12 && word32(25) == Cached(ax::layout::kSurroundBuffer) &&
              word32(27) == Cached(ax::layout::kOutputBuffers + 384) && words[29] == 15,
          "the October 2006 OUTPUT, without a volume, and END");

    // 0x120-byte PBs, each with the address of its update buffer.
    for (uint32_t i = 0; i < ax::kVoiceCountOctober2006; i++)
    {
        const uint8_t* bytes = memory.At(ax::layout::kPbs + i * 0x120, ax::kPbSizeOctober2006);
        const ax::Pb pb = ax::LoadPbOctober2006(bytes);
        const uint32_t next = i == ax::kVoiceCountOctober2006 - 1 ? 0 : Cached(ax::layout::kPbs + (i + 1) * 0x120);
        Check(ax::Join(pb.next_hi, pb.next_lo) == next &&
                  ax::Join(pb.curr_hi, pb.curr_lo) == Cached(ax::layout::kPbs + i * 0x120),
              "an October 2006 PB's links");
        Check(xsflib::LoadBe32(bytes + 0x58) == Cached(ax::layout::kUpdateBuffers + i * 0x100) &&
                  xsflib::LoadBe16(bytes + 0x52) == 0,
              "an October 2006 PB's update block");
    }
}

void TestOctober2006Voices()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);
    ax.Init(ax::Build::kOctober2006);

    // The build of October 2006 has 64 voices.
    std::vector<ax::Voice*> voices;
    for (int i = 0; i < ax::kVoiceCountOctober2006; i++)
    {
        voices.push_back(ax.AcquireVoice(16, nullptr));
    }

    Check(ax.VoiceCount() == 64, "64 voices in the build of October 2006");
    Check(ax.AcquireVoice(16, nullptr) == nullptr, "no 65th voice");

    // A voice's changes land at that layout's offsets. The update block stays.
    ax::Voice* voice = voices[0];
    ax::PbAddr address;
    address.format = static_cast<uint16_t>(ax::SampleFormat::kPcm16);
    address.current_address_hi = 0x0800;
    voice->SetAddr(address);
    voice->pb.mix.l = 0x4000;
    voice->pb.remote = 1;
    voice->sync |= ax::sync::kMix | ax::sync::kRemote;
    voice->SetState(ax::VoiceState::kRun);

    ax.StartFrame();

    const uint8_t* bytes = memory.At(ax::layout::kPbs + voice->index * 0x120, ax::kPbSizeOctober2006);
    Check(xsflib::LoadBe16(bytes + 0x10) == 1 && xsflib::LoadBe16(bytes + 0x14) == 0x4000 &&
              xsflib::LoadBe16(bytes + 0x84) == 0x0800 && xsflib::LoadBe16(bytes + 0xa8) == 0x800 &&
              xsflib::LoadBe16(bytes + 0xd4) == 1,
          "a voice's changes in its October 2006 PB");
    Check(xsflib::LoadBe32(bytes + 0x58) == Cached(ax::layout::kUpdateBuffers + voice->index * 0x100),
          "the update block left as it was");
}

void TestOctober2006Cycles()
{
    ntdlib::dsp::WiiMemory memory;
    ax::System ax(memory);
    ax.Init(ax::Build::kOctober2006);

    // The build of October 2006 counts more cycles for a voice. Fewer voices fit in a frame.
    std::vector<uint32_t> dropped;
    const auto on_drop = [&dropped](ax::Voice* voice)
    {
        dropped.push_back(voice->index);
    };
    for (int i = 0; i < ax::kVoiceCountOctober2006; i++)
    {
        ax::Voice* voice = ax.AcquireVoice(16, on_drop);
        voice->pb.src.ratio_hi = 2;
        voice->pb.SetMixerCtrl(0xffffffff);
        voice->SetState(ax::VoiceState::kRun);
    }

    ax.StartFrame();
    ax.FinishFrame();

    // Each voice takes (264 + 728) * 6 cycles to convert and 4 * 3735 + 140 to mix. 14 of them fit.
    Check(dropped.size() == 50, "the voices that the build of October 2006 drops");
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestInit();
        TestAllocation();
        TestSync();
        TestCycleBudget();
        TestOctober2006Layout();
        TestOctober2006Voices();
        TestOctober2006Cycles();
    };
    return xsflib::testing::RunTest("Wii AX", tests);
}
