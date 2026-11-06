// SPDX-License-Identifier: MIT

// Pikmin 2's conductors: the files (.cnd) in AudioRes/Conductor.arc that pick what the tracks of a cave's music play.
// The game's sound layer reads a conductor into a PSAutoBgm::Conductor. Addresses are in the USA release's main.dol.
// The music's timebase is 120 ticks a beat.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::sequence_export::jaudio
{

// A module of a conductor's track: the phrases that the track plays for a while, and how. JADUtility's parameters
// give the fields, in the order in which PSAutoBgm::Module's constructor (0x80339e5c) lists them.
struct ConductorModule
{
    int16_t start_beats = 0; // the beats before the module starts
    int16_t beats = 0;       // the beats that it plays before the track's next module, if the track has one
    uint8_t in_order = 0;    // 1 if it takes its phrases' lengths in the order of its list
    uint8_t bank = 0;        // the bank and the program that its tracks play
    uint8_t program = 0;
    int16_t phrase_list = 0; // its entry in the sequence's table of lists of phrases
    int16_t length_list = 0; // its entry in the sequence's table of lists of lengths
    uint8_t phrases = 0;     // the phrases in each round of phrases (PSAutoBgm::OnCycle)
    uint8_t rests = 0;       // the rests in each round of rests (PSAutoBgm::OffCycle)
    int16_t transpose = 0;   // in semitones
    uint8_t volume = 0;      // in 127ths
    uint8_t pan = 0;         // in 127ths
    uint8_t half_beat = 0;   // not 0 if the module starts 60 ticks later
};

// A track of a conductor. It drives the child of the piece's first track with its number (PSAutoBgm::Track's
// constructor, 0x803394f8).
struct ConductorTrack
{
    std::string name;
    int16_t start_beats = 0; // the beats before the track starts
    uint8_t half_beat = 0;   // not 0 if the track starts 60 ticks later
    uint8_t follows = 0x10;  // the track whose picks it takes, or 0x10 for none
    uint8_t send = 0;        // its effect send, in 127ths (PSAutoBgm::AutoBgm::startSeq, 0x8033c058)
    std::vector<ConductorModule> modules;
};

// A conductor (PSAutoBgm::Conductor, set up at 0x8033d470).
struct Conductor
{
    uint8_t poll = 0;  // the ticks between the root track's calls to the game
    uint8_t tempo = 0; // the root track's tempo
    std::vector<ConductorTrack> tracks;
};

// Reads a conductor file. Throws FormatError if the file is too short for the tracks and modules that it lists.
Conductor ReadConductor(std::span<const uint8_t> cnd);

} // namespace ntdlib::sequence_export::jaudio
