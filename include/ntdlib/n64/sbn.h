// SPDX-License-Identifier: MIT

// The sound archive of Intelligent Systems' engine for the Nintendo 64. Paper Mario uses this engine.
//
// An SBN archive starts with a 64-byte header: the signature "SBN ", the size, the offset and the count of a table of
// files, and the offset of the file "INIT". Each entry of the table holds a file's offset and, in its last 4 bytes, a
// format and a size. A BK file's size in the table leaves out its samples, and each file's header gives the whole size.
//
// INIT lists the banks to load at the start, the songs (a BGM file and up to three BK files for each) and other files.
//
// A BGM file holds up to four compositions. A composition is a list of 32-bit commands that play phrases and loop over
// them. A phrase holds 16 words, one for each track: the offset of the track's commands from the phrase and flags. A
// track's commands are bytes:
//
//     0x00          the end of the phrase, or the return from a branch
//     0x01 - 0x77   a wait of that many ticks
//     0x78 - 0x7F   a longer wait, with a second byte
//     0x80 - 0xD3   a note: its key, then a velocity and a length of one or two bytes
//     0xE0 - 0xFF   the commands in BgmOp
//
// A detour (0xFE) reads a number of bytes from another place in the file, and goes on after it from where it was.
// A branch (0xFC) chooses a place from a table by a value that the game sets.
//
// A BK file holds up to 16 instruments. Each names its VADPCM samples, its loop, its book and its envelopes by offsets
// from the start of the file.
//
// The readers keep every byte. The writers lay the fields over those bytes. A file then comes back byte for byte.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/n64/vadpcm.h"

namespace ntdlib::n64
{

constexpr uint8_t kSbnFormatBgm = 0x10;
constexpr uint8_t kSbnFormatSef = 0x20;
constexpr uint8_t kSbnFormatBank = 0x30;
constexpr uint8_t kSbnFormatOther = 0x40; // PER, PRG and MSEQ

class SbnArchive
{
public:
    struct Entry
    {
        friend bool operator==(const Entry&, const Entry&) = default;

        uint32_t offset = 0; // from the start of the archive
        uint8_t format = 0;
        uint32_t size = 0; // as the table gives it: a BK file's size leaves out its samples
    };

    // A song in INIT: its BGM file, and the BK files that it loads into banks 0 to 2 of the auxiliary set. A file index
    // of 0 means no file.
    struct Song
    {
        friend bool operator==(const Song&, const Song&) = default;

        uint16_t bgm = 0;
        std::array<uint16_t, 3> banks = {};
    };

    // A BK file that the engine loads at its start.
    struct InitBank
    {
        friend bool operator==(const InitBank&, const InitBank&) = default;

        uint16_t file = 0;
        uint8_t bank = 0;
        uint8_t bank_set = 0;
    };

    // Reads the archive at the start of `data`. `data` can run on past it. Anything else throws xsflib::FormatError.
    static SbnArchive Parse(std::span<const uint8_t> data);
    static bool Matches(std::span<const uint8_t> data);

    const std::vector<Entry>& Entries() const;

    // A file's bytes, with the size that its header gives. Throws std::out_of_range for another index, and
    // xsflib::FormatError for a file that leaves the archive.
    std::span<const uint8_t> File(std::size_t index) const;

    const std::vector<Song>& Songs() const;
    const std::vector<InitBank>& InitBanks() const;

    // The other files in INIT's third list. The engine reads the file of sound effects, PER and PRG from its first
    // three.
    const std::vector<uint16_t>& OtherFiles() const;

    std::span<const uint8_t> Bytes() const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    std::vector<Song> songs_;
    std::vector<InitBank> init_banks_;
    std::vector<uint16_t> other_files_;
};

// A note of a drum track: a BGM file's table holds them for keys from 72 on, and a PER file for the keys below 72.
struct BgmDrum
{
    friend bool operator==(const BgmDrum&, const BgmDrum&) = default;

