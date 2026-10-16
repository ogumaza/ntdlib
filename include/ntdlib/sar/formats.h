// SPDX-License-Identifier: MIT

// NintendoWare sound format readers: the 3DS's BCSAR archives and their CSEQ, CBNK, CWAR, CWAV and CGRP files, and the
// BFSAR archives of the Wii U and the Switch, with their FSEQ, FBNK, FWAR, FWAV and FGRP files. The F formats are the
// 3DS's C formats with new names, laid out the same way. Each file is in the byte order that its byte order mark gives:
// little-endian on the 3DS and the Switch, big-endian on the Wii U. The readers support CSAR 2.3 on the 3DS, FSAR 2.0.0
// to 2.3.0 on the Wii U, and FSAR 2.2.0 and 2.5.0 on the Switch. The layouts follow nw::snd's readers on the 3DS.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"

namespace ntdlib::sar
{

// An item ID's type, in its top byte: a sound, a bank, a player, a wave archive and a group.
constexpr uint32_t kSoundItem = 0x01;
constexpr uint32_t kBankItem = 0x03;
constexpr uint32_t kPlayerItem = 0x04;
constexpr uint32_t kWaveArchiveItem = 0x05;
constexpr uint32_t kGroupItem = 0x06;

// An absent item: a bank slot with no bank, or a bank's wave entry with no wave. nw::snd checks the wave index for this
// value.
constexpr uint32_t kNoItem = 0xFFFFFFFF;

// A group's file ID when the game keeps the group outside the archive (see SoundArchive::ExternalGroups).
constexpr uint32_t kExternalGroupFile = 0xFFFFFFFF;

// The type of item `id`, from its top byte.
constexpr uint32_t ItemType(uint32_t id)
{
    return id >> 24;
}

// The index of item `id` among the items of its type, from its low three bytes.
constexpr uint32_t ItemNumber(uint32_t id)
{
    return id & 0xFFFFFF;
}

// The ID of item `index` of type `type`.
constexpr uint32_t ItemId(uint32_t type, uint32_t index)
{
    return type << 24 | index;
}

// The name of group `name`'s file: the group's name and ".bfgrp".
inline std::string GroupFileName(const std::string& name)
{
    return name + ".bfgrp";
}

// Whether a file named `file_name` holds group `name`: GroupFileName(name), in any case.
inline bool IsGroupFileName(const std::string& file_name, const std::string& name)
{
    return xsflib::AsciiLower(file_name) == xsflib::AsciiLower(GroupFileName(name));
}

// The index of item `id` of type `type`, or nothing if it's of another type or past `count`.
inline std::optional<std::size_t> ItemIndex(uint32_t id, uint32_t type, std::size_t count)
{
    const std::size_t index = ItemNumber(id);
    if (ItemType(id) != type || index >= count)
    {
        return std::nullopt;
    }

    return index;
}

// The console whose files the readers read. It sets the files' magic, the byte orders they accept and the wording of
// some messages.
enum class Flavor
{
    kCtr,  // the 3DS: CSAR, CSEQ, CBNK, CWAR, CWAV and CGRP files, little-endian
    kCafe, // the Wii U: FSAR, FSEQ, FBNK, FWAR, FWAV and FGRP files, big-endian
    kNx,   // the Switch: the Wii U's formats, little-endian
};

// The rules that the readers follow. kReadPastDamage reads on past damage and reports it. kStopAtFirstProblem stops at
// the first problem. Its messages depend on the flavor: the 3DS's messages for kCtr, and the messages of the Wii U and
// the Switch for kCafe and kNx.
enum class Rules
{
    kReadPastDamage,
    kStopAtFirstProblem,
};

// How the readers read and report bad input. Each program sets the behaviour it has.
struct Options
{
    Flavor flavor = Flavor::kCtr;

    // How each Reader reports a read past the end (see binary.h).
    xsflib::ReaderOptions reader;

    // Whether the readers read a group's whole file table before they locate any of its files. A group whose table is
    // damaged part of the way through then locates none of them. Without this, the files listed before the damage stay
    // located. With kReadPastDamage only.
    bool group_table_first = false;

    Rules rules = Rules::kReadPastDamage;
};

enum class WaveEncoding : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
    kImaAdpcm = 3 // the 3DS only
};

