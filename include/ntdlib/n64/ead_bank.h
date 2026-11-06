// SPDX-License-Identifier: MIT

// The sound banks of Nintendo EAD's sound engines for the Nintendo 64. A bank holds instruments, drums and, in the
// later engine, sound effects. Offsets in a bank count from its start and point to objects that several others can
// share.
//
// An instrument holds a release rate, an envelope, and three samples with their tunings: one for low notes, one for its
// normal range, and one for high notes. A drum holds a release rate, a pan, a sample with its tuning and an envelope.
// An envelope is a list of pairs of 16-bit values: a delay and a target. It ends with a delay of 0, -1, -2 or -3, which
// stop it, hold it, or send it to a pair or back to its start.
//
// The early engine's bank starts with 16 bytes: the number of instruments, the number of drums, and 8 more bytes. The
// offsets count from the end of those 16 bytes. An offset of the drums' table and of each instrument follow. A sample
// holds two bytes of flags and two of padding, the sample's address, a loop, a book and its size.
//
// The later engine's bank gives its counts in the table of banks. It starts with the offsets of the drums' table and of
// the sound effects, and the offset of each instrument follows. A sample holds its codec, its medium and its size in
// one 32-bit field, then the address, a loop and a book. An address counts from the start of a table of samples. A
// loop holds 16 bytes, with a state of 32 bytes after them if its count isn't 0.
//
// The reader gathers each object once and keeps the bank's bytes. The writer lays the fields over those bytes at each
// object's offset. An unchanged bank then comes back byte for byte. An object that the reader never saw goes at the
// end.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/ead_script.h"

namespace ntdlib::n64
{

// The later engine's codecs. The early engine's samples are all kEadCodecAdpcm.
constexpr uint8_t kEadCodecAdpcm = 0;
constexpr uint8_t kEadCodecS8 = 1;
constexpr uint8_t kEadCodecS16InMemory = 2;
constexpr uint8_t kEadCodecSmallAdpcm = 3;
constexpr uint8_t kEadCodecReverb = 4;
constexpr uint8_t kEadCodecS16 = 5;

// An envelope's delays that end its list.
constexpr int16_t kEnvelopeDisable = 0;
constexpr int16_t kEnvelopeHang = -1;
constexpr int16_t kEnvelopeGoto = -2;
constexpr int16_t kEnvelopeRestart = -3;

// A delay that doesn't end the list. Pokémon Stadium 2's build of the later engine reads this point as a command that
// takes no time. A note that reaches it before its first frame starts at its target volume, with no ramp from silence.
// Other builds treat it as an ordinary delay.
constexpr int16_t kEnvelopeSkip = -4;

struct EadEnvelope
{
    friend bool operator==(const EadEnvelope&, const EadEnvelope&) = default;

    struct Point
    {
        friend bool operator==(const Point&, const Point&) = default;

        int16_t delay = 0;
        int16_t target = 0;
    };

    uint32_t offset = kNoOffset;
    std::vector<Point> points = {}; // the last one ends the list, if the bank holds an end
};

struct EadSample
{
    friend bool operator==(const EadSample&, const EadSample&) = default;

    uint32_t offset = kNoOffset;
    uint8_t codec = kEadCodecAdpcm;
    uint8_t medium = 0;
    bool unknown_bit = false;
    uint8_t loaded = 0; // the engine's mark that it has made the object's offsets into pointers
    uint32_t size = 0;
    uint32_t address = 0; // from the start of the bank's samples
    int32_t loop = kNoObject;
    int32_t book = kNoObject;
};

// A sample and the scale of its frequency: its rate against the output's, for a note at the middle of the scale.
struct EadTunedSample
{
    friend bool operator==(const EadTunedSample&, const EadTunedSample&) = default;

    int32_t sample = kNoObject;
    float tuning = 0.0F;
};

struct EadInstrument
{
    friend bool operator==(const EadInstrument&, const EadInstrument&) = default;

    uint32_t offset = kNoOffset;
    uint8_t loaded = 0;
    uint8_t normal_range_low = 0;
    uint8_t normal_range_high = 127;
    uint8_t release_rate = 0;
    int32_t envelope = kNoObject;
    EadTunedSample low = {};
    EadTunedSample normal = {};
    EadTunedSample high = {};
};

struct EadDrum
{
    friend bool operator==(const EadDrum&, const EadDrum&) = default;

    uint32_t offset = kNoOffset;
    uint8_t release_rate = 0;
    uint8_t pan = 64;
    uint8_t loaded = 0;
    EadTunedSample sound = {};
    int32_t envelope = kNoObject;
};

struct EadSoundEffect
{
    friend bool operator==(const EadSoundEffect&, const EadSoundEffect&) = default;

    EadTunedSample sound = {};
};

struct EadBank
{
    friend bool operator==(const EadBank&, const EadBank&) = default;

    // Reads an early engine's bank. `data` is the bank's entry in its file. A damaged bank throws xsflib::FormatError.
    static EadBank ParseEarly(std::span<const uint8_t> data);

    // Reads a later engine's bank, with the counts from its table.
    static EadBank ParseLater(std::span<const uint8_t> data, std::size_t instrument_count, std::size_t drum_count,
                              std::size_t effect_count);

    std::vector<uint8_t> Write() const;

    EadEngine engine = EadEngine::kEarly;

    // What the lists below index. A program or a drum without an object holds kNoObject.
    std::vector<int32_t> program_instruments = {};
    std::vector<int32_t> drum_notes = {};
    std::vector<EadSoundEffect> effects = {};

    std::vector<EadInstrument> instruments = {};
    std::vector<EadDrum> drums = {};
    std::vector<EadSample> samples = {};
    std::vector<EadEnvelope> envelopes = {};
    std::vector<BankLoop> loops = {};
    std::vector<BankBook> books = {};

    // The offset of the table of offsets of the drums, or 0, and of the sound effects, or 0.
    uint32_t drums_table_offset = 0;
    uint32_t effects_offset = 0;

    // The bank's bytes as read. The writer lays the objects over them.
    std::vector<uint8_t> image = {};
};

// A later engine's table of sequences, banks or samples: a header of 16 bytes with the number of entries and the place
// that the entries count from, then an entry of 16 bytes for each.
struct EadTable
{
    friend bool operator==(const EadTable&, const EadTable&) = default;

    struct Entry
    {
        friend bool operator==(const Entry&, const Entry&) = default;

        uint32_t address = 0;
        uint32_t size = 0;
        uint8_t medium = 0;
        uint8_t cache_policy = 0;

        // A bank's sample tables (data1, low and high bytes) and its counts of instruments, drums and effects
        // (data2's high byte, its low byte and data3). A sequence's entries hold zeros here.
        uint16_t data1 = 0;
        uint16_t data2 = 0;
        uint16_t data3 = 0;
    };

    static constexpr std::size_t kHeaderSize = 16;
    static constexpr std::size_t kEntrySize = 16;

    static EadTable Parse(std::span<const uint8_t> data);
    std::vector<uint8_t> Write() const;

    uint16_t medium_parameter = 0;
    uint32_t base_address = 0;
    std::vector<uint8_t> padding = {};
    std::vector<Entry> entries = {};
};

// An early engine's table of the banks that each sequence uses: a 16-bit offset for each sequence, then for each a
// count and the banks' numbers. The last number comes first in the sequence's use of them.
std::vector<std::vector<uint8_t>> ParseEarlyBankSets(std::span<const uint8_t> data, std::size_t sequence_count);

} // namespace ntdlib::n64
