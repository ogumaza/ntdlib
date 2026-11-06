// SPDX-License-Identifier: MIT

// The games on Software Creations' N64 Music Tools, also known as libmus. A game keeps its songs as separate files and
// its samples in a pair of files: a table of waves that starts with "N64 PtrTables" and the samples themselves.
//
// A song has channels that each play one note at a time, and in the library's later form a master track. Each channel
// reads a list of commands and notes. A note holds a wave, an envelope, a pan and a velocity, and a new note on a
// channel cuts the one before. Separate lists give the channel's volume and pitch for each tick. The player's
// arithmetic:
//
// - A tick lasts 256 / tempo callbacks of the player. A script's tempo of b gives a tempo of b * 256 * 96 / 120 / 60,
// in
//   whole numbers. 120 stands for about 96 ticks a second. The callbacks come every 16666 microseconds, rounded to
//   whole samples of the output: 367 samples at a rate of 22047, or 60.07 a second.
// - A note at key n plays a wave at 2^((n + detune) / 12) of the output rate. Above 2, the note is silent.
// - An envelope moves in straight lines between bytes: from the first value to the peak, on to the sustain, and from
//   wherever it stands to nothing in a fixed time. Its times count ticks.
// - A voice's volume is min(32767, (volume * envelope * velocity * song volume) >> 13) * master >> 15. The mixer
// squares
//   it. Velocity and CC7 act squared in SoundFont and SFZ too.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/n64/games.h"
#include "ntdlib/sequence_export/n64/instruments.h"

namespace ntdlib::sequence_export::n64
{

// What the game's code sets for a song.
struct LibmusSettings
{
    int song_volume = 0x80;
    int master_volume = 0x7FFF;
    uint32_t output_rate = 22050;
    bool early_form = false;

    // The scale that the game's code gives every tempo, out of 128.
    int tempo_scale = 128;
};

// A wave and an envelope, which a preset plays. The envelope holds a song's seven bytes: its time unit in ticks, the
// first value, the attack's units, the peak, the decay's units, the sustain and the release's units.
struct LibmusPresetKey
{
    friend bool operator==(const LibmusPresetKey&, const LibmusPresetKey&) = default;

    int32_t wave = 0;
    std::array<uint8_t, 7> envelope = {};
};

// A recorded song, with what each of its presets plays.
struct LibmusPerformance
{
    Performance performance;
    std::vector<LibmusPresetKey> presets;
    double ticks_per_second = 0.0;
};

// Plays `song`, a whole song file. `detunes` holds each wave's detune in semitones, in the bank's order.
LibmusPerformance PerformLibmus(std::span<const uint8_t> song, std::span<const double> detunes,
                                const LibmusSettings& settings, const PerformOptions& options);

// The zone of a preset, without its wave.
Zone LibmusZone(const LibmusPresetKey& key, double detune, const LibmusSettings& settings, double ticks_per_second);

// Unpacks a file of a game's directory. The file starts with a byte of 1 and its unpacked size in 24 bits. A flag byte
// then gives the kinds of the next eight items, from its top bit down. A flag of 0 marks a byte to copy. A flag of 1
// marks a copy of earlier output: the high nibble of its first byte and all of its second byte give the distance, and a
// distance of 0 ends the file. The low nibble n gives a length of n + 1. If n is 0, a third byte b gives b + 0x11.
std::vector<uint8_t> UnpackDirectoryFile(std::span<const uint8_t> file);

// A game on the N64 Music Tools.
class LibmusGame
{
public:
    // Reads the game's songs and banks from `image`, which stays valid while the game lives.
    static std::unique_ptr<LibmusGame> Open(std::span<const uint8_t> image, const LibmusProfile& profile);

    std::vector<ntdlib::sequence_export::Sequence> Sequences() const;
    Conversion Convert(std::size_t index, const PerformOptions& options);

private:
    struct BankWave
    {
        uint32_t base = 0;
        uint32_t length = 0;
        bool vadpcm = false;
        uint32_t loop = 0;
        uint32_t book = 0;
    };

    struct Bank
    {
        std::size_t first_song = 0;
        std::vector<uint8_t> unpacked_pointers; // the wave table, if the image holds it compressed
        std::span<const uint8_t> pointers;
        std::span<const uint8_t> samples;
        std::vector<BankWave> waves;
        std::vector<double> detunes;
        std::map<int32_t, std::shared_ptr<const Wave>> decoded;
    };

    LibmusGame() = default;

    std::string NameOf(std::size_t index, const char* beside = nullptr) const;
    bool Repeats(std::size_t later, std::size_t earlier);
    Bank& BankOfSong(std::size_t index);
    std::shared_ptr<const Wave> WaveAt(Bank& bank, int32_t index) const;

    std::span<const uint8_t> image_;
    const LibmusProfile* profile_ = nullptr;
    std::vector<std::vector<uint8_t>> unpacked_songs_; // the songs, if the image holds them compressed
    std::vector<std::span<const uint8_t>> songs_;
    std::vector<bool> repeats_;
    std::vector<std::string> names_; // the names in the game's table of songs, if it has one
    std::vector<Bank> banks_;
};

} // namespace ntdlib::sequence_export::n64
