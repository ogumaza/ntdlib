// SPDX-License-Identifier: MIT

// NintendoWare for Revolution (NW4R) sound format readers: BRSAR archives and their RSEQ, RBNK, RWAR and RWAV files.
// Layouts follow NW4R SND's readers as linked into Wii Sports (2007), with the fields that the formats' later versions
// add. The parsed data points into the source bytes. The source bytes must outlive it.
//
// Archives come in two generations, and so do their banks. In RSAR 1.1, a bank (RBNK 1.1) describes its waves in a WAVE
// block, and its wave data is the raw samples. From RSAR 1.4, a bank (RBNK 1.2) may instead have no WAVE block and an
// RWAR of RWAV files for its wave data. A group holds each file with its wave data. A game loads them together.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/reference.h"
#include "xsflib/binary.h"

namespace ntdlib::rsar
{

// The texts of the readers' FormatErrors, in one of two wordings. The short texts read like "bad magic, expected RSAR"
// and "read out of bounds at offset 8". With them, a block past the end of a file throws. The sentences read like "not
// an RSAR file" and "a read at offset 8 runs past the end of the data". With them, a block past the end counts as
// missing.
enum class Wording : uint8_t
{
    kShort,
    kSentences,
};

// The rules that a program selects for these readers. The defaults are the short texts and printable names, without the
// fields that only playback uses.
struct ReadOptions
{
    // The texts of FormatError (see Wording). The short texts come with checks of their own: a file's size before its
    // header, the count of each instrument and wave table, an INFO table that isn't there, and the rejections of CSAR,
    // FSAR and SDAT archives. They also say how much of a truncated archive there is when its tables can't be read.
    // With the sentences, a missing INFO table reads as empty.
    Wording wording = Wording::kShort;

    // Make names printable: control characters, line breaks and invalid UTF-8 bytes become '?'. An item whose name is
    // empty, or whose name table is damaged, gets a made-up name. Otherwise names are as the archive has them, an empty
    // one stays empty, and a damaged name table fails the archive.
    bool printable_names = true;

    // Read the fields that only playback uses: players, the sounds' player, priorities, remote filter and user
    // parameters, wave sounds' details, the INFO block's size, groups' sizes and items, channels' mix volumes, the
    // whole ADPCM context of every channel that has one, regions' wave data location and surround pan, and sequences'
    // labels. A sequence sound without details then has bank 0 and allocates no track. An archive also counts as
    // truncated when one of its groups ends past it.
    bool playback_fields = false;

    // Before version 1.1, take a sound's type from its detail reference's data type. Otherwise take it from the
    // reference to the sound in the sound table.
    bool type_from_detail = true;
};

enum class WaveEncoding : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
};

// DSP-ADPCM predictor coefficients (8 pairs), and the decoder's history at the start: the two samples before the first.
// The gain, the predictor and scale at the start, and the state at the loop start come with playback_fields.
struct DspAdpcmInfo
{
    std::array<int16_t, 16> coefs{};
    int16_t yn1 = 0, yn2 = 0;
    uint16_t gain = 0;
    uint16_t pred_scale = 0;
    uint16_t loop_pred_scale = 0;
    int16_t loop_yn1 = 0;
    int16_t loop_yn2 = 0;
};

// One wave channel's sample data and initial decoder state.
struct WaveChannel
{
    std::span<const uint8_t> data; // sample data from the channel's start to the end of the wave data
    std::optional<DspAdpcmInfo> adpcm;
    std::array<uint32_t, 4> volumes{}; // front left, front right, rear left, rear right, as 8.24 fixed point
};

// A wave's encoding, sample rate, loop bounds and channels: an RWAV file, or a wave info in a version 1.1 bank's WAVE
// block. The file gives the loop as DSP addresses: nibbles for DSP-ADPCM, with two header nibbles in each 8-byte frame
// of 14 samples. The reader turns them into samples. WaveFileReader::ReadWaveParam (main.dol 0x8004eca8) does the same.
struct Wave
{
    // Reads an RWAV file. Its samples are in its DATA block.
    static Wave ParseFile(std::span<const uint8_t> rwav, const ReadOptions& options = {});

