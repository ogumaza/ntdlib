// SPDX-License-Identifier: MIT

#include "ntdlib/sar/byte_order.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kBankInfoBlock = 0x5800;
constexpr uint16_t kWaveSoundInfoBlock = 0x6800;
constexpr uint16_t kWaveTableReference = 0x0100; // from INFO to the table of waves
constexpr uint16_t kItemTableReference = 0x0101; // from INFO to the table of instruments or of wave sounds
constexpr uint16_t kInstrumentReference = 0x5900;
constexpr uint16_t kNoRegionReference = 0x5903;
constexpr uint16_t kDirectTable = 0x6000;
constexpr uint16_t kRangeTable = 0x6001;
constexpr uint16_t kIndexTable = 0x6002;
constexpr int32_t kNoOffset = -1;

// The optional parameters that the walk knows: a region's original key, volume, pan, pitch, flags and envelope, and a
// wave sound's pan, pitch, filter, sends and envelope.
constexpr uint32_t kRegionOptions = 0x21F;
constexpr uint32_t kWaveSoundOptions = 0x307;
constexpr int kSendsOption = 8;
constexpr int kEnvelopeOption = 9;
constexpr std::size_t kEnvelopeBytes = 5;

// The first version of a wave file that holds the loop's original start.
constexpr uint32_t kOriginalLoopStartVersion = 0x00010200;

constexpr std::size_t kDspAdpcmFrameBytes = 8;
constexpr std::size_t kDspAdpcmFrameSamples = 14;

// A file's bytes in the other byte order. Each field that the walk reads goes into the copy in the other order. The
// walk marks the bytes of each field. A field that overlaps another field throws, and so does a byte that no field
// holds and that isn't zero.
class Swap
{
public:
    Swap(std::span<const uint8_t> file, bool big_endian)
        : in_(file, big_endian),
          out_(file.begin(), file.end()),
          fields_(file.size(), Field::kNone),
          big_endian_(big_endian)
    {
    }

    uint8_t U8(std::size_t at)
    {
        const uint8_t value = in_.U8(at);
        Mark(at, 1);

        return value;
    }

    uint16_t U16(std::size_t at)
    {
        const uint16_t value = in_.U16(at);
        Mark(at, 2);
        big_endian_ ? xsflib::StoreLe16(&out_[at], value) : xsflib::StoreBe16(&out_[at], value);

        return value;
    }

    uint32_t U32(std::size_t at)
    {
        const uint32_t value = in_.U32(at);
        Mark(at, 4);
        big_endian_ ? xsflib::StoreLe32(&out_[at], value) : xsflib::StoreBe32(&out_[at], value);

        return value;
    }

    // A reference: its type, 2 bytes of padding and an offset. Returns the type and the offset.
    std::pair<uint16_t, int32_t> Reference(std::size_t at)
    {
        const uint16_t type = U16(at);
        U16(at + 2);

        return {type, static_cast<int32_t>(U32(at + 4))};
    }

    // Bytes that stay as they are.
    void Bytes(std::size_t at, std::size_t count)
    {
        in_.Sub(at, count);
        for (std::size_t k = 0; k < count; ++k)
        {
            Mark(at + k, 1);
        }
    }

    // The copy, with `version` in the header. Throws xsflib::FormatError for a byte that no field holds and that isn't
    // zero.
    std::vector<uint8_t> Take(uint32_t version)
    {
        for (std::size_t k = 0; k < fields_.size(); ++k)
        {
            if (fields_[k] == Field::kNone && out_[k] != 0)
            {
                char message[96];
                std::snprintf(message, sizeof(message), "a byte at 0x%zX that no known structure holds", k);
                throw xsflib::FormatError(message);
            }
        }
        big_endian_ ? xsflib::StoreLe32(&out_[8], version) : xsflib::StoreBe32(&out_[8], version);

        return std::move(out_);
    }

    // Whether the walk has walked the structure of kind `kind` at `at` already. Marks it walked.
    bool Walked(int kind, std::size_t at)
    {
        return !walked_.insert({kind, at}).second;
    }

private:
    // What a byte of the file holds: no field yet, the first byte of a field of 1, 2 or 4 bytes, or a later byte.
    enum class Field : uint8_t
    {
        kNone,
        kFirstOf1,
        kFirstOf2,
        kFirstOf4,
        kLater,
    };

