// SPDX-License-Identifier: MIT

// JAudio2's instrument banks (IBNK) of both versions: version 0 (The Wind Waker's layout) and version 1 (Twilight
// Princess's layout), with their instruments, drum sets, oscillators and effects. A bank can also be little-endian. The
// Switch's releases of GameCube and Wii games store their banks that way. The tag is then "KNBI", and each field is
// little-endian in the width of its type. A version 1 key region's high key is the top byte of a 32-bit word there. The
// other bytes of the bank's records, such as a velocity region's high velocity, keep their places. The Switch's release
// of Pikmin 2 has little-endian banks of version 0. Their drum table lies 100 words further on, at 0x544. Their size
// leaves those words out. The Switch's release of Pikmin keeps the GameCube's banks of version 0 with each offset
// widened to 64 bits. Each bank's tag and size keep the big-endian order, and its BANK chunk's tag is reversed. Its
// other fields are little-endian. Its size gives the size of the bank with 32-bit offsets.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace ntdlib::jaudio
{

// A point of an oscillator's table (JASOscillator::Point). Modes 0-3 move to the value along a curve over the time
// (linear, square, square root or the "sample cell" curve). Mode 13 loops to the point that the value gives, 14 holds
// and 15 stops.
struct OscPoint
{
    int16_t mode = 0;
    int16_t time = 0;
    int16_t value = 0;
};

// An oscillator (JASOscillator::Data): an envelope that moves a note's volume, pitch, pan or effect send over time.
struct Oscillator
{
    uint8_t target = 0;            // 0 volume, 1 pitch, 2 pan, 3 effect send, 4 surround
    float rate = 1.0f;             // the table's speed
    std::vector<OscPoint> attack;  // played from note-on; empty for none
    std::vector<OscPoint> release; // played from note-off; empty for none

    // The release table is the attack table (a version 1 bank names the same offset for both). A release then continues
    // along the table from where the attack is.
    bool release_is_attack = false;

    float scale = 1.0f;
    float vertex = 0.0f;
};

// An effect of an instrument or a drum: a value that multiplies a note's volume or pitch, or moves its pan, effect send
// or surround (JASInstRand and JASInstSense).
struct InstEffect
{
    enum class Kind : uint8_t
    {
        kRandom, // base plus a random amount up to width either way
        kSense,  // from `low` at 0 to `high` at 127 of the key or velocity, through 1 at `center`
    };

    Kind kind = Kind::kSense;
    uint8_t target = 0; // 0 volume, 1 pitch, 2 pan, 3 effect send, 4 surround

    // kRandom
    float base = 1.0f;
    float width = 0.0f;

    // kSense
    uint8_t trigger = 0; // 1 velocity, 2 key
    uint8_t center = 0;
    float low = 1.0f;
    float high = 1.0f;
};

// The wave that a range of velocities plays, up to `high_velocity`.
struct VelocityRegion
{
    uint8_t high_velocity = 127;
    uint16_t wave_id = 0;
    float volume = 1.0f;
    float pitch = 1.0f;
    uint16_t system_id = 0; // the high half of the wave's id word: some archives name the wave's system by it
};

// The velocity regions that a range of keys plays, up to `high_key`.
struct KeyRegion
{
    uint8_t high_key = 127;
    std::vector<VelocityRegion> velocities;
};

// An instrument (JASBasicInst).
struct Instrument
{
    float volume = 1.0f;
    float pitch = 1.0f;
    std::vector<std::size_t> oscillators; // into Bank::oscillators, at most two
    std::vector<InstEffect> effects;
    std::vector<KeyRegion> keys;

    // The instrument's flags in a version 0 bank: the low byte of the word at +4 of its record. Pikmin's JAudio limits
    // the instrument's voices by them (0x27911c in the main of the Switch's release). The low 4 bits pick the limit.
    // The bits above them say what a note past the limit does. A version 1 bank leaves them 0.
    uint8_t flags = 0;
};

// A drum set's sound for one key (JASDrumSet::TPerc). A drum plays its wave at its rate whatever the key.
struct Drum
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    uint16_t release = 1000; // a direct release: its time in the low 14 bits, its curve in the top two
    std::vector<InstEffect> effects;
    std::vector<VelocityRegion> velocities;
};

// A drum set (JASDrumSet).
struct DrumSet
{
    std::array<std::optional<Drum>, 128> keys;
};

// An instrument bank (IBNK).
struct Bank
{
    // Reads the bank at the start of `file`. `file` can go on past the bank: a version 1 bank's chunks can end past its
    // size. A bank with 64-bit offsets is read to the end of `file`. The bank plays the waves of wave system
    // `wave_system`. Throws FormatError if the bank is damaged or of an unknown version.
    static Bank Parse(std::span<const uint8_t> file, uint32_t wave_system);

    uint32_t number = 0;      // the number a sequence selects the bank by
    uint32_t wave_system = 0; // the number of the wave system it plays
    uint32_t version = 0;
    std::vector<Oscillator> oscillators;
    std::map<uint32_t, std::variant<Instrument, DrumSet>> programs; // drum sets from 0xe4 in version 0 banks

    // The numbers of the wave systems that the velocity regions name by system_id, when they aren't all wave_system.
    // A region whose system_id isn't here plays a wave of wave_system. Luigi's Mansion has banks like that (aaf.h).
    std::map<uint16_t, uint32_t> named_systems;
};

} // namespace ntdlib::jaudio
