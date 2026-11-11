// SPDX-License-Identifier: MIT

// Readers and writers of the files of Procyon Studio's sound driver for the DS, Digital Sound Elements (DSE): songs
// (smdl), wave banks (swdl) and banks of sound effects (sedl). Every file is little-endian and of version 0x415. A file
// starts with a header that gives its magic word, its size and its version. Chunks follow. A chunk's header has 16
// bytes: the label, 8 bytes that depend on the chunk's kind, and the size of what follows.
//
// A song holds a song chunk with the song's settings, then a track chunk for each track and an end chunk (eoc). Each
// track chunk ends on a multiple of 4 bytes, padded with 0x98. A track holds a preamble of 4 bytes (its number, its
// channel and two zeros), then its events. A wave bank holds its waves' parameters (wavi), its programs (prgi), its key
// groups (kgrp), its samples (pcmd) and an end chunk (eod), each on a multiple of 16 bytes. A bank's waves can lie in
// another bank, the main bank. A bank of sound effects holds its sequences (seq), two tables (mcrl and bnkl) and an end
// chunk.
//
// The readers keep every byte of a file. The writers give the file back byte for byte, and lay out a changed one in the
// same way.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::dse
{

constexpr uint16_t kVersion = 0x0415;

// An event of a track: a note, a pause or a command. A note's code is its velocity, from 0 to 0x7F. Its first argument
// gives the key in its low 4 bits, the change of octave plus 2 in the next 2 and the number of the length's bytes in
// the top 2. The length's bytes follow, the low byte first. A pause's code, from 0x80 to 0x8F, gives one of 16 fixed
// lengths. A command's code is 0x90 or more.
struct Event
{
    friend bool operator==(const Event&, const Event&) = default;

    uint8_t code = 0;
    std::vector<uint8_t> arguments;
};

constexpr uint8_t kEndTrack = 0x98;

// The codes of the driver's pauses and commands, from 0x80 on. A code below 0x80 plays a note. 0x98 ends a track
// (kEndTrack).
enum Code : uint8_t
{
    kFirstPause = 0x80,
    kLastPause = 0x8F,
    kWaitAgain = 0x90,
    kWaitChange = 0x91,
    kWait1Byte = 0x92,
    kWait2Bytes = 0x93,
    kWait3Bytes = 0x94,
    kWaitForVoices = 0x95,
    kLoopPoint = 0x99,
    kRepeatStart = 0x9C,
    kRepeatEnd = 0x9D,
    kRepeatExit = 0x9E,
    kOctave = 0xA0,
    kOctaveChange = 0xA1,
    kTempo = 0xA4,
    kTempoAgain = 0xA5,
    kBank = 0xA8,
    kBankHigh = 0xA9,
    kBankLow = 0xAA,
    kSkipByte = 0xAB,
    kProgram = 0xAC,
    kSongFade = 0xAF,
    kEnvelopeDefaults = 0xB0,
    kAttackBegin = 0xB1,
    kAttackTime = 0xB2,
    kHoldTime = 0xB3,
    kDecayAndSustain = 0xB4,
    kSustainTime = 0xB5,
    kReleaseTime = 0xB6,
    kLengthMultiplier = 0xBC,
    kLfoLevel = 0xBE,
    kDamper = 0xBF,
    kLegato = 0xC0,
    kOptionalVolume = 0xC3,
    kSkipTwo = 0xCB,
    kTuning = 0xD0,
    kTuningSemitones = 0xD1,
    kTuningSteps = 0xD2,
    kTuningWord = 0xD3,
    kTuningSlide = 0xD4,
    kRandomKeys = 0xD5,
    kJitter = 0xD6,
    kKeyBend = 0xD7,
    kUnusedWord = 0xD8,
    kBendRange = 0xDB,
    kBendLfo = 0xDC,
    kBendLfoEnvelope = 0xDD,
    kBendLfoSwitch = 0xDF,
    kVolume = 0xE0,
    kVolumeChange = 0xE1,
    kVolumeSlide = 0xE2,
    kExpression = 0xE3,
    kVolumeLfo = 0xE4,
    kVolumeLfoEnvelope = 0xE5,
    kVolumeLfoSwitch = 0xE7,
    kPan = 0xE8,
    kPanChange = 0xE9,
    kPanSlide = 0xEA,
    kPanLfo = 0xEC,
    kPanLfoEnvelope = 0xED,
    kPanLfoSwitch = 0xEF,
    kLfo = 0xF0,
    kLfoEnvelope = 0xF1,
    kLfoParameter = 0xF2,
    kLfoSwitch = 0xF3,
    kSignalGame = 0xF6,
    kSkipTwoAgain = 0xF8,
};

// The number of argument bytes that follow `code`, with `next` the byte after the code. A note's arguments depend on
// that byte. Returns -1 for a code that the driver takes as invalid: the track stops there.
int ArgumentBytes(uint8_t code, uint8_t next);

// A track of a song or of a sound effect.
struct Track
{
    friend bool operator==(const Track&, const Track&) = default;

    uint8_t number = 0;
    uint8_t channel = 0;
    std::array<uint8_t, 2> reserved = {}; // the preamble's last two bytes

    // The events up to the first that stops the track: the end (0x98) or a code that the driver takes as invalid. A
    // track that fills its chunk without such an event holds every event of the chunk. The driver then reads on past
    // the chunk.
    std::vector<Event> events = {};

    // The bytes after that event that the chunk's size counts. The driver never reads them.
    std::vector<uint8_t> unread = {};

    // Whether the chunk's size leaves out the last event. That event is then the first byte of the padding. The writer
    // keeps that layout while the event's byte falls in the padding.
    bool end_in_padding = false;
};

// A song's or a sound effect's settings: the 48 bytes after a song chunk's header. Bytes 2 and 3 give the ticks of a
// quarter note, byte 6 the number of tracks and byte 7 the number of channels. The writers keep every byte as it is.
struct SequenceInfo
{
    friend bool operator==(const SequenceInfo&, const SequenceInfo&) = default;

    static constexpr std::size_t kSize = 0x30;

    uint16_t TicksPerQuarter() const;
    uint8_t TrackCount() const;
    uint8_t ChannelCount() const;

    // Each setter writes its value where the getter reads it.
    void SetTicksPerQuarter(uint16_t ticks);
    void SetTrackCount(uint8_t count);
    void SetChannelCount(uint8_t count);

    std::array<uint8_t, kSize> bytes = {};
};

// A song (smdl).
struct Song
{
    friend bool operator==(const Song&, const Song&) = default;

    static constexpr std::size_t kHeaderSize = 0x40;

    // Reads `file`. Throws xsflib::FormatError for another magic word or version, a chunk header that the file's kind
    // doesn't have, a track without an end, or a chunk past the end of `file`.
    static Song Parse(std::span<const uint8_t> file);

    std::vector<uint8_t> Write() const;

    // Every byte of the header: the magic word, the size at 0x08, the version at 0x0C, the date at 0x18 and the name
    // at 0x20, ended by a zero. The tools leave other bytes after the zero. The writer sets the size.
    std::array<uint8_t, kHeaderSize> header = {};
    SequenceInfo info;
    std::vector<Track> tracks;
};

// A wave's parameters: an entry of 64 bytes of a bank's wavi chunk.
struct WaveInfo
{
    friend bool operator==(const WaveInfo&, const WaveInfo&) = default;

    static constexpr std::size_t kSize = 0x40;

    uint16_t Id() const;
    uint16_t Format() const; // 0 for PCM8, 0x100 for PCM16, 0x200 for IMA-ADPCM
    bool Loops() const;
    uint32_t SampleRate() const;
    uint32_t SamplePosition() const; // in the samples of the bank that holds them
    uint32_t LoopStart() const;      // in 32-bit words
    uint32_t LoopLength() const;     // in 32-bit words

    // Each setter writes its value where the getter reads it. SetLoops writes 1 or 0.
    void SetId(uint16_t id);
    void SetFormat(uint16_t format);
    void SetLoops(bool loops);
    void SetSampleRate(uint32_t rate);
    void SetSamplePosition(uint32_t position);
    void SetLoopStart(uint32_t start);
    void SetLoopLength(uint32_t length);

    std::array<uint8_t, kSize> bytes = {};
};

// A program of a bank's prgi chunk: its header, its LFOs, 16 bytes that the tools fill, and its splits. The header
// gives the number of the splits in its bytes 2 and 3, and the number of the LFOs in byte 11. The writer sets both.
struct Program
{
    friend bool operator==(const Program&, const Program&) = default;

    uint16_t Id() const;

    // The program's bytes as the driver keeps them: the header, the LFOs, the 16 bytes after them and the splits.
    std::vector<uint8_t> Bytes() const;

    std::array<uint8_t, 0x10> header = {};
    std::vector<std::array<uint8_t, 0x10>> lfos;
    std::array<uint8_t, 0x10> filler = {};
    std::vector<std::array<uint8_t, 0x30>> splits;
};

// A view of a split of a program (Program::splits): 48 bytes that play a wave of the bank for the keys and the
// velocities in its ranges. Each getter reads its value where the driver reads it. The tools write the key range and
// the velocity range twice, the copy 2 bytes on, and the driver reads the first. The tools also write a root key that
// the driver doesn't read: 60 less the coarse tuning. The driver reads the envelope's 16 bytes from kEnvelope.
//
// `Byte` is `const uint8_t` for a view that only reads the bytes (SplitView), and `uint8_t` for one that also writes
// them (MutableSplitView). The setters write the values that take more than one byte or a sign. The other values are
// single bytes, written at their places in Bytes().
template <typename Byte>
class BasicSplitView
{
public:
    static constexpr std::size_t kSize = 0x30;

    // The places of the values.
    static constexpr std::size_t kIndex = 0x01;
    static constexpr std::size_t kBendRange = 0x02;
    static constexpr std::size_t kLowKey = 0x04;
    static constexpr std::size_t kHighKey = 0x05;
    static constexpr std::size_t kLowVelocity = 0x08;
    static constexpr std::size_t kHighVelocity = 0x09;
    static constexpr std::size_t kWave = 0x12;
    static constexpr std::size_t kTuning = 0x14;
    static constexpr std::size_t kRootKey = 0x16;
    static constexpr std::size_t kCoarseTuning = 0x17;
    static constexpr std::size_t kVolume = 0x18;
    static constexpr std::size_t kPan = 0x19;
    static constexpr std::size_t kKeyGroup = 0x1A;
    static constexpr std::size_t kEnvelope = 0x20;
    static constexpr std::size_t kEnvelopeMultiplier = 0x21;
    static constexpr std::size_t kAttackBegin = 0x28;
    static constexpr std::size_t kAttackTime = 0x29;
    static constexpr std::size_t kDecayTime = 0x2A;
    static constexpr std::size_t kSustainLevel = 0x2B;
    static constexpr std::size_t kHoldTime = 0x2C;
    static constexpr std::size_t kSustainTime = 0x2D;
    static constexpr std::size_t kReleaseTime = 0x2E;

    // The place of the copy of a key range or a velocity range, from the place of the range's first value.
    static constexpr std::size_t kRangeCopy = 2;

    explicit BasicSplitView(std::span<Byte, kSize> bytes) : bytes_(bytes)
    {
    }

    std::span<Byte, kSize> Bytes() const
    {
        return bytes_;
    }

    // The split's index among the program's splits. The driver's search for a note's splits stops after the split
    // whose index is the program's count of splits less 1.
    uint8_t Index() const
    {
        return bytes_[kIndex];
    }

    // In semitones.
    uint8_t BendRange() const
    {
        return bytes_[kBendRange];
    }

    uint8_t LowKey() const
    {
        return bytes_[kLowKey];
    }

    uint8_t HighKey() const
    {
        return bytes_[kHighKey];
    }

    uint8_t LowVelocity() const
    {
        return bytes_[kLowVelocity];
    }

    uint8_t HighVelocity() const
    {
        return bytes_[kHighVelocity];
    }

    // The wave's slot in the bank's table (WaveBank::waves).
    uint16_t Wave() const
    {
        return xsflib::LoadLe16(&bytes_[kWave]);
    }

    // In 256ths of a semitone.
    int16_t Tuning() const
    {
        return static_cast<int16_t>(xsflib::LoadLe16(&bytes_[kTuning]));
    }

    uint8_t RootKey() const
    {
        return bytes_[kRootKey];
    }

    // In semitones.
    int8_t CoarseTuning() const
    {
        return static_cast<int8_t>(bytes_[kCoarseTuning]);
    }

    uint8_t Volume() const
    {
        return bytes_[kVolume];
    }

    // 0x40 is the centre.
    uint8_t Pan() const
    {
        return bytes_[kPan];
    }

    // The key group's slot in the bank's kgrp chunk (Instruments::key_groups).
    uint8_t KeyGroup() const
    {
        return bytes_[kKeyGroup];
    }

    std::span<Byte, 16> Envelope() const
    {
        return bytes_.template subspan<kEnvelope, 16>();
    }

    uint8_t EnvelopeMultiplier() const
    {
        return bytes_[kEnvelopeMultiplier];
    }

    uint8_t AttackBegin() const
    {
        return bytes_[kAttackBegin];
    }

    uint8_t AttackTime() const
    {
        return bytes_[kAttackTime];
    }

    uint8_t DecayTime() const
    {
        return bytes_[kDecayTime];
    }

    uint8_t SustainLevel() const
    {
        return bytes_[kSustainLevel];
    }

    uint8_t HoldTime() const
    {
        return bytes_[kHoldTime];
    }

    uint8_t SustainTime() const
    {
        return bytes_[kSustainTime];
    }

    uint8_t ReleaseTime() const
    {
        return bytes_[kReleaseTime];
    }

    // Writes the key range and its copy.
    void SetKeys(uint8_t low, uint8_t high) const
        requires(!std::is_const_v<Byte>)
    {
        bytes_[kLowKey] = low;
        bytes_[kHighKey] = high;
        bytes_[kLowKey + kRangeCopy] = low;
        bytes_[kHighKey + kRangeCopy] = high;
    }

    // Writes the velocity range and its copy.
    void SetVelocities(uint8_t low, uint8_t high) const
        requires(!std::is_const_v<Byte>)
    {
        bytes_[kLowVelocity] = low;
        bytes_[kHighVelocity] = high;
        bytes_[kLowVelocity + kRangeCopy] = low;
        bytes_[kHighVelocity + kRangeCopy] = high;
    }

    void SetWave(uint16_t wave) const
        requires(!std::is_const_v<Byte>)
    {
        xsflib::StoreLe16(&bytes_[kWave], wave);
    }

    void SetTuning(int16_t tuning) const
        requires(!std::is_const_v<Byte>)
    {
        xsflib::StoreLe16(&bytes_[kTuning], static_cast<uint16_t>(tuning));
    }

    void SetCoarseTuning(int8_t semitones) const
        requires(!std::is_const_v<Byte>)
    {
        bytes_[kCoarseTuning] = static_cast<uint8_t>(semitones);
    }

private:
    std::span<Byte, kSize> bytes_;
};

using SplitView = BasicSplitView<const uint8_t>;
using MutableSplitView = BasicSplitView<uint8_t>;

// A bank's programs and key groups: its prgi and kgrp chunks.
struct Instruments
{
    friend bool operator==(const Instruments&, const Instruments&) = default;

    std::vector<std::optional<Program>> programs; // by the program's number, the table's slots
    std::vector<std::array<uint8_t, 8>> key_groups;
    std::vector<uint8_t> after_key_groups; // the bytes that pad the kgrp chunk to 16 bytes
};

// A wave bank (swdl).
struct WaveBank
{
    friend bool operator==(const WaveBank&, const WaveBank&) = default;

    static constexpr std::size_t kHeaderSize = 0x50;

    // A bank's header gives this size of samples when the bank holds none.
    static constexpr uint32_t kNoSamples = 0xAAAA0000;

    // Reads `file`. Throws xsflib::FormatError as Song::Parse does, and for a table that names an entry past its
    // chunk or an entry of another number.
    static WaveBank Parse(std::span<const uint8_t> file);

    std::vector<uint8_t> Write() const;

    // Every byte of the header, with the date and the name as in a song's. The writer sets the size at 0x08, the size
    // of the samples at 0x40 when the bank holds them, the slots of the waves at 0x46, those of the programs at 0x48
    // when the bank holds programs, and the size of the wavi chunk at 0x4C.
    std::array<uint8_t, kHeaderSize> header = {};
    std::vector<std::optional<WaveInfo>> waves; // by the wave's number, the table's slots
    std::optional<Instruments> instruments;
    std::optional<std::vector<uint8_t>> samples; // the pcmd chunk
    std::vector<uint8_t> after_samples;          // the bytes that pad the pcmd chunk to 16 bytes
};

// A table of records of a bank of sound effects (mcrl or bnkl): a 16-bit offset for each slot, then the records in
// the order of their slots. An empty slot holds 0.
struct RecordTable
{
    friend bool operator==(const RecordTable&, const RecordTable&) = default;

    std::vector<std::optional<std::vector<uint8_t>>> slots;
};

// A sound effect: a sequence of a bank's seq chunk.
struct EffectSequence
{
    friend bool operator==(const EffectSequence&, const EffectSequence&) = default;

    SequenceInfo info;
    std::vector<Track> tracks;
};

// A bank of sound effects (sedl).
struct SoundEffects
{
    friend bool operator==(const SoundEffects&, const SoundEffects&) = default;

    static constexpr std::size_t kHeaderSize = 0x40;

    // Reads `file`. Throws xsflib::FormatError as WaveBank::Parse does.
    static SoundEffects Parse(std::span<const uint8_t> file);

    std::vector<uint8_t> Write() const;

    // The ids of the wave banks that a game loads for effect `effect`, from the record of its slot in the bnkl chunk:
    // the number of the ids in 16 bits, 16 bits of zeros, then the ids. Nothing for a slot without a record, and only
    // the ids that the record holds.
    std::vector<uint16_t> BankIds(std::size_t effect) const;

    // Every byte of the header, with the date and the name as in a song's and the bank's number at 0x0E. The writer
    // sets the size at 0x08 and the number of the sequences' slots at 0x30.
    std::array<uint8_t, kHeaderSize> header = {};
    std::vector<std::optional<EffectSequence>> sequences; // the seq chunk's slots
    RecordTable macros;                                   // mcrl
    RecordTable bank_lists;                               // bnkl
};

} // namespace ntdlib::dse