// DSP-ADPCM predictor coefficients (8 pairs) and initial sample history. With kStopAtFirstProblem, the readers also
// read the first frame's predictor and scale, and the decoder's state at the loop's start.
struct DspAdpcmInfo
{
    std::array<int16_t, 16> coefs{};
    uint16_t pred_scale = 0;
    int16_t yn1 = 0, yn2 = 0;
    uint16_t loop_pred_scale = 0;
    int16_t loop_yn1 = 0, loop_yn2 = 0;
};

// Initial sample and step index for an IMA-ADPCM channel.
struct ImaAdpcmInfo
{
    int16_t sample = 0;
    uint8_t step_index = 0;
};

// One wave channel's sample data and initial decoder state.
struct WaveChannel
{
    std::span<const uint8_t> data; // sample data from the channel's start to the end of DATA
    std::optional<DspAdpcmInfo> adpcm;
    std::optional<ImaAdpcmInfo> ima;
};

// A wave file's (CWAV or FWAV) encoding, byte order, sample rate, loop bounds and channels.
struct Wave
{
    static Wave Parse(std::span<const uint8_t> file, const Options& options);

    WaveEncoding encoding = WaveEncoding::kPcm16;
    bool big_endian = true; // the byte order of PCM16 samples
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // sample count; exclusive end of playback for both looping and one-shot waves
    std::vector<WaveChannel> channels;
};

// The wave files in a wave archive (CWAR or FWAR).
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> file, const Options& options);

    std::vector<std::span<const uint8_t>> waves;
};

// A region's envelope as MML values: attack, decay, sustain, hold and release.
struct Adshr
{
    // Default when a region has no ADSHR: five 127s. nw::snd's BankFile sets them up when it starts. On the 3DS,
    // code.bin 0x4c4edc fills 0x5e9800 with 127s.
    uint8_t attack = 127, decay = 127, sustain = 127, hold = 127, release = 127;
};

// Wave and playback settings for a key/velocity region.
struct VelocityRegion
{
    uint32_t wave_id_index = 0;
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    float pitch = 1.0f;
    bool ignore_note_off = false;
    uint8_t key_group = 0;

    // The interpolation. The 3DS's DSP: 0 or above 2 polyphase, 1 linear, 2 none. The Wii U's AX: 0 the 4-tap filters,
    // 1 linear, other values none.
    uint8_t interpolation = 0;

    Adshr adshr;
};

// A key range, subdivided into velocity ranges.
struct KeyRegion
{
    // A velocity range and its optional region. A velocity can pass 127, and a direct table's range has no upper bound.
    struct Vel
    {
        int lo = 0, hi = 127;
        std::optional<VelocityRegion> region;
    };

    // The upper bound of a direct table's one region. The region covers every value. With kReadPastDamage, a direct key
    // table reaches 127 instead.
    static constexpr int kAnyValue = std::numeric_limits<int>::max();

    int lo = 0, hi = 127;
    std::vector<Vel> velocities;
};

// A bank instrument's key regions.
struct Instrument
{
    // Finds the velocity region for a key and velocity: the first key region that holds the key, and the first of its
    // velocity regions that holds the velocity (nw::snd BankFileReader::ReadNoteInfo).
    const VelocityRegion* Find(int key, int velocity) const;

    std::vector<KeyRegion> keys;
};

// A bank wave reference: wave archive ID and index within it.
struct WaveId
{
    uint32_t wave_archive_item = 0; // 0x05xxxxxx item id
    uint32_t index = 0;
};

// A bank's (CBNK or FBNK) instruments and wave references.
struct Bank
{
    static Bank Parse(std::span<const uint8_t> file, const Options& options);

    std::vector<WaveId> waves;
    std::vector<std::optional<Instrument>> instruments;
};

// A sequence file's (CSEQ or FSEQ) bytecode. Its bytecode may hold several sequences. Each starts at one of its labels.
struct Sequence
{
    // A label: its name, and its offset into `data`.
    struct Label
    {
        std::string name;
        uint32_t offset = 0;
    };

    static Sequence Parse(std::span<const uint8_t> file, const Options& options);

