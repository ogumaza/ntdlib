// SPDX-License-Identifier: MIT

#include "ntdlib/commands/sequence.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace ntdlib::commands
{
namespace
{

// The kinds' names, in the order of Kind.
constexpr std::array<std::string_view, kKindCount> kNames = {
    "note",
    "wait",
    "program",
    "open_track",
    "jump",
    "call",
    "return",
    "loop_start",
    "loop_end",
    "end",
    "allocate_tracks",
    "timebase",
    "envelope_hold",
    "monophonic",
    "velocity_range",
    "biquad_type",
    "biquad_value",
    "bank_select",
    "front_bypass",
    "pan",
    "volume",
    "main_volume",
    "transpose",
    "pitch_bend",
    "bend_range",
    "priority",
    "note_wait",
    "tie",
    "portamento_from",
    "portamento",
    "portamento_time",
    "attack",
    "decay",
    "sustain",
    "release",
    "envelope_reset",
    "volume2",
    "print_variable",
    "surround_pan",
    "low_pass",
    "send",
    "main_send",
    "initial_pan",
    "mute",
    "damper",
    "tempo",
    "sweep_pitch",
    "lfo_curve",
    "lfo_phase",
    "lfo_depth",
    "lfo_speed",
    "lfo_type",
    "lfo_range",
    "lfo_delay",
    "lfo_period",
    "set_variable",
    "add_variable",
    "subtract_variable",
    "multiply_variable",
    "divide_variable",
    "shift_variable",
    "random_variable",
    "and_variable",
    "or_variable",
    "xor_variable",
    "not_variable",
    "modulo_variable",
    "compare_equal",
    "compare_greater_or_equal",
    "compare_greater",
    "compare_less_or_equal",
    "compare_less",
    "compare_not_equal",
    "user_procedure",
    "other",
    "data",
};

} // namespace

std::string_view Name(Kind kind)
{
    return kNames[static_cast<std::size_t>(kind)];
}

std::optional<Kind> KindNamed(std::string_view name)
{
    for (std::size_t i = 0; i < kNames.size(); ++i)
    {
        if (kNames[i] == name)
        {
            return static_cast<Kind>(i);
        }
    }

    return std::nullopt;
}

bool HasTarget(Kind kind)
{
    return kind == Kind::kOpenTrack || kind == Kind::kJump || kind == Kind::kCall;
}

} // namespace ntdlib::commands
