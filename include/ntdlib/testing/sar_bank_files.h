// SPDX-License-Identifier: MIT

// Builds the parts of NintendoWare's sound files for tests, laid out as the real ones are: the blocks and headers of a
// file, waves, wave archives and banks. sar_files.h builds the other files from them. Each builder takes the flavor of
// ntdlib's sar reader. The 3DS's files (CWAV, CWAR and CBNK) are little-endian. The Wii U's files (FWAV, FWAR and
// FBNK) are big-endian. The Switch's files are the Wii U's formats, little-endian. A builder given a bool builds the
// Wii U's files for true and the Switch's files for false.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rounding.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::testing::sar_files
{

// The flavor of a builder given a bool: the Wii U's flavor for big-endian files, and the Switch's flavor for
// little-endian files.
inline sar::Flavor FlavorOf(bool big_endian)
{
    return big_endian ? sar::Flavor::kCafe : sar::Flavor::kNx;
}

// Whether the files of `flavor` are big-endian. Only the Wii U's files are.
inline bool BigEndian(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCafe;
}

// The magic of a file of kind `kind`, such as WAV: C and the kind for the 3DS, F and the kind for the others.
inline std::string Magic(sar::Flavor flavor, const char* kind)
{
    return (flavor == sar::Flavor::kCtr ? "C" : "F") + std::string(kind);
}

// A file's version word: `ctr` for the 3DS's files, `cafe` for the files of the Wii U and the Switch.
inline uint32_t Version(sar::Flavor flavor, uint32_t ctr, uint32_t cafe)
{
    return flavor == sar::Flavor::kCtr ? ctr : cafe;
}

// Bytes in one byte order, with references between what's in them.
class Bytes
{
public:
    explicit Bytes(bool big_endian) : big_endian_(big_endian)
    {
    }

    size_t Size() const
    {
        return data.size();
    }

    // Appends `size` zero bytes, and returns where they start.
    size_t Add(size_t size)
    {
        const size_t at = data.size();
        data.resize(at + size);

        return at;
    }

    // Appends `bytes`, and returns where they start.
    size_t Add(const std::vector<uint8_t>& bytes)
    {
        const size_t at = data.size();
        data.insert(data.end(), bytes.begin(), bytes.end());

        return at;
    }

    void Align(size_t alignment)
    {
        data.resize(AlignUp(data.size(), alignment));
    }

    void U8(size_t at, uint8_t value)
    {
        data[at] = value;
    }

    void U16(size_t at, uint16_t value)
    {
        big_endian_ ? xsflib::StoreBe16(&data[at], value) : xsflib::StoreLe16(&data[at], value);
    }

    void U32(size_t at, uint32_t value)
    {
        big_endian_ ? xsflib::StoreBe32(&data[at], value) : xsflib::StoreLe32(&data[at], value);
    }

    // A reference: its type, and the target's offset from `base`.
    void Ref(size_t at, uint16_t type, size_t target, size_t base)
    {
        U16(at, type);
        U32(at + 4, static_cast<uint32_t>(target - base));
    }

    // A reference to nothing.
    void NullRef(size_t at, uint16_t type)
    {
        U16(at, type);
        U32(at + 4, 0xFFFFFFFF);
    }

    // A reference followed by the size of what it refers to.
    void SizedRef(size_t at, uint16_t type, size_t target, size_t base, size_t size)
    {
        Ref(at, type, target, base);
        U32(at + 8, static_cast<uint32_t>(size));
    }

    // Appends option parameters: a bit field with each value's bit set, then the values in the order given. Callers
    // give them in the order of their bits.
    size_t Options(const std::vector<std::pair<int, uint32_t>>& values)
    {
        const size_t at = Add(4 + 4 * values.size());
        uint32_t flags = 0;
        for (size_t i = 0; i < values.size(); ++i)
        {
            flags |= 1u << values[i].first;
            U32(at + 4 + 4 * i, values[i].second);
        }
        U32(at, flags);

        return at;
    }

    std::vector<uint8_t> data;

private:
    bool big_endian_;
};

struct Block
{
    uint16_t type;
    const char* magic;
    std::vector<uint8_t> body;
};

// An entry of the block table in a file header: the type, the offset and the size of a block.
struct BlockEntry
{
    uint16_t type;
    uint32_t offset;
    uint32_t size;
};

// Writes the header of `file` over its first bytes: the magic, the byte order mark, the header's size, the version,
// the file's size, the number of blocks and the block table. The file has to contain every block already.
inline void WriteFileHeader(Bytes& file, const std::string& magic, uint16_t header_size, uint32_t version,
                            const std::vector<BlockEntry>& blocks)
{
    std::memcpy(file.data.data(), magic.data(), 4);
    file.U16(4, 0xFEFF);
    file.U16(6, header_size);
    file.U32(8, version);
    file.U32(0x0C, static_cast<uint32_t>(file.Size()));
    file.U16(0x10, static_cast<uint16_t>(blocks.size()));
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        file.U16(0x14 + i * 12, blocks[i].type);
        file.U32(0x18 + i * 12, blocks[i].offset);
        file.U32(0x1C + i * 12, blocks[i].size);
    }
}

