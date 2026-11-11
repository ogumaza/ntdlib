// SPDX-License-Identifier: MIT

// A sound archive of the Wii (RSAR) as the fields of its items, and a builder that lays such an archive out as
// NintendoWare for Revolution's tools do. ReadArchiveModel reads every field of an archive that the tools laid out.
// BuildArchive gives that archive back byte for byte from its model. It builds an archive from a model made from
// nothing the same way. WithNewWaveSounds and WithNewStreamSound add sounds that copy another sound's settings.
//
// The tools lay an archive out as follows. The header takes 64 bytes. It lists the SYMB, INFO and FILE blocks, in that
// order, and gives the archive's size. Zeros fill its other bytes. The blocks follow the header without gaps. SYMB and
// INFO end on multiples of 32 bytes.
//
// SYMB starts with the offsets of the string table and of four Patricia trees: the sounds', the players', the groups'
// and the banks'. The offsets count from the end of the block's size. The string table holds its count of strings and
// each string's offset. The names follow it, each ended by a zero: the players', the groups', the banks' and then the
// sounds', each kind in the order of its table. An item's string index is its name's place there. An item without a
// name has no string index. The trees follow the names. One version of the tools starts them on a multiple of 4 bytes.
// Each tree holds its root's index and its count of nodes, then nodes of 20 bytes, as in the later consoles' STRG
// blocks (ntdlib/sar/string_block.h). A leaf holds a name's string index and the item's index in its table. The names
// go into each tree in the order of their items. A tree without names has its root at 0xFFFFFFFF.
//
// INFO starts with references to the tables of the sounds, the banks, the players, the files and the groups, and to
// the player's settings. Each table holds its count and a reference to each entry. The tables follow in that order,
// each with its entries. A sound's entry takes 44 bytes. Its details follow it, then its 3D settings. A file's entry
// takes 28 bytes. A file beside the archive has its path after the entry, then zeros to a multiple of 4 bytes. Most
// versions of the tools end each path with a zero. A table of the places where groups hold the file follows, then the
// places. A group's entry takes 40 bytes. The table of its items follows it, then the items. The player's settings come
// last.
//
// FILE holds the groups' files, from 32 bytes into the block: each group's files one after another, then their wave
// data, then the next group. Each file's size and each wave data's size are multiples of 32 bytes. The last group of
// the table has no name. It holds the files that the other groups don't hold, in the order of their IDs. A file beside
// the archive lies in no group. A file's copies in several groups hold the same bytes.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"

namespace ntdlib::rsar
{

// A sound's 3D settings (Sound3DParam): the flags of the parameters that the distance changes, the curve and the
// ratio by which the volume falls with the distance, and the doppler factor.
struct Sound3dModel
{
    friend bool operator==(const Sound3dModel&, const Sound3dModel&) = default;

    uint32_t flags = 0;
    uint8_t decay_curve = 1;
    uint8_t decay_ratio = 128;
    uint8_t doppler_factor = 0;
};

// A sound of the sound table. The fields keep the names that SoundInfo gives them.
struct SoundModel
{
    friend bool operator==(const SoundModel&, const SoundModel&) = default;

    std::string name; // empty for a sound without a name
    SoundType type = SoundType::kSequence;
    uint32_t file_id = 0;
    uint32_t player_id = 0; // an index into the player table
    uint8_t volume = 127;
    uint8_t player_priority = 64;
    uint8_t remote_filter = 0;
    std::array<uint32_t, 2> user_params{};
    uint8_t pan_mode = 0;  // read from version 1.2 on
    uint8_t pan_curve = 0; // read from version 1.2 on
    uint8_t actor_player_id = 0;
    Sound3dModel settings_3d;

    // Sequence sounds.
    uint32_t start_offset = 0;
    uint32_t bank = 0; // an index into the bank table
    uint32_t allocate_track_flags = 1;

    // Sequence sounds and wave sounds. NW4R SND reads the release priority fix from version 1.3 on.
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;

    // Wave sounds: the sound's index among its file's wave sounds, and the count of its tracks.
    int32_t wave_sound_index = 0;
    uint32_t allocate_track_count = 1;

    // Stream sounds: where the stream starts, the channels and the tracks that it allocates.
    uint32_t stream_start = 0;
    uint16_t allocate_channel_count = 2;
    uint16_t stream_track_flags = 1;
};

// A bank: its file.
struct BankModel
{
    friend bool operator==(const BankModel&, const BankModel&) = default;

    std::string name;
    uint32_t file_id = 0;
};

// A sound player: how many sounds it plays at once, and the size of its heap.
struct PlayerModel
{
    friend bool operator==(const PlayerModel&, const PlayerModel&) = default;

    std::string name;
    uint8_t playable_sound_count = 1;
    uint32_t heap_size = 0;
};

// A group with a name, and the files that it holds in order. A group can hold a file more than once.
struct GroupModel
{
    friend bool operator==(const GroupModel&, const GroupModel&) = default;

