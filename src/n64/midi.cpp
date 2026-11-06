// SPDX-License-Identifier: MIT

#include "ntdlib/n64/midi.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

constexpr uint32_t kMaxVariableLength = 0x0FFFFFFF;
constexpr std::size_t kVariableLengthBytes = 4;
constexpr std::size_t kLoopEndOffsetBytes = 4;
constexpr std::size_t kLoopEndPayload = 2 + kLoopEndOffsetBytes;

// A reference costs 4 bytes. The SDK's tool makes none under 7 bytes, and neither does Build.
constexpr std::size_t kMinBlock = 7;
constexpr std::size_t kMaxBlock = 255;
constexpr std::size_t kMaxBlockDistance = 0xFFFF;

constexpr std::size_t kMidiHeaderSize = 14;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::array<uint8_t, 4> kHeaderId = {'M', 'T', 'h', 'd'};
constexpr std::array<uint8_t, 4> kTrackId = {'M', 'T', 'r', 'k'};

// The number of data bytes after a channel message's status. A note on's duration comes on top in compressed files.
int DataBytes(uint8_t status)
{
    const int kind = status & 0xF0;

    return (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
}

void AppendVariableLength(std::vector<uint8_t>& out, uint32_t value)
{
    if (value > kMaxVariableLength)
    {
        throw xsflib::FormatError("a MIDI value of more than 28 bits");
    }

    std::array<uint8_t, kVariableLengthBytes> bytes = {};
    std::size_t count = 0;
    do
    {
        bytes[count++] = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
    } while (value != 0);

    while (count > 1)
    {
        out.push_back(static_cast<uint8_t>(bytes[--count] | 0x80));
    }
    out.push_back(bytes[0]);
}

// Reads a track's bytes, with the place in the file of each.
class ByteSource
{
public:
    virtual ~ByteSource() = default;

    virtual uint8_t Next() = 0;
    virtual uint32_t Position() const = 0;
};

// Reads a compressed track as the player does.
class CompressedSource final : public ByteSource
{
public:
    CompressedSource(std::span<const uint8_t> file, std::size_t start) : file_(file), position_(start)
    {
    }

    uint8_t Next() override
    {
        if (block_left_ > 0)
        {
            block_left_--;

            return At(block_position_++);
        }

        const uint8_t byte = At(position_++);
        if (byte != CompressedMidi::kBlockCode)
        {
            return byte;
        }

        const uint8_t first = At(position_++);
        if (first == CompressedMidi::kBlockCode)
        {
            return byte;
        }

        const std::size_t distance = (static_cast<std::size_t>(first) << 8) | At(position_);
        const std::size_t length = At(position_ + 1);
        const std::size_t code = position_ - 2;
        position_ += 2;
        if (distance > code || length == 0)
        {
            throw xsflib::FormatError("a block of a compressed MIDI track at offset " + std::to_string(code) +
                                      " leaves the file");
        }

        block_position_ = code - distance;
        block_left_ = length - 1;

        return At(block_position_++);
    }

    uint32_t Position() const override
    {
        return static_cast<uint32_t>(block_left_ > 0 ? block_position_ : PeekPosition());
    }

    // The place of the next literal byte in the file, after any block.
    std::size_t RawPosition() const
    {
        return position_;
    }

    bool InBlock() const
    {
        return block_left_ > 0;
    }

private:
    uint8_t At(std::size_t offset) const
    {
        if (offset >= file_.size())
        {
            throw xsflib::FormatError("a compressed MIDI track runs past the end of the file");
        }

        return file_[offset];
    }

    // The place that the next byte comes from. A block's code stands for the block's first byte.
    std::size_t PeekPosition() const
    {
        if (position_ + 3 < file_.size() && file_[position_] == CompressedMidi::kBlockCode &&
            file_[position_ + 1] != CompressedMidi::kBlockCode)
        {
            const std::size_t distance = (static_cast<std::size_t>(file_[position_ + 1]) << 8) | file_[position_ + 2];
            if (distance <= position_)
            {
                return position_ - distance;
            }
        }

        return position_;
    }

    std::span<const uint8_t> file_;
    std::size_t position_;
    std::size_t block_position_ = 0;
    std::size_t block_left_ = 0;
};

uint32_t ReadVariableLength(ByteSource& source)
{
    uint32_t value = 0;
    for (std::size_t i = 0; i < kVariableLengthBytes; i++)
    {
        const uint8_t byte = source.Next();
        value = (value << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0)
        {
            return value;
        }
    }

    throw xsflib::FormatError("a MIDI variable-length value of more than 4 bytes");
}

// The number of bytes that a meta event of a compressed track holds, or -1 for a type that the player doesn't read.
int CompressedMetaBytes(uint8_t type)
{
    switch (type)
    {
    case kMidiMetaTempo:
        return 3;
    case kCompressedLoopStart:
        return 2;
    case kCompressedLoopEnd:
        return static_cast<int>(kLoopEndPayload);
    case kMidiMetaEndOfTrack:
        return 0;
    default:
        return -1;
    }
}

std::vector<MidiEvent> DecodeCompressedTrack(std::span<const uint8_t> file, std::size_t start, bool& ended)
{
    CompressedSource source(file, start);
    std::vector<MidiEvent> events;
    uint8_t last_status = 0;

    // The raw place in the file after each loop start, by the index of the loop start.
    std::unordered_map<std::size_t, int32_t> after_loop_start;
    ended = false;
    while (!ended)
    {
        MidiEvent event;
        event.file_offset = source.Position();
        event.delta = ReadVariableLength(source);

        const uint8_t first = source.Next();
        if (first == kMidiMeta)
        {
            event.status = kMidiMeta;
            event.type = source.Next();

            const int bytes = CompressedMetaBytes(event.type);
            if (bytes < 0)
            {
                throw xsflib::FormatError("a compressed MIDI track holds the unknown meta event " +
                                          std::to_string(event.type));
            }

            event.payload.resize(static_cast<std::size_t>(bytes));
            for (uint8_t& byte : event.payload)
            {
                byte = source.Next();
            }

            if (event.type == kCompressedLoopStart && !source.InBlock())
            {
                after_loop_start[source.RawPosition()] = static_cast<int32_t>(events.size());
            }
            else if (event.type == kCompressedLoopEnd && !source.InBlock())
            {
                const uint32_t offset = xsflib::LoadBe32(event.payload.data() + 2);
                const auto found = after_loop_start.find(source.RawPosition() - offset);
                if (found != after_loop_start.end())
                {
                    event.loop_start = found->second;
                }
            }
            else if (event.type == kMidiMetaEndOfTrack)
            {
                ended = true;
            }
        }
        else
        {
            if ((first & 0x80) != 0)
            {
                event.status = first;
                event.data1 = source.Next();
                last_status = first;
            }
            else
            {
                if (last_status == 0)
                {
                    throw xsflib::FormatError("a compressed MIDI track uses a running status before any status");
                }

                event.status = last_status;
                event.data1 = first;
                event.running_status = true;
            }

            if ((event.status & 0xF0) == 0xF0)
            {
                throw xsflib::FormatError("a compressed MIDI track holds the status " + std::to_string(event.status));
            }

            if (DataBytes(event.status) == 2)
            {
                event.data2 = source.Next();
            }

            if ((event.status & 0xF0) == 0x90)
            {
                event.duration = ReadVariableLength(source);
            }
        }

        events.push_back(std::move(event));
    }

    return events;
}

void AppendCompressedEvent(std::vector<uint8_t>& out, const MidiEvent& event)
{
    AppendVariableLength(out, event.delta);
    if (event.status == kMidiMeta)
    {
        out.push_back(kMidiMeta);
        out.push_back(event.type);
        out.insert(out.end(), event.payload.begin(), event.payload.end());

        return;
    }

    if (!event.running_status)
    {
        out.push_back(event.status);
    }

    out.push_back(event.data1);
    if (DataBytes(event.status) == 2)
    {
        out.push_back(event.data2);
    }

    if ((event.status & 0xF0) == 0x90)
    {
        AppendVariableLength(out, event.duration);
    }
}

// Writes tracks of a compressed file. Earlier output serves as the source of blocks. A block's source holds no 0xFE.
// That byte would read as a byte of the block rather than as a code. It holds no 0xFF either. That keeps meta events
// out of blocks. A loop end then finds its loop start in the raw bytes.
class CompressedWriter
{
public:
    explicit CompressedWriter(std::vector<uint8_t>& out) : out_(out)
    {
    }

    void AppendEvents(const std::vector<MidiEvent>& events)
    {
        // The offset of a loop end depends on the blocks before it. The track therefore goes out in stretches that
        // end at loop starts and loop ends.
        std::vector<std::size_t> after_event(events.size());
        std::vector<uint8_t> pending;
        for (std::size_t i = 0; i < events.size(); i++)
        {
            MidiEvent event = events[i];
            if (event.IsMeta(kCompressedLoopEnd))
            {
                // The offset leads from the event's end. Everything before the event therefore goes out first.
                Flush(pending);
                const bool has_start = event.loop_start >= 0 && static_cast<std::size_t>(event.loop_start) < i;
                const std::size_t loop_place = has_start ? after_event[static_cast<std::size_t>(event.loop_start)] : 0;
                WriteLoopEnd(event, has_start, loop_place);
                after_event[i] = out_.size();
                Remember();
                continue;
            }

            AppendCompressedEvent(pending, event);
            if (event.IsMeta(kCompressedLoopStart))
            {
                Flush(pending);
                after_event[i] = out_.size();
            }
        }

        Flush(pending);
    }

private:
    static bool Copyable(uint8_t byte)
    {
        return byte != CompressedMidi::kBlockCode && byte != kMidiMeta;
    }

    // A byte of 0xFE takes two bytes in the file, wherever it lies in a track.
    static std::size_t EscapedSize(std::span<const uint8_t> bytes)
    {
        return bytes.size() +
               static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), CompressedMidi::kBlockCode));
    }

    void AppendEscaped(std::span<const uint8_t> bytes)
    {
        for (const uint8_t byte : bytes)
        {
            if (byte == CompressedMidi::kBlockCode)
            {
                out_.push_back(CompressedMidi::kBlockCode);
            }

            out_.push_back(byte);
        }
    }

    // Writes a loop end with its delta. The offset counts the event's bytes in the file, and a byte of 0xFE in the
    // offset lengthens them. A distance of 0x1FE therefore has no offset that fits: the offset 0x1FE takes a byte more
    // and leads to 0x1FF, and the offset 0x1FF takes a byte less and leads to 0x1FE. A delta that starts with an extra
    // 0x80 reads the same and moves the distance on.
    void WriteLoopEnd(const MidiEvent& event, bool has_start, std::size_t loop_place)
    {
        std::vector<uint8_t> delta;
        AppendVariableLength(delta, event.delta);
        std::array<uint8_t, kLoopEndPayload> payload = {};
        std::copy_n(event.payload.begin(), std::min(event.payload.size(), payload.size()), payload.begin());
        constexpr std::size_t kMaxPadding = 4;
        std::size_t padding = 0;
        if (has_start)
        {
            bool done = false;
            for (; padding <= kMaxPadding && !done; padding++)
            {
                const std::size_t fixed_bytes = out_.size() + padding + EscapedSize(delta) + 2 +
                                                EscapedSize(std::span<const uint8_t>(payload).first(2)) +
                                                kLoopEndOffsetBytes;
                for (std::size_t escapes = 0; escapes <= kLoopEndOffsetBytes && !done; escapes++)
                {
                    const auto offset = static_cast<uint32_t>(fixed_bytes + escapes - loop_place);
                    xsflib::StoreBe32(payload.data() + 2, offset);
                    done = EscapedSize(std::span<const uint8_t>(payload).subspan(2)) == kLoopEndOffsetBytes + escapes;
                }

                if (done)
                {
                    break;
                }
            }
        }

        out_.insert(out_.end(), padding, uint8_t{0x80});
        AppendEscaped(delta);
        out_.push_back(kMidiMeta);
        out_.push_back(kCompressedLoopEnd);
        AppendEscaped(payload);
    }

    static uint64_t KeyOf(const uint8_t* bytes)
    {
        uint64_t key = 0;
        for (std::size_t i = 0; i < kMinBlock; i++)
        {
            key = (key << 8) | bytes[i];
        }

        return key;
    }

    // Records the places of the bytes that went out since the last call, for the search for blocks.
    void Remember()
    {
        while (remembered_ + kMinBlock <= out_.size())
        {
            bool copyable = true;
            for (std::size_t i = 0; i < kMinBlock; i++)
            {
                copyable = copyable && Copyable(out_[remembered_ + i]);
            }
            if (copyable)
            {
                places_[KeyOf(out_.data() + remembered_)].push_back(remembered_);
            }

            remembered_++;
        }
    }

    // Writes `bytes` with blocks where they fit, and empties `bytes`.
    void Flush(std::vector<uint8_t>& bytes)
    {
        std::size_t position = 0;
        while (position < bytes.size())
        {
            Remember();

            std::size_t best_length = 0;
            std::size_t best_place = 0;
            if (position + kMinBlock <= bytes.size())
            {
                FindBlock(bytes, position, best_length, best_place);
            }

            if (best_length >= kMinBlock)
            {
                const std::size_t distance = out_.size() - best_place;
                out_.push_back(CompressedMidi::kBlockCode);
                out_.push_back(static_cast<uint8_t>(distance >> 8));
                out_.push_back(static_cast<uint8_t>(distance));
                out_.push_back(static_cast<uint8_t>(best_length));
                position += best_length;
            }
            else
            {
                if (bytes[position] == CompressedMidi::kBlockCode)
                {
                    out_.push_back(CompressedMidi::kBlockCode);
                }

                out_.push_back(bytes[position]);
                position++;
            }
        }

        bytes.clear();
        Remember();
    }

    void FindBlock(const std::vector<uint8_t>& bytes, std::size_t position, std::size_t& best_length,
                   std::size_t& best_place) const
    {
        for (std::size_t i = 0; i < kMinBlock; i++)
        {
            if (!Copyable(bytes[position + i]))
            {
                return;
            }
        }

        const auto found = places_.find(KeyOf(bytes.data() + position));
        if (found == places_.end())
        {
            return;
        }

        const std::size_t limit = std::min(kMaxBlock, bytes.size() - position);
        const std::vector<std::size_t>& places = found->second;
        for (auto place = places.rbegin(); place != places.rend(); ++place)
        {
            if (out_.size() - *place > kMaxBlockDistance)
            {
                break;
            }

            std::size_t length = kMinBlock;
            while (length < limit && *place + length < out_.size() &&
                   out_[*place + length] == bytes[position + length] && Copyable(bytes[position + length]))
            {
                length++;
            }

            if (length > best_length)
            {
                best_length = length;
                best_place = *place;
            }
        }
    }

    std::vector<uint8_t>& out_;
    std::size_t remembered_ = 0;
    std::unordered_map<uint64_t, std::vector<std::size_t>> places_;
};