// The header of a file: the file's size, the number of blocks, and the table of blocks. Each entry of the table gives
// a block's type, its offset and its size.
constexpr size_t kFileSizeField = 0x0C;
constexpr size_t kBlockCountField = 0x10;
constexpr size_t kBlockTable = 0x14;
constexpr size_t kBlockEntrySize = 12;

// Where the table of blocks of `file`, a file of `flavor`, lists the block of type `type`, or nothing.
inline std::optional<size_t> FindBlockEntry(std::span<const uint8_t> file, sar::Flavor flavor, uint16_t type)
{
    const auto load16 = [&](size_t at)
    {
        return BigEndian(flavor) ? xsflib::LoadBe16(&file[at]) : xsflib::LoadLe16(&file[at]);
    };
    const size_t end = kBlockTable + kBlockEntrySize * load16(kBlockCountField);
    for (size_t entry = kBlockTable; entry < end; entry += kBlockEntrySize)
    {
        if (load16(entry) == type)
        {
            return entry;
        }
    }

    return std::nullopt;
}

// A whole file: the header and its block table, then each block's magic, size and body. Each part is aligned to 32
// bytes.
inline std::vector<uint8_t> File(bool big_endian, const char* magic, uint32_t version, const std::vector<Block>& blocks)
{
    Bytes file(big_endian);
    file.Add(0x14 + 12 * blocks.size());
    file.Align(0x20);
    const auto header_size = static_cast<uint16_t>(file.Size());

    std::vector<BlockEntry> entries;
    for (const Block& block : blocks)
    {
        const size_t start = file.Add(8);
        std::memcpy(file.data.data() + start, block.magic, 4);
        file.Add(block.body);
        file.Align(0x20);
        const auto size = static_cast<uint32_t>(file.Size() - start);
        file.U32(start + 4, size);
        entries.push_back({block.type, static_cast<uint32_t>(start), size});
    }

    WriteFileHeader(file, magic, header_size, version, entries);

    return file.data;
}

// A wave. Each DSP-ADPCM channel gets coefficients of 1 to 16, a frame header of its first byte, the values 100 and 101
// for its sample history (yn1 and yn2), and 102 to 104 for its loop context. Each IMA-ADPCM channel (encoding 3) starts
// from sample 100 and step index 10, and its loop context holds sample 200 and step index 20.
struct WaveSpec
{
    uint8_t encoding = 2;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::vector<std::vector<uint8_t>> channels;
};

