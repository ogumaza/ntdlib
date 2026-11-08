// SPDX-License-Identifier: MIT

// The effects on Skyward Sword HD's aux buses. The game's port of NW4R SND keeps AxManager's lists of effects
// (0x2c6f930). In place of AXFX's reverbs, it has a reverb class (virtual table 0x1699d38) on nn::audio's reverb for
// the CPU (audio::AuxReverb). The game holds 8 such reverbs (0x4cbd260). The class runs only in NW4R SND's DPL2 output
// mode, and the layer sets that mode as it starts (0x1026ec0). Each frame of the render thread then runs the reverbs
// of a bus in place on its 4 channels: 160 samples of each at 32 kHz.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/nx/audio/reverb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/nx/layer.h"

namespace ntdlib::rvl::nx
{

// The parameters that the game gives the reverb class's SetParam (0x10104e8), in their order there. The times are in
// seconds. The fused time is the decay time, the damping is the decay ratio of the high frequencies, and the fused
// gain is the late reverb's gain.
struct ReverbParam
{
    float predelay_time = 0.0f;
    float fused_time = 0.0f;
    float coloration = 0.0f;
    float damping = 0.0f;
    float out_gain = 0.0f;
    int32_t early_mode = 0;
    float predelay_time_max = 0.0f;
    int32_t fused_mode = 0;
    float early_gain = 0.0f;
    float fused_gain = 0.0f;
};

// The reverb that the game puts on aux bus A when a scene gives no type of room (0xf761c0 and 0xf75450), and the
// reverb on aux bus B (0xf32878 at start-up, and 0xf2b000).
inline constexpr ReverbParam kBusAReverb = {0.02f, 3.0f, 0.6f, 0.4f, 1.0f, 7, 0.02f, 4, 0.0f, 1.0f};
inline constexpr ReverbParam kBusBReverb = {0.1f, 2.0f, 0.2f, 0.7f, 1.0f, 4, 0.15f, 4, 0.3f, 1.0f};

// The reverb's parameters that the class gives nn::audio for `param` (SetParam, then StartUp at 0x1010a78 to
// 0x1010b90). SetParam holds the gains, the coloration and the damping to 0 to 1, the times to 0 and up, and the
// pre-delay to the most. An early mode of 5 to 8 becomes a cathedral (3), and any other mode a hall (2): the switch
// falls through. The fused mode picks the late mode from a table (0x13a8130). One outside 0 to 5 becomes a plate (2).
// The coloration is divided by 0.6 and held to 1. The reverb gain is 1, and the dry gain 0.
ntdlib::nx::audio::ReverbParameterSet ReverbParameters(const ReverbParam& param);

class Effects
{
public:
    // AxManager::AppendEffect (0x100e7e8) of a reverb with `param` on bus `bus` (0 or 1). The bus's return volume
    // becomes full. The reverb starts up on the first of the game's reverbs that no other takes: once made, a reverb
    // starts again with its delay lines cleared. A bus whose list was empty gets AxManager's callback (0x100e994) on
    // `layer`. The callback then gives silence for two frames.
    void AppendReverb(ax::System& ax, Layer& layer, int bus, const ReverbParam& param);

    // Whether a bus has an effect.
    bool Any() const
    {
        return !lists_[0].empty() || !lists_[1].empty();
    }

    // AxManager::AuxCallbackFunc (0x100e994) in the DPL2 mode, the callback that the layer runs on each frame of bus
    // `bus`: silence while the bus waits, or while it has no effects. Otherwise each effect's UpdateBuffer (0x1010cb8)
    // runs nn::audio's reverb on the bus's samples in place.
    void AuxCallback(int bus, std::span<int32_t, Layer::kAuxSamples> samples);

private:
    // The game's reverbs, in the order the objects first took them, and whether an object holds each.
    std::vector<ntdlib::nx::audio::AuxReverb> slots_;
    std::vector<bool> slot_used_;

    // AxManager's lists of effects: the game's reverb that each object runs on. And the frames that each bus still
    // waits.
    std::array<std::vector<std::size_t>, 2> lists_;
    std::array<int, 2> wait_counters_{};
};

} // namespace ntdlib::rvl::nx
