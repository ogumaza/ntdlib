// SPDX-License-Identifier: MIT

// libultra's bank file: the instruments of a sound bank, without their sample data. The SDK's tools make it from a text
// description. It is the .ctl file beside a .tbl file of samples. Every field is big-endian. The file starts with the
// revision "B1" and a table of offsets of banks. The objects of a bank point to each other by their offsets from the
// file's start. Two pointers can share one object. An object lies on a multiple of 8 bytes.
//
//     bank:       a count of instruments, flags, padding, a sample rate, a percussion instrument and the instruments
//     instrument: volume, pan, priority, flags, tremolo and vibrato settings, a pitch bend range and the sounds
//     sound:      an envelope, a key map, a wave, the sample's pan and volume, and flags
//     wave:       the sample's offset in the .tbl file and its length in bytes, a type, flags, a loop and a book
//
// The reader gathers each object once, in the order of the file's offsets, and keeps the file's bytes. The writer lays
// the fields over those bytes at each object's offset. An unchanged file then comes back byte for byte. An object that
// the reader never saw, or a list that grew past what its place holds, goes at the end of the file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/n64/vadpcm.h"

namespace ntdlib::n64
{

constexpr uint16_t kBankFileRevision = 0x4231; // "B1"

// The offset of an object that has no place in the file yet.
constexpr uint32_t kNoOffset = 0;

// An index that points to no object.
constexpr int32_t kNoObject = -1;

// The wave types.
constexpr uint8_t kWaveVadpcm = 0;
constexpr uint8_t kWaveRaw16 = 1;

struct BankEnvelope
{
    friend bool operator==(const BankEnvelope&, const BankEnvelope&) = default;

    uint32_t offset = kNoOffset;

    // Times in microseconds. A release time of -1 holds the sound.
    int32_t attack_time = 0;
    int32_t decay_time = 0;
    int32_t release_time = 0;
    uint8_t attack_volume = 0;
    uint8_t decay_volume = 0;
};

struct BankKeyMap
{
    friend bool operator==(const BankKeyMap&, const BankKeyMap&) = default;

    uint32_t offset = kNoOffset;
    uint8_t velocity_min = 0;
    uint8_t velocity_max = 0;
    uint8_t key_min = 0;
    uint8_t key_max = 0;
    uint8_t key_base = 0;
    int8_t detune = 0; // in cents
};

// A wave's loop. `count` of 0xFFFFFFFF loops for ever. A VADPCM wave's loop holds the decoder's state at the loop's
// start. A raw wave's loop has no state.
struct BankLoop
{
    friend bool operator==(const BankLoop&, const BankLoop&) = default;

    uint32_t offset = kNoOffset;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t count = 0;
    uint32_t sample_end = 0; // in a later EAD engine's bank: where a sample ends after its loop
    bool has_state = false;
    VadpcmState state = {};
};

struct BankBook
{
    friend bool operator==(const BankBook&, const BankBook&) = default;

    uint32_t offset = kNoOffset;
    VadpcmBook book = {};
};

struct BankWave
{
    friend bool operator==(const BankWave&, const BankWave&) = default;

    uint32_t offset = kNoOffset;
    uint32_t base = 0; // the sample's offset in the .tbl file
    uint32_t length = 0;
    uint8_t type = kWaveVadpcm;
    uint8_t flags = 0;
    int32_t loop = kNoObject;
    int32_t book = kNoObject;
};

struct BankSound
{
    friend bool operator==(const BankSound&, const BankSound&) = default;

    uint32_t offset = kNoOffset;
    int32_t envelope = kNoObject;
    int32_t key_map = kNoObject;
    int32_t wave = kNoObject;
    uint8_t pan = 64;
    uint8_t volume = 127;
    uint8_t flags = 0;
};

struct BankInstrument
{
    friend bool operator==(const BankInstrument&, const BankInstrument&) = default;

    uint32_t offset = kNoOffset;
    uint8_t volume = 127;
    uint8_t pan = 64;
    uint8_t priority = 0;
    uint8_t flags = 0;
    uint8_t tremolo_type = 0;
    uint8_t tremolo_rate = 0;
    uint8_t tremolo_depth = 0;
    uint8_t tremolo_delay = 0;
    uint8_t vibrato_type = 0;
    uint8_t vibrato_rate = 0;
    uint8_t vibrato_depth = 0;
    uint8_t vibrato_delay = 0;
    int16_t bend_range = 0; // in cents
    std::vector<int32_t> sounds = {};

    // The number of sounds that the object's place in the file holds.
    std::size_t capacity = 0;
};

struct Bank
{
    friend bool operator==(const Bank&, const Bank&) = default;

    uint32_t offset = kNoOffset;
    uint8_t flags = 0;
    uint8_t padding = 0;
    int32_t sample_rate = 0;
    int32_t percussion = kNoObject;
    std::vector<int32_t> instruments = {}; // kNoObject for a program without an instrument

    // The number of instruments that the object's place in the file holds.
    std::size_t capacity = 0;
};

// A bank file's objects. The objects point to each other by their indices in these lists.
struct BankFile
{
    friend bool operator==(const BankFile&, const BankFile&) = default;

    // Reads the file at the start of `data`. `data` can run on past the file. A file that isn't a bank file, or an
    // object that leaves `data`, throws xsflib::FormatError.
    static BankFile Parse(std::span<const uint8_t> data);

    // Whether `data` starts with a bank file that Parse reads.
    static bool Matches(std::span<const uint8_t> data);

    std::vector<uint8_t> Write() const;

    std::vector<Bank> banks = {};
    std::vector<BankInstrument> instruments = {};
    std::vector<BankSound> sounds = {};
    std::vector<BankEnvelope> envelopes = {};
    std::vector<BankKeyMap> key_maps = {};
    std::vector<BankWave> waves = {};
    std::vector<BankLoop> loops = {};
    std::vector<BankBook> books = {};

    // The file's bytes as read, up to the end of its last object. The writer lays the objects over them.
    std::vector<uint8_t> image = {};
};

} // namespace ntdlib::n64
