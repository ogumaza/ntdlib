// SPDX-License-Identifier: MIT

#include "ntdlib/dse/formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/rounding.h"

namespace ntdlib::dse
{

namespace
{

// The 8 bytes after a chunk's label, by the chunk's kind.
constexpr std::array<uint8_t, 8> kBankChunk = {0x00, 0x00, 0x15, 0x04, 0x10, 0x00, 0x00, 0x00};
constexpr std::array<uint8_t, 8> kTrackChunk = {0x00, 0x00, 0x00, 0x01, 0x04, 0xFF, 0x00, 0x00};

// A song chunk's header, whose size field holds 0xFFFFFFB0.
constexpr std::array<uint8_t, 16> kSongChunk = {'s',  'o',  'n',  'g',  0x00, 0x00, 0x00, 0x01,
                                                0x10, 0xFF, 0x00, 0x00, 0xB0, 0xFF, 0xFF, 0xFF};

// The arguments of the commands from 0x90 on, by the code less 0x90. -1 marks a code that the driver takes as invalid.
// The lengths are those that the driver's handler of each command reads. The table of the handlers lies at 0x020B0B90
// in the ARM9 of Explorers of Sky's North American release.
constexpr std::array<int8_t, 0x70> kCommandArguments = {
    0, 1,  1,  2,  3,  1,  -1, -1, 0,  0,  -1, -1, 1,  0,  0,  -1, // 0x90
    1, 1,  -1, -1, 1,  1,  -1, -1, 2,  1,  1,  1,  1,  -1, -1, 3,  // 0xA0
    0, 1,  1,  1,  2,  1,  1,  -1, -1, -1, -1, -1, 1,  -1, 1,  1,  // 0xB0
    0, -1, -1, 1,  -1, -1, -1, -1, -1, -1, -1, 2,  -1, -1, -1, -1, // 0xC0
    1, 1,  1,  2,  3,  2,  2,  2,  2,  -1, -1, 1,  5,  4,  -1, 1,  // 0xD0
    1, 1,  3,  1,  5,  4,  -1, 1,  1,  1,  3,  -1, 5,  4,  -1, 1,  // 0xE0
    5, 4,  2,  3,  -1, -1, 1,  -1, 2,  -1, -1, -1, -1, -1, -1, -1, // 0xF0
};

template <std::size_t N>
std::array<uint8_t, N> Take(const xsflib::Reader& r, std::size_t at)
{
    std::array<uint8_t, N> out{};
    std::ranges::copy(r.Sub(at, N), out.begin());
    return out;
}

// Checks a chunk's label and the 8 bytes after it at `at`, and gives the size of what follows.
uint32_t ChunkSize(const xsflib::Reader& r, std::size_t at, const char* label, const std::array<uint8_t, 8>& kind)
{
    const std::span<const uint8_t> head = r.Sub(at, 16);
    if (std::memcmp(head.data(), label, 4) != 0)
    {
        throw xsflib::FormatError(std::string("the file has no ") + label + " chunk where one belongs");
    }
    if (!std::ranges::equal(head.subspan(4, 8), kind))
    {
        throw xsflib::FormatError(std::string("the ") + label + " chunk's header isn't one that the file's kind has");
    }

    return xsflib::LoadLe32(&head[12]);
}

// Checks the magic word and the version, and gives the file's size.
void CheckHeader(const xsflib::Reader& r, const char* magic)
{
    if (std::memcmp(r.Sub(0, 4).data(), magic, 4) != 0)
    {
        throw xsflib::FormatError(std::string("not a DSE ") + magic + " file");
    }
    if (r.U16(0x0C) != kVersion)
    {
        throw xsflib::FormatError("the file's version isn't 0x415");
    }
}

// Reads a track chunk at `at`, and gives the place after it and its padding. The events can run on into the padding.
Track ReadTrack(const xsflib::Reader& r, std::size_t at, std::size_t& next)
{
    const uint32_t size = ChunkSize(r, at, "trk ", kTrackChunk);
    const std::size_t start = at + 16;
    const std::size_t end = start + r.Sub(start, size).size();
    if (size < 4)
    {
        throw xsflib::FormatError("a track chunk is shorter than its preamble");
    }
    const std::size_t padded = std::min(AlignUp(end, 4), r.Size());

    Track track{.number = r.U8(start), .channel = r.U8(start + 1), .reserved = {r.U8(start + 2), r.U8(start + 3)}};
    std::size_t place = start + 4;
    while (true)
    {
        if (place >= padded)
        {
            // A track can fill its chunk without an end. The driver then reads on past the chunk.
            if (place == end && !track.events.empty())
            {
                break;
            }
            throw xsflib::FormatError("a track has no end");
        }

        const uint8_t code = r.U8(place);
        const int arguments = ArgumentBytes(code, place + 1 < padded ? r.U8(place + 1) : 0);
        const std::size_t count = static_cast<std::size_t>(std::max(arguments, 0));
        if (place + 1 + count > padded)
        {
            throw xsflib::FormatError("a track has no end");
        }

        const std::span<const uint8_t> args = r.Sub(place + 1, count);
        track.events.push_back({code, {args.begin(), args.end()}});
        place += 1 + count;
        if (code == kEndTrack || arguments < 0)
        {
            break;
        }
    }

    if (place > end)
    {
        if (place != end + 1 || track.events.back().code != kEndTrack || !track.events.back().arguments.empty())
        {
            throw xsflib::FormatError("a track's events run past its chunk");
        }
        track.end_in_padding = true;
    }
    else
    {
        const std::span<const uint8_t> unread = r.Sub(place, end - place);
        track.unread.assign(unread.begin(), unread.end());
    }

    next = AlignUp(end, 4);
    return track;
}

// Bytes that grow as they're written.
class Writer
{
public:
    std::size_t Size() const
    {
        return bytes_.size();
    }