// Reads a track chunk of a standard MIDI file.
class ChunkSource final : public ByteSource
{
public:
    ChunkSource(std::span<const uint8_t> file, std::size_t start, std::size_t end)
        : file_(file), position_(start), end_(end)
    {
    }

    uint8_t Next() override
    {
        if (position_ >= end_)
        {
            throw xsflib::FormatError("a MIDI track runs past the end of its chunk");
        }

        return file_[position_++];
    }

    uint32_t Position() const override
    {
        return static_cast<uint32_t>(position_);
    }

    bool AtEnd() const
    {
        return position_ >= end_;
    }

private:
    std::span<const uint8_t> file_;
    std::size_t position_;
    std::size_t end_;
};

std::vector<MidiEvent> DecodeStandardTrack(std::span<const uint8_t> file, const StandardMidi::Chunk& chunk)
{
    ChunkSource source(file, chunk.offset, static_cast<std::size_t>(chunk.offset) + chunk.length);
    std::vector<MidiEvent> events;
    uint8_t last_status = 0;
    bool ended = false;
    while (!ended && !source.AtEnd())
    {
        MidiEvent event;
        event.file_offset = source.Position();
        event.delta = ReadVariableLength(source);

        const uint8_t first = source.Next();
        if (first == kMidiMeta || first == 0xF0 || first == 0xF7)
        {
            event.status = first;
            if (first == kMidiMeta)
            {
                event.type = source.Next();
                ended = event.type == kMidiMetaEndOfTrack;
            }

            event.payload.resize(ReadVariableLength(source));
            for (uint8_t& byte : event.payload)
            {
                byte = source.Next();
            }
        }
        else
        {
            if ((first & 0x80) != 0)
            {
                event.status = first;
                event.data1 = source.Next();
                last_status = first;
            }
            else
            {
                if (last_status == 0)
                {
                    throw xsflib::FormatError("a MIDI track uses a running status before any status");
                }

                event.status = last_status;
                event.data1 = first;
                event.running_status = true;
            }

            if (DataBytes(event.status) == 2)
            {
                event.data2 = source.Next();
            }
        }

        events.push_back(std::move(event));
    }

    return events;
}

