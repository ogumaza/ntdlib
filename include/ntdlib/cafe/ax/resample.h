// SPDX-License-Identifier: MIT

// Sample-rate conversion (AXPBSRC): how the renderer makes each output sample from a voice's last four samples and its
// position between them. AXSetVoiceSrcType's types 1 to 4 select linear interpolation or one of three 4-tap filters,
// for 8, 12 and 16 kHz. Type 0 takes the samples as they are, and doesn't come here. Both run as in the microcode (see
// render.h). The filters' coefficients are in the DSP's coefficient ROM (ntdlib/dsp/coef_rom.h).

#pragma once

#include <array>
#include <cstdint>

#include "ntdlib/cafe/ax/pb.h"

namespace ntdlib::cafe::ax
{

// The output sample for a position `frac` (0.16) among the voice's last four samples, `last`, oldest first. Linear
// interpolation (0x0683) goes from the oldest sample to the second oldest. With `newest`, the interpolation goes from
// the second newest to the newest instead. Cemu's does that for ADPCM and PCM8 (cemu_departures.h). The 4-tap filter
// `filter` (0x0630; 0, 1 or 2: 8, 12 or 16 kHz, when `mode` is kFourTap) centres on the middle two.
int16_t Resample(SrcMode mode, uint16_t filter, const std::array<int16_t, 4>& last, uint32_t frac, bool newest = false);

} // namespace ntdlib::cafe::ax