    // Reads the wave info at `offset` in `info`. The samples start at the wave info's data offset in `wave_data`.
    static Wave ParseInfo(const xsflib::Reader& info, std::size_t offset, std::span<const uint8_t> wave_data,
                          const ReadOptions& options = {});

    WaveEncoding encoding = WaveEncoding::kPcm16;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past the last sample played: the wave's length, looping or not

    // DSP addresses as the file stores them: in nibbles for ADPCM (two for each 8-byte frame's header), in samples for
    // PCM16 and in bytes for PCM8. The end is the last sample's address.
    uint32_t loop_start_address = 0;
    uint32_t loop_end_address = 0;

    std::vector<WaveChannel> channels;
};

// RWAV files in a wave archive (RWAR).
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> file, const ReadOptions& options = {});

    std::vector<std::span<const uint8_t>> waves;
};

// A bank's region: the wave a range of keys and velocities plays, and how (InstParam).
struct Region
{
    int32_t wave_index = 0; // into the bank's waves
    uint8_t attack = 127, decay = 127, sustain = 127, release = 127;
    uint8_t hold = 0;               // builds from 2008 only
    uint8_t wave_data_location = 0; // 0: an index into the bank's waves (playback_fields)
    uint8_t note_off_type = 0;      // 1: ignores the note-off at the end of a note's length (builds from 2008 only)
    uint8_t key_group = 0; // the alternate assign: a new note of the group releases the others (2008 builds only)
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    uint8_t surround_pan = 0; // playback_fields
    float tune = 1.0f;
};

// RBNK: a bank of instruments, each a tree of key and velocity tables leading to regions, and the waves they play.
class Bank
{
public:
    // Reads a bank's instruments. For a version 1.1 bank, also reads where its WAVE block describes its waves. `file`
    // has to outlive the bank.
    static Bank Parse(std::span<const uint8_t> file, const ReadOptions& options = {});

    // Reads a bank and all its waves from `wave_data`: the raw samples for a version 1.1 bank, an RWAR for a later one.
    // `file` and `wave_data` have to outlive the bank.
    static Bank Parse(std::span<const uint8_t> file, std::span<const uint8_t> wave_data,
                      const ReadOptions& options = {});

    // The bank's version, such as 0x0101 for RBNK 1.1.
    uint16_t Version() const
    {
        return version_;
    }

    // The number of programs that the instrument table has. Some of them may play nothing.
    uint32_t InstrumentCount() const
    {
        return static_cast<uint32_t>(instruments_.size());
    }

    // The region that plays `key` at `velocity` on `program`, or nullptr (BankFileReader::ReadInstInfo, main.dol
    // 0x80035d94): the instrument's reference, then a key table, then a velocity table, each a range table (the first
    // entry whose upper key is at least the key) or an index table (by key from its lowest). The chain ends at a
    // region. Any other type of reference, a key outside a table and a negative wave index play nothing.
    const Region* Find(uint32_t program, int key, int velocity) const;

    // True for a version 1.1 bank. Such a bank's WAVE block describes its waves.
    bool HasWaveBlock() const
    {
        return wave_block_;
    }

    // Reads wave `index` of a version 1.1 bank. The bank's samples are in `wave_data`. Throws FormatError if the wave
    // can't be read.
    Wave ReadWave(std::size_t index, std::span<const uint8_t> wave_data) const;

    // The waves that Parse read with the bank's wave data.
    const std::vector<Wave>& Waves() const
    {
        return waves_;
    }

private:
    // The data types of a bank's references.
    enum : uint8_t
    {
        kNone = 0,
        kRegion = 1,
        kRangeTable = 2,
        kIndexTable = 3,
    };

