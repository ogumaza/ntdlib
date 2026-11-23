// SPDX-License-Identifier: MIT

// The sound data of Nintendo SPD's sound driver for the DS. The driver runs on the ARM7. It plays sequences in a format
// of its own on the hardware channels: PCM and IMA-ADPCM samples, square waves and noise.
//
// A game keeps its sound data in one file that starts with the magic word `SDFDAT`. The file's header gives a table of
// blocks at 0x14: an offset from the file's start and a size for each block, with (0, 0) for an empty slot. A block
// holds sounds, instruments and samples, and the game loads it whole. The driver keeps two blocks at once: set A,
// whose programs are numbered from 0 to 63, and set B, whose programs are numbered from 64 on. A sound of either block
// can play the programs of both. In the one game read so far, block 0 is set A in every scene.
//
// A block starts with its size, the offsets of its program table, its kit table and its sound table, and the number of
// its sounds. Fixed tables follow: the maps of 16 programs at 0x14, the maps of 8 drum kits at 0x814, 64 words at
// 0xA54 that the driver reads at the end of a sound, and 16 words of pitch modulation at 0xB54. The program and kit
// tables give an offset for each program and kit of its list of instruments. An instrument has 12 bytes. A sample's
// header has 12 bytes, and its data follows.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib::spd
{

// The programs of a set at most, and the instruments that a program's map can name.
constexpr int kProgramMaps = 16;
constexpr int kProgramMapSize = 128;
// The kits of a set at most, and the keys of a kit's map: 36 to 107.
constexpr int kKitMaps = 8;
constexpr int kFirstKitKey = 36;
constexpr int kKitKeys = 72;
// The pitch modulations of a set.
constexpr int kModulations = 16;
// The first program of set B.
constexpr int kSetBPrograms = 64;

// The offsets of a block's fixed tables.
constexpr std::size_t kProgramMapsOffset = 0x14;
constexpr std::size_t kKitMapsOffset = 0x814;
constexpr std::size_t kEndActionsOffset = 0xA54;
constexpr std::size_t kModulationsOffset = 0xB54;
constexpr std::size_t kBlockHeaderEnd = 0xB94;

// The kinds of an instrument, from the low 3 bits of its third word. The driver gives a square wave to the channels of
// its first group and noise to those of its second. A kind's name for 1 and 2 follows the channels' order on the DS.
enum class InstrumentKind : uint8_t
{
    kSquare = 1,
    kNoise = 2,
    kSample = 3,
    kKit = 7,
};

// An instrument's envelope and pitch bend range, from its second word. The times count the driver's frames.
struct Envelope
{
    int bend_range = 0; // semitones for a pitch bend of 128
    int attack = 0;     // frames from silence to the peak
    int decay = 0;      // frames from the peak to the sustain level
    int sustain = 0;    // the sustain level in 64ths of the peak, from 1 to 64
    int release = 0;    // frames from the sustain level to silence
};

// An instrument of a program's or a kit's list.
struct Instrument
{
    uint32_t sample = 0;   // the offset of the sample's header in the block, for a sample
    uint32_t envelope = 0; // see Envelope
    uint32_t settings = 0; // the kind in bits 0-2, then the fields below

    InstrumentKind Kind() const;
    // The kit of a kit instrument (bits 3-9), with its set's offset of 64 for set B.
    int Kit() const;
    // A square wave's duty in eighths, less 1 (bits 3-5).
    int Duty() const;
    // The pitch modulation (bits 10-16), or none.
    std::optional<int> Modulation() const;
    // A kit's instrument plays every key at this key when it has one (bits 17-24).
    std::optional<int> FixedKey() const;
    // A kit's instrument with a fixed key plays at this pan, from 0 to 127 (bits 25-31).
    int KitPan() const;
    Envelope EnvelopeOf() const;
};

// The formats of SOUNDxCNT, bits 29-30.
enum class SampleFormat : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kImaAdpcm = 2,
};