void AppendStandardEvent(std::vector<uint8_t>& out, const MidiEvent& event, uint8_t& last_status)
{
    AppendVariableLength(out, event.delta);
    if (event.status == kMidiMeta || event.status == 0xF0 || event.status == 0xF7)
    {
        out.push_back(event.status);
        if (event.status == kMidiMeta)
        {
            out.push_back(event.type);
        }

        AppendVariableLength(out, static_cast<uint32_t>(event.payload.size()));
        out.insert(out.end(), event.payload.begin(), event.payload.end());

        return;
    }

    if (!event.running_status || event.status != last_status)
    {
        out.push_back(event.status);
    }

    last_status = event.status;
    out.push_back(event.data1);
    if (DataBytes(event.status) == 2)
    {
        out.push_back(event.data2);
    }
}

} // namespace

bool operator==(const MidiEvent& a, const MidiEvent& b)
{
    return a.delta == b.delta && a.status == b.status && a.type == b.type && a.data1 == b.data1 && a.data2 == b.data2 &&
           a.duration == b.duration && a.running_status == b.running_status && a.payload == b.payload &&
           a.loop_start == b.loop_start;
}

bool MidiEvent::IsChannel() const
{
    return status >= 0x80 && status < 0xF0;
}

bool MidiEvent::IsMeta(uint8_t meta_type) const
{
    return status == kMidiMeta && type == meta_type;
}

