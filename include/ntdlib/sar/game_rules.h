// SPDX-License-Identifier: MIT

// What a game's code does with the sounds of a sound archive that the archive doesn't say. The code may turn tracks of
// a sound down as the sound plays (nn::atk's SetTrackVolume through the sound's handle). It may put a reverb on an aux
// bus while some sounds play and leave it there for the others. It may give a sound's variables values as it starts
// the sound (WriteVariable). A rule holds for one release: the archive whose size and SHA-1 it names. Any other archive
// gets no rules.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ntdlib::sar
{

// A state of a sound whose tracks the game's code turns down: the words that name the state, and the tracks that the
// code has turned down to 0, a bit for each, bit 0 for track 0. The code leaves the other tracks at 1. The state that
// the sound starts in has no words.
struct TrackState
{
    std::string name;
    uint16_t muted = 0;
};

// A sound whose tracks the game's code turns down: its label in the archive, and its states. The first is the state
// that the code starts the sound in.
struct LayeredSound
{
    std::string label;
    std::vector<TrackState> states;
};

// The kinds of reverb that a game's code puts on an aux bus: the I3DL2 reverb (nn::atk's EffectI3dl2Reverb or an aux
// effect), and the audio renderer's reverb (nn::atk's EffectReverb).
enum class ReverbType : uint8_t
{
    kI3dl2,
    kReverb,
};

// A reverb that the game's code puts on an aux bus: the preset's name, its 12 parameters and its type.
//
// An I3DL2 reverb's parameters are in the order of the audio renderer's I3dl2ReverbParameterSet. Those are the room's
// gain and its high-frequency gain (millibels), the late reverb's decay time (seconds) and its decay ratio at high
// frequencies, the reflections' gain (millibels) and delay (seconds), the reverb's gain (millibels) and the late
// reverb's delay (seconds), the diffusion and the density (percent), the reference frequency (Hz) and the dry gain.
//
// The renderer's reverb has the early reflections' mode (0 to 4), their gain, the pre-delay (microseconds), the late
// reverb's mode (0 to 4) and gain, the decay time (microseconds), the decay ratio at high frequencies, the
// coloration, the reverb's gain, the out gain, the dry gain and nn::atk's channel mode (0 to 3 for 1, 2, 4 or 6
// channels). Each value is the one that nn::atk's setter is given.
//
// The dry gain is what the reverb passes on of its input unchanged.
struct ReverbPreset
{
    std::string name;
    std::array<float, 12> parameters{};
    ReverbType type = ReverbType::kI3dl2;

    // The dry gain: the last parameter of an I3DL2 reverb, and the one before it for the renderer's reverb.
    float DryGain() const
    {
        return parameters[type == ReverbType::kReverb ? 10 : 11];
    }
};

// Values of a sound's variables, by their numbers in the sequence commands: 0 to 15 for the sound's variables, 16 to 31
// for the global ones.
using VariableValues = std::map<int, int16_t>;

// The values that the game's code can give the variables of some sounds as it starts them: the sounds whose label is
// `label`, or begins with it when it ends in '*', and the states, in the order that a tool tries them. A sound's
// variables start at -1. A tool gives a sound that never sounds at -1 the first of these states in which it sounds.
struct VariableStarts
{
    std::string label;
    std::vector<VariableValues> states;
};

// The rules of a sound archive.
struct ArchiveRules
{
    // The sounds whose tracks the game's code turns down, in the archive's order.
    std::vector<LayeredSound> layered;

    // The reverbs that the game's code puts on aux bus B, by name.
    std::vector<ReverbPreset> bus_b_presets;

    // The sounds that play with a reverb on aux bus B, by label: the preset's name. The other sounds start with bus B
    // empty. Once one of these sounds has played, its reverb stays on bus B for the sounds that follow.
    std::map<std::string, std::string, std::less<>> bus_b;

    // The other sounds that send to aux bus B. Each starts with bus B empty, and plays later with each reverb of
    // bus_b_presets there.
    std::vector<std::string> bus_b_later;

    // The reverb that the game's code keeps on aux bus B for every sound, or empty.
    std::string bus_b_always;

    // The values that the game's code gives some sounds' variables as it starts them, the first match for a label
    // first.
    std::vector<VariableStarts> starts;

    // Whether the game's code keeps a delay on aux bus A, rather than a reverb. Where a reverb on bus B takes CC91, the
    // sends to bus A then leave it (PerformOptions::bus_a_delay).
    bool bus_a_delay = false;

    // Whether the archive has rules.
    bool Empty() const
    {
        return layered.empty() && bus_b_presets.empty() && bus_b.empty() && bus_b_later.empty() &&
               bus_b_always.empty() && starts.empty() && !bus_a_delay;
    }

    // The preset named `name`, or none.
    const ReverbPreset* FindBusBPreset(std::string_view name) const;
};

// A state that a sound plays in: the words that name it, the tracks that the game's code has turned down to 0 (a bit
// for each), and the reverb on aux bus B (a name of ArchiveRules::bus_b_presets, or empty for none). The state that the
// sound starts in has no words.
struct SoundState
{
    std::string name;
    uint16_t muted = 0;
    std::string bus_b;
};

// The states of the sound `label` under `rules`, the state that it starts in first. A layered sound plays in its
// states. A sound of bus_b_later starts with bus B empty. Its later states have the name of each reverb, without the
// I3DL2_ before it, and the word "reverb", such as "Hall reverb". Any other sound has one state. Each state holds the
// reverb that bus_b or bus_b_always gives the sound.
std::vector<SoundState> StatesOf(const ArchiveRules& rules, std::string_view label);

// The states that the game's code can give the variables of the sound `label` as it starts it, under `rules`: those of
// the first entry of ArchiveRules::starts that matches the label. None for any other sound.
std::span<const VariableValues> StartsOf(const ArchiveRules& rules, std::string_view label);

// The rules of the sound archive `archive`. None for an archive that no rule names.
const ArchiveRules& RulesOf(std::span<const uint8_t> archive);

// The rules of the sound archive of `size` bytes whose SHA-1 is `sha1`, as 40 lowercase hex digits. None for an
// archive that no rule names.
const ArchiveRules& RulesOf(std::size_t size, std::string_view sha1);

} // namespace ntdlib::sar
