// SPDX-License-Identifier: MIT

// Readers of the DS's sound archive (SDAT) and of the files it holds: sequences (SSEQ), sequence archives (SSAR),
// banks (SBNK), wave archives (SWAR), waves (SWAV) and streams (STRM). Every file is little-endian. Each starts with
// a header of 16 bytes: the magic word, the byte order mark 0xFEFF, the version, the file's size, the header's size
// and the number of blocks.
//
// An archive's INFO block lists the items of eight kinds by index: sequences, sequence archives, banks, wave archives,
// players, groups, stream players and streams. An index without an item has no record. The SYMB block, when there is
// one, names the items. The FAT block gives each file's place in the archive. A sequence or a bank names its file by
// its ID in the FAT.
//
// The parsed data points into the source bytes. The source bytes must outlive it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::sdat
{

// A file's header: the first 16 bytes of every file of the archive, and of the archive.
struct FileHeader
{
    friend bool operator==(const FileHeader&, const FileHeader&) = default;

    // Reads the header at the start of `file`. Throws xsflib::FormatError for another magic word, another byte order
    // mark, or a size past the end of `file`.
    static FileHeader Parse(std::span<const uint8_t> file, const char* magic);

    uint16_t version = 0x0100;
    uint32_t size = 0;
    uint16_t header_size = 0x10;
    uint16_t blocks = 1;
};

enum class WaveEncoding : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kImaAdpcm = 2,
};

// The 32-bit words before a wave's samples in `encoding`: the word of the decoder's state in IMA-ADPCM, and none in
// PCM.
constexpr uint32_t HeaderWords(WaveEncoding encoding)
{
    return encoding == WaveEncoding::kImaAdpcm ? 1 : 0;
}

// The samples in the first `words` 32-bit words of a wave in `encoding`: 4 a word in PCM8, 2 in PCM16 and 8 in
// IMA-ADPCM, after its header word. 0 for an unknown encoding.
constexpr uint32_t SamplesInWords(WaveEncoding encoding, uint32_t words)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return words * 4;
    case WaveEncoding::kPcm16:
        return words * 2;
    case WaveEncoding::kImaAdpcm:
        return words > 0 ? (words - 1) * 8 : 0;
    }

    return 0;
}

// A wave: its parameters (SNDWaveParam) and its samples. The DS's channel plays the samples from the start, and from
// the end goes back to the loop's start when the wave loops. The loop's start and length count 32-bit words. An
// IMA-ADPCM wave's data starts with a word of the decoder's state: the first sample and the index into the table of
// steps. The loop's start counts that word.
struct Wave
{
    // Reads the parameters at `at` in `r`, with the samples up to `end`.
    static Wave Parse(const xsflib::Reader& r, std::size_t at, std::size_t end);

    // Reads a wave file (SWAV).
    static Wave ParseFile(std::span<const uint8_t> swav);

    // The samples that the loop's start and length give.
    uint32_t Samples() const;

    // The sample at which the loop starts.
    uint32_t LoopStartSample() const;

    WaveEncoding encoding = WaveEncoding::kPcm8;
    uint8_t loop = 0; // 1 when the wave loops
    uint16_t sample_rate = 0;
    uint16_t timer = 0;       // the channel's timer: 16,756,991 / sample_rate, mostly rounded down
    uint16_t loop_start = 0;  // in words
    uint32_t loop_length = 0; // in words, after the loop's start
    std::span<const uint8_t> data;
};

// A wave archive (SWAR): waves, each at an offset from the file's start.
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> swar);

    FileHeader header;
    std::vector<uint32_t> offsets;
    std::vector<Wave> waves;
};

enum class InstrumentType : uint8_t
{
    kNone = 0,
    kPcm = 1,       // a wave of a wave archive
    kPsg = 2,       // a square wave. The wave field holds the duty, 0 to 7
    kNoise = 3,     // the noise generator
    kDirectPcm = 4, // a wave at an address in memory, set at run time
    kDummy = 5,     // a note that plays nothing
    kDrumSet = 16,  // one region for each key of a range
    kKeySplit = 17, // up to 8 regions, each up to a key
};

