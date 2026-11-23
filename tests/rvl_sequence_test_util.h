// SPDX-License-Identifier: MIT

// What the tests of the Wii's model of NW4R SND share: the builds that they play under, an archive of a sequence, the
// AX model and the engine on it, a wave without loops, a voice that plays it, and the first frame of a sequence.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/sequence.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/testing/rsar_files.h"

namespace rvl_sequence_tests
{

using namespace ntdlib::rvl;
using ntdlib::testing::rsar_files::Bytes;
using xsflib::testing::Check;

// Builds of NW4R SND: those of Wii Sports, Mario Kart Wii, Wii Music and Animal Crossing, the builds of November 20
// 2008 and September 24 2009, and the one that comes with AX of October 2006.
constexpr snd::SndBuild kBuild2007{20070608};
constexpr snd::SndBuild kBuildMarch2008{20080308};
constexpr snd::SndBuild kBuildJune2008{20080620};
constexpr snd::SndBuild kBuildSeptember2008{20080919};
constexpr snd::SndBuild kBuildNovember2008{20081120};
constexpr snd::SndBuild kBuildSeptember2009{20090924};
constexpr snd::SndBuild kBuildOctober2006{0, true};

// An archive whose sequence sound has the tracks in the mask `tracks` and plays track 0 from the start of `commands`.
// The archive's waves loop unless `loop` is false.
inline ntdlib::rsar::SoundArchive MakeArchive(const Bytes& commands, uint32_t tracks, bool loop = true)
{
    return ntdlib::rsar::SoundArchive::Load(
        ntdlib::testing::rsar_files::MakeArchive(ntdlib::testing::rsar_files::MakeSequence(commands), 0, tracks, loop),
        snd::kRsarOptions);
}

// AX, initialised as the game does before NW4R SND starts, with a compressor table of zeros. The build of NW4R SND that
// comes with AX of October 2006 runs on that AX.
inline ax::System& Initialised(ax::System& ax, snd::SndBuild build)
{
    ax.Init(build.October2006() ? ax::Build::kOctober2006 : ax::Build::kDecember2006);

    return ax;
}

// The AX model and the engine on it. The engine follows `build` of NW4R SND, with the pools of `pools`. A frame runs as
// in the game: AX's interrupt, then the sound frame.
struct Model
{
    explicit Model(const Bytes& commands = {0xff}, snd::SndBuild build = {}, uint32_t tracks = 1,
                   const snd::EngineOptions& pools = {})
        : archive(MakeArchive(commands, tracks)), engine{memory, Initialised(ax, build), archive, build, pools}
    {
    }

    void RunFrame()
    {
        ax.StartFrame();
        ax.FinishFrame();
        engine.FrameProcess();
    }

    // The PB of the first voice that AX hands out: the last of its voices.
    ax::Pb FirstPb() const
    {
        return ax.ReadPb(ax.VoiceCount() - 1);
    }

    // Runs `frames` frames, then returns the first voice's PB as the microcode reads it in the next.
    ax::Pb PbAfter(int frames)
    {
        for (int i = 0; i < frames; i++)
        {
            RunFrame();
        }

        ax.StartFrame();
        const ax::Pb next = FirstPb();
        ax.FinishFrame();
        engine.FrameProcess();

        return next;
    }

    // Runs frames until the sound ends, at most `frames` of them. Returns the number of frames run.
    int RunUntilEnd(int frames)
    {
        int ran = 0;
        while (engine.IsBusy() && ran < frames)
        {
            RunFrame();
            ran++;
        }

        return ran;
    }

    ntdlib::dsp::WiiMemory memory;
    ax::System ax{memory};
    ntdlib::rsar::SoundArchive archive;
    snd::Engine engine;
};

// A second of a 32 kHz PCM16 mono wave that doesn't loop.
inline snd::WaveData MakeWave(ntdlib::dsp::WiiMemory& memory)
{
    const std::vector<uint8_t> kSamples(64000, 0);
    snd::WaveData wave;
    wave.format = ntdlib::rsar::WaveEncoding::kPcm16;
    wave.sample_rate = 32000;
    wave.loop_end = 32000;
    wave.channels[0].data = memory.Store(kSamples.data(), kSamples.size());

    return wave;
}

// An AX voice of priority 31 with MakeWave's wave, acquired after the first frame of `model`.
inline snd::AxVoice& WaveVoice(Model& model)
{
    model.RunFrame();
    const snd::WaveData wave = MakeWave(model.memory);
    snd::AxVoice* voice = model.engine.AxVoices().AcquireAxVoice(31, nullptr);
    voice->Setup(wave.channels[0].data, ax::SampleFormat::kPcm16, 32000);
    voice->SetAddr(false, wave.channels[0].data, 0, wave.loop_end);

    return *voice;
}

// The voices' PBs after the first frame of a sequence of `commands` played under `build`, and the model to play on.
struct Started
{
    Started(const Bytes& commands, snd::SndBuild build) : model(commands, build)
    {
        player = model.engine.StartSequence(0);
        pb = model.PbAfter(1);
    }

    Model model;
    snd::SeqPlayer* player = nullptr;
    ax::Pb pb;
};

} // namespace rvl_sequence_tests