    std::span<const uint8_t> data; // DATA block payload
    bool big_endian = false;       // the byte order of the commands' 16- and 24-bit arguments: the file's byte order
    std::vector<Label> labels;     // from the LABL block, with kStopAtFirstProblem on the Wii U and the Switch
};

enum class SoundType
{
    kSequence,
    kStream,
    kWaveSound,
    kUnknown
};

// Sound metadata. Missing names are generated from type and index, e.g. SEQ_12. As in other item names, control
// characters, line breaks and invalid UTF-8 bytes become '?'.
struct SoundInfo
{
    std::string name;
    uint32_t file_id = 0;
    uint8_t volume = 127;
    uint8_t pan_mode = 0;  // nw::snd mode: 0 dual (pan stereo channels separately), 1 balance
    uint8_t pan_curve = 0; // nw::snd curve: 0 is the default square-root law
    SoundType type = SoundType::kUnknown;

    // Read with kStopAtFirstProblem on the Wii U and the Switch.
    uint32_t player_id = 0; // 0x04xxxxxx item ID
    uint8_t player_priority = 64;
    uint8_t actor_player_id = 0;

    // Sequence sounds only.
    std::vector<uint32_t> banks; // up to four bank item IDs (0x03xxxxxx), or 0xFFFFFFFF for an empty slot
    uint32_t allocate_track_flags = 1;
    uint32_t start_offset = 0;
    uint8_t channel_priority = 64;     // added to each track's priority
    bool release_priority_fix = false; // a released channel keeps its priority
};

// Bank name and file ID. With kReadPastDamage, unnamed banks use BANK_<index>.
struct BankInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// Wave archive name and file ID. With kReadPastDamage, unnamed wave archives use WARC_<index>.
struct WaveArchiveInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// A sound player's name and how many sounds it plays at once. Read with kStopAtFirstProblem on the Wii U and the
// Switch.
struct PlayerInfo
{
    std::string name;
    uint32_t playable_sound_max = 0;
};

// A group's name and file ID: 0xFFFFFFFF for a group the game keeps outside the archive (see AddGroupFile).
struct GroupInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// A CSAR or FSAR archive's sound, bank and wave archive tables. Files may be stored directly in the archive, in groups
// (CGRP or FGRP) inside it, or in group files that the game keeps beside it.
class SoundArchive
{
public:
    // Reads the archive's tables and locates files stored in groups. Throws FormatError if the tables cannot be read.
    // With kReadPastDamage, the readers go on past a damaged name table, group or file entry, and report it (see
    // GroupErrors). With kStopAtFirstProblem, they stop at the first problem.
    static SoundArchive Load(std::vector<uint8_t> bytes, const Options& options);

    // The version in the archive's header: 0x02030000 for CSAR 2.3, and 0x00020200 for FSAR 2.2.0.
    uint32_t Version() const
    {
        return version_;
    }

    // The options that Load read the archive with. A program finds the archive's flavor there.
    const Options& ReadOptions() const
    {
        return options_;
    }

    const std::vector<SoundInfo>& Sounds() const
    {
        return sounds_;
    }

    const std::vector<BankInfo>& Banks() const
    {
        return banks_;
    }

    const std::vector<WaveArchiveInfo>& WaveArchives() const
    {
        return wave_archives_;
    }

    // The sound players. Read with kStopAtFirstProblem on the Wii U and the Switch.
    const std::vector<PlayerInfo>& Players() const
    {
        return players_;
    }

    // The groups. Read with kStopAtFirstProblem on the Wii U and the Switch.
    const std::vector<GroupInfo>& Groups() const
    {
        return groups_;
    }

    // The IDs of the files that group `group` of Groups() loads, in the order of its file's table. The game loads them
    // all with the group: the files whose copies the group's file holds, and the files in the archive that it links to.
    // Empty for a group that the game keeps outside the archive. Throws FormatError if the group's file can't be read.
    std::vector<uint32_t> GroupFileIds(std::size_t group) const;

    // The index of the first sound named `name`.
    std::optional<uint32_t> FindSound(const std::string& name) const;

    // The number of entries in the archive's file table.
    std::size_t FileCount() const
    {
        return files_.size();
    }

