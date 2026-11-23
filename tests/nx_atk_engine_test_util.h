// SPDX-License-Identifier: MIT

// What the tests of the model of nn::atk on the Switch's renderer share: the options of the archives' files, a
// reverb like the reference game's stages use, a looping archive with two tracks, and the frames that a model renders
// and their peaks.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/atk/sequence.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/sar/formats.h"
#include "nwsnd_engine_files.h"

namespace nx_atk_engine_tests
{

namespace atk = ntdlib::nx::atk;
namespace audio = ntdlib::nx::audio;

using nwsnd_engine_files::ConstantPcmWave;
using nwsnd_engine_files::ConstantWave;
using nwsnd_engine_files::LoopingAdpcmWave;
using nwsnd_engine_files::PcmStream;
using nwsnd_engine_files::PlainWaveSound;
using nwsnd_engine_files::ProgramArchive;
using nwsnd_engine_files::StreamArchive;
using nwsnd_engine_files::StreamTrack;
using nwsnd_engine_files::WaveSoundArchive;

// The Switch's files and samples are little-endian.
constexpr bool kBigEndian = false;

// How the engine reads the Switch's archives: with the rules that stop at the first problem.
const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kNx, {}, false, ntdlib::sar::Rules::kStopAtFirstProblem};

// A reverb like the ones the reference game's stages use.
constexpr audio::I3dl2ReverbParameterSet kStageReverb{-1000.0f, -1000.0f, 2.0f,  0.8f, -700.0f, 0.005f,
                                                      -500.0f,  0.04f,    50.0f, 3.0f, 6000.0f, 0.25f};

// Whether a frame's output has any sound.
inline bool Sounds(std::span<const int16_t> output)
{
    for (const int16_t sample : output)
    {
        if (sample != 0)
        {
            return true;
        }
    }

    return false;
}

// The output of the model's next `frames` frames.
inline std::vector<int16_t> RunFrames(atk::ArchiveModel& model, int frames)
{
    std::vector<int16_t> output;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.renderer.DeviceOutput();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// An archive whose sequence plays two tracks that loop for ever. Track 0 plays a looping stereo wave in DSP-ADPCM at
// 32 kHz, in notes of random lengths from 12 to 48 ticks, and sends to aux bus A. Track 1 plays a loud wave of 21 ms
// that doesn't loop. Its voices stop at the wave's end, and the renderer's depop fades their last samples out. Without
// `track_1_plays`, track 1 rests for each of its notes' lengths instead. `send` is track 0's send to aux bus A.
inline ntdlib::sar::SoundArchive LoopingArchive(bool track_1_plays = true, uint8_t send = 80)
{
    std::vector<uint8_t> tracks = {
        0xFE, 0x03, 0x00,                       // tracks 0 and 1
        0xB0, 48,   0xE1, 120,  0x00,           // a timebase of 48, 120 beats a minute
        0x88, 0x01, 0x1F, 0x00, 0x00,           // track 1 at 0x1F
        0xD9, send,                             // track 0 sends to aux bus A
        0xA0, 60,   100,  12,   0x00, 48, 0x00, // 0x0F: key 60 at a random length
        64,   80,   24,                         // key 64
        0x80, 8,                                // a rest
        0x89, 0x0F, 0x00, 0x00,                 // back to 0x0F
        0x81, 1,                                // 0x1F: track 1 plays program 1
        0xC1, 127,                              // at full volume
        67,   127,  48,                         // 0x23: key 67
        0x80, 24,                               // a rest
        0x89, 0x23, 0x00, 0x00,                 // back to 0x23
    };
    if (!track_1_plays)
    {
        // A rest of 48 ticks takes the place of key 67. The jump back to 0x23 still reaches it.
        tracks.erase(tracks.begin() + 0x23, tracks.begin() + 0x26);
        tracks.insert(tracks.begin() + 0x23, {0x80, 48});
    }

    return ntdlib::sar::SoundArchive::Load(
        ProgramArchive(kBigEndian, tracks, {LoopingAdpcmWave(), ConstantPcmWave(kBigEndian, 1000, 0x7000)}, 0x3),
        kSarOptions);
}

// Options with the reverb of the reference game's stages on aux bus A.
inline atk::EngineOptions StageReverbOptions()
{
    atk::EngineOptions options;
    options.aux_reverbs[0] = kStageReverb;

    return options;
}

// The largest magnitudes of an output frame's left and right samples.
inline std::array<int32_t, 2> PeaksOf(std::span<const int16_t> frame)
{
    std::array<int32_t, 2> peaks = {0, 0};
    for (std::size_t t = 0; t < frame.size(); ++t)
    {
        peaks[t % 2] = std::max(peaks[t % 2], std::abs(static_cast<int32_t>(frame[t])));
    }

    return peaks;
}

} // namespace nx_atk_engine_tests
