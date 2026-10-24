// SPDX-License-Identifier: MIT

// A sound archive of the 3DS (CSAR), the Wii U or the Switch (FSAR) as the fields of its items, and a builder that lays
// such an archive out as NintendoWare's tools do. ReadArchiveModel reads every field of an archive that the tools laid
// out. BuildArchive gives that archive back byte for byte from its model. It builds an archive from a model made from
// nothing the same way.
//
// The tools lay an archive out as follows. The header takes 64 bytes. It lists the STRG, INFO and FILE blocks, in that
// order, and gives the archive's size. Zeros fill its other bytes. The blocks follow the header without gaps. STRG
// holds the names of the items in the order of INFO's tables (ntdlib/sar/string_block.h). An item's string index is
// its name's place there. An item without a name has no string index. INFO holds the tables (ntdlib/sar/info_block.h).
// STRG and INFO end on multiples of 32 bytes. FILE holds the files with bytes of their own in the order of their IDs.
// The first one starts at the first multiple of the files' alignment after the block's size: 64 bytes from the
// archive's start on the Switch, and 32 on the 3DS and the Wii U. Each next file starts at the first multiple after the
// end of the file before it. Zeros fill the gaps. The block ends at the end of its last file, or at the first multiple
// after the block's size when it holds no file. A file whose copy a group's file holds can take the copy's place in the
// FILE block (ntdlib/sar/groups.h). It then has no bytes of its own. A file that only the group files beside the
// archive hold has no place. A file beside the archive, such as a stream, has a path. The file table gives each file's
// place and size with a reference. The reference of a group's file has type 0, and every other one type 0x1F00. A
// file's entry on the 3DS holds no list of groups. There, the entry of a file without a place follows the archive's
// version.
//
// A sound's entry holds its file, its player, its volume and its remote filter, then its options: its string index,
// its pan, its priorities, its single play, the offset of its 3D settings, option 17 and the user's parameters, in the
// order of their bits. Its 3D settings follow. Sequence sounds and wave sounds hold one more 32-bit value after them.
// The details follow last: a sequence sound's list of banks, its tracks, its start and its channel priority, a wave
// sound's index and its channel priority, or a stream sound's tracks and sends (ntdlib/sar/tables.h). On the 3DS, the
// details follow the archive's version. A wave sound of 2.0.0.0 holds sends after them, and a stream sound of 2.0.0.0
// holds a value after its 3D settings. The other tables' entries hold their file IDs and lists as the tools write
// them.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// A sound's 3D settings: the flags of the parameters that the distance changes, the ratio and the curve by which the
// volume falls with the distance, and the doppler factor.
struct Sound3dModel
{
    friend bool operator==(const Sound3dModel&, const Sound3dModel&) = default;

    uint32_t flags = 0xF;
    float decay_ratio = 0.5f;
    uint8_t decay_curve = 1;
    uint8_t doppler_factor = 0;
};

// The fields that a stream sound's details hold after its sends: none, a reference to an extension, or that
// reference and the ID of the stream's prefetch file. The tools write both fields from FSAR 2.2.0 on. Archives of 2.1.0
// hold no field or the reference, and archives before 2.1.0 hold no sends either. The reference of most stream sounds
// leads to no extension.
enum class StreamFields
{
    kNone,
    kExtension,
    kPrefetch
};

// A stream sound's extension: three 32-bit values, which NintendoWare's sound library takes as the type of the stream
// and the frames where its loop starts and ends. The extension follows the rest of the details.
struct StreamExtension
{
    friend bool operator==(const StreamExtension&, const StreamExtension&) = default;

    uint32_t stream_type = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
};

// A sound of the sound table. The fields of ntdlib's reader keep the names that SoundInfo gives them.
struct SoundModel
{
    friend bool operator==(const SoundModel&, const SoundModel&) = default;

    std::string name; // empty for a sound without a name
    SoundType type = SoundType::kSequence;
    uint32_t file_id = 0;
    uint32_t player_id = ItemId(kPlayerItem, 0);
    uint8_t volume = 127;
    uint8_t remote_filter = 0;
    uint8_t pan_mode = 0;
    uint8_t pan_curve = 0;
    uint8_t player_priority = 64;
    uint8_t actor_player_id = 0;
    std::optional<uint32_t> single_play; // the value of option 3: the type of the single play and its time
    Sound3dModel settings_3d;
    std::optional<uint32_t> option_17;                   // the value of option 17
    std::array<std::optional<uint32_t>, 4> user_options; // the values of options 28 to 31, the user's parameters
    // The value after the 3D settings of a sequence sound, a wave sound, or a stream sound of a 3DS archive of 2.0.0.0.
    uint32_t after_3d = 0;

    // Sequence sounds.
    std::vector<uint32_t> banks; // bank item IDs, or kNoItem for an empty slot
    uint32_t allocate_track_flags = 1;
    uint32_t start_offset = 0;

    // Sequence sounds and wave sounds.
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;

    // Wave sounds.
    uint32_t wave_sound_index = 0;

    // Stream sounds.
    StreamSoundInfo stream;
    StreamFields stream_fields = StreamFields::kPrefetch;
    std::optional<StreamExtension> stream_extension; // with kExtension or kPrefetch, or with stream_options
    uint32_t prefetch_file_id = kNoItem; // with kPrefetch: the ID of the stream's prefetch file, or kNoItem for none