    std::string name;
    std::vector<uint32_t> file_ids;
};

// A file of the file table: its bytes and its wave data, or the path of a file beside the archive, such as a stream,
// with the size that its entry gives.
struct FileModel
{
    friend bool operator==(const FileModel&, const FileModel&) = default;

    std::vector<uint8_t> bytes = {};
    std::vector<uint8_t> wave_data = {};
    std::optional<std::string> path = std::nullopt;
    uint32_t path_size = 0;
};

// What the archive's player reserves room for: the sounds and tracks of each kind that play at once, and the
// channels of the streams.
struct PlayerSettings
{
    friend bool operator==(const PlayerSettings&, const PlayerSettings&) = default;

    uint16_t sequence_sounds = 64;
    uint16_t sequence_tracks = 96;
    uint16_t stream_sounds = 4;
    uint16_t stream_tracks = 4;
    uint16_t stream_channels = 8;
    uint16_t wave_sounds = 64;
    uint16_t wave_tracks = 64;
};

// A sound archive of the Wii. `groups` holds the groups with names. BuildArchive adds the last group. The model
// also keeps the two choices by which the versions of the tools lay an archive out differently.
struct ArchiveModel
{
    friend bool operator==(const ArchiveModel&, const ArchiveModel&) = default;

    uint16_t version = 0x0104; // such as 0x0101 for RSAR 1.1
    PlayerSettings player_settings;
    std::vector<SoundModel> sounds;
    std::vector<BankModel> banks;
    std::vector<PlayerModel> players;
    std::vector<GroupModel> groups;
    std::vector<FileModel> files;
    bool aligned_trees = false;   // SYMB's trees start on a multiple of 4 bytes
    bool terminated_paths = true; // a path whose length is a multiple of 4 bytes takes a zero after it too
};

// The model of `archive`, which NintendoWare for Revolution's tools laid out. BuildArchive gives `archive` back byte
// for byte from it. Throws xsflib::FormatError for bytes that aren't an RSAR, for a version before 1.1 or after 1.4,
// and for an archive laid out another way, such as one without names, one with a group beside it, or one whose copies
// of a file differ.
ArchiveModel ReadArchiveModel(std::span<const uint8_t> archive);

// The archive of `model`, laid out as NintendoWare for Revolution's tools lay an archive out. Throws
// std::invalid_argument for a version before 1.1 or after 1.4, a sound of an unknown kind, an item that names a file,
// a bank or a player that the model lacks, a group that names a file beside the archive, a file beside the archive
// with bytes, a file whose size or wave data's size isn't a multiple of 32 bytes, a name with a zero byte, two names
// alike in one tree, and an archive past 4 GB.
std::vector<uint8_t> BuildArchive(const ArchiveModel& model);

// A wave sound to add to an archive's model: a copy of sound `model` with another name, file and index.
struct NewWaveSound
{
    uint32_t model = 0;   // the index of the sound that the new one copies: a wave sound or a sequence sound
    std::string name;     // empty for a sound without a name
    uint32_t file_id = 0; // the wave sound file that holds the wave sound
    int32_t index = 0;    // the wave sound's index in the file
};

// `archive` with `sounds` at the end of its sound table, in order. Each new sound copies its model's player, volume,
// player priority, remote filter, user parameters, pan mode, pan curve, actor player, 3D settings, channel priority and
// release priority fix. It takes the new sound's name, file and index. A wave sound model gives its count of tracks,
// and a sequence sound model a count of 1. The new sound's other fields keep the defaults of SoundModel. Throws
// std::invalid_argument for a model past the sound table or of another kind, a file past the file table, and a name
// that another sound holds.
ArchiveModel WithNewWaveSounds(ArchiveModel archive, std::span<const NewWaveSound> sounds);

// A stream sound to add to an archive's model: a copy of sound `model` with another name, file and stream fields.
struct NewStreamSound
{
    uint32_t model = 0;   // the index of the sound that the new one copies: a sound of any kind
    std::string name;     // empty for a sound without a name
    uint32_t file_id = 0; // the stream file, such as a file beside the archive
    uint32_t stream_start = 0;
    uint16_t allocate_channel_count = 2; // a count from version 1.4 on, and a bit for each channel before
    uint16_t stream_track_flags = 1;     // a bit for each track
};

// `archive` with `sound` at the end of its sound table. The new sound copies its model's player, volume, player
// priority, remote filter, user parameters, pan mode, pan curve, actor player and 3D settings. A stream sound's entry
// holds no channel priority and no release priority fix. The new sound takes its name, file, start, channels and tracks
// from `sound`. Its other fields keep the defaults of SoundModel. Throws std::invalid_argument for a model past the
// sound table, a file past the file table, and a name that another sound holds.
ArchiveModel WithNewStreamSound(ArchiveModel archive, const NewStreamSound& sound);

} // namespace ntdlib::rsar
