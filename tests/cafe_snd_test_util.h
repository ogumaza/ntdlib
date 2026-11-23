// SPDX-License-Identifier: MIT

// What the tests of the Wii U's model of nw::snd share: the options of the archives' files, the bits of a float,
// a looping archive with two tracks, a standard reverb on the TV's aux bus A, and the frames that a model renders.

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "nwsnd_engine_files.h"

namespace cafe_snd_tests
{

const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kCafe, {}, false, ntdlib::sar::Rules::kStopAtFirstProblem};

using ntdlib::cafe::snd::AxVoice;
using ntdlib::cafe::snd::AxVoices;
using ntdlib::cafe::snd::EffectSetup;
using ntdlib::cafe::snd::EffectType;
using ntdlib::cafe::snd::OutputMode;
using ntdlib::cafe::snd::WaveBuffer;
using nwsnd_engine_files::ConstantPcmWave;
using nwsnd_engine_files::ConstantWave;
using nwsnd_engine_files::LoopingAdpcmWave;
using nwsnd_engine_files::PcmStream;
using nwsnd_engine_files::PlainWaveSound;
using nwsnd_engine_files::ProgramArchive;
using nwsnd_engine_files::StreamArchive;
using nwsnd_engine_files::StreamTrack;
using nwsnd_engine_files::WaveSoundArchive;

// The Wii U's files and samples are big-endian.
constexpr bool kBigEndian = true;

inline uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

namespace snd = ntdlib::cafe::snd;

// An archive whose sequence plays two tracks that loop for ever. Track 0 plays a looping stereo wave in DSP-ADPCM at
// 32 kHz, in notes of random lengths from 12 to 48 ticks, and sends to aux bus A. Track 1 plays a loud wave of 21 ms
// that doesn't loop, 7 semitones up. Each note sounds for 14 ms, until the wave's end. Without `track_1_plays`, track 1
// rests for each of its notes' lengths instead. `send` is track 0's send to aux bus A.
inline ntdlib::sar::SoundArchive LoopingArchive(bool track_1_plays = true, uint8_t send = 80)
{
    std::vector<uint8_t> tracks = {
        0xFE, 0x00, 0x03,                       // tracks 0 and 1
        0xB0, 48,   0xE1, 0x00, 120,            // a timebase of 48, 120 beats a minute
        0x88, 0x01, 0x00, 0x00, 0x1F,           // track 1 at 0x1F
        0xD9, send,                             // track 0 sends to aux bus A
        0xA0, 60,   100,  0x00, 12,   0x00, 48, // 0x0F: key 60 at a random length
        64,   80,   24,                         // key 64
        0x80, 8,                                // a rest
        0x89, 0x00, 0x00, 0x0F,                 // back to 0x0F
        0x81, 1,                                // 0x1F: track 1 plays program 1
        0xC1, 127,                              // at full volume
        67,   127,  48,                         // 0x23: key 67
        0x80, 24,                               // a rest
        0x89, 0x00, 0x00, 0x23,                 // back to 0x23
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

// Options with a standard reverb on the TV's aux bus A.
inline snd::EngineOptions ReverbOptions()
{
    std::vector<uint32_t> params = {Bits(0.01f), Bits(2.2f),  Bits(0.6f), Bits(0.3f), Bits(1.0f),
                                    4,           Bits(0.05f), 2,          Bits(0.3f), Bits(0.55f)};
    snd::EngineOptions options;
    options.effects = {{snd::kEffectDeviceTv, 0, EffectType::kReverbStd, params, {}}};

    return options;
}

// The output of the next `frames` frames.
inline std::vector<int16_t> RunFrames(snd::ArchiveModel& model, int frames)
{
    std::vector<int16_t> output;
    for (int i = 0; i < frames; ++i)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.system.Output();
        output.insert(output.end(), frame.begin(), frame.end());
    }

    return output;
}

// Whether any sample isn't 0.
inline bool Sounds(std::span<const int16_t> output)
{
    return std::any_of(output.begin(), output.end(), [](int16_t sample) { return sample != 0; });
}

} // namespace cafe_snd_tests
