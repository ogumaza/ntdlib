// SPDX-License-Identifier: MIT

#include "ntdlib/universomo/formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::universomo
{

namespace
{

constexpr std::size_t kSongHeaderSize = 0x27;
constexpr std::size_t kEventSize = 4;
constexpr std::size_t kInstrumentSize = 4;
constexpr std::size_t kBankHeaderSize = 13;
constexpr std::size_t kSampleHeaderSize = 14;
constexpr std::size_t kSoundSize = 7;

// Whether the data starts with `magic`.
bool HasMagic(std::span<const uint8_t> data, std::string_view magic)
{
    return data.size() >= magic.size() && std::equal(magic.begin(), magic.end(), data.begin());
}

// Throws unless `count` records of `size` bytes fit between `offset` and the end of the data.
void CheckRecords(const xsflib::Reader& reader, std::size_t offset, uint64_t count, std::size_t size)
{
    if (offset > reader.Size() || count > (reader.Size() - offset) / size)
    {
        throw xsflib::FormatError("the file ends before offset " + std::to_string(offset + count * size));
    }
}

Track ReadTrack(const xsflib::Reader& reader, std::size_t& offset)
{
    Track track;
    track.first_rows = reader.U8(offset);
    const uint32_t count = reader.U32(offset + 1);
    offset += 5;
    CheckRecords(reader, offset, count, kEventSize);

    track.events.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        track.events.push_back(
            {reader.U8(offset), reader.U8(offset + 1), reader.U8(offset + 2), reader.U8(offset + 3)});
        offset += kEventSize;
    }

    return track;
}

void CheckSong(const Song& song)
{
    if (song.orders.empty())
    {
        throw xsflib::FormatError("the song has no order");
    }

    if (song.restart >= song.orders.size())
    {
        throw xsflib::FormatError("the restart order " + std::to_string(song.restart) + " is past the order list");
    }

    for (std::size_t i = 0; i < song.orders.size(); ++i)
    {
        if (song.orders[i] >= song.patterns.size())
        {
            throw xsflib::FormatError("order " + std::to_string(i) + " names pattern " +
                                      std::to_string(song.orders[i]) + " of " + std::to_string(song.patterns.size()));
        }
    }
}

} // namespace

Song ReadSong(std::span<const uint8_t> data)
{
    const xsflib::Reader reader(data);
    if (data.size() < kSongHeaderSize || !HasMagic(data, "UNIMUSICSQ:"))
    {
        throw xsflib::FormatError("not a UNIMUSIC song");
    }

    // The header.
    Song song;
    song.tempo = reader.U32(0x0B);
    song.speed = reader.U32(0x0F);
    const uint32_t order_count = reader.U32(0x13);
    song.track_count = reader.U32(0x17);
    const uint32_t pattern_count = reader.U32(0x1B);
    const uint32_t instrument_count = reader.U32(0x1F);
    song.restart = reader.U32(0x23);
    if (song.track_count > kMaxTracks)
    {
        throw xsflib::FormatError("the song has " + std::to_string(song.track_count) + " tracks, more than " +
                                  std::to_string(kMaxTracks));
    }

    // The instruments' samples and the order list.
    std::size_t offset = kSongHeaderSize;
    CheckRecords(reader, offset, instrument_count, 1);
    const std::span<const uint8_t> samples = reader.Sub(offset, instrument_count);
    offset += instrument_count;
    CheckRecords(reader, offset, order_count, 1);
    const std::span<const uint8_t> orders = reader.Sub(offset, order_count);
    song.orders.assign(orders.begin(), orders.end());
    offset += order_count;

    // The patterns. Each one takes at least 4 bytes, so a count past the file's end fails before the reservation.
    CheckRecords(reader, offset, pattern_count, 4);
    song.patterns.reserve(pattern_count);
    for (uint32_t p = 0; p < pattern_count; ++p)
    {
        Pattern pattern;
        pattern.rows = reader.U32(offset);
        offset += 4;
        pattern.tracks.reserve(song.track_count);
        for (uint32_t t = 0; t < song.track_count; ++t)
        {
            pattern.tracks.push_back(ReadTrack(reader, offset));
        }

        song.patterns.push_back(std::move(pattern));
    }

    // The instruments' settings.
    CheckRecords(reader, offset, instrument_count, kInstrumentSize);
    song.instruments.reserve(instrument_count);
    for (uint32_t i = 0; i < instrument_count; ++i)
    {
        const std::size_t at = offset + kInstrumentSize * i;
        song.instruments.push_back({samples[i], reader.U8(at), reader.U8(at + 1),
                                    static_cast<int8_t>(reader.U8(at + 2)), static_cast<int8_t>(reader.U8(at + 3))});
    }

    CheckSong(song);

    return song;
}

SampleBank ReadSampleBank(std::span<const uint8_t> data)
{
    const xsflib::Reader reader(data);
    if (data.size() < kBankHeaderSize || !HasMagic(data, "UNISAMBA"))
    {
        throw xsflib::FormatError("not a UNISAMBA sample bank");
    }

    SampleBank bank;
    bank.version = reader.U8(8);
    const uint32_t count = reader.U32(9);
    CheckRecords(reader, kBankHeaderSize, count, 4);
    const std::size_t base = kBankHeaderSize + std::size_t{4} * count;

    bank.samples.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t at = base + reader.U32(kBankHeaderSize + std::size_t{4} * i);
        const uint32_t size = reader.U32(at);
        Sample sample;
        sample.bits = reader.U8(at + 4);
        sample.loop_start = reader.U32(at + 5);
        sample.loop_length = reader.U32(at + 9);
        sample.looped = reader.U8(at + 13) != 0;
        if (sample.bits != 8 && sample.bits != 16)
        {
            throw xsflib::FormatError("sample " + std::to_string(i) + " has " + std::to_string(sample.bits) + " bits");
        }

        const std::span<const uint8_t> bytes = reader.Sub(at + kSampleHeaderSize, size);
        sample.data.assign(bytes.begin(), bytes.end());
        bank.samples.push_back(std::move(sample));
    }

    return bank;
}

std::vector<Sound> ReadSoundTable(std::span<const uint8_t> data)
{
    const xsflib::Reader reader(data);
    if (data.size() < 4 || !HasMagic(data, "AU0"))
    {
        throw xsflib::FormatError("not a table of sounds");
    }

    const uint8_t count = reader.U8(3);
    CheckRecords(reader, 4, count, kSoundSize);
    std::vector<Sound> sounds(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::span<const uint8_t> bytes = reader.Sub(4 + kSoundSize * i, kSoundSize);
        Sound& sound = sounds[i];
        std::copy(bytes.begin(), bytes.end(), sound.bytes.begin());
        sound.loops = bytes[1] != 0;
        sound.kind = static_cast<SoundKind>(bytes[2]);
    }

    return sounds;
}

std::string SongPath(uint32_t number)
{
    return "/au" + std::to_string(number) + ".ums";
}

} // namespace ntdlib::universomo