    // A reference in the tree, resolved: a region (an index into regions_), a table (an index into tables_), or
    // nothing.
    struct Node
    {
        uint8_t type = kNone;
        uint32_t index = 0;
    };

    // A range table (keys holds each entry's upper key) or an index table (keys holds its lowest and highest key).
    struct Table
    {
        uint8_t type = kRangeTable;
        std::vector<uint8_t> keys;
        std::vector<Node> entries;
    };

    // The entry of table `node` for `key`, or nullptr.
    const Node* Choose(const Node& node, int key) const;

    std::span<const uint8_t> file_;
    ReadOptions options_;
    uint16_t version_ = 0;
    std::vector<Node> instruments_;
    std::vector<Table> tables_;
    std::vector<Region> regions_;
    bool wave_block_ = false;
    std::vector<std::size_t> wave_infos_; // offsets of a version 1.1 bank's wave infos
    std::vector<Wave> waves_;
};

// RSEQ bytecode. A file can hold several sequences. Each starts at a label (playback_fields).
struct Sequence
{
    struct Label
    {
        std::string name;
        uint32_t offset; // into data
    };

    static Sequence Parse(std::span<const uint8_t> file, const ReadOptions& options = {});

    std::span<const uint8_t> data; // the commands, the base of a sound's start offset
    std::vector<Label> labels;
};

enum class SoundType : uint8_t
{
    kUnknown = 0,
    kSequence = 1,
    kStream = 2,
    kWaveSound = 3,
};

// Sound metadata. Missing names are generated from type and index, e.g. SEQ_12. With printable_names, control
// characters, line breaks and invalid UTF-8 bytes become '?'. The same goes for other item names.
struct SoundInfo
{
    std::string name;
    uint32_t file_id = 0;
    uint32_t player_id = 0; // playback_fields, like the rest of the fields that playback alone uses
    uint8_t volume = 127;
    uint8_t player_priority = 64;
    uint8_t remote_filter = 0;
    uint32_t user_params[2] = {};
    uint8_t pan_mode = 1;  // 0 dual (pan stereo channels from their sides), 1 balance (the only mode before RSAR 1.2)
    uint8_t pan_curve = 0; // 0 is the default square-root law
    SoundType type = SoundType::kUnknown;

    // Sequence sounds only.
    uint32_t bank = 0xffffffff; // index into SoundArchive::Banks()
    uint32_t allocate_track_flags = 1;
    uint32_t start_offset = 0;

    // Sequence and wave sounds, playback_fields only: their notes' priority, whether it stays fixed on release (from
    // version 1.3), and a wave sound's index in its file. A wave sound's tracks are in allocate_track_flags.
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;
    int32_t sub_index = 0;
};

// Bank name and file ID. Unnamed banks use BANK_<index>.
struct BankInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// A sound player: a channel of the game's sound system that sounds play on. One example is a player for music.
struct PlayerInfo
{
    std::string name;        // from the symbols, or made up from the index (PLAYER_2) when the archive has none
    uint8_t sound_count = 0; // the number of sounds it plays at once
    uint32_t heap_size = 0;
};

// A file in a group: its contents and its wave data. Banks and wave sound files have wave data.
struct FileData
{
    std::span<const uint8_t> file;
    std::span<const uint8_t> wave_data;
};

// RSAR sound and bank tables, with the files that groups hold in the archive. The object keeps the archive's bytes. The
// spans it returns stay valid as long as the object exists.
class SoundArchive
{
public:
    // A group of files that a game loads into memory together: their data, then their wave data (playback_fields).
    struct Group
    {
        struct Item
        {
            uint32_t file_id;
            uint32_t offset;
            uint32_t size;
            uint32_t wave_data_offset;
            uint32_t wave_data_size;
        };

        bool internal = false; // held in the archive, rather than in a file beside it
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t wave_data_offset = 0;
        uint32_t wave_data_size = 0;
        std::vector<Item> items;
    };

