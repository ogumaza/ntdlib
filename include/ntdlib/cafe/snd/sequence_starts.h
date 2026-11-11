// SPDX-License-Identifier: MIT

// How known Wii U games start some of their sequences, and the later states that the games give them. A game's code can
// set the variables that a sequence waits for as it starts the sound, or turn some tracks of a piece down through the
// sound's handle. The lists below find a sequence by the SHA-1 of its sound archive and its label. Any other sequence,
// and every sequence of an unknown archive, starts as nw::snd starts it: with its variables at -1 and every track at
// full volume. `archive_sha1` is the SHA-1 of the sound archive in 40 lowercase hex digits.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ntdlib::cafe::snd
{

// A value that a game's code gives one of a sequence's variables. Without a track, `index` numbers the variable as
// sequence_export::PerformOptions::variables does: 0 to 15 for the sequence's variables and 16 to 31 for the global
// ones. With a track from 0 to 15, `index` is the track's variable from 0 to 15, which the track's commands number
// 32 + `index`. An entry with index kNone sets nothing.
struct SequenceVariable
{
    static constexpr uint8_t kNone = 0xff;
    static constexpr int8_t kNoTrack = -1;

    int8_t track = kNoTrack;
    uint8_t index = kNone;
    int16_t value = -1;
};

// The variables that a game sets as it starts a sequence, or that a state of the sequence holds.
using SequenceVariables = std::array<SequenceVariable, 2>;

// How a known game starts a sequence: the variables that it sets, and the tracks that play. `tracks` holds a bit for
// each track that plays, bit 0 for track 0. The game's code turns the others down to 0 through the sound's handle.
struct SequenceStart
{
    SequenceVariables variables = {};
    uint16_t tracks = 0xffff;
};

// Another state of a sequence: other values of the variables, on which the sequence's branches play something else, or
// other tracks that play. A tool converts the sequence once more for each state, after the sequence itself, with the
// state in place of the sequence's start. The name of the state's file is the sequence's name followed by `suffix`.
// `variables` and `tracks` have SequenceStart's meanings.
struct SequenceArrangement
{
    std::string_view suffix;
    SequenceVariables variables = {};
    uint16_t tracks = 0xffff;
};

// How a known game starts the sequence labelled `sound`. An unlisted sequence or archive starts with no variables set
// and every track playing.
SequenceStart StartOfSequence(std::string_view archive_sha1, std::string_view sound);

// The later states of the sequence labelled `sound` in a known game. An unlisted sequence or archive has none.
std::span<const SequenceArrangement> SequenceArrangements(std::string_view archive_sha1, std::string_view sound);

// The labels of the sequences that the lists hold for the archive whose SHA-1 is `archive_sha1`, in the order of the
// lists. An unlisted archive has none.
std::vector<std::string_view> ListedSequences(std::string_view archive_sha1);

} // namespace ntdlib::cafe::snd