    void U8(uint8_t value)
    {
        bytes_.push_back(value);
    }

    void U16(uint16_t value)
    {
        U8(static_cast<uint8_t>(value));
        U8(static_cast<uint8_t>(value >> 8));
    }

    void U32(uint32_t value)
    {
        U16(static_cast<uint16_t>(value));
        U16(static_cast<uint16_t>(value >> 16));
    }

    void Bytes(std::span<const uint8_t> data)
    {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    // Pads to a multiple of `alignment` with `filler`'s bytes while they last, then with `value`.
    void Pad(std::size_t alignment, std::span<const uint8_t> filler, uint8_t value)
    {
        const std::size_t count = AlignUp(bytes_.size(), alignment) - bytes_.size();
        for (std::size_t i = 0; i < count; i++)
        {
            U8(i < filler.size() && filler.size() == count ? filler[i] : value);
        }
    }

    void Put16(std::size_t at, uint32_t value)
    {
        xsflib::StoreLe16(&bytes_[at], static_cast<uint16_t>(value));
    }

    void Put32(std::size_t at, uint32_t value)
    {
        Put16(at, value);
        Put16(at + 2, value >> 16);
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
};

// Writes a chunk's header, and gives the place of its size.
std::size_t ChunkHeader(Writer& w, const char* label, const std::array<uint8_t, 8>& kind)
{
    w.Bytes({reinterpret_cast<const uint8_t*>(label), 4});
    w.Bytes(kind);
    const std::size_t size_at = w.Size();
    w.U32(0);
    return size_at;
}

void WriteTrack(Writer& w, const Track& track)
{
    const std::size_t size_at = ChunkHeader(w, "trk ", kTrackChunk);
    const std::size_t start = w.Size();
    w.U8(track.number);
    w.U8(track.channel);
    w.Bytes(track.reserved);
    for (const Event& event : track.events)
    {
        w.U8(event.code);
        w.Bytes(event.arguments);
    }
    w.Bytes(track.unread);

    // A last event that lies in the padding stays there while the chunk's size leaves room for it before the next
    // multiple of 4.
    std::size_t size = w.Size() - start;
    if (track.end_in_padding && track.unread.empty() && size % 4 != 1)
    {
        size--;
    }
    w.Put32(size_at, static_cast<uint32_t>(size));
    w.Pad(4, {}, kEndTrack);
}

void WriteEnd(Writer& w, const char* label, const std::array<uint8_t, 8>& kind)
{
    ChunkHeader(w, label, kind);
}

// Reads a table of `slots` 16-bit offsets at `base`, each to an entry of the chunk that ends at `end`.
std::vector<uint16_t> ReadOffsets(const xsflib::Reader& r, std::size_t base, std::size_t slots, std::size_t end)
{
    std::vector<uint16_t> offsets;
    for (std::size_t i = 0; i < slots; i++)
    {
        const uint16_t offset = r.U16(base + 2 * i);
        if (offset != 0 && (offset < 2 * slots || base + offset > end))
        {
            throw xsflib::FormatError("a table names an entry outside its chunk");
        }
        offsets.push_back(offset);
    }

    return offsets;
}

// Reads a chunk's track chunks from `at` until another label, and gives the place after them.
std::vector<Track> ReadTracks(const xsflib::Reader& r, std::size_t& at)
{
    std::vector<Track> tracks;
    while (at + 4 <= r.Size() && std::memcmp(r.Sub(at, 4).data(), "trk ", 4) == 0)
    {
        tracks.push_back(ReadTrack(r, at, at));
    }

    return tracks;
}

RecordTable ReadRecords(const xsflib::Reader& r, std::size_t base, std::size_t end)
{
    // The table runs to the first record. Each record runs to the next, and the last to the chunk's end.
    RecordTable table;
    std::size_t first = end - base;
    uint16_t last = 0;
    std::vector<uint16_t> offsets;
    for (std::size_t i = 0; 2 * i < first; i++)
    {
        const uint16_t offset = r.U16(base + 2 * i);
        if (offset != 0)
        {
            if (offset < 2 * (i + 1) || base + offset > end || offset <= last)
            {
                throw xsflib::FormatError("a table of records isn't in the order of its records");
            }
            first = std::min<std::size_t>(first, offset);
            last = offset;
        }
        offsets.push_back(offset);
    }

    for (std::size_t i = 0; i < offsets.size(); i++)
    {
        if (offsets[i] == 0)
        {
            table.slots.emplace_back();
            continue;
        }

        std::size_t next = end - base;
        for (std::size_t j = i + 1; j < offsets.size(); j++)
        {
            if (offsets[j] != 0)
            {
                next = offsets[j];
                break;
            }
        }
        const std::span<const uint8_t> record = r.Sub(base + offsets[i], next - offsets[i]);
        table.slots.emplace_back(std::vector<uint8_t>(record.begin(), record.end()));
    }

    return table;
}

void WriteRecords(Writer& w, const char* label, const RecordTable& table)
{
    const std::size_t size_at = ChunkHeader(w, label, kBankChunk);
    const std::size_t base = w.Size();
    for (std::size_t i = 0; i < table.slots.size(); i++)
    {
        w.U16(0);
    }
    for (std::size_t i = 0; i < table.slots.size(); i++)
    {
        if (table.slots[i])
        {
            w.Put16(base + 2 * i, static_cast<uint32_t>(w.Size() - base));
            w.Bytes(*table.slots[i]);
        }
    }
    w.Put32(size_at, static_cast<uint32_t>(w.Size() - base));
    w.Pad(16, {}, 0);
}

} // namespace

int ArgumentBytes(uint8_t code, uint8_t next)
{
    if (code < 0x80)
    {
        return 1 + (next >> 6);
    }
    if (code < 0x90)
    {
        return 0;
    }

    return kCommandArguments[code - 0x90];
}

uint16_t SequenceInfo::TicksPerQuarter() const
{
    return xsflib::LoadLe16(&bytes[2]);
}

uint8_t SequenceInfo::TrackCount() const
{
    return bytes[6];
}

uint8_t SequenceInfo::ChannelCount() const
{
    return bytes[7];
}

void SequenceInfo::SetTicksPerQuarter(uint16_t ticks)
{
    xsflib::StoreLe16(&bytes[2], ticks);
}

void SequenceInfo::SetTrackCount(uint8_t count)
{
    bytes[6] = count;
}

void SequenceInfo::SetChannelCount(uint8_t count)
{
    bytes[7] = count;
}

Song Song::Parse(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file);
    CheckHeader(r, "smdl");