// A sample: its header and the bytes that the hardware plays. The header has 12 bytes, and the sample's data follows.
// The driver gives the hardware the header's address plus 10 as the data's start. Super Princess Peach loads its blocks
// at 0x0214A140, a multiple of 4, and the hardware ignores an address's low 2 bits. The hardware therefore plays from
// the header's byte 8: the loop's length in words and two zeros come first, and the data's last word never plays.
struct Sample
{
    uint32_t offset = 0;           // the header's offset in the block
    uint8_t control = 0;           // SOUNDxCNT bits 24-31 without the start bit: the repeat mode and the format
    uint8_t root_key = 0;          // the key that plays the sample at its timer
    uint16_t timer = 0;            // the hardware timer's period at the root key: 16,756,991 Hz over this
    uint16_t loop_start = 0;       // SOUNDxPNT, in words
    uint32_t loop_length = 0;      // SOUNDxLEN, in words
    std::span<const uint8_t> data; // from the header's byte 8, (loop_start + loop_length) words

    SampleFormat Format() const;
    // Whether the hardware repeats the loop (repeat mode 1).
    bool Loops() const;
    // The rate at which the hardware plays the sample at the root key.
    double Rate() const;
};

// A sound of a block's sound table: the offset of its header and a word of flags. Bits 20-25 and 26-31 of the flags
// name actions of the table at 0xA54.
struct Sound
{
    uint32_t offset = 0;
    uint32_t flags = 0;
    std::vector<uint32_t> tracks; // the offsets of the tracks' first events
};

// The player of a sound's flags (bits 0-3). Player 0 plays the music.
int PlayerOf(uint32_t flags);

// The volume of a sound's flags (bits 4-10).
int VolumeOf(uint32_t flags);

// A block of the sound data. It keeps a view of the block's bytes.
class Block
{
public:
    // Reads a block. Throws xsflib::FormatError for a block whose header or tables run past its end.
    explicit Block(std::span<const uint8_t> bytes);

    std::span<const uint8_t> Bytes() const
    {
        return bytes_;
    }
    int ProgramCount() const
    {
        return program_count_;
    }
    int KitCount() const
    {
        return kit_count_;
    }
    int SoundCount() const
    {
        return sound_count_;
    }

    // The sound at `index`, or none for an empty entry. Throws xsflib::FormatError for a sound that runs past the
    // block.
    std::optional<Sound> SoundAt(int index) const;
    // The instrument that `F1 index` selects in `program` of this block, or none where the program's map has none.
    std::optional<Instrument> ProgramInstrument(int program, int index) const;
    // The instrument of `key` in `kit` of this block, or none where the kit's map has none or the key is outside 36 to
    // 107.
    std::optional<Instrument> KitInstrument(int kit, int key) const;
    // The modulation word at `index`, from 0 to 15.
    uint32_t ModulationWord(int index) const;
    // The sample whose header is at `offset`. Throws xsflib::FormatError for a sample that runs past the block.
    Sample SampleAt(uint32_t offset) const;
    // The 16-bit event at `offset`. Throws xsflib::FormatError past the block.
    uint16_t EventAt(uint32_t offset) const;

private:
    std::span<const uint8_t> bytes_;
    uint32_t program_table_ = 0;
    uint32_t kit_table_ = 0;
    uint32_t sound_table_ = 0;
    int program_count_ = 0;
    int kit_count_ = 0;
    int sound_count_ = 0;
};

// A block's place in the sound data file.
struct BlockPlace
{
    int slot = 0; // its slot in the header's table
    uint32_t offset = 0;
    uint32_t size = 0;
};

// Whether `bytes` start with the sound data's magic word.
bool IsSoundData(std::span<const uint8_t> bytes);

// The blocks of a sound data file, in the order of the header's slots. Throws xsflib::FormatError for a file without
// the magic word, or whose table names a block past the file's end.
std::vector<BlockPlace> ReadBlocks(std::span<const uint8_t> bytes);

} // namespace ntdlib::spd