    uint16_t bank_patch = 0; // the bank in its high byte
    uint16_t key_base = 0;   // in cents
    uint8_t volume = 0;
    int8_t pan = 0;
    uint8_t reverb = 0;
    uint8_t random_tune = 0;
    uint8_t random_volume = 0;
    uint8_t random_pan = 0;
    uint8_t random_reverb = 0;
    uint8_t padding = 0;
};

constexpr std::size_t kBgmDrumSize = 12;

// An instrument that a BGM file or a PRG file names.
struct BgmInstrument
{
    friend bool operator==(const BgmInstrument&, const BgmInstrument&) = default;

    uint16_t bank_patch = 0;
    uint8_t volume = 0;
    int8_t pan = 0;
    uint8_t reverb = 0;
    int8_t coarse_tune = 0; // in semitones
    int8_t fine_tune = 0;   // in cents
    uint8_t padding = 0;
};

constexpr std::size_t kBgmInstrumentSize = 8;

// The commands of a composition, in their top 4 bits.
enum class BgmCompositionOp : uint8_t
{
    kEnd = 0,
    kPlayPhrase = 1,
    kStartLoop = 3,
    kWait = 4,
    kEndLoop = 5,
    kEndLoopUnless = 6, // only while the game's flag is off
    kEndLoopIf = 7,     // only while the game's flag is on
};

struct BgmCompositionCommand
{
    friend bool operator==(const BgmCompositionCommand&, const BgmCompositionCommand&) = default;

    uint32_t offset = 0; // in the file
    uint32_t raw = 0;
    BgmCompositionOp Op() const;
    uint32_t PhraseOffset(uint32_t composition) const; // the phrase's offset in the file, for kPlayPhrase
    uint8_t Label() const;                             // the label of a loop's start or end
    uint8_t LoopCount() const;                         // the end of a loop: the times round it, or 0 for ever
};

// A track of a phrase.
struct BgmTrackInfo
{
    friend bool operator==(const BgmTrackInfo&, const BgmTrackInfo&) = default;

    uint32_t offset = 0; // of the first command in the file, or 0 for a track that doesn't play
    uint8_t polyphony = 0;
    bool drums = false;
    uint8_t linked_track = 0; // from 1, or 0
    bool disabled = false;
};

constexpr int kBgmTrackCount = 16;

class BgmFile
{
public:
    static BgmFile Parse(std::span<const uint8_t> data);
    static bool Matches(std::span<const uint8_t> data);

    // The composition's offset in the file, or 0 for none.
    uint32_t CompositionOffset(int index) const;

    // The composition's commands up to and including its end. Throws xsflib::FormatError for one that leaves the file.
    std::vector<BgmCompositionCommand> Composition(uint32_t offset) const;

    // The tracks of the phrase at `offset` in the file.
    std::array<BgmTrackInfo, kBgmTrackCount> Phrase(uint32_t offset) const;

    std::vector<uint8_t> Write() const;
    std::span<const uint8_t> Bytes() const;

    std::string name; // 4 bytes
    uint8_t timing_preset = 0;
    std::vector<BgmDrum> drums;
    std::vector<BgmInstrument> instruments;

private:
    std::vector<uint8_t> bytes_;
    std::array<uint16_t, 4> compositions_ = {};
    uint16_t drums_offset_ = 0;
    uint16_t instruments_offset_ = 0;
};

enum class BgmOp : uint8_t
{
    kEnd,             // 0x00 outside a branch
    kReturn,          // 0x00 in a branch
    kWait,            // `length` ticks
    kNote,            // `key`, `velocity`, `length`
    kTempo,           // 0xE0
    kMasterVolume,    // 0xE1
    kMasterTranspose, // 0xE2
    kBusEffect,       // 0xE3
    kTempoFade,       // 0xE4
    kMasterVolumeFade,
    kMasterEffect,
    kUnused,
    kPatch, // 0xE8
    kInstrumentVolume,
    kInstrumentPan,
    kInstrumentReverb,
    kTrackVolume,
    kCoarseTune,
    kFineTune,
    kTrackDetune,
    kTremolo, // 0xF0
    kTremoloRate,
    kTremoloDepth,
    kTremoloStop,
    kRandomPan,
    kInstrument,
    kInstrumentVolumeFade,
    kReverbType,
    kUnused2, // 0xF8 to 0xFB
    kBranch,  // 0xFC
    kEventTrigger,
    kDetour,
    kSpecial,
    kInvalid, // 0xD4 to 0xDF
};

// A command of a track, with its offset in the file.
struct BgmCommand
{
    uint32_t offset = 0;
    uint8_t opcode = 0;
    BgmOp op = BgmOp::kEnd;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint16_t length = 0;
    std::array<uint8_t, 4> args = {};
};

// Where a track reads, in the form of the game's player.
struct BgmCursor
{
    friend bool operator==(const BgmCursor&, const BgmCursor&) = default;

