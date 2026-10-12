// SPDX-License-Identifier: MIT

// The DSP for each console's memory. The DSP's sources build as one unity build, and this file comes last in it. Every
// member's definition is then visible here, and these lines instantiate all of them.

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"

namespace ntdlib::dsp
{

template class Dsp<WiiMemory>;
template class Dsp<GameCubeMemory>;
template class Dsp<WiiUMemory>;

} // namespace ntdlib::dsp