// A region's parameters (SNDInstParam), with its type. A PCM region names a wave by its index and by the bank's slot
// of wave archives (0 to 3).
struct Region
{
    friend bool operator==(const Region&, const Region&) = default;

    InstrumentType type = InstrumentType::kNone;
    uint8_t padding = 0;
    uint16_t wave = 0;
    uint16_t wave_archive = 0;
    uint8_t root_key = 60;
    uint8_t attack = 127;
    uint8_t decay = 127;
    uint8_t sustain = 127;
    uint8_t release = 127;
    uint8_t pan = 64;
};

// An instrument of a bank. A record of the bank's table gives its type and where its data lies in the bank file.
struct Instrument
{
    InstrumentType type = InstrumentType::kNone;
    uint32_t offset = 0; // from the bank file's start

    // kPcm to kDummy: the one region. Its type is the instrument's.
    Region region = {};

    // kDrumSet: the keys from min_key to max_key, one region each.
    uint8_t min_key = 0;
    uint8_t max_key = 0;

    // kKeySplit: each region's last key. A key of 0 ends the list.
    std::array<uint8_t, 8> split_keys{};

    // kDrumSet and kKeySplit.
    std::vector<Region> regions = {};
};

// A bank (SBNK). Each instrument's record gives its type in the low byte and its offset in the high 24 bits.
struct Bank
{
    static Bank Parse(std::span<const uint8_t> sbnk);

    FileHeader header;
    std::vector<Instrument> instruments;
};

// A sequence file (SSEQ): the bytecode of one sequence. The offsets in its commands count from the bytecode's start.
struct Sequence
{
    static Sequence Parse(std::span<const uint8_t> sseq);

    FileHeader header;
    uint32_t data_offset = 0x1C; // the bytecode's offset from the file's start
    std::span<const uint8_t> bytecode;
};

// A sequence of a sequence archive: where it starts in the archive's bytecode, and its settings.
struct ArchivedSequence
{
    friend bool operator==(const ArchivedSequence&, const ArchivedSequence&) = default;

    uint32_t offset = 0;
    uint16_t bank = 0;
    uint8_t volume = 0;
    uint8_t channel_priority = 0;
    uint8_t player_priority = 0;
    uint8_t player = 0;
    uint16_t reserved = 0;
};

// A sequence archive (SSAR): sequences that share one block of bytecode. Sound effects come in these.
struct SequenceArchive
{
    static SequenceArchive Parse(std::span<const uint8_t> ssar);

    FileHeader header;
    uint32_t data_offset = 0;
    std::vector<ArchivedSequence> sequences;
    std::span<const uint8_t> bytecode;
};

// A stream (STRM): samples in blocks, each block holding each channel's part in turn.
struct Stream
{
    static Stream Parse(std::span<const uint8_t> strm);

    // The bytes of `channel`'s part of block `block`.
    std::span<const uint8_t> Block(uint32_t channel, uint32_t block) const;

    FileHeader header;
    WaveEncoding encoding = WaveEncoding::kPcm16;
    uint8_t loop = 0;
    uint8_t channels = 1;
    uint8_t head_padding = 0;
    uint16_t sample_rate = 0;
    uint16_t timer = 0;      // 16,756,991 / sample_rate / 32: a 32nd of a wave's timer at that rate
    uint32_t loop_start = 0; // in samples
    uint32_t samples = 0;
    uint32_t data_offset = 0; // the samples' offset from the file's start
    uint32_t blocks = 0;
    uint32_t block_bytes = 0;
    uint32_t block_samples = 0;
    uint32_t last_block_bytes = 0;
    uint32_t last_block_samples = 0;
    std::array<uint8_t, 32> reserved{};
    std::span<const uint8_t> data; // from data_offset to the end of the DATA block
};

// The bits of a file's ID in the INFO record of a wave archive or a stream: bits 0 to 23. Bits 24 to 31 hold flags.
constexpr uint32_t kFileIdBits = 0xFFFFFF;

// The settings of a sequence, from the INFO block or a sequence archive.
struct SequenceInfo
{
    uint32_t file_id = 0;
    uint16_t bank = 0;
    uint8_t volume = 0;
    uint8_t channel_priority = 0;
    uint8_t player_priority = 0;
    uint8_t player = 0;
    uint16_t reserved = 0;
    std::string name;
};

