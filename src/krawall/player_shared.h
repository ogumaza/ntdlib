// SPDX-License-Identifier: MIT

// What the files of the Krawall player share: the effects of the effect column and the volume column, and the BIOS
// division.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "xsflib/bytes.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/krawall/build.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"

namespace ntdlib::krawall
{

// The effects of the effect column, as the module's patterns number them.
enum EffectNumber : int
{
    kSpeed = 1,
    kBpm = 2,
    kSpeedBpm = 3,
    kPatternJump = 4,
    kPatternBreak = 5,
    kVolumeSlideS3m = 6,
    kVolumeSlideXm = 7,
    kVolumeSlideDownFine = 8,
    kVolumeSlideUpFine = 9,
    kPortaDownXm = 10,
    kPortaDownS3m = 11,
    kPortaDownFine = 12,
    kPortaDownExtraFine = 13,
    kPortaUpXm = 14,
    kPortaUpS3m = 15,
    kPortaUpFine = 16,
    kPortaUpExtraFine = 17,
    kVolume = 18,
    kPortaNote = 19,
    kVibrato = 20,
    kTremor = 21,
    kArpeggio = 22,
    kVolumeSlideVibrato = 23,
    kVolumeSlidePorta = 24,
    kChannelVolume = 25,
    kChannelVolumeSlide = 26,
    kOffset = 27,
    kPanSlide = 28,
    kRetrig = 29,
    kTremolo = 30,
    kFineVibrato = 31,
    kGlobalVolume = 32,
    kGlobalVolumeSlide = 33,
    kPan = 34,
    kPanbrello = 35,
    kMark = 36,
    kGlissando = 37,
    kVibratoWave = 38,
    kTremoloWave = 39,
    kPanbrelloWave = 40,
    kPatternLoop = 43,
    kNoteCut = 44,
    kNoteDelay = 45,
    kVolumeSlideVibratoXm = 49,
    kVolumeSlidePortaXm = 50,
    kEffectCount = 51,
};

// The volume column's effects, by the column's high digit from 6.
enum ColumnEffect : int
{
    kColumnVolumeSlideDown = 0,
    kColumnVolumeSlideUp = 1,
    kColumnFineVolumeDown = 2,
    kColumnFineVolumeUp = 3,
    kColumnVibratoSpeed = 4,
    kColumnVibrato = 5,
    kColumnPan = 6,
    kColumnPanSlideLeft = 7,
    kColumnPanSlideRight = 8,
    kColumnPortaNote = 9,
};

// Returns the quotient and remainder from the BIOS's signed Div operation. For a zero divisor, the player skips the
// call and uses `n` and 0.
inline std::pair<int32_t, int32_t> Div(int32_t n, int32_t d)
{
    if (d == 0)
    {
        return {n, 0};
    }
    if (n == INT32_MIN && d == -1)
    {
        return {n, 0};
    }

    return {n / d, n % d};
}

} // namespace ntdlib::krawall
