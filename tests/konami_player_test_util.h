// SPDX-License-Identifier: MIT

// What the tests of Konami's player share: the timing of the test carts' output, the carts and the runs of the player.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "xsflib/binary.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"
#include "konami_output_image.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/archive_source.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/wave/blep.h"

namespace konami_player_tests
{

using namespace konami_testing;

using ntdlib::agb::kFrameCycles;
using ntdlib::agb::Rom;
using ntdlib::konami::ArchivePlayer;
using ntdlib::konami::ArchivePlayerOptions;
using ntdlib::konami::kTracks;
using ntdlib::konami::Output;

constexpr uint32_t kPoint = 1254;
constexpr uint32_t kStore = kFrameCycles / 6;
constexpr size_t kEnhanced = ntdlib::agb::kEnhancedFrameSamples;

// A block mixer's new byte waits behind the 3 blocks that its mixer and DMA hold back. Before the first stores' bytes,
// the FIFOs hold the zeros of the cycles up to the stores, and of that lead.
constexpr uint32_t kBlockSilence = kStore / kPoint + 48;

// A ring mixer's new byte plays once the DMA comes round the ring of 704 points. The ring's DMA stands 224 points, a
// frame's, ahead of the byte that the first stores mix.
constexpr uint32_t kRingSilence = kStore / kPoint + 704 - 224;

// In the enhanced mode, the silence before the first stores' samples: the same time at 804 samples a frame, from the
// next sample on, and the half width of the PSG's band-limited steps. The hardware model holds the FIFOs back by that
// half width, to keep them in time with the steps.
constexpr size_t kEnhancedStart = size_t((uint64_t(kStore + 48 * kPoint) * kEnhanced + kFrameCycles) / kFrameCycles) +
                                  size_t(ntdlib::wave::BandLimitedSteps::kHalfWidth);

template <typename E, typename F>
inline bool Throws(F f)
{
    try
    {
        f();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

inline bool AllAre(const std::vector<int8_t>& bytes, int value)
{
    return std::all_of(bytes.begin(), bytes.end(), [value](int8_t b) { return b == value; });
}

// A sample looped from its start, of one value throughout.
inline std::vector<int8_t> Constant(int value)
{
    return std::vector<int8_t>(16, int8_t(value));
}

// A block mixer's song: track 4 plays a looped sample of `first`, and track 5 one of `second`, each with a level of
// 128. Such a level gives each byte as it stands, and a level in the enhanced mode of 1.
inline Rom TwoVoices(int first, int second)
{
    Image m;
    m.Mixer(kRecords24);
    m.Song(0, kBase, 0x100, {{4, 0x00}, {5, 0x10}}, Revision::kUltimateMasters);
    m.PutBytes(kBase, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xA8, 0x10, 0x01, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x1000, 16, 0, Constant(first));
    m.Sample(1, 0x1000, 16, 0, Constant(second));
    return m.ToRom();
}

inline DriverInfo Blocks()
{
    return InfoOf(Revision::kUltimateMasters);
}

// A sample of a whole sine in 64 points, looped.
inline Rom Sine(uint32_t step)
{
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    std::vector<int8_t> bytes;
    for (int i = 0; i < 64; i++)
    {
        bytes.push_back(int8_t(std::lround(100.0 * std::sin(2.0 * 3.14159265358979 * i / 64.0))));
    }
    m.Sample(0, step, 64, 0, bytes);
    return m.ToRom();
}

inline std::vector<int16_t> Frames(ArchivePlayer& player, int frames)
{
    std::vector<int16_t> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

inline std::vector<float> EnhancedFrames(ArchivePlayer& player, int frames)
{
    std::vector<float> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

inline ArchivePlayerOptions EnhancedOptions()
{
    ArchivePlayerOptions options;
    options.enhanced = true;
    return options;
}

inline bool Near(float value, double expected, double within = 0.002)
{
    const bool near = std::fabs(double(value) - expected) <= within;
    if (!near)
    {
        std::printf("%f, expected %f\n", double(value), expected);
    }

    return near;
}

} // namespace konami_player_tests