    // Marks the `size` bytes of a field at `at`. The same field can come again, from a structure that two others share.
    // Throws xsflib::FormatError for a field that overlaps another.
    void Mark(std::size_t at, std::size_t size)
    {
        const Field first = size == 1 ? Field::kFirstOf1 : size == 2 ? Field::kFirstOf2 : Field::kFirstOf4;
        bool same = fields_[at] == first;
        bool free = fields_[at] == Field::kNone;
        for (std::size_t k = 1; k < size; ++k)
        {
            same = same && fields_[at + k] == Field::kLater;
            free = free && fields_[at + k] == Field::kNone;
        }
        if (same)
        {
            return;
        }
        if (!free)
        {
            throw xsflib::FormatError("structures that overlap");
        }

        fields_[at] = first;
        for (std::size_t k = 1; k < size; ++k)
        {
            fields_[at + k] = Field::kLater;
        }
    }

    xsflib::Reader in_;
    std::vector<uint8_t> out_;
    std::vector<Field> fields_;
    bool big_endian_ = false;
    std::set<std::pair<int, std::size_t>> walked_;
};

// The kinds of structure that the walk can reach from more than one place.
enum Structure
{
    kInstrument,
    kKeyRegion,
    kVelocityRegion,
    kWaveSound,
    kTrack,
    kEvent,
    kSends,
    kEnvelope,
};

// A wave archive's item ID and a wave's index for each wave.
void WalkWaveTable(Swap& swap, std::size_t table)
{
    const uint32_t count = swap.U32(table);
    for (uint32_t i = 0; i < count; ++i)
    {
        swap.U32(table + 4 + std::size_t{i} * 8);
        swap.U32(table + 8 + std::size_t{i} * 8);
    }
}

// An envelope: a reference to its 5 bytes.
void WalkEnvelope(Swap& swap, std::size_t at)
{
    if (swap.Walked(kEnvelope, at))
    {
        return;
    }

    const auto [type, offset] = swap.Reference(at);
    swap.Bytes(at + static_cast<std::size_t>(offset), kEnvelopeBytes);
}

// A wave sound's sends: the main send, the number of effect sends, then the effect sends, a byte each.
void WalkSends(Swap& swap, std::size_t at)
{
    if (swap.Walked(kSends, at))
    {
        return;
    }

    swap.U8(at);
    swap.Bytes(at + 2, swap.U8(at + 1));
}

// Optional parameters at `at`. Only the bits of `known` may be set. The values of the sends and of the envelope count
// from `base`.
void WalkOptions(Swap& swap, std::size_t at, uint32_t known, std::size_t base)
{
    const uint32_t bits = swap.U32(at);
    if ((bits & ~known) != 0)
    {
        char message[64];
        std::snprintf(message, sizeof(message), "optional parameters 0x%X that ntdlib doesn't know", bits & ~known);
        throw xsflib::FormatError(message);
    }

    std::size_t place = at + 4;
    for (int bit = 0; bit < 32; ++bit)
    {
        if ((bits >> bit & 1) == 0)
        {
            continue;
        }

        const uint32_t offset = swap.U32(place);
        if (bit == kSendsOption)
        {
            WalkSends(swap, base + offset);
        }
        else if (bit == kEnvelopeOption)
        {
            WalkEnvelope(swap, base + offset);
        }
        place += 4;
    }
}

// A bank's region or a wave sound's note: the wave's index in the table of waves, then the optional parameters, whose
// offsets count from the region's start.
void WalkVelocityRegion(Swap& swap, std::size_t at)
{
    if (swap.Walked(kVelocityRegion, at))
    {
        return;
    }

    swap.U32(at);
    WalkOptions(swap, at + 4, kRegionOptions, at);
}

// A reference at `at` to a region table: one region (direct), the regions up to each key or velocity (range), or one
// region for each from a first to a last (index). The references in the table count from its start. Calls `walk` for
// each region. A reference of kNoRegionReference or without an offset names no region.
template <typename Walk>
void WalkRegionTable(Swap& swap, std::size_t at, Walk walk)
{
    const auto [type, offset] = swap.Reference(at);
    const std::size_t table = at + static_cast<std::size_t>(offset);
    const auto region = [&swap, &walk, table](std::size_t reference)
    {
        const auto [region_type, region_offset] = swap.Reference(reference);
        if (region_type != kNoRegionReference && region_offset != kNoOffset)
        {
            walk(table + static_cast<std::size_t>(region_offset));
        }
    };

    switch (type)
    {
    case kDirectTable:
        region(table);
        break;

    case kRangeTable:
        {
            const uint32_t count = swap.U32(table);
            swap.Bytes(table + 4, count);
            const std::size_t references = table + 4 + ((std::size_t{count} + 3) & ~std::size_t{3});
            for (uint32_t i = 0; i < count; ++i)
            {
                region(references + std::size_t{i} * 8);
            }
            break;
        }

    case kIndexTable:
        {
            const uint8_t first = swap.U8(table);
            const uint8_t last = swap.U8(table + 1);
            for (int k = first; k <= last; ++k)
            {
                region(table + 4 + static_cast<std::size_t>(k - first) * 8);
            }
            break;
        }

    default:
        throw xsflib::FormatError("a region table of a type that ntdlib doesn't know");
    }
}

