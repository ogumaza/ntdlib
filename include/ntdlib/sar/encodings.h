// SPDX-License-Identifier: MIT

// The wave encodings of the DS's sound archives (ntdlib::sdat), the Wii's (ntdlib::rsar) and those of the 3DS, the Wii
// U and the Switch (ntdlib::sar), each as another's. The later consoles' encodings cover the other two: PCM8, PCM16,
// DSP-ADPCM, and the 3DS's IMA-ADPCM. The DS plays no DSP-ADPCM, and the Wii no IMA-ADPCM.

#pragma once

#include <optional>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::sar
{

// The later consoles' encoding of `encoding`, a DS wave's encoding. A value outside the enum gives IMA-ADPCM.
constexpr WaveEncoding EncodingOf(sdat::WaveEncoding encoding)
{
    switch (encoding)
    {
    case sdat::WaveEncoding::kPcm8:
        return WaveEncoding::kPcm8;
    case sdat::WaveEncoding::kPcm16:
        return WaveEncoding::kPcm16;
    case sdat::WaveEncoding::kImaAdpcm:
        break;
    }

    return WaveEncoding::kImaAdpcm;
}

// The later consoles' encoding of `encoding`, a Wii wave's encoding. A value outside the enum gives PCM16.
constexpr WaveEncoding EncodingOf(rsar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case rsar::WaveEncoding::kPcm8:
        return WaveEncoding::kPcm8;
    case rsar::WaveEncoding::kPcm16:
        return WaveEncoding::kPcm16;
    case rsar::WaveEncoding::kDspAdpcm:
        return WaveEncoding::kDspAdpcm;
    }

    return WaveEncoding::kPcm16;
}

} // namespace ntdlib::sar

namespace ntdlib::sdat
{

// The DS's encoding of `encoding`, an encoding of the later consoles. Nothing for DSP-ADPCM, which the DS doesn't play,
// and for a value outside the enum.
constexpr std::optional<WaveEncoding> EncodingOf(sar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case sar::WaveEncoding::kPcm8:
        return WaveEncoding::kPcm8;
    case sar::WaveEncoding::kPcm16:
        return WaveEncoding::kPcm16;
    case sar::WaveEncoding::kImaAdpcm:
        return WaveEncoding::kImaAdpcm;
    case sar::WaveEncoding::kDspAdpcm:
        break;
    }

    return std::nullopt;
}

} // namespace ntdlib::sdat

namespace ntdlib::rsar
{

// The Wii's encoding of `encoding`, an encoding of the later consoles. Nothing for IMA-ADPCM, which the Wii doesn't
// play, and for a value outside the enum.
constexpr std::optional<WaveEncoding> EncodingOf(sar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case sar::WaveEncoding::kPcm8:
        return WaveEncoding::kPcm8;
    case sar::WaveEncoding::kPcm16:
        return WaveEncoding::kPcm16;
    case sar::WaveEncoding::kDspAdpcm:
        return WaveEncoding::kDspAdpcm;
    case sar::WaveEncoding::kImaAdpcm:
        break;
    }

    return std::nullopt;
}

} // namespace ntdlib::rsar
