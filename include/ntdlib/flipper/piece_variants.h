// SPDX-License-Identifier: MIT

// The variants of pieces of music in a ripper's list of sounds: the starts of Twilight Princess's pieces
// (jaudio/z2_starts.h), the states of its pieces (jaudio/z2_states.h) and the states that other JAudio games give a
// piece during play. Each variant comes after its piece and shares the piece's number.
//
// The functions take the ripper's sound type, with the fields `id`, `stream`, `shares_number` and `piece_state` (a
// z2::PieceState). `variant_of(sound, suffix)` makes a variant: a copy of the sound with `suffix` on its label and
// `shares_number` set.

#pragma once

#include <cstddef>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/z2_starts.h"
#include "ntdlib/jaudio/z2_states.h"

namespace ntdlib::flipper
{

// Puts after each sound of `sounds` the variants that `variants_of(sound)` returns. `variants_of` may change the sound.
template <typename Sound, typename VariantsOf>
void AddVariantsAfterEach(std::vector<Sound>& sounds, const VariantsOf& variants_of)
{
    std::vector<Sound> listed;
    for (Sound& sound : sounds)
    {
        listed.push_back(std::move(sound));
        std::vector<Sound> variants = variants_of(listed.back());
        listed.insert(listed.end(), std::make_move_iterator(variants.begin()), std::make_move_iterator(variants.end()));
    }

    sounds = std::move(listed);
}

// Puts after each piece of `sounds`, and after the sounds that share its number and follow it, the variants that
// `variants_of(piece)` returns. `variants_of` may change the piece. A stream, and a sound that shares a piece's number,
// gets no variants.
template <typename Sound, typename VariantsOf>
void AddPieceVariants(std::vector<Sound>& sounds, const VariantsOf& variants_of)
{
    std::vector<Sound> listed;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const std::size_t piece = listed.size();
        listed.push_back(std::move(sounds[i]));
        if (listed[piece].stream || listed[piece].shares_number)
        {
            continue;
        }

        std::vector<Sound> variants = variants_of(listed[piece]);

        // The piece's other variants stay right after it.
        while (i + 1 < sounds.size() && sounds[i + 1].shares_number)
        {
            listed.push_back(std::move(sounds[++i]));
        }

        listed.insert(listed.end(), std::make_move_iterator(variants.begin()), std::make_move_iterator(variants.end()));
    }

    sounds = std::move(listed);
}

// Gives the pieces whose tracks the game mutes as it starts them the children to mute (`starts`). A start with a
// suffix becomes a variant after its piece, such as "Z2BGM_FIELD_LINK_DAY_riding". A stream gets no start.
template <typename Sound, typename VariantOf>
void AddMutedStarts(std::span<const jaudio::MutedStart> starts, std::vector<Sound>& sounds, const VariantOf& variant_of)
{
    if (starts.empty())
    {
        return;
    }

    const auto starts_of = [starts, &variant_of](Sound& piece)
    {
        std::vector<Sound> variants;
        for (const jaudio::MutedStart& start : starts)
        {
            if (start.sound != piece.id || piece.stream)
            {
                continue;
            }

            if (start.suffix.empty())
            {
                piece.piece_state.muted_children = start.muted_children;
                continue;
            }

            Sound variant = variant_of(piece, start.suffix);
            variant.piece_state.muted_children = start.muted_children;
            variants.push_back(std::move(variant));
        }

        return variants;
    };
    AddVariantsAfterEach(sounds, starts_of);
}

// Gives the pieces their states (`states`). A state without a suffix is the piece's. A state with a suffix becomes a
// variant after the piece and its other starts, such as "Z2BGM_TOAL_VILLEGE_status1".
template <typename Sound, typename VariantOf>
void AddPieceStates(std::span<const jaudio::MusicState> states, std::vector<Sound>& sounds, const VariantOf& variant_of)
{
    if (states.empty())
    {
        return;
    }

    const auto states_of = [states, &variant_of](Sound& piece)
    {
        std::vector<Sound> variants;
        for (const jaudio::MusicState& state : states)
        {
            if (state.sound != piece.id)
            {
                continue;
            }

            if (state.suffix.empty())
            {
                piece.piece_state = z2::PieceStateOf(state);
                continue;
            }

            Sound variant = variant_of(piece, state.suffix);
            variant.piece_state = z2::PieceStateOf(state);
            variants.push_back(std::move(variant));
        }

        return variants;
    };
    AddPieceVariants(sounds, states_of);
}

} // namespace ntdlib::flipper