inline std::vector<uint8_t> Wave(sar::Flavor flavor, const WaveSpec& spec)
{
    const bool big_endian = BigEndian(flavor);
    Bytes info(big_endian);
    Bytes data(big_endian);
    info.Add(0x14);
    info.U8(0, spec.encoding);
    info.U8(1, spec.loop);
    info.U32(4, spec.sample_rate);
    info.U32(8, spec.loop_start);
    info.U32(12, spec.loop_end);

    const size_t table = info.Add(4 + 8 * spec.channels.size());
    info.U32(table, static_cast<uint32_t>(spec.channels.size()));
    for (size_t i = 0; i < spec.channels.size(); ++i)
    {
        const size_t channel = info.Add(0x14);
        info.Ref(table + 4 + 8 * i, 0x7100, channel, table);

        data.Align(0x20);
        info.Ref(channel, 0x1F00, data.Add(spec.channels[i]), 0);
        if (spec.encoding == 3)
        {
            const size_t ima = info.Add(8);
            info.Ref(channel + 8, 0x0301, ima, channel);
            info.U16(ima, 100);
            info.U8(ima + 2, 10);
            info.U16(ima + 4, 200);
            info.U8(ima + 6, 20);
            continue;
        }
        if (spec.encoding != 2)
        {
            continue;
        }

        const size_t adpcm = info.Add(0x30);
        info.Ref(channel + 8, 0x0300, adpcm, channel);
        for (int k = 0; k < 16; ++k)
        {
            info.U16(adpcm + 2 * k, static_cast<uint16_t>(k + 1));
        }
        info.U16(adpcm + 32, spec.channels[i].empty() ? 0 : spec.channels[i][0]);
        for (int k = 0; k < 5; ++k)
        {
            info.U16(adpcm + 34 + 2 * k, static_cast<uint16_t>(100 + k));
        }
    }

    return File(big_endian, Magic(flavor, "WAV").c_str(), Version(flavor, 0x02010000, 0x00010100),
                {{0x7000, "INFO", info.data}, {0x7001, "DATA", data.data}});
}

inline std::vector<uint8_t> Wave(bool big_endian, const WaveSpec& spec)
{
    return Wave(FlavorOf(big_endian), spec);
}

inline std::vector<uint8_t> WaveArchive(sar::Flavor flavor, const std::vector<std::vector<uint8_t>>& waves)
{
    const bool big_endian = BigEndian(flavor);
    Bytes info(big_endian);
    Bytes files(big_endian);
    info.Add(4 + 12 * waves.size());
    info.U32(0, static_cast<uint32_t>(waves.size()));
    for (size_t i = 0; i < waves.size(); ++i)
    {
        files.Align(0x20);
        info.SizedRef(4 + 12 * i, 0x1F00, files.Add(waves[i]), 0, waves[i].size());
    }

    return File(big_endian, Magic(flavor, "WAR").c_str(), Version(flavor, 0x01000000, 0x00010000),
                {{0x6800, "INFO", info.data}, {0x6801, "FILE", files.data}});
}

inline std::vector<uint8_t> WaveArchive(bool big_endian, const std::vector<std::vector<uint8_t>>& waves)
{
    return WaveArchive(FlavorOf(big_endian), waves);
}

// A velocity region. The bytes are ints here. Tests can then give them as plain numbers. Otherwise compilers would warn
// about narrowing inside std::optional.
struct Region
{
    uint32_t wave = 0; // an index into the bank's waves
    std::optional<int> original_key, volume, pan;
    std::optional<float> pitch;
    std::optional<uint32_t> flags; // ignore note off, key group << 8 and interpolation type << 16
    std::optional<std::array<uint8_t, 5>> adshr;
};

// A region table: kDirect has one entry, kRange's entries each run up to their key, and kIndex has one entry for each
// key from `first`. An empty entry plays nothing. Region's bytes and the keys are both ints.
template <typename Entry>
struct Table
{
    enum Kind
    {
        kDirect,
        kRange,
        kIndex
    };

