// SPDX-License-Identifier: MIT

// Checks of a list of Donkey Kong Jungle Beat's chord layers (ntdlib/jaudio/jungle_beat_states.h), for the tests of the
// lists and of the game profiles that use them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "xsflib/testing/check.h"

#include "ntdlib/jaudio/jungle_beat_states.h"

namespace ntdlib::testing
{

// Checks the chord layers `layers`, at most as many as the driver's table holds. Each belongs to a piece of music,
// 0x01000000 plus an index below 0x100, and names a stage and one or two records of the chord database below 0x100.
// Its sound ID gives back its index and its piece. A piece's layers have different suffixes, and none is the suffix of
// one of `layer_states`. Each message starts with `name`.
inline void CheckChordLayers(const std::string& name, std::span<const jaudio::JungleBeatChordLayer> layers,
                             std::span<const jaudio::JungleBeatState> layer_states)
{
    using xsflib::testing::Check;
    Check(layers.size() <= jaudio::kMaxChordLayers, name + ": the number of chord layers");
    for (std::size_t i = 0; i < layers.size(); i++)
    {
        const jaudio::JungleBeatChordLayer& layer = layers[i];
        const std::string what = name + ": chord layer " + std::to_string(i);
        Check((layer.piece & 0xffffff00) == 0x01000000 && layer.stage != 0 && layer.stage < 0x100 &&
                  layer.record != 0 && layer.record < 0x100 && layer.second < 0x100 && layer.suffix && *layer.suffix,
              what);

        // The sound ID gives the index back, in bits 16 to 22 and bit 15, and the piece's ID.
        const uint32_t id = jaudio::ChordLayerId(layer.piece, static_cast<uint32_t>(i));
        Check(((id >> 16 & 0x7f) | (id >> 8 & 0x80)) == i &&
                  (id & ~(jaudio::kChordLayer | 0x7f0000u | jaudio::kChordLayerHigh)) == layer.piece,
              what + ": its sound ID");
        for (std::size_t j = 0; j < i; j++)
        {
            Check(layers[j].piece != layer.piece || std::string(layers[j].suffix) != layer.suffix,
                  what + ": its suffix");
        }

        for (const jaudio::JungleBeatState& state : layer_states)
        {
            Check(state.piece != layer.piece || std::string(state.suffix) != layer.suffix,
                  what + ": its suffix and the states'");
        }
    }
}

} // namespace ntdlib::testing