    // A stream sound of a 3DS archive of 2.3.0.0: the values of the extension that its details hold as options, a bit
    // each, from bit 0 for the type of the stream. Without one, the sound has no extension.
    uint8_t stream_options = 0;
};

// A sound group: sounds with consecutive IDs, and the files that they play. A wave sound set lists the wave archives
// of its wave sound file too.
struct SoundGroupModel
{
    friend bool operator==(const SoundGroupModel&, const SoundGroupModel&) = default;

    std::string name;
    uint32_t first_sound = kNoItem; // a sound's item ID, or kNoItem for a group without sounds
    uint32_t last_sound = kNoItem;
    std::vector<uint32_t> file_ids;
    std::optional<std::vector<uint32_t>> wave_archives; // the item IDs of a wave sound set's wave archives
};

// A bank: its file, and the wave archives that its entry lists. The tools list them for a few banks.
struct BankModel
{
    friend bool operator==(const BankModel&, const BankModel&) = default;

    std::string name;
    uint32_t file_id = 0;
    std::vector<uint32_t> wave_archives = {}; // item IDs
};

// A wave archive: its file, whether the game loads its waves one at a time, and the count of its waves, which the
// tools give with that choice.
struct WaveArchiveModel
{
    friend bool operator==(const WaveArchiveModel&, const WaveArchiveModel&) = default;

    std::string name = {};
    uint32_t file_id = 0;
    bool load_individually = false;
    std::optional<uint32_t> wave_count = std::nullopt;
};

// A group: its file, or kExternalGroupFile for a group that the game keeps beside the archive.
struct GroupModel
{
    friend bool operator==(const GroupModel&, const GroupModel&) = default;

    std::string name;
    uint32_t file_id = 0;
};

// A sound player: how many sounds it plays at once, and the size of its heap.
struct PlayerModel
{
    friend bool operator==(const PlayerModel&, const PlayerModel&) = default;

    std::string name;
    uint32_t playable_sound_max = 1;
    uint32_t heap_size = 0;
};

// What the archive's player reserves room for: the sounds and tracks of each kind that play at once, the channels of
// the streams, and the size of each stream's buffer in blocks.
struct PlayerSettings
{
    friend bool operator==(const PlayerSettings&, const PlayerSettings&) = default;

    uint16_t sequence_sounds = 64;
    uint16_t sequence_tracks = 64;
    uint16_t stream_sounds = 4;
    uint16_t stream_tracks = 4;
    uint16_t stream_channels = 8;
    uint16_t wave_sounds = 64;
    uint16_t wave_tracks = 64;
    uint8_t stream_buffer_times = 1;
};

// Where a file's bytes lie: in the FILE block, in a copy that a group's file in the FILE block holds, in the group
// files beside the archive only, or in a file beside the archive.
enum class FilePlace
{
    kBytes,
    kCopy,
    kGroupsOnly,
    kPath
};

// A file of the file table.
struct FileModel
{
    friend bool operator==(const FileModel&, const FileModel&) = default;

    FilePlace place = FilePlace::kBytes;
    std::vector<uint8_t> bytes = {}; // with kBytes
    uint32_t copy_group = 0; // with kCopy: the index of the group whose file holds the copy that gives the place
    std::string path = {};   // with kPath
    std::vector<uint32_t> groups = {}; // the item IDs of the groups that hold the file, in the list's order
};

// A sound archive of the 3DS, the Wii U or the Switch.
struct ArchiveModel
{
    friend bool operator==(const ArchiveModel&, const ArchiveModel&) = default;

    Flavor flavor = Flavor::kNx;
    uint32_t version = 0x00020500; // FSAR's version, such as 0x00020200 for 2.2.0, or CSAR's, such as 0x02000000
    PlayerSettings player_settings;
    std::vector<SoundModel> sounds;
    std::vector<SoundGroupModel> sound_groups;
    std::vector<BankModel> banks;
    std::vector<WaveArchiveModel> wave_archives;
    std::vector<GroupModel> groups;
    std::vector<PlayerModel> players;
    std::vector<FileModel> files;
};

// The model of `archive`, an archive of `flavor` that NintendoWare's tools laid out. BuildArchive gives `archive` back
// byte for byte from it. Throws xsflib::FormatError for bytes that aren't an archive of the console, for options and
// fields that the model doesn't hold, and for an archive laid out another way, such as an archive without names or a
// 3DS archive with a sound or a file whose layout in the archive's version the model doesn't know.
ArchiveModel ReadArchiveModel(std::span<const uint8_t> archive, Flavor flavor);

// The archive of `model`, laid out as NintendoWare's tools lay an archive out. Throws std::invalid_argument for a sound
// of an unknown kind, fields after a stream sound's sends in a version before 2.1.0, a stream sound's extension without
// a reference to it, a 3DS stream sound's options, tracks or extension in a version without them, a 3DS stream sound,
// wave sound or file without a place in a version whose layout the model doesn't know, a file of kCopy whose group's
// file the FILE block doesn't hold or doesn't hold a copy of the file in, an archive past 4 GB, and the names that
// StringBlock::Write refuses, such as two names alike.
std::vector<uint8_t> BuildArchive(const ArchiveModel& model);

} // namespace ntdlib::sar