    // The names of the groups that the archive lists with no file: the game keeps them outside it and loads them
    // itself. Unnamed groups use GROUP_<index>. Read with kReadPastDamage. kStopAtFirstProblem leaves it empty: see
    // Groups().
    const std::vector<std::string>& ExternalGroups() const
    {
        return external_groups_;
    }

    // Adds an external group's file (see ExternalGroups). The files it holds that the archive lacks can be read with
    // FileData afterwards. Throws FormatError if `bytes` isn't a group file.
    void AddGroupFile(std::vector<uint8_t> bytes);

    // Whether `bytes` is a group file of this archive, read as AddGroupFile reads it. A game can keep another archive's
    // group files in the same folder. Every file of the group must have an ID in the archive's file table. A file whose
    // ID the archive's tables give a sequence sound, a wave sound, a bank or a wave archive must be a file of that
    // kind. At least one file must be such a file.
    bool MatchesGroupFile(std::span<const uint8_t> bytes) const;

    // The group files that AddGroupFile took, in the order it took them. A program can keep them with the archive.
    const std::vector<std::vector<uint8_t>>& GroupFiles() const
    {
        return group_files_;
    }

    // Returns a file's contents, or an empty span for external files and files absent from all readable groups.
    std::span<const uint8_t> FileData(uint32_t file_id) const;

    // Where a file starts in the archive, for a file that the archive holds, directly or in a group inside it. Nothing
    // for an external file, for a file in a group file added, or for a file absent from all readable groups.
    std::optional<std::size_t> FileOffset(uint32_t file_id) const;

    // The path that the file table gives an external file. Read with kStopAtFirstProblem on the Wii U and the Switch.
    // Empty for a file in the archive.
    std::string ExternalPath(uint32_t file_id) const;

    // True if any part of the file extends past the archive. For files stored only in groups, true if no readable group
    // contains the file and some groups extend past the archive.
    bool FileMissing(uint32_t file_id) const
    {
        return file_id < files_.size() && files_[file_id].missing;
    }

    // True if the archive is shorter than the size in its header. With kStopAtFirstProblem, also if a file lies past
    // the archive's end.
    bool Truncated() const
    {
        return truncated_;
    }

    // True if the archive's name table is damaged. Its sounds and banks then go by number.
    bool NamesDamaged() const
    {
        return names_damaged_;
    }

    // Errors reading groups. Files stored only in unreadable groups are unavailable.
    const std::vector<std::string>& GroupErrors() const
    {
        return group_errors_;
    }

private:
    // A file's location within the archive or a group file added to it.
    struct FileEntry
    {
        std::size_t offset = 0;
        uint32_t size = 0;
        uint32_t source = 0;   // 0 for the archive, n for the nth group file added
        bool internal = false; // stored in the FILE block, directly or in a group, or in a group file added
        bool missing = false;  // lies past the end of the archive
        bool grouped = false;  // no location in the file table; may be stored in groups
        std::string external_path;
    };

    // Reads tables through `r` and locates files stored in groups. Throws FormatError if the tables cannot be read.
    void ReadTables(const xsflib::Reader& r);

    // Locates files stored only in groups using group `name`. Keeps locations already found in earlier groups. `group`
    // is the group file's table entry. Appends truncated files to `cut_short` with the group name, and the error to
    // `unreadable` if the group can't be read.
    void ReadGroup(const std::string& name, const FileEntry& group,
                   std::vector<std::pair<std::string, uint32_t>>& cut_short, std::vector<std::string>& unreadable);

    // Reads tables through `r` with kStopAtFirstProblem.
    void ReadTablesStrictly(const xsflib::Reader& r);

    Options options_;
    std::vector<uint8_t> bytes_;
    std::vector<std::vector<uint8_t>> group_files_;
    std::vector<SoundInfo> sounds_;
    std::vector<BankInfo> banks_;
    std::vector<WaveArchiveInfo> wave_archives_;
    std::vector<PlayerInfo> players_;
    std::vector<GroupInfo> groups_;
    std::vector<std::string> external_groups_;
    std::vector<FileEntry> files_;
    uint32_t version_ = 0;
    bool truncated_ = false;
    bool names_damaged_ = false;
    bool groups_cut_ = false; // some groups lie past the end of the archive
    std::vector<std::string> group_errors_;
};

} // namespace ntdlib::sar
