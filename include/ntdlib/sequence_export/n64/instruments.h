// SPDX-License-Identifier: MIT

// What every engine's conversion shares: the instruments that a performance's presets play, in terms that SoundFont and
// SFZ both take, the performance itself, and the files of a converted sequence.
//
// An engine's performer records a sequence on a Performance. Each preset is an Instrument: zones of a key range and a
// velocity range that play a wave at a level, a pan, a tuning and an envelope. BuildFiles writes the MIDI file, the
// SoundFont and the SFZ files from them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/pcm.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::n64
{

// The longest performance that a sequence without a loop gets, in seconds. A sequence that runs on past it is cut.
inline constexpr double kLongestSeconds = 900.0;

// A decoded wave. `name` tells it apart from the game's other waves. It names the file of its samples.
struct Wave
{
    std::string name;
    seqlib::Pcm pcm;
    uint32_t rate = 0; // the samples a second that plays the wave at its zone's tuning
};

// A low-frequency oscillator of a zone: a vibrato's depth in cents or a tremolo's in decibels, either way of the
// centre.
struct Lfo
{
    double hz = 0.0;
    double delay_ms = 0.0;
    double depth = 0.0;
    bool wheel = false; // CC1 scales the depth, and `depth` is what it comes to at 127
};

// A run of keys and velocities of an instrument that plays a wave one way. The envelope's fall times are the times
// that a fall of 100 dB takes, as SoundFont counts them.
struct Zone
{
    int low_key = 0;
    int high_key = 127;
    int low_velocity = 0;
    int high_velocity = 127;
    int root_key = 60;
    double tune_cents = 0.0;
    bool follows_key = true; // false: every key plays at the root key's pitch
    double gain = 1.0;       // the amplitude at the top of the envelope, with the velocity and the controllers at 127
    double pan = 0.0;        // from -1 (left) to 1 (right)
    double delay_ms = 0.0;
    double attack_ms = 0.0;
    double hold_ms = 0.0;
    double decay_ms = 0.0;
    double sustain_db = 0.0; // the fall from the top to the sustain
    double release_ms = 0.0;
    int exclusive_class = 0;  // above 0: a new note of the same class on the same channel cuts this one
    double low_pass_hz = 0.0; // a low-pass filter's cutoff, or 0 for none
    std::optional<Lfo> vibrato = std::nullopt;
    std::optional<Lfo> tremolo = std::nullopt;
    std::shared_ptr<const Wave> wave = nullptr;
};

// An instrument of a sequence's SoundFont, or a <master> of its SFZ files.
struct Instrument
{
    std::string name;
    std::vector<Zone> zones;
};

// A recorded sequence, with its presets.
struct Performance : ntdlib::sequence_export::PerformanceBase
{
    std::vector<Instrument> presets;
};

// How the performers play.
struct PerformOptions
{
    int loops = 1;     // the times that the main loop plays
    uint32_t seed = 0; // the first state of a game's random numbers

    // The name that the files' comments give the program.
    std::string program = "ntdlib";
};

// The files of a converted sequence, its performance, and what the report needs.
struct Conversion
{
    Performance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    seqlib::sfz::Instruments sfz;
    std::size_t samples = 0;
    int channels = 0;
    std::vector<std::string> warnings;
};

// Writes the MIDI file, the SoundFont and the SFZ files of `performance`. `title` names them. `program` goes in their
// comments.
Conversion BuildFiles(Performance performance, const std::string& title, const std::string& program);

// The attenuation in centibels of an amplitude.
double Centibels(double amplitude);

} // namespace ntdlib::sequence_export::n64