    Kind kind = kDirect;
    int first = 0;
    std::vector<std::pair<int, std::optional<Entry>>> entries;
};

using VelocityTable = Table<Region>;
using KeyTable = Table<VelocityTable>;

struct BankSpec
{
    std::vector<std::pair<uint32_t, uint32_t>> waves; // wave archive item ID and index
    std::vector<std::optional<KeyTable>> instruments;
};

namespace detail
{

// Appends an envelope's five bytes behind a reference, and stores the reference's offset from `base` in the option
// value at `slot`. A region's offset counts from the region's start, and a wave sound's from its settings.
inline void PutEnvelope(Bytes& b, size_t slot, size_t base, const std::array<uint8_t, 5>& adshr)
{
    const size_t ref = b.Add(8);
    const size_t envelope = b.Add(8);
    b.U32(slot, static_cast<uint32_t>(ref - base));
    b.Ref(ref, 0x0000, envelope, ref);
    std::memcpy(b.data.data() + envelope, adshr.data(), 5);
}

inline size_t PutRegion(Bytes& b, const Region& region)
{
    const size_t at = b.Add(4);
    b.U32(at, region.wave);

    std::vector<std::pair<int, uint32_t>> values;
    if (region.original_key)
    {
        values.push_back({0, static_cast<uint8_t>(*region.original_key)});
    }
    if (region.volume)
    {
        values.push_back({1, static_cast<uint8_t>(*region.volume)});
    }
    if (region.pan)
    {
        values.push_back({2, static_cast<uint8_t>(*region.pan)});
    }
    if (region.pitch)
    {
        values.push_back({3, std::bit_cast<uint32_t>(*region.pitch)});
    }
    if (region.flags)
    {
        values.push_back({4, *region.flags});
    }
    if (region.adshr)
    {
        values.push_back({9, 0});
    }
    const size_t options = b.Options(values);
    if (region.adshr)
    {
        PutEnvelope(b, options + 4 * values.size(), at, *region.adshr);
    }

    return at;
}

} // namespace detail

// Writes the reference at `owner` to a region table of `table`, and the table, and what its entries name.
template <typename Entry, typename PutEntry>
void PutTable(Bytes& b, size_t owner, const Table<Entry>& table, PutEntry put_entry)
{
    const size_t n = table.entries.size();
    size_t refs = 0;
    size_t q = 0;
    if (table.kind == Table<Entry>::kDirect)
    {
        q = refs = b.Add(8);
        b.U16(owner, 0x6000);
    }
    else if (table.kind == Table<Entry>::kRange)
    {
        q = b.Add(4 + AlignUp(n, 4) + 8 * n);
        refs = q + 4 + AlignUp(n, 4);
        b.U16(owner, 0x6001);
        b.U32(q, static_cast<uint32_t>(n));
        for (size_t i = 0; i < n; ++i)
        {
            b.U8(q + 4 + i, static_cast<uint8_t>(table.entries[i].first));
        }
    }
    else
    {
        q = b.Add(4 + 8 * n);
        refs = q + 4;
        b.U16(owner, 0x6002);
        b.U8(q, static_cast<uint8_t>(table.first));
        b.U8(q + 1, static_cast<uint8_t>(static_cast<size_t>(table.first) + n - 1));
    }
    b.U32(owner + 4, static_cast<uint32_t>(q - owner));

    for (size_t i = 0; i < n; ++i)
    {
        if (table.entries[i].second)
        {
            b.Ref(refs + 8 * i, 0x5901, put_entry(b, *table.entries[i].second), q);
        }
        else
        {
            b.NullRef(refs + 8 * i, 0x5903);
        }
    }
}

// Appends a velocity table behind a reference of 8 bytes, with its regions. Returns the reference's offset.
inline size_t PutVelocityTable(Bytes& b, const VelocityTable& table)
{
    const size_t at = b.Add(8);
    PutTable(b, at, table, detail::PutRegion);

    return at;
}