    Song song;
    song.header = Take<kHeaderSize>(r, 0);
    if (!std::ranges::equal(r.Sub(kHeaderSize, 16), kSongChunk))
    {
        throw xsflib::FormatError("the file has no song chunk where one belongs");
    }
    song.info.bytes = Take<SequenceInfo::kSize>(r, kHeaderSize + 16);

    std::size_t at = kHeaderSize + 16 + SequenceInfo::kSize;
    song.tracks = ReadTracks(r, at);
    if (ChunkSize(r, at, "eoc ", kTrackChunk) != 0 || at + 16 != file.size())
    {
        throw xsflib::FormatError("the song doesn't end with its eoc chunk");
    }

    return song;
}

std::vector<uint8_t> Song::Write() const
{
    Writer w;
    w.Bytes(header);
    w.Bytes(kSongChunk);
    w.Bytes(info.bytes);
    for (const Track& track : tracks)
    {
        WriteTrack(w, track);
    }
    WriteEnd(w, "eoc ", kTrackChunk);
    w.Put32(0x08, static_cast<uint32_t>(w.Size()));
    return w.Take();
}

uint16_t WaveInfo::Id() const
{
    return xsflib::LoadLe16(&bytes[0x02]);
}

uint16_t WaveInfo::Format() const
{
    return xsflib::LoadLe16(&bytes[0x12]);
}

bool WaveInfo::Loops() const
{
    return bytes[0x15] != 0;
}

uint32_t WaveInfo::SampleRate() const
{
    return xsflib::LoadLe32(&bytes[0x20]);
}

uint32_t WaveInfo::SamplePosition() const
{
    return xsflib::LoadLe32(&bytes[0x24]);
}

uint32_t WaveInfo::LoopStart() const
{
    return xsflib::LoadLe32(&bytes[0x28]);
}

uint32_t WaveInfo::LoopLength() const
{
    return xsflib::LoadLe32(&bytes[0x2C]);
}

void WaveInfo::SetId(uint16_t id)
{
    xsflib::StoreLe16(&bytes[0x02], id);
}

void WaveInfo::SetFormat(uint16_t format)
{
    xsflib::StoreLe16(&bytes[0x12], format);
}

void WaveInfo::SetLoops(bool loops)
{
    bytes[0x15] = loops ? 1 : 0;
}

void WaveInfo::SetSampleRate(uint32_t rate)
{
    xsflib::StoreLe32(&bytes[0x20], rate);
}

void WaveInfo::SetSamplePosition(uint32_t position)
{
    xsflib::StoreLe32(&bytes[0x24], position);
}

void WaveInfo::SetLoopStart(uint32_t start)
{
    xsflib::StoreLe32(&bytes[0x28], start);
}

void WaveInfo::SetLoopLength(uint32_t length)
{
    xsflib::StoreLe32(&bytes[0x2C], length);
}

uint16_t Program::Id() const
{
    return xsflib::LoadLe16(&header[0x00]);
}

std::vector<uint8_t> Program::Bytes() const
{
    std::vector<uint8_t> bytes(header.begin(), header.end());
    for (const auto& lfo : lfos)
    {
        bytes.insert(bytes.end(), lfo.begin(), lfo.end());
    }
    bytes.insert(bytes.end(), filler.begin(), filler.end());
    for (const auto& split : splits)
    {
        bytes.insert(bytes.end(), split.begin(), split.end());
    }
    return bytes;
}

WaveBank WaveBank::Parse(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file);
    CheckHeader(r, "swdl");