// A key region: a reference to its table of velocity regions.
void WalkKeyRegion(Swap& swap, std::size_t at)
{
    if (swap.Walked(kKeyRegion, at))
    {
        return;
    }

    const auto velocity = [&swap](std::size_t region)
    {
        WalkVelocityRegion(swap, region);
    };
    WalkRegionTable(swap, at, velocity);
}

// A bank's INFO block: references to the table of waves and to the table of instruments. An instrument holds a
// reference to its table of key regions. A reference of another type than kInstrumentReference names no instrument.
void WalkBank(Swap& swap, std::size_t info)
{
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto [type, offset] = swap.Reference(info + i * 8);
        const std::size_t table = info + static_cast<std::size_t>(offset);
        if (type == kWaveTableReference)
        {
            WalkWaveTable(swap, table);
            continue;
        }
        if (type != kItemTableReference)
        {
            throw xsflib::FormatError("a bank's INFO block with a reference that ntdlib doesn't know");
        }

        const uint32_t count = swap.U32(table);
        for (uint32_t k = 0; k < count; ++k)
        {
            const auto [instrument_type, instrument_offset] = swap.Reference(table + 4 + std::size_t{k} * 8);
            const std::size_t instrument = table + static_cast<std::size_t>(instrument_offset);
            if (instrument_type != kInstrumentReference || swap.Walked(kInstrument, instrument))
            {
                continue;
            }

            const auto key = [&swap](std::size_t region)
            {
                WalkKeyRegion(swap, region);
            };
            WalkRegionTable(swap, instrument, key);
        }
    }
}

// A table of references: a count, then a reference to each entry, from the table's start. Calls `walk` for each entry.
template <typename Walk>
void WalkTable(Swap& swap, std::size_t table, Walk walk)
{
    const uint32_t count = swap.U32(table);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto [type, offset] = swap.Reference(table + 4 + std::size_t{i} * 8);
        walk(table + static_cast<std::size_t>(offset));
    }
}

// A note event: its position and its length, as floats, and its note's index.
void WalkEvent(Swap& swap, std::size_t at)
{
    if (swap.Walked(kEvent, at))
    {
        return;
    }

    swap.U32(at);
    swap.U32(at + 4);
    swap.U32(at + 8);
}

// A wave sound's track: a reference to its table of note events.
void WalkTrack(Swap& swap, std::size_t at)
{
    if (swap.Walked(kTrack, at))
    {
        return;
    }

    const auto [type, offset] = swap.Reference(at);
    const auto event = [&swap](std::size_t entry)
    {
        WalkEvent(swap, entry);
    };
    WalkTable(swap, at + static_cast<std::size_t>(offset), event);
}

// A wave sound: references to its settings, to its table of tracks and to its table of notes, from its start. The
// values of the settings' sends and envelope count from the settings' start.
void WalkWaveSound(Swap& swap, std::size_t at)
{
    if (swap.Walked(kWaveSound, at))
    {
        return;
    }

    const auto [settings_type, settings] = swap.Reference(at);
    WalkOptions(swap, at + static_cast<std::size_t>(settings), kWaveSoundOptions,
                at + static_cast<std::size_t>(settings));

    const auto [tracks_type, tracks] = swap.Reference(at + 8);
    const auto track = [&swap](std::size_t entry)
    {
        WalkTrack(swap, entry);
    };
    WalkTable(swap, at + static_cast<std::size_t>(tracks), track);

    const auto [notes_type, notes] = swap.Reference(at + 16);
    const auto note = [&swap](std::size_t entry)
    {
        WalkVelocityRegion(swap, entry);
    };
    WalkTable(swap, at + static_cast<std::size_t>(notes), note);
}

// A wave sound file's INFO block: references to the table of waves and to the table of wave sounds.
void WalkWaveSoundFile(Swap& swap, std::size_t info)
{
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto [type, offset] = swap.Reference(info + i * 8);
        const std::size_t table = info + static_cast<std::size_t>(offset);
        if (type == kWaveTableReference)
        {
            WalkWaveTable(swap, table);
            continue;
        }
        if (type != kItemTableReference)
        {
            throw xsflib::FormatError("a wave sound file's INFO block with a reference that ntdlib doesn't know");
        }

        const auto sound = [&swap](std::size_t entry)
        {
            WalkWaveSound(swap, entry);
        };
        WalkTable(swap, table, sound);
    }
}