// Appends the table of waves by wave archive item ID and index, and stores the reference to it at 0.
inline void PutWaveTable(Bytes& info, const std::vector<std::pair<uint32_t, uint32_t>>& waves)
{
    const size_t table = info.Add(4 + 8 * waves.size());
    info.Ref(0, 0x0100, table, 0);
    info.U32(table, static_cast<uint32_t>(waves.size()));
    for (size_t i = 0; i < waves.size(); ++i)
    {
        info.U32(table + 4 + 8 * i, waves[i].first);
        info.U32(table + 8 + 8 * i, waves[i].second);
    }
}

// Starts the INFO block of a bank or a wave sound file: references to two tables, then the first table, of the waves
// by wave archive item ID and index. The caller writes the second table and its reference, at 8.
inline void PutWaves(Bytes& info, const std::vector<std::pair<uint32_t, uint32_t>>& waves)
{
    info.Add(16);
    PutWaveTable(info, waves);
}

// The helpers above under the names that they had in detail.
namespace detail
{

using sar_files::BigEndian;
using sar_files::FlavorOf;
using sar_files::Magic;
using sar_files::PutTable;
using sar_files::PutVelocityTable;
using sar_files::PutWaves;
using sar_files::Version;

} // namespace detail

inline std::vector<uint8_t> Bank(sar::Flavor flavor, const BankSpec& spec)
{
    const bool big_endian = BigEndian(flavor);
    Bytes info(big_endian);
    PutWaves(info, spec.waves);

    const size_t instruments = info.Add(4 + 8 * spec.instruments.size());
    info.Ref(8, 0x0101, instruments, 0);
    info.U32(instruments, static_cast<uint32_t>(spec.instruments.size()));
    for (size_t i = 0; i < spec.instruments.size(); ++i)
    {
        if (!spec.instruments[i])
        {
            info.NullRef(instruments + 4 + 8 * i, 0x5902);
            continue;
        }

        const size_t instrument = info.Add(8);
        info.Ref(instruments + 4 + 8 * i, 0x5900, instrument, instruments);
        PutTable(info, instrument, *spec.instruments[i], PutVelocityTable);
    }

    return File(big_endian, Magic(flavor, "BNK").c_str(), Version(flavor, 0x01000100, 0x00010000),
                {{0x5800, "INFO", info.data}});
}

inline std::vector<uint8_t> Bank(bool big_endian, const BankSpec& spec)
{
    return Bank(FlavorOf(big_endian), spec);
}

// A wave of `length` PCM16 samples at 32,000 Hz, each 0x1000, in the byte order that `big_endian` gives. The default
// length is 1 s. The wave plays once.
inline WaveSpec ConstantWave(bool big_endian, uint32_t length = 32000)
{
    std::vector<uint8_t> samples(2 * size_t{length});
    for (size_t i = 0; i < samples.size(); i += 2)
    {
        samples[big_endian ? i : i + 1] = 0x10;
    }

    WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = length;
    wave.channels = {samples};

    return wave;
}

// A bank of `programs` programs with one region each. Program i plays wave i of wave archive 0 at every key and
// velocity, from original key 60. Its envelope (127, 127, 127, 0, 127) starts at full level, holds it and releases at
// the fastest rate.
inline BankSpec OneRegionBank(uint32_t programs = 1)
{
    BankSpec bank;
    for (uint32_t wave = 0; wave < programs; ++wave)
    {
        Region region;
        region.wave = wave;
        region.original_key = 60;
        region.adshr = {127, 127, 127, 0, 127};
        VelocityTable velocities;
        velocities.entries = {{0, region}};
        KeyTable keys;
        keys.entries = {{0, velocities}};
        bank.waves.emplace_back(sar::ItemId(sar::kWaveArchiveItem, 0), wave);
        bank.instruments.push_back(keys);
    }

    return bank;
}

} // namespace ntdlib::testing::sar_files