    WaveBank bank;
    bank.header = Take<kHeaderSize>(r, 0);

    // The waves' parameters, in the order of their numbers.
    std::size_t at = kHeaderSize;
    const uint32_t wavi_size = ChunkSize(r, at, "wavi", kBankChunk);
    std::size_t base = at + 16;
    std::size_t end = base + r.Sub(base, wavi_size).size();
    for (const uint16_t offset : ReadOffsets(r, base, r.U16(0x46), end))
    {
        if (offset == 0)
        {
            bank.waves.emplace_back();
            continue;
        }

        WaveInfo wave{Take<WaveInfo::kSize>(r, base + offset)};
        if (wave.Id() != bank.waves.size())
        {
            throw xsflib::FormatError("a wave's slot isn't its number");
        }
        bank.waves.push_back(wave);
    }
    at = AlignUp(end, 16);

    if (std::memcmp(r.Sub(at, 4).data(), "prgi", 4) == 0)
    {
        Instruments instruments;
        const uint32_t prgi_size = ChunkSize(r, at, "prgi", kBankChunk);
        base = at + 16;
        end = base + r.Sub(base, prgi_size).size();
        for (const uint16_t offset : ReadOffsets(r, base, r.U16(0x48), end))
        {
            if (offset == 0)
            {
                instruments.programs.emplace_back();
                continue;
            }

            Program program;
            std::size_t p = base + offset;
            program.header = Take<0x10>(r, p);
            if (program.Id() != instruments.programs.size())
            {
                throw xsflib::FormatError("a program's slot isn't its number");
            }
            const std::size_t splits = xsflib::LoadLe16(&program.header[0x02]);
            const std::size_t lfos = program.header[0x0B];
            p += 0x10;
            for (std::size_t i = 0; i < lfos; i++, p += 0x10)
            {
                program.lfos.push_back(Take<0x10>(r, p));
            }
            program.filler = Take<0x10>(r, p);
            p += 0x10;
            for (std::size_t i = 0; i < splits; i++, p += 0x30)
            {
                program.splits.push_back(Take<0x30>(r, p));
            }
            if (p > end)
            {
                throw xsflib::FormatError("a program runs past its chunk");
            }
            instruments.programs.push_back(std::move(program));
        }
        at = AlignUp(end, 16);

        const uint32_t kgrp_size = ChunkSize(r, at, "kgrp", kBankChunk);
        base = at + 16;
        for (std::size_t i = 0; i + 8 <= kgrp_size; i += 8)
        {
            instruments.key_groups.push_back(Take<8>(r, base + i));
        }
        end = base + r.Sub(base, kgrp_size).size();
        const std::span<const uint8_t> pad = r.Sub(end, std::min(AlignUp(end, 16), r.Size()) - end);
        instruments.after_key_groups.assign(pad.begin(), pad.end());
        at = AlignUp(end, 16);
        bank.instruments = std::move(instruments);
    }