struct SequenceArchiveInfo
{
    uint32_t file_id = 0;
    std::string name;
    std::vector<std::string> sequence_names;
};

// The wave archive slots of a bank's record (BankInfo::wave_archives). A region names a slot. A slot of kNoWaveArchive
// names no wave archive.
constexpr int kWaveArchiveSlots = 4;
constexpr uint16_t kNoWaveArchive = 0xFFFF;

struct BankInfo
{
    uint32_t file_id = 0;
    std::array<uint16_t, kWaveArchiveSlots> wave_archives{}; // 0xFFFF: none
    std::string name;
};

struct WaveArchiveInfo
{
    uint32_t file_id = 0; // bits 0 to 23. Bits 24 to 31 hold flags: bit 24 loads the waves one at a time
    std::string name;
};

struct PlayerInfo
{
    uint8_t max_sequences = 0;
    uint8_t padding = 0;
    uint16_t channels = 0; // a bit for each channel that the player's sequences may take. 0: any
    uint32_t heap_size = 0;
    std::string name;
};

// An item of a group: what the game loads together.
struct GroupItem
{
    friend bool operator==(const GroupItem&, const GroupItem&) = default;

    uint8_t type = 0; // 0: a sequence, 1: a bank, 2: a wave archive, 3: a sequence archive
    uint8_t load = 0; // bits for what the load includes
    uint16_t padding = 0;
    uint32_t index = 0;
};

struct GroupInfo
{
    std::vector<GroupItem> items;
    std::string name;
};

struct StreamPlayerInfo
{
    uint8_t channel_count = 0;
    std::array<uint8_t, 16> channels{};
    std::array<uint8_t, 7> reserved{};
    std::string name;
};

struct StreamInfo
{
    uint32_t file_id = 0; // bits 0 to 23. Bits 24 to 31 hold flags
    uint8_t volume = 0;
    uint8_t player_priority = 0;
    uint8_t player = 0;
    uint8_t flags = 0; // bit 0: play a stereo stream in mono
    std::array<uint8_t, 4> reserved{};
    std::string name;
};

// A file's place in the archive, from the FAT.
struct FileEntry
{
    friend bool operator==(const FileEntry&, const FileEntry&) = default;

    uint32_t offset = 0; // from the archive's start
    uint32_t size = 0;
    uint32_t reserved1 = 0;
    uint32_t reserved2 = 0;
};

// The SYMB block's lists of names. Each kind's list gives each index a name or none. The list of the sequence archives
// also gives each archive the list of its sequences' names, or none.
struct Symbols
{
    friend bool operator==(const Symbols&, const Symbols&) = default;

    std::array<std::vector<std::optional<std::string>>, 8> names;
    std::vector<std::optional<std::vector<std::optional<std::string>>>> sequence_names;
};

// A sound archive (SDAT).
struct Archive
{
    // Reads an archive. Throws xsflib::FormatError when a block, a table or a file lies outside the archive.
    static Archive Parse(std::span<const uint8_t> sdat);

    // A file's bytes. Throws xsflib::FormatError for an ID past the FAT.
    std::span<const uint8_t> File(uint32_t id) const;

    FileHeader header;
    bool has_symbols = false;
    Symbols symbols; // the SYMB block's lists, when the archive has names. The records' names come from them
    std::vector<std::optional<SequenceInfo>> sequences;
    std::vector<std::optional<SequenceArchiveInfo>> sequence_archives;
    std::vector<std::optional<BankInfo>> banks;
    std::vector<std::optional<WaveArchiveInfo>> wave_archives;
    std::vector<std::optional<PlayerInfo>> players;
    std::vector<std::optional<GroupInfo>> groups;
    std::vector<std::optional<StreamPlayerInfo>> stream_players;
    std::vector<std::optional<StreamInfo>> streams;
    std::vector<FileEntry> files;
    std::span<const uint8_t> bytes;
};

// Every sound archive in `file` that reads: at its start, or at a multiple of 4 bytes inside it. Games keep them in
// files of any name, and some in a larger file. Each archive points into `file`.
std::vector<Archive> FindArchives(std::span<const uint8_t> file);

} // namespace ntdlib::sdat
