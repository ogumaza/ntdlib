// SPDX-License-Identifier: MIT

// What the tests of Ubisoft Milan's player share: the test cartridge, its loops and samples, and the check for an
// exception.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "xsflib/binary.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/ubimilan_image.h"
#include "ntdlib/ubimilan/archive_player.h"
#include "ntdlib/ubimilan/archive_source.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/output.h"

namespace ubimilan_player_tests
{

namespace agb = ntdlib::agb;
using agb::Rom;
using ntdlib::testing::ubimilan_image::TestImage;
using ntdlib::ubimilan::ArchivePlayer;
using ntdlib::ubimilan::ArchivePlayerOptions;
using ntdlib::ubimilan::DriverInfo;

constexpr int kKitTrack = 9;
constexpr int kByteSample = 256;

// The bytes of a sample of `points`, all `point`, with a loop over its whole length.
inline int ConstantLoop(TestImage& image, int point, uint32_t points = 256)
{
    return image.AddSample(std::vector<int8_t>(points, int8_t(point)), true, 0, points);
}

// A loop of a sine of `amplitude`, in `period` points of 8 at a time.
inline int SineLoop(TestImage& image, int amplitude, uint32_t period)
{
    std::vector<int8_t> points;
    for (uint32_t i = 0; i < period; i++)
    {
        points.push_back(int8_t(std::lround(amplitude * std::sin(2 * std::numbers::pi * i / period))));
    }

    return image.AddSample(points, true, 0, period);
}

struct Cart
{
    explicit Cart(const TestImage& image) : rom(image.ToRom())
    {
        std::string error;
        XSFLIB_CHECK(ntdlib::ubimilan::DetectDriver(rom, {}, info, error));
    }

    Rom rom;
    DriverInfo info;
};

// A cart whose one piece plays a kit note of the level `level`, and square 1's key 60.
inline TestImage KitAndSquare(int level)
{
    TestImage image;
    image.SetKey(40, ConstantLoop(image, level));
    image.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    image.AddPiece(1, {image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x00, 0x00, 0x90, 60, 0xF0, 0xFF, 0x00, 0xFC})});
    return image;
}

template <typename Exception, typename Function>
inline bool Throws(Function function)
{
    try
    {
        function();
    }
    catch (const Exception&)
    {
        return true;
    }

    return false;
}

} // namespace ubimilan_player_tests