// The bytes that the samples of `count` samples take in `encoding`.
std::size_t SampleBytes(WaveEncoding encoding, uint32_t count)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return count;

    case WaveEncoding::kPcm16:
        return std::size_t{count} * 2;

    default:
        return (std::size_t{count} + kDspAdpcmFrameSamples - 1) / kDspAdpcmFrameSamples * kDspAdpcmFrameBytes;
    }
}

// Throws std::invalid_argument for `what`, a wave or a wave archive, when it or the flavor that it goes to is the
// 3DS's.
void CheckFlavors(Flavor from, Flavor to, const char* what)
{
    if (from == Flavor::kCtr || to == Flavor::kCtr)
    {
        throw std::invalid_argument(std::string(what) + " of the Wii U or the Switch");
    }
}

} // namespace

std::vector<uint8_t> WithOtherByteOrder(std::span<const uint8_t> file, uint32_t version)
{
    // The kind of file, from its magic, and its byte order.
    const bool bank = file.size() >= 4 && std::memcmp(file.data(), "FBNK", 4) == 0;
    const bool wave_sound = file.size() >= 4 && std::memcmp(file.data(), "FWSD", 4) == 0;
    if (!bank && !wave_sound)
    {
        throw xsflib::FormatError("not a bank file (FBNK) or a wave sound file (FWSD)");
    }
    if (file.size() < 0x14 || !((file[4] == 0xFE && file[5] == 0xFF) || (file[4] == 0xFF && file[5] == 0xFE)))
    {
        throw xsflib::FormatError("a file without a byte order mark");
    }

    // The header: the magic, the byte order mark, the header's size, the version, the file's size and the number of
    // blocks.
    Swap swap(file, file[4] == 0xFE);
    swap.Bytes(0, 4);
    swap.U16(4);
    swap.U16(6);
    swap.U32(8);
    swap.U32(12);
    const uint16_t blocks = swap.U16(0x10);
    swap.U16(0x12);

    // Each block's type, offset and size in the header, and the block's magic and size.
    std::optional<std::size_t> info;
    const uint16_t info_type = bank ? kBankInfoBlock : kWaveSoundInfoBlock;
    for (uint16_t i = 0; i < blocks; ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        const uint16_t type = swap.U16(entry);
        swap.U16(entry + 2);
        const uint32_t offset = swap.U32(entry + 4);
        swap.U32(entry + 8);
        swap.Bytes(offset, 4);
        swap.U32(offset + 4);
        if (type == info_type)
        {
            info = offset + 8;
        }
    }
    if (!info)
    {
        throw xsflib::FormatError("a file without its INFO block");
    }

    // The block's structures.
    bank ? WalkBank(swap, *info) : WalkWaveSoundFile(swap, *info);

    return swap.Take(version);
}

WaveFile ConvertWave(const WaveFile& wave, Flavor flavor, uint32_t version)
{
    CheckFlavors(wave.flavor, flavor, "a wave");

    // The settings, and the loop's original start for the version.
    WaveFile converted = wave;
    converted.flavor = flavor;
    converted.version = version;
    if (version < kOriginalLoopStartVersion)
    {
        converted.original_loop_start = 0;
    }
    else if (wave.version < kOriginalLoopStartVersion)
    {
        converted.original_loop_start = wave.loop ? wave.loop_start : 0;
    }

    // The samples that the length requires, in the console's byte order.
    const std::size_t bytes = SampleBytes(wave.encoding, wave.loop_end);
    const bool other_order =
        wave.encoding == WaveEncoding::kPcm16 && (wave.flavor == Flavor::kCafe) != (flavor == Flavor::kCafe);
    for (WaveFile::Channel& channel : converted.channels)
    {
        if (channel.samples.size() > bytes)
        {
            channel.samples.resize(bytes);
        }
        for (std::size_t k = 0; other_order && k + 1 < channel.samples.size(); k += 2)
        {
            std::swap(channel.samples[k], channel.samples[k + 1]);
        }
    }

    return converted;
}

WaveArchiveFile ConvertWaveArchive(const WaveArchiveFile& archive, Flavor flavor, uint32_t version,
                                   uint32_t wave_version)
{
    CheckFlavors(archive.flavor, flavor, "a wave archive");

    WaveArchiveFile converted;
    converted.flavor = flavor;
    converted.version = version;
    for (const std::vector<uint8_t>& wave : archive.waves)
    {
        converted.waves.push_back(ConvertWave(WaveFile::Read(wave), flavor, wave_version).Write());
    }

    return converted;
}

} // namespace ntdlib::sar
