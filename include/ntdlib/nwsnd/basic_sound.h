// SPDX-License-Identifier: MIT

// The parameters that nwsnd's players of sequences, wave sounds and streams share (BasicSoundPlayer in nw::snd and
// nn::atk).

#pragma once

#include <cstdint>

namespace ntdlib::nwsnd
{

// What BasicSound gives a player from the sound's info, and what the game sets (BasicSoundPlayer's parameters). Each
// offset is the field's offset in a sequence's player.
template <typename Policy>
struct BasicSoundParams
{
    float volume = 1.0f;              // +0x2C
    float pitch = 1.0f;               // +0x30
    float lpf = 0.0f;                 // +0x34
    float biquad_value = 0.0f;        // +0x38
    int8_t biquad_type = -1;          // +0x3C: replaces the type of the tracks or the sound when 0 or more
    uint32_t pan_mode = 0;            // +0x40
    uint32_t pan_curve = 0;           // +0x44
    uint32_t output_line = 1;         // +0x48
    typename Policy::Outputs outputs; // +0x50
    const typename Policy::Receiver* receiver = nullptr;
};

} // namespace ntdlib::nwsnd