    if (std::memcmp(r.Sub(at, 4).data(), "pcmd", 4) == 0)
    {
        const uint32_t pcmd_size = ChunkSize(r, at, "pcmd", kBankChunk);
        const std::span<const uint8_t> samples = r.Sub(at + 16, pcmd_size);
        bank.samples.emplace(samples.begin(), samples.end());
        end = at + 16 + samples.size();
        const std::span<const uint8_t> pad = r.Sub(end, std::min(AlignUp(end, 16), r.Size()) - end);
        bank.after_samples.assign(pad.begin(), pad.end());
        at = AlignUp(end, 16);
    }

    if (ChunkSize(r, at, "eod ", kBankChunk) != 0 || at + 16 != file.size())
    {
        throw xsflib::FormatError("the bank doesn't end with its eod chunk");
    }

    return bank;
}

std::vector<uint8_t> WaveBank::Write() const
{
    Writer w;
    w.Bytes(header);

    // The table's slots, padded to 16 bytes with 0xAA, then the entries in the order of their numbers.
    std::size_t size_at = ChunkHeader(w, "wavi", kBankChunk);
    std::size_t base = w.Size();
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        w.U16(0);
    }
    w.Pad(16, {}, 0xAA);
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        if (waves[i])
        {
            w.Put16(base + 2 * i, static_cast<uint32_t>(w.Size() - base));
            w.Bytes(waves[i]->bytes);
        }
    }
    const std::size_t wavi_size = w.Size() - base;
    w.Put32(size_at, static_cast<uint32_t>(wavi_size));
    w.Pad(16, {}, 0);

    if (instruments)
    {
        // The table's slots, padded to 16 bytes with zeros, then the programs.
        size_at = ChunkHeader(w, "prgi", kBankChunk);
        base = w.Size();
        for (std::size_t i = 0; i < instruments->programs.size(); i++)
        {
            w.U16(0);
        }
        w.Pad(16, {}, 0);
        for (std::size_t i = 0; i < instruments->programs.size(); i++)
        {
            const std::optional<Program>& program = instruments->programs[i];
            if (!program)
            {
                continue;
            }

            w.Put16(base + 2 * i, static_cast<uint32_t>(w.Size() - base));
            std::array<uint8_t, 0x10> head = program->header;
            head[0x02] = static_cast<uint8_t>(program->splits.size());
            head[0x03] = static_cast<uint8_t>(program->splits.size() >> 8);
            head[0x0B] = static_cast<uint8_t>(program->lfos.size());
            w.Bytes(head);
            for (const std::array<uint8_t, 0x10>& lfo : program->lfos)
            {
                w.Bytes(lfo);
            }
            w.Bytes(program->filler);
            for (const std::array<uint8_t, 0x30>& split : program->splits)
            {
                w.Bytes(split);
            }
        }
        w.Put32(size_at, static_cast<uint32_t>(w.Size() - base));
        w.Pad(16, {}, 0);

        size_at = ChunkHeader(w, "kgrp", kBankChunk);
        base = w.Size();
        for (const std::array<uint8_t, 8>& group : instruments->key_groups)
        {
            w.Bytes(group);
        }
        w.Put32(size_at, static_cast<uint32_t>(w.Size() - base));
        w.Pad(16, instruments->after_key_groups, 0);
    }

    if (samples)
    {
        size_at = ChunkHeader(w, "pcmd", kBankChunk);
        w.Bytes(*samples);
        w.Put32(size_at, static_cast<uint32_t>(samples->size()));
        w.Pad(16, after_samples, 0);
    }

    WriteEnd(w, "eod ", kBankChunk);
    w.Put32(0x08, static_cast<uint32_t>(w.Size()));
    if (samples)
    {
        w.Put32(0x40, static_cast<uint32_t>(samples->size()));
    }
    w.Put16(0x46, static_cast<uint32_t>(waves.size()));
    if (instruments)
    {
        w.Put16(0x48, static_cast<uint32_t>(instruments->programs.size()));
    }
    w.Put32(0x4C, static_cast<uint32_t>(wavi_size));
    return w.Take();
}