    uint32_t position = 0;
    uint32_t saved = 0;      // where a detour goes on
    uint32_t branch_end = 0; // where a branch goes on, or 0
    uint8_t detour_left = 0; // the bytes left of a detour
};

// Reads the command at `cursor` and moves the cursor past it. It follows a detour's return, a branch's return (kReturn,
// and the caller reads on) and starts of detours (kDetour leaves the cursor at the detour). A branch leaves the cursor
// after its arguments. The caller chooses the place. Throws xsflib::FormatError when the command leaves the file.
BgmCommand ReadBgmCommand(std::span<const uint8_t> file, BgmCursor& cursor);

// The table entry that a branch's arguments name: a place in the file, and whether the track's notes are drums from
// there on.
struct BgmBranchTarget
{
    uint32_t position = 0;
    bool drums = false;
};

BgmBranchTarget ReadBgmBranch(std::span<const uint8_t> file, const BgmCommand& branch, uint8_t value);

// Every path of every composition, up to where its commands stop.
struct BgmWalk
{
    int compositions = 0;
    int phrases = 0;
    int tracks = 0;
    std::size_t commands = 0;
    std::size_t notes = 0;
    int branches = 0;
    int detours = 0;
    std::vector<uint32_t> bad_offsets;
};

BgmWalk WalkBgm(const BgmFile& file);

// A BK file's instrument.
struct BkInstrument
{
    friend bool operator==(const BkInstrument&, const BkInstrument&) = default;

    uint32_t offset = 0; // of the instrument in the file
    uint32_t wave = 0;   // from the start of the file. 0 for an instrument without samples
    uint32_t wave_size = 0;
    uint32_t loop_state = 0; // of 16 samples, or 0
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t loop_count = 0;
    uint32_t book = 0;
    uint16_t book_size = 0; // in bytes
    uint16_t key_base = 0;  // in cents
    uint32_t sample_rate = 0;
    uint8_t type = 0; // 0 VADPCM, 1 raw 16-bit
    std::array<uint8_t, 7> unused = {};
    uint32_t envelopes = 0;
};

constexpr std::size_t kBkInstrumentCount = 16;

class BkFile
{
public:
    static BkFile Parse(std::span<const uint8_t> data);
    static bool Matches(std::span<const uint8_t> data);

    // The instrument's book. Throws xsflib::FormatError when its size isn't a whole number of predictors or it leaves
    // the file.
    VadpcmBook Book(const BkInstrument& instrument) const;

    // The state that a wave holds at its loop's start.
    VadpcmState LoopState(const BkInstrument& instrument) const;

    std::span<const uint8_t> Wave(const BkInstrument& instrument) const;

    std::vector<uint8_t> Write() const;
    std::span<const uint8_t> Bytes() const;

    std::string name;
    std::array<int32_t, kBkInstrumentCount> slots = {}; // the index in `instruments`, or -1
    std::vector<BkInstrument> instruments;

private:
    std::vector<uint8_t> bytes_;
};

// The tables of a PER and a PRG file, after their 16-byte headers.
std::vector<BgmDrum> ParsePerFile(std::span<const uint8_t> data);
std::vector<BgmInstrument> ParsePrgFile(std::span<const uint8_t> data);

// Returns the offset of the archive's first copy in an image, or the image's size when there's none.
std::size_t FindSbnArchive(std::span<const uint8_t> image);

} // namespace ntdlib::n64