    // Reads the archive's tables and locates the files that groups hold. Throws FormatError if the tables cannot be
    // read.
    static SoundArchive Load(std::vector<uint8_t> bytes, const ReadOptions& options = {});

    // The archive's version, such as 0x0104 for RSAR 1.4.
    uint16_t Version() const
    {
        return version_;
    }

    const std::vector<SoundInfo>& Sounds() const
    {
        return sounds_;
    }

    const std::vector<BankInfo>& Banks() const
    {
        return banks_;
    }

    // The players (playback_fields).
    const std::vector<PlayerInfo>& Players() const
    {
        return players_;
    }

    // The index of the first sound named `name`, if any.
    std::optional<uint32_t> FindSound(const std::string& name) const;

    // Returns a file's contents, from the first group that holds it and its wave data inside the archive, or else the
    // first that holds the file, or an empty span if none does.
    std::span<const uint8_t> FileData(uint32_t file_id) const;

    // Returns the wave data that goes with FileData, or an empty span if no group holds both inside the archive.
    std::span<const uint8_t> WaveData(uint32_t file_id) const;

    // A file's contents and wave data, from the first group that holds the file and its wave data inside the archive
    // (playback_fields). Returns nothing for a file that only an external group or file holds, or that lies past the
    // end of a truncated archive.
    std::optional<rsar::FileData> File(uint32_t file_id) const;

    // The groups (playback_fields).
    const std::vector<Group>& Groups() const
    {
        return groups_;
    }

    // The size of the INFO block. A game loads this block to use the archive (playback_fields).
    uint32_t InfoSize() const
    {
        return info_size_;
    }

    // True if no group holds the file inside the archive, and one would if the archive weren't cut short: it extends
    // past the end.
    bool FileMissing(uint32_t file_id) const
    {
        return file_id < files_.size() && files_[file_id].missing;
    }

    // True if the file has wave data that no group holds inside the archive, and one would if the archive weren't cut
    // short.
    bool WaveDataMissing(uint32_t file_id) const
    {
        return file_id < files_.size() && files_[file_id].wave_data_missing;
    }

    // True if the archive is shorter than the size in its header. With playback_fields, also true if one of its
    // internal groups ends past it.
    bool Truncated() const
    {
        return truncated_;
    }

    // True if the archive's name table is damaged. Its sounds and banks then go by number (printable_names).
    bool NamesDamaged() const
    {
        return names_damaged_;
    }

    // The archive's bytes. The spans that File returns point into them.
    std::span<const uint8_t> Bytes() const
    {
        return bytes_;
    }

private:
    // A file's place in the archive.
    struct FileEntry
    {
        std::size_t offset = 0;
        uint32_t size = 0;
        std::size_t wave_data_offset = 0;
        uint32_t wave_data_size = 0;
        bool found = false;             // a group holds the file inside the archive
        bool wave_data_found = false;   // and its wave data
        bool missing = false;           // a group holds the file past the end of the archive
        bool wave_data_missing = false; // a group holds its wave data past the end of the archive
    };

    // A place where the archive keeps a file: a group, and an item of the group.
    struct FilePosition
    {
        uint32_t group;
        uint32_t index;
    };

    // Reads tables through `r`, and locates the files that groups hold. Throws FormatError if the tables cannot be
    // read.
    void ReadTables(const xsflib::Reader& r);

    std::vector<uint8_t> bytes_;
    ReadOptions options_;
    uint16_t version_ = 0;
    std::vector<SoundInfo> sounds_;
    std::vector<BankInfo> banks_;
    std::vector<PlayerInfo> players_;
    std::vector<FileEntry> files_;
    std::vector<std::vector<FilePosition>> positions_; // where each file is, by file id
    std::vector<Group> groups_;
    uint32_t info_size_ = 0;
    bool truncated_ = false;
    bool names_damaged_ = false;
};

} // namespace ntdlib::rsar