bool MidiEvent::IsNoteOn() const
{
    return (status & 0xF0) == 0x90;
}

uint32_t MidiEvent::TempoMicroseconds() const
{
    if (!IsMeta(kMidiMetaTempo) || payload.size() < 3)
    {
        return 0;
    }

    return (static_cast<uint32_t>(payload[0]) << 16) | (static_cast<uint32_t>(payload[1]) << 8) | payload[2];
}

CompressedMidi CompressedMidi::Parse(std::span<const uint8_t> data)
{
    if (data.size() < kHeaderSize)
    {
        throw xsflib::FormatError("a compressed MIDI file is shorter than its header");
    }

    for (std::size_t track = 0; track < kTrackCount; track++)
    {
        const uint32_t offset = xsflib::LoadBe32(data.data() + track * 4);
        if (offset == 0)
        {
            continue;
        }

        if (offset < kHeaderSize || offset >= data.size())
        {
            throw xsflib::FormatError("track " + std::to_string(track) + " of a compressed MIDI file leaves the file");
        }

        bool ended = false;
        DecodeCompressedTrack(data, offset, ended);
    }

    CompressedMidi file;
    file.bytes_.assign(data.begin(), data.end());

    return file;
}

bool CompressedMidi::Matches(std::span<const uint8_t> data)
{
    try
    {
        if (data.size() < kHeaderSize)
        {
            return false;
        }

        const int32_t division = xsflib::LoadBe32(data.data() + kTrackCount * 4) > 0x7FFF
                                     ? 0
                                     : static_cast<int32_t>(xsflib::LoadBe32(data.data() + kTrackCount * 4));
        bool any = false;
        for (std::size_t track = 0; track < kTrackCount; track++)
        {
            any = any || xsflib::LoadBe32(data.data() + track * 4) != 0;
        }

        Parse(data);

        return any && division > 0;
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }
}

