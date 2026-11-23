// SPDX-License-Identifier: MIT

// Tests of the variants of pieces in a ripper's list of sounds (flipper/piece_variants.h): where each variant goes,
// its label and its state.

#include "ntdlib/flipper/piece_variants.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/z2_starts.h"
#include "ntdlib/jaudio/z2_states.h"

namespace
{

namespace z2 = ntdlib::flipper::z2;
using xsflib::testing::Check;

// A ripper's sound, with the fields that the functions use.
struct Sound
{
    uint32_t id = 0;
    std::string label;
    bool stream = false;
    bool shares_number = false;
    z2::PieceState piece_state;
};

Sound VariantOf(const Sound& sound, std::string_view suffix)
{
    Sound variant = sound;
    variant.label += suffix;
    variant.shares_number = true;

    return variant;
}

// The labels of `sounds`, with "+" before each that shares a number.
std::string Labels(const std::vector<Sound>& sounds)
{
    std::string labels;
    for (const Sound& sound : sounds)
    {
        labels += (labels.empty() ? "" : " ") + std::string(sound.shares_number ? "+" : "") + sound.label;
    }

    return labels;
}

void CheckMutedStarts()
{
    constexpr std::array<ntdlib::jaudio::MutedStart, 3> kStarts = {{
        {.sound = 1, .muted_children = 0x6, .suffix = ""},
        {.sound = 1, .muted_children = 0x1, .suffix = "_riding"},
        {.sound = 3, .muted_children = 0x2, .suffix = "_quiet"},
    }};
    std::vector<Sound> sounds = {
        {.id = 1, .label = "field"}, {.id = 2, .label = "town"}, {.id = 3, .label = "voice", .stream = true}};
    ntdlib::flipper::AddMutedStarts(kStarts, sounds, VariantOf);
    Check(Labels(sounds) == "field +field_riding town voice", "muted starts: " + Labels(sounds));
    Check(sounds[0].piece_state.muted_children == 0x6 && sounds[1].piece_state.muted_children == 0x1 &&
              sounds[3].piece_state.muted_children == 0,
          "muted starts: the children to mute, and none for a stream");

    // An empty table changes nothing.
    std::vector<Sound> none = {{.id = 1, .label = "field"}};
    ntdlib::flipper::AddMutedStarts({}, none, VariantOf);
    Check(none.size() == 1 && none[0].piece_state.muted_children == 0, "muted starts: an empty table changes nothing");
}

void CheckPieceStates()
{
    ntdlib::jaudio::MusicState start{.sound = 1, .suffix = "", .status_port = 1};
    ntdlib::jaudio::MusicState later{.sound = 1, .suffix = "_status2", .status_port = 2};
    ntdlib::jaudio::MusicState other{.sound = 2, .suffix = "_status3", .status_port = 3};
    const std::array<ntdlib::jaudio::MusicState, 3> states = {start, later, other};
    std::vector<Sound> sounds = {{.id = 1, .label = "village"},
                                 {.id = 1, .label = "village_riding", .shares_number = true},
                                 {.id = 2, .label = "voice", .stream = true}};
    ntdlib::flipper::AddPieceStates(states, sounds, VariantOf);
    Check(Labels(sounds) == "village +village_riding +village_status2 voice",
          "piece states: after the piece's other starts, and none for a stream: " + Labels(sounds));
    Check(sounds[0].piece_state == z2::PieceStateOf(start) && sounds[1].piece_state == z2::PieceState{} &&
              sounds[2].piece_state == z2::PieceStateOf(later),
          "piece states: the piece's state and the variant's");
}

void CheckVariantsAfterEach()
{
    std::vector<Sound> sounds = {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}};
    const auto layers_of = [](const Sound& sound)
    {
        std::vector<Sound> layers;
        if (sound.id == 1)
        {
            layers.push_back(VariantOf(sound, "_combo1"));
            layers.push_back(VariantOf(sound, "_combo2"));
        }

        return layers;
    };
    ntdlib::flipper::AddVariantsAfterEach(sounds, layers_of);
    Check(Labels(sounds) == "a +a_combo1 +a_combo2 b", "variants after each sound: " + Labels(sounds));
}

} // namespace

int main()
{
    const auto checks = []
    {
        CheckMutedStarts();
        CheckPieceStates();
        CheckVariantsAfterEach();
    };

    return xsflib::testing::RunTest("piece_variants", checks);
}
