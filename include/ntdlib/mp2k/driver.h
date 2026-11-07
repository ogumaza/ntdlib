// SPDX-License-Identifier: MIT

// Locating MP2K, Nintendo's MusicPlayer2000 sound driver, and its tables in a ROM.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"

namespace ntdlib::mp2k
{
using ntdlib::agb::Rom;

// The DirectSound channels the driver has room for.
constexpr int kMaxDirectChannels = 12;

// The points of each side's buffer in the driver's SoundInfo. The sound DMA plays the buffer round, and the mixer
// writes a frame's part ahead of it.
constexpr uint32_t kPcmBufferSize = 1584;

// The most tracks a song can have, and the most a music player can play.
constexpr int kMaxTracks = 16;

// User-supplied overrides for driver detection.
struct DriverOverrides
{
    uint32_t song_table = 0;
    int song_count = 0;
};

// A music player: the driver plays each song on one of these, which the song table names.
struct Player
{
    int track_count = 0; // the most tracks it plays; a song's other tracks stay silent
};

// The songs that the game's code starts with another number of DirectSound channels than the init's.
struct SongChannelList
{
    uint32_t routine = 0; // the game's routine that sets the number
    int channels = 0;
    std::vector<int> songs;
};

// The player's volume that m4aMPlayVolumeControl sets for a track at its full level.
constexpr int kFullPlayerVolume = 0x100;

// What a game's code gives a song when it starts it. Before the start, it calls m4aSoundMode with `mode`. The model
// takes the mode's DirectSound channels and reverb. After the start, it calls m4aMPlayImmInit and then
// m4aMPlayVolumeControl with `volume` for every track of the song's player.
struct SongSetting
{
    int song = 0;
    uint32_t mode = 0; // 0 for no call
    int volume = kFullPlayerVolume;
};

// The driver's tables and settings in a ROM.
struct DriverInfo
{
    uint32_t song_start = 0; // the routine that starts a song (Thumb), or 0 if it wasn't found
    uint32_t song_table = 0; // 8 bytes a song: its header, and the number of its music player
    int song_count = 0;
    uint32_t player_table = 0;
    std::vector<Player> players;
    uint32_t sound_mode = 0;           // the setting the init gives the driver, or 0 if it wasn't found
    int max_channels = 8;              // DirectSound channels
    int master_volume = 15;            // 0-15
    int reverb = 0;                    // 0-127
    int rate_index = 4;                // 1-12, the setting of the mixer's rate
    int samples_per_frame = 224;       // the mixer's output for a frame
    int mix_rate = 13379;              // Hz, as the driver works it out
    uint32_t step_scale = 627;         // the mixer's step for each Hz of a sample's rate, in 2^-23 points
    uint8_t dac_bits = 0x40;           // the output's resolution: bits 6 and 7 of the sound bias register's high byte
    bool special_samples = false;      // the mixer can play samples backwards, and compressed samples
    bool mono = false;                 // the mixer has one output, and plays a sample at the average of its volumes
    bool camelot_sequencer = false;    // Camelot's changes to how a note takes a channel and sets its volume and rate
    bool camelot_top_priority = false; // the notes of a player whose priority is 120 take a priority of 255
    bool camelot_mixer = false;        // Camelot's mixer, with straight-line releases and synth voices
    int camelot_output_points = 2;     // the output stage's points for each of Camelot's mixer's points
    int camelot_echo = 53;             // the echo's level from each side's old output to the same side, over 128
    int camelot_cross_echo = -8;       // the echo's level from each side's old output to the other side, over 128
    int camelot_tap = 176;             // how far back in the mixer's points the left side takes the right side's output
    bool camelot_adpcm = false;        // Camelot's mixer plays a fixed-pitch sample of a negative size as 4-bit steps
    bool metroid_sequencer = false;    // the sequencer of Metroid Fusion and Metroid: Zero Mission
    uint32_t metroid_psg_table = 0;    // the Metroid games' table of PSG frequency settings, or 0
    bool metroid_short_notes = false;  // a note reads a key and a velocity, and no third byte (Metroid Fusion)
    SongChannelList song_channels;     // the songs that the game's code gives another number of DirectSound channels
    std::vector<SongSetting> song_settings; // what the game's code gives some songs when it starts them
    std::vector<std::string> log;           // detection results, for a listing of what it found
    std::vector<std::string> warnings;      // assumptions detection had to make
};

// The bytes that a note reads at most after its code: a key, a velocity and an extra length, or only a key and a
// velocity where the driver has `metroid_short_notes`.
inline int NoteArguments(const DriverInfo& info)
{
    return info.metroid_short_notes ? 2 : 3;
}

// Finds the driver in `rom`. Returns false and sets `error` if no song could be found, or returns false with `error`
// empty if the game shows no sign of the driver and no song table is given.
bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error);

// Returns the address of song `song`'s header, or 0 if it's past the table.
uint32_t SongAddress(const Rom& rom, const DriverInfo& info, int song);

// Returns the DirectSound channels that the mixer has while song `song` plays. A game's code can set them for each
// song it starts: Kuru Kuru Kururin from a table, the Fire Emblem games through a routine (song_channels), and other
// games through their song settings.
int SongChannels(const Rom& rom, const DriverInfo& info, int song);

// Returns the volume that the game's code gives every track of song `song` after the start, where kFullPlayerVolume
// is the full level.
int SongVolume(const DriverInfo& info, int song);

// Returns the music player that plays song `song`. A song whose player number is past the table gets a player of 16
// tracks.
Player SongPlayer(const Rom& rom, const DriverInfo& info, int song);

} // namespace ntdlib::mp2k