CompressedMidi CompressedMidi::Build(std::span<const std::vector<MidiEvent>> tracks, int32_t division)
{
    if (tracks.size() > kTrackCount)
    {
        throw xsflib::FormatError("a compressed MIDI file holds at most 16 tracks");
    }

    CompressedMidi file;
    file.bytes_.resize(kHeaderSize, 0);
    xsflib::StoreBe32(file.bytes_.data() + kTrackCount * 4, static_cast<uint32_t>(division));
    CompressedWriter writer(file.bytes_);
    for (std::size_t track = 0; track < tracks.size(); track++)
    {
        if (tracks[track].empty())
        {
            continue;
        }

        xsflib::StoreBe32(file.bytes_.data() + track * 4, static_cast<uint32_t>(file.bytes_.size()));
        writer.AppendEvents(tracks[track]);
    }

    return file;
}

int32_t CompressedMidi::Division() const
{
    return static_cast<int32_t>(xsflib::LoadBe32(bytes_.data() + kTrackCount * 4));
}

uint32_t CompressedMidi::TrackOffset(std::size_t track) const
{
    return track < kTrackCount ? xsflib::LoadBe32(bytes_.data() + track * 4) : 0;
}

std::vector<uint8_t> CompressedMidi::ExpandTrack(std::size_t track) const
{
    std::vector<uint8_t> out;
    for (const MidiEvent& event : DecodeTrack(track))
    {
        AppendCompressedEvent(out, event);
    }

    return out;
}

