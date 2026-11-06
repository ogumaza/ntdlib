// SPDX-License-Identifier: MIT

// The two sequence formats of libultra's sequence players: standard MIDI files of format 0, played by ALSeqPlayer, and
// compressed MIDI, played by ALCSPlayer.
//
// A standard MIDI file holds a header chunk and track chunks. The MIDI specification gives their form. libultra loops
// such a file through controllers 102 and 103.
//
// A compressed MIDI file starts with 16 big-endian offsets of tracks and the ticks of a quarter note: 68 bytes. An
// offset of 0 means that the channel has no track. A track holds events like those of a standard MIDI file, with three
// differences. A note on holds its duration as a variable-length value after its velocity, and no note off follows. A
// meta event has no length: a tempo (0x51) holds 3 bytes, a loop start (0x2E) holds its number and 0xFF, a loop end
// (0x2D) holds a count, a running count and a 32-bit offset, and an end of track (0x2F) holds nothing. And a byte of
// 0xFE starts a block code. 0xFE 0xFE stands for one byte of 0xFE. Otherwise a 16-bit distance back from the code's
// first byte and a length follow, and the player reads that many raw bytes from there. Those bytes hold no further
// codes. A block can reach back into earlier tracks.
//
// A loop end's offset leads back, in the file's bytes, from the end of the event to the bytes after its loop start.
//
// The readers keep every byte of a file and give it back byte for byte. Build makes a new file from events.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::n64
{

constexpr uint8_t kMidiMeta = 0xFF;
constexpr uint8_t kMidiMetaEndOfTrack = 0x2F;
constexpr uint8_t kMidiMetaTempo = 0x51;
constexpr uint8_t kCompressedLoopStart = 0x2E;
constexpr uint8_t kCompressedLoopEnd = 0x2D;

// libultra's controllers that mark a standard MIDI file's loop.
constexpr uint8_t kMidiLoopStartController = 102;
constexpr uint8_t kMidiLoopEndController = 103;

// An event of a track. Two events compare equal whatever their places in a file.
struct MidiEvent
{
    friend bool operator==(const MidiEvent& a, const MidiEvent& b);

    bool IsChannel() const;
    bool IsMeta(uint8_t type) const;
    bool IsNoteOn() const;

    // The tempo event's microseconds of a quarter note.
    uint32_t TempoMicroseconds() const;

    uint32_t delta = 0;    // ticks after the track's previous event
    uint8_t status = 0;    // a channel message's status, 0xFF for a meta event, or 0xF0 or 0xF7 for a system exclusive
    uint8_t type = 0;      // a meta event's type
    uint8_t data1 = 0;     // a channel message's first data byte
    uint8_t data2 = 0;     // a channel message's second data byte, if it has one
    uint32_t duration = 0; // the ticks of a compressed note on

    // Whether the event left out its status byte.
    bool running_status = false;

    // What a meta event or a system exclusive holds after its type and any length. A loop end's offset stays here as
    // the file holds it. Build works out a new one.
    std::vector<uint8_t> payload = {};

    // The index in the track of the loop start that a compressed loop end leads back to, or -1.
    int32_t loop_start = -1;

    // The offset in the file of the first byte of the event. For an event that a block holds, it lies in the block's
    // source. It's the place that a sequence player reads the event from.
    uint32_t file_offset = 0;
};

// A compressed MIDI file.
class CompressedMidi
{
public:
    static constexpr std::size_t kTrackCount = 16;
    static constexpr std::size_t kHeaderSize = 68;
    static constexpr uint8_t kBlockCode = 0xFE;

    // Reads the file in `data`. Every track has to decode, up to its end of track, inside `data`. A bad file throws
    // xsflib::FormatError.
    static CompressedMidi Parse(std::span<const uint8_t> data);

    // Whether `data` is a file that Parse reads.
    static bool Matches(std::span<const uint8_t> data);

    // Makes a file from up to 16 tracks of events, with blocks where they save bytes. An empty track gets the offset 0.
    static CompressedMidi Build(std::span<const std::vector<MidiEvent>> tracks, int32_t division);

    int32_t Division() const;

    // The offset of a track in the file, or 0.
    uint32_t TrackOffset(std::size_t track) const;

    // The track's bytes as the player reads them: blocks and doubled block codes expanded.
    std::vector<uint8_t> ExpandTrack(std::size_t track) const;

    // The track's events. A track without an offset has none.
    std::vector<MidiEvent> DecodeTrack(std::size_t track) const;

    std::span<const uint8_t> Bytes() const;

private:
    std::vector<uint8_t> bytes_;
};

// A standard MIDI file.
class StandardMidi
{
public:
    // A chunk's place. `offset` gives the first byte after its header.
    struct Chunk
    {
        friend bool operator==(const Chunk&, const Chunk&) = default;

        std::array<uint8_t, 4> id = {};
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    // Reads the file at the start of `data`, up to the end of its last track. `data` can run on past the file. Every
    // track has to decode. A bad file throws xsflib::FormatError.
    static StandardMidi Parse(std::span<const uint8_t> data);

    // Whether `data` starts with a file that Parse reads.
    static bool Matches(std::span<const uint8_t> data);

    // Makes a file with one track chunk for each list of events. The events keep their running status where they say
    // so and the status allows it.
    static StandardMidi Build(uint16_t format, uint16_t division, std::span<const std::vector<MidiEvent>> tracks);

    uint16_t Format() const;
    uint16_t Division() const;
    std::size_t TrackCount() const;
    const std::vector<Chunk>& Chunks() const;

    // The track's events, up to its end of track.
    std::vector<MidiEvent> DecodeTrack(std::size_t track) const;

    std::span<const uint8_t> Bytes() const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<Chunk> chunks_;
    std::vector<std::size_t> tracks_;
};

} // namespace ntdlib::n64