SoundEffects SoundEffects::Parse(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file);
    CheckHeader(r, "sedl");

    SoundEffects effects;
    effects.header = Take<kHeaderSize>(r, 0);

    // The table's slots, then the sequences in their order. Each sequence holds its settings, its tracks and an eoc
    // chunk.
    std::size_t at = kHeaderSize;
    const uint32_t seq_size = ChunkSize(r, at, "seq ", kBankChunk);
    std::size_t base = at + 16;
    std::size_t end = base + r.Sub(base, seq_size).size();
    std::size_t last = 0;
    for (const uint16_t offset : ReadOffsets(r, base, r.U16(0x30), end))
    {
        if (offset == 0)
        {
            effects.sequences.emplace_back();
            continue;
        }
        if (offset < last)
        {
            throw xsflib::FormatError("the sequences aren't in the order of their slots");
        }

        EffectSequence sequence;
        sequence.info.bytes = Take<SequenceInfo::kSize>(r, base + offset);
        std::size_t p = base + offset + SequenceInfo::kSize;
        sequence.tracks = ReadTracks(r, p);
        if (ChunkSize(r, p, "eoc ", kTrackChunk) != 0 || p + 16 > end)
        {
            throw xsflib::FormatError("a sound effect doesn't end with its eoc chunk");
        }
        last = p + 16 - base;
        effects.sequences.push_back(std::move(sequence));
    }
    at = AlignUp(end, 16);

    const uint32_t mcrl_size = ChunkSize(r, at, "mcrl", kBankChunk);
    effects.macros = ReadRecords(r, at + 16, at + 16 + r.Sub(at + 16, mcrl_size).size());
    at = AlignUp(at + 16 + mcrl_size, 16);

    const uint32_t bnkl_size = ChunkSize(r, at, "bnkl", kBankChunk);
    effects.bank_lists = ReadRecords(r, at + 16, at + 16 + r.Sub(at + 16, bnkl_size).size());
    at = AlignUp(at + 16 + bnkl_size, 16);

    if (ChunkSize(r, at, "eod ", kBankChunk) != 0 || at + 16 != file.size())
    {
        throw xsflib::FormatError("the bank doesn't end with its eod chunk");
    }

    return effects;
}