std::vector<MidiEvent> CompressedMidi::DecodeTrack(std::size_t track) const
{
    const uint32_t offset = TrackOffset(track);
    if (offset == 0)
    {
        return {};
    }

    bool ended = false;

    return DecodeCompressedTrack(bytes_, offset, ended);
}

std::span<const uint8_t> CompressedMidi::Bytes() const
{
    return bytes_;
}

StandardMidi StandardMidi::Parse(std::span<const uint8_t> data)
{
    const xsflib::Reader reader(data, true);
    if (data.size() < kMidiHeaderSize || !std::equal(kHeaderId.begin(), kHeaderId.end(), data.begin()))
    {
        throw xsflib::FormatError("not a standard MIDI file");
    }

    const uint32_t header_length = reader.U32(4);
    if (header_length < kMidiHeaderSize - kChunkHeaderSize)
    {
        throw xsflib::FormatError("a standard MIDI file with a short header");
    }

    StandardMidi file;
    const std::size_t track_count = reader.U16(10);
    std::size_t position = kChunkHeaderSize + header_length;
    file.chunks_.push_back({kHeaderId, kChunkHeaderSize, header_length});
    while (file.tracks_.size() < track_count)
    {
        Chunk chunk;
        const auto id = reader.Sub(position, 4);
        std::copy(id.begin(), id.end(), chunk.id.begin());
        chunk.offset = static_cast<uint32_t>(position + kChunkHeaderSize);
        chunk.length = reader.U32(position + 4);
        reader.Sub(chunk.offset, chunk.length);
        if (chunk.id == kTrackId)
        {
            file.tracks_.push_back(file.chunks_.size());
        }

        file.chunks_.push_back(chunk);
        position = chunk.offset + static_cast<std::size_t>(chunk.length);
    }

    const auto kept = data.first(position);
    file.bytes_.assign(kept.begin(), kept.end());
    for (std::size_t track = 0; track < track_count; track++)
    {
        file.DecodeTrack(track);
    }

    return file;
}

bool StandardMidi::Matches(std::span<const uint8_t> data)
{
    try
    {
        Parse(data);

        return true;
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }
}

StandardMidi StandardMidi::Build(uint16_t format, uint16_t division, std::span<const std::vector<MidiEvent>> tracks)
{
    std::vector<uint8_t> out(kMidiHeaderSize, 0);
    std::copy(kHeaderId.begin(), kHeaderId.end(), out.begin());
    xsflib::StoreBe32(out.data() + 4, static_cast<uint32_t>(kMidiHeaderSize - kChunkHeaderSize));
    xsflib::StoreBe16(out.data() + 8, format);
    xsflib::StoreBe16(out.data() + 10, static_cast<uint16_t>(tracks.size()));
    xsflib::StoreBe16(out.data() + 12, division);
    for (const std::vector<MidiEvent>& events : tracks)
    {
        out.insert(out.end(), kTrackId.begin(), kTrackId.end());

        const std::size_t length_place = out.size();
        out.resize(out.size() + 4);

        uint8_t last_status = 0;
        for (const MidiEvent& event : events)
        {
            AppendStandardEvent(out, event, last_status);
        }

        xsflib::StoreBe32(out.data() + length_place, static_cast<uint32_t>(out.size() - length_place - 4));
    }

    return Parse(out);
}

uint16_t StandardMidi::Format() const
{
    return xsflib::LoadBe16(bytes_.data() + 8);
}

uint16_t StandardMidi::Division() const
{
    return xsflib::LoadBe16(bytes_.data() + 12);
}

std::size_t StandardMidi::TrackCount() const
{
    return tracks_.size();
}

const std::vector<StandardMidi::Chunk>& StandardMidi::Chunks() const
{
    return chunks_;
}

std::vector<MidiEvent> StandardMidi::DecodeTrack(std::size_t track) const
{
    return DecodeStandardTrack(bytes_, chunks_.at(tracks_.at(track)));
}

std::span<const uint8_t> StandardMidi::Bytes() const
{
    return bytes_;
}

} // namespace ntdlib::n64
