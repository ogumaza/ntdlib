// SPDX-License-Identifier: MIT

// Readers of the files of Universomo's sound driver for the DS: songs (.ums, "UNIMUSIC"), the sample bank (.usb,
// "UNISAMBA") and the table of sounds (the file `audio`, "AU0"). Every field is little-endian. The layouts follow the
// driver's loaders in Beat City (USA, NTR-BEYE): 0x0202C17C for a song and 0x0202C764 for the bank.
//
// A song is a tracker module on XM's conventions: an order list of patterns, each pattern a number of rows, and in
// each pattern a list of events for each track. Each track plays on one of the DS's hardware channels. A row lasts
// `speed` calls of the driver's update, one call a frame.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::universomo
{

// The bit of an event's note byte that marks an effect. The low 7 bits then give the effect's number.
constexpr uint8_t kEffectFlag = 0x80;

// The note byte of a note cut. No file of Beat City has one.
constexpr uint8_t kNoteCut = 127;

// The most tracks a song can have: the driver keeps state for 16 hardware channels.
constexpr uint32_t kMaxTracks = 16;

// The effects that the driver supports, by number.
enum Effect : uint8_t
{
    kPitchSlideUp = 2,
    kPitchSlideDown = 3,
    kTonePortamento = 4,
    kVolumeSlide = 5,
    kVibrato = 6,
    kSampleOffset = 7,
    kNoteDelay = 8,
    kSpeed = 9,
    kFineVolumeSlide = 10,
    kVolume = 11,
    kArpeggio = 12,
};

// An event of a track: 4 bytes in the file.
struct Event
{
    friend bool operator==(const Event&, const Event&) = default;

    // Byte 0: the volume column of a note, or the parameter of an effect.
    uint8_t value = 0;

    // Byte 1: 0 for no note (a change of volume), 1 to 126 for a note, kNoteCut, or kEffectFlag with an effect's
    // number.
    uint8_t note = 0;

    // Byte 2: the instrument, from 1. 0 keeps the track's instrument.
    uint8_t instrument = 0;

    // Byte 3: the rows until the track's next event. 0 puts the next event on the same row.
    uint8_t rows = 0;
};

// A track's part of a pattern.
struct Track
{
    friend bool operator==(const Track&, const Track&) = default;

    // The rows before the first event.
    uint8_t first_rows = 0;
    std::vector<Event> events;
};

struct Pattern
{
    friend bool operator==(const Pattern&, const Pattern&) = default;

    uint32_t rows = 0;
    std::vector<Track> tracks; // one for each of the song's tracks
};

// An instrument: a sample of the bank with its settings.
struct Instrument
{
    friend bool operator==(const Instrument&, const Instrument&) = default;

    uint8_t sample = 0;  // the sample's number in the bank
    uint8_t volume = 0;  // 0 to 64
    uint8_t pan = 0;     // the driver doesn't read it
    int8_t finetune = 0; // in 128ths of a semitone
    int8_t relative_note = 0;
};

// A song file. The reader ignores bytes after the instruments.
struct Song
{
    friend bool operator==(const Song&, const Song&) = default;

    // The word at 0x0B: 150 in every file of Beat City. The driver stores it and never reads it.
    uint32_t tempo = 0;

    // The calls of the update a row.
    uint32_t speed = 0;

    // The order that a looped song goes back to after its last order.
    uint32_t restart = 0;

    uint32_t track_count = 0;
    std::vector<uint8_t> orders; // a pattern's number for each order
    std::vector<Pattern> patterns;
    std::vector<Instrument> instruments;
};

// A sample of the bank: signed 8-bit or signed 16-bit little-endian PCM. The DS's channels play it directly. The loop's
// start and length are in bytes.
struct Sample
{
    friend bool operator==(const Sample&, const Sample&) = default;

    uint8_t bits = 0; // 8 or 16
    uint32_t loop_start = 0;
    uint32_t loop_length = 0;
    bool looped = false;
    std::vector<uint8_t> data;
};

struct SampleBank
{
    friend bool operator==(const SampleBank&, const SampleBank&) = default;

    uint8_t version = 0; // the byte at 8: 0x3A in Beat City
    std::vector<Sample> samples;
};

// What the table of sounds gives for a song's number. ReadSoundTable keeps any other value of the byte as it is. Beat
// City's table uses only these three.
enum class SoundKind : uint8_t
{
    kEffect = 0, // a sound effect that plays once
    kMusic = 1,
    kLoopedEffect = 2,
};

// An entry of the table of sounds: 7 bytes. The driver never reads the table. The game's code reads it.
struct Sound
{
    friend bool operator==(const Sound&, const Sound&) = default;

    bool loops = false;                  // byte 1
    SoundKind kind = SoundKind::kEffect; // byte 2
    std::array<uint8_t, 7> bytes = {};
};

// Read a song. Throws xsflib::FormatError for a file that isn't a song, that ends early, that names a pattern or a
// restart past the end of its lists, or that has no order or more than kMaxTracks tracks.
Song ReadSong(std::span<const uint8_t> data);

// Read the sample bank. Throws xsflib::FormatError for a file that isn't a bank, that ends early, or whose sample has
// a width other than 8 or 16 bits.
SampleBank ReadSampleBank(std::span<const uint8_t> data);

// Read the table of sounds: an entry for each song's number. Throws xsflib::FormatError for a file that isn't the
// table or that ends early.
std::vector<Sound> ReadSoundTable(std::span<const uint8_t> data);

// The path of a song's file in the cartridge's file system.
std::string SongPath(uint32_t number);

} // namespace ntdlib::universomo