std::vector<uint8_t> SoundEffects::Write() const
{
    Writer w;
    w.Bytes(header);

    // The table's slots, padded to 16 bytes with zeros, then the sequences.
    const std::size_t size_at = ChunkHeader(w, "seq ", kBankChunk);
    const std::size_t base = w.Size();
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        w.U16(0);
    }
    w.Pad(16, {}, 0);
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        if (!sequences[i])
        {
            continue;
        }

        w.Put16(base + 2 * i, static_cast<uint32_t>(w.Size() - base));
        w.Bytes(sequences[i]->info.bytes);
        for (const Track& track : sequences[i]->tracks)
        {
            WriteTrack(w, track);
        }
        WriteEnd(w, "eoc ", kTrackChunk);
    }
    w.Put32(size_at, static_cast<uint32_t>(w.Size() - base));
    w.Pad(16, {}, 0);

    WriteRecords(w, "mcrl", macros);
    WriteRecords(w, "bnkl", bank_lists);
    WriteEnd(w, "eod ", kBankChunk);
    w.Put32(0x08, static_cast<uint32_t>(w.Size()));
    w.Put16(0x30, static_cast<uint32_t>(sequences.size()));
    return w.Take();
}

std::vector<uint16_t> SoundEffects::BankIds(std::size_t effect) const
{
    std::vector<uint16_t> ids;
    if (effect >= bank_lists.slots.size() || !bank_lists.slots[effect] || bank_lists.slots[effect]->size() < 4)
    {
        return ids;
    }
    const std::vector<uint8_t>& record = *bank_lists.slots[effect];
    const std::size_t count = record[0] | record[1] << 8;
    for (std::size_t i = 0; i < count && 4 + 2 * i + 1 < record.size(); i++)
    {
        ids.push_back(xsflib::LoadLe16(&record[4 + 2 * i]));
    }
    return ids;
}

} // namespace ntdlib::dse
