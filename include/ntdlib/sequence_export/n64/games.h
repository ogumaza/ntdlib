// SPDX-License-Identifier: MIT

// What the converter knows about each game: what the game's code changes in its sound. The sound files say nothing of
// it. Each image with a profile is found by its hash.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/n64/ead_script.h"

namespace ntdlib::sequence_export::n64
{

// The game's table of song volumes: a 16-bit volume at `offset + song * stride`, as offsets in the image. A song past
// the table's `count` plays at `other`.
struct SongVolumes
{
    std::size_t offset = 0;
    std::size_t stride = 0;
    std::size_t count = 0;
    int other = 0x7FFF;
};

// A song that the game's code names, by the game's words or by where it plays.
struct SongName
{
    std::size_t index = 0;
    const char* name = nullptr;
};

// The effect that a game's synthesizer has on its auxiliary bus: ALSynConfig's fxType.
enum class BusEffect
{
    kNone,
    kSmallRoom,
    kBigRoom,
    kChorus,
    kFlange,
    kEcho,
    kCustom,
};

// Whose player reads a game's songs on libultra's synthesizer.
enum class LibultraPlayer
{
    // The SDK's sequence players.
    kSdk,

    // The player that Ambrella wrote for Hey You, Pikachu!. It reads standard MIDI files and answers the controllers
    // of General MIDI.
    kAmbrella,

    // The SDK's compressed sequence player as HAL changed it for Pokémon Snap. A voice's volume holds the player's
    // scale in the place of the sound's volume, times a channel's scale out of 128. CC21 sets the player's scale, which
    // starts at 120. CC8 sets a channel's, which starts at 127.
    kHal,
};

// A game on libultra's sequence players.
// A song that the game's code also plays at another rate. The rate divides the time between the song's events. The
// tempo changes and the pitch doesn't. This version makes a track of its own, named by `beside`.
struct LibultraRateChange
{
    std::size_t song = 0;
    double rate = 1.0;
    const char* beside = nullptr;
};

struct LibultraProfile
{
    // The rate that the game gives the synthesizer. A sample plays at this rate at its key map's base key.
    uint32_t output_rate = 22050;

    // The offset in the image of the sequence file, and of the bank file that its songs play.
    std::size_t sequences = 0;
    std::size_t bank = 0;

    // The songs are in a table of Rare's compressed blocks, which the image's bytes lead to.
    bool rare_sequences = false;

    // The songs are this many standard MIDI files, one after the other, from `sequences`.
    std::size_t midi_files = 0;

    LibultraPlayer player = LibultraPlayer::kSdk;

    // The game's library ramps a voice's volume in a straight line of amplitude, where the SDK's multiplies. The
    // compact libraries do, and GoldenEye 007's does.
    bool line_ramps = false;

    // The volume that the game gives the sequence player for a song, if it has a table.
    SongVolumes volumes;
    std::span<const uint16_t> volume_list;
    int volume = 0x7FFF;

    // The game's sequence player has the SDK's oscillators (ALSeqpConfig's initOsc). Without them, a bank's tremolo and
    // vibrato do nothing.
    bool oscillators = false;

    // The bend range in cents that the game's code gives every program of its bank after loading it. 0 keeps the
    // bank's ranges.
    int program_bend_range = 0;

    // The effect on the auxiliary bus. Without an effect, the bus mixes into the main bus.
    BusEffect effect = BusEffect::kNone;

    std::span<const SongName> names;
    std::span<const LibultraRateChange> rate_changes;
};

// A call of the game's code that changes a sequence while it plays, at an audio frame after its start. Ocarina of
// Time's function at 0x800C5CD8 sets Ganon's Tower's music for each room in this way. The call sets every channel's
// pan weight. Its second part, at 0x800C5D84, sets the player's volume scale, writes a channel's IO port and gives a
// reverb to each channel whose IO port holds a value. -1 leaves a setting out.
struct EadGameCall
{
    int frame = -1;       // the audio frame of the call
    int volume_delay = 0; // the audio frames that the second part waits after the first
    int pan_weight = -1;  // every channel's pan weight
    int volume = -1;      // the player's volume scale, out of 127
    int io_channel = -1;  // the channel whose IO port the second part writes
    int io_port = 0;
    int io_value = 0;
    int reverb_port = -1; // the IO port whose value v gives a channel a reverb of v - volume + 127, or 127 out of range
};

// A way that an EAD game's code plays a sequence, where it has more than one.
struct EadTrack
{
    std::size_t sequence = 0;
    const char* name = nullptr;
    int variation = 0;            // the value that the game's call gives the sequence's variation commands
    uint16_t silent_channels = 0; // the channels that the game's code fades to nothing
    int tempo_change = 0;         // the later engine's change of tempo that the game's code sets, in beats a minute

    // What the game's code writes to the later engine's ports before it starts the sequence. -1 stands for nothing.
    std::array<int8_t, 8> ports = {-1, -1, -1, -1, -1, -1, -1, -1};

    EadGameCall call; // what the game's code changes while the sequence plays
};

// A track's ports, with a value in one of them or two.
constexpr std::array<int8_t, 8> EadPorts(std::size_t port, int8_t value, std::size_t second = 8,
                                         int8_t second_value = -1)
{
    std::array<int8_t, 8> ports = {-1, -1, -1, -1, -1, -1, -1, -1};
    ports[port] = value;
    if (second < ports.size())
    {
        ports[second] = second_value;
    }

    return ports;
}

// How a build of an EAD engine turns volumes and an envelope into a note's amplitude. Each envelope moves in straight
// lines from point to point. The builds differ in what those lines run between and in what they square.
enum class EadLevels
{
    // The lines run between the envelope's values. The amplitude is the line's square, times the volumes.
    kEarly,

    // The lines run between the squares of the envelope's values. The amplitude is the line's square, times the
    // volumes. The velocity's square arrives at the mixer as it stands: 127 squared out of 32768.
    kEnvelopeSquared,

    // The lines run between the squares of the envelope's values. The amplitude is the line, times the square of the
    // volumes.
    kVolumesSquared,
};

// A game on one of Nintendo EAD's sound engines.
struct EadProfile
{
    ntdlib::n64::EadEngine engine = ntdlib::n64::EadEngine::kEarly;

    // The rate of the game's output, and the rate of a sample at a tuning of 1. The engine's early builds take the two
    // for the same.
    double output_rate = 32006.0;
    double wave_rate = 32006.0;
    int updates_per_frame = 4;

    EadLevels levels = EadLevels::kEarly;

    // The game's build of the engine reads an envelope's point with the delay kEnvelopeSkip as a command that takes no
    // time.
    bool envelope_skip = false;

    // The layers that a channel holds in the game's build of the later engine. The number of a layer past them stands
    // for the first.
    std::size_t channel_layers = 4;

    // The game's build of the later engine reads a vibrato as 65536 / (wave * depth / 4096 + 65536).
    bool inverse_vibrato = false;

    // The scale that the game's code gives the sequence player for its music.
    double player_volume = 1.0;

    // The game's table of levels for the player's scale: a byte for each sequence from `first_player_level` on, out of
    // 127, as an offset in the image. A value above 127 names an entry of the second table.
    std::size_t player_levels = 0;
    std::size_t player_levels_indirect = 0;
    std::size_t first_player_level = 0;
    std::size_t player_level_count = 0;

    // The game's table of music volumes: a byte for each sequence, out of 127, as an offset in the image.
    std::size_t song_volumes = 0;
    std::size_t song_volumes_count = 0;

    // The sequences that only play the game's sound effects.
    std::span<const std::size_t> effect_sequences;

    // The value that the game's call gives a sequence's variation commands. The engine's default is -1.
    int variation = -1;

    // The sequences that the game's code plays in more than one way. Each way is a track. Another sequence is one
    // track.
    std::span<const EadTrack> tracks;

    std::span<const SongName> names;
};

// A bank of a game on the N64 Music Tools: the offsets in the image of its table of waves and of its samples.
struct LibmusBankPlace
{
    std::size_t first_song = 0; // the songs from this one on, up to the next bank's, play on this bank
    std::size_t pointers = 0;
    std::size_t samples = 0;
};

// A game that keeps its songs as packed files in a directory. The directory holds a file's start and end in the image.
// One file holds the game's table of songs. Each of the table's entries holds the number of a file and the address of
// a name.
struct LibmusFiles
{
    std::size_t directory = 0;
    std::size_t table_file = 0;
    std::size_t table = 0; // the table's offset in its file
    std::size_t table_count = 0;
    uint32_t file_address = 0; // the address where the game's code loads the table's file
};

// A change that the game's code makes to a song's tempo, from a time after the song's start.
struct LibmusTempoChange
{
    std::size_t song = 0;
    int scale = 128; // out of 128

    // The game's code also plays the song without the change. This version then makes a track of its own.
    const char* beside = nullptr;
};

// A game on Software Creations' N64 Music Tools.
struct LibmusProfile
{
    uint32_t output_rate = 22050;

    // Songs in the library's early form, with no master track and without a song's table of waves.
    bool early_form = false;

    // The game's compressed directory of a bank and songs, as an offset in the image, or 0 if it has none.
    std::size_t directory = 0;

    // The game's table of songs: the offsets in the image of each song's start and end.
    std::size_t songs = 0;
    std::size_t song_count = 0;

    // The game's table of music volumes, as an offset in the image: the byte after the first of each entry.
    std::size_t song_volumes = 0;
    std::size_t song_volume_stride = 4;

    // The volumes that the game's code gives each song, if they aren't in a table.
    std::span<const uint8_t> song_volume_list;

    // The volume that the game's code gives all its songs, if it isn't 0.
    int song_volume = 0;

    // The volume that the game's code gives all its songs.
    int master_volume = 0x7FFF;

    std::span<const LibmusBankPlace> banks;

    // The game's packed files, if it keeps its songs that way.
    LibmusFiles files;

    std::span<const LibmusTempoChange> tempo_changes;
};

// A game on Intelligent Systems' engine. The image holds the archive, and the game's code holds the engine's tables.
// What the game's code changes in a song while it plays. Each change is a track of its own.
struct SbnChange
{
    std::size_t song = 0;
    int mix = 0;            // the value that the song's branches read
    bool near = false;      // the game's code gives the mix its near level, not its full level
    bool linked = false;    // the linked tracks play in place of the tracks that they link to
    std::size_t levels = 0; // a list of tracks and levels in the image, or 0 for none
    const char* name = nullptr;
};

struct SbnProfile
{
    uint32_t output_rate = 32006;

    // The game's table of ticks a beat, by a song's timing preset, as an offset in the image.
    std::size_t ticks_per_beat = 0;

    // The game's table of an envelope's times, in microseconds.
    std::size_t envelope_times = 0;
    std::size_t envelope_time_count = 0;

    // The game's table that turns the first byte of a custom envelope's step into an index of the times.
    std::size_t custom_times = 0;
    std::size_t custom_time_count = 0;

    // The lists of the game's default envelope: press, then release, of four bytes each.
    std::size_t default_envelope = 0;

    std::span<const SongName> names;
    std::span<const SbnChange> changes;
};

struct GameProfile
{
    const char* sha1 = nullptr;
    const char* title = nullptr;
    LibultraProfile libultra;
    const EadProfile* ead = nullptr;       // the game's engine is one of EAD's, if it isn't null
    const LibmusProfile* libmus = nullptr; // the game's engine is the N64 Music Tools, if it isn't null
    const SbnProfile* sbn = nullptr;       // the game's engine is Intelligent Systems', if it isn't null
};

// The profile of the image with `sha1`, or nullptr.
const GameProfile* FindProfile(const std::string& sha1);

} // namespace ntdlib::sequence_export::n64
