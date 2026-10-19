// SPDX-License-Identifier: MIT

// Tests of the CSAR and FSAR readers and of DecodeWave, on files built here in each flavor's format: the formats of the
// 3DS, the Wii U and the Switch. Most of them run for every flavor.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/wave/adpcm.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

using namespace xsflib;

using namespace ntdlib;
using namespace ntdlib::sar;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};

// The flavor's console, for the checks' messages.
std::string NameOf(Flavor flavor)
{
    switch (flavor)
    {
    case Flavor::kCtr:
        return "3DS";
    case Flavor::kCafe:
        return "Wii U";
    case Flavor::kNx:
        return "Switch";
    }

    return "?";
}

// The options that read each flavor's archives with kReadPastDamage. On the 3DS, a group's files are located as the
// group's table is read.
Options OptionsOf(Flavor flavor)
{
    return {flavor, {}, flavor != Flavor::kCtr};
}

// The options that read each flavor's archives with kStopAtFirstProblem. On the 3DS, a read past the end has another
// wording.
Options StrictOptionsOf(Flavor flavor)
{
    const xsflib::ReaderOptions reader{flavor == Flavor::kCtr ? xsflib::BoundsMessage::kCutShort
                                                              : xsflib::BoundsMessage::kOutOfBounds};
    return {flavor, reader, false, Rules::kStopAtFirstProblem};
}

// A test file's format: the first letter of its magic, its byte order and an archive's version.
struct Format
{
    char letter = 'C';
    bool big_endian = false;
    uint32_t version = 0x02030000;
};

Format FormatOf(Flavor flavor)
{
    if (flavor == Flavor::kCtr)
    {
        return {'C', false, 0x02030000};
    }

    return {'F', flavor == Flavor::kCafe, 0x00020200};
}

// The magic of a file of kind `kind` (SAR, WAV and so on) in format `f`.
std::string MagicOf(const Format& f, const char* kind)
{
    return std::string(1, f.letter) + kind;
}

std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Converts bytes to a string for comparison.
std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

// Writer with a growing buffer, in the byte order of `big_endian`.
struct Writer
{
    void U16(std::size_t at, uint32_t v)
    {
        bytes[at + (big_endian ? 0 : 1)] = static_cast<uint8_t>(v >> 8);
        bytes[at + (big_endian ? 1 : 0)] = static_cast<uint8_t>(v);
    }

    void U32(std::size_t at, uint32_t v)
    {
        U16(at + (big_endian ? 0 : 2), v >> 16);
        U16(at + (big_endian ? 2 : 0), v & 0xffff);
    }

    // Appends `n` zero bytes and returns their offset.
    std::size_t Add(std::size_t n)
    {
        bytes.resize(bytes.size() + n);

        return bytes.size() - n;
    }

    // Appends `data` and returns its offset.
    std::size_t Add(std::span<const uint8_t> data)
    {
        const std::size_t at = Add(data.size());
        std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));

        return at;
    }

    std::vector<uint8_t> bytes;
    bool big_endian = false;
};

// Builds a header for a file of kind `kind` in format `f`, with space for `blocks` block entries. The caller fills the
// entries.
Writer BinaryFileHeader(const Format& f, const char* kind, int blocks)
{
    Writer w;
    w.big_endian = f.big_endian;
    w.Add(0x40);
    std::memcpy(w.bytes.data(), MagicOf(f, kind).data(), 4);
    w.U16(0x04, 0xfeff);
    w.U16(0x06, 0x40);
    w.U16(0x10, static_cast<uint32_t>(blocks));

    return w;
}

// Fills block table entry `i` and writes the block's magic and size.
void Block(Writer& w, int i, uint16_t type, const char* magic, std::size_t at, std::size_t size)
{
    w.U16(0x14 + i * 12, type);
    w.U32(0x18 + i * 12, static_cast<uint32_t>(at));
    w.U32(0x1c + i * 12, static_cast<uint32_t>(size));
    std::memcpy(w.bytes.data() + at, magic, 4);
    w.U32(at + 4, static_cast<uint32_t>(size));
}

// Builds a PCM16 wave declaring `length` samples. Each of `channels` channels uses `samples`. If `loops` is set, the
// entire wave loops.
std::vector<uint8_t> Pcm16Wav(const Format& f, uint32_t length, const std::vector<int16_t>& samples,
                              uint32_t channels = 1, bool loops = false)
{
    Writer w = BinaryFileHeader(f, "WAV", 2);

    // INFO: PCM16, optional loop from sample 0, and channel references sharing one channel. Samples start at the DATA
    // payload; no ADPCM metadata.
    const std::size_t info = w.Add(8 + 0x18 + channels * 8 + 16), body = info + 8, table = body + 0x14;
    const std::size_t channel = table + 4 + channels * 8;
    w.bytes[body] = 1;
    w.bytes[body + 1] = loops ? 1 : 0;
    w.U32(body + 4, 32000);
    w.U32(body + 12, length);
    w.U32(table, channels);
    for (uint32_t c = 0; c < channels; c++)
    {
        w.U32(table + 8 + c * 8, static_cast<uint32_t>(channel - table));
    }
    Block(w, 0, 0x7000, "INFO", info, w.bytes.size() - info);

    const std::size_t data = w.Add(8);
    for (const int16_t s : samples)
    {
        w.U16(w.Add(2), static_cast<uint16_t>(s));
    }
    Block(w, 1, 0x7001, "DATA", data, w.bytes.size() - data);

    return w.bytes;
}

// Builds a one-channel wave with `encoding`, declaring `length` samples, with `data` as its samples. The channel's
// ADPCM reference has type `adpcm_type` and points to `adpcm`'s 16-bit values.
std::vector<uint8_t> CodedWav(const Format& f, uint8_t encoding, uint32_t length, const std::vector<uint8_t>& data,
                              uint16_t adpcm_type, const std::vector<int16_t>& adpcm)
{
    Writer w = BinaryFileHeader(f, "WAV", 2);

    // INFO: the encoding, then a channel table of one reference. The channel holds a sample reference and an ADPCM
    // reference, followed by the ADPCM values.
    const std::size_t info = w.Add(8 + 0x18 + 8 + 16 + adpcm.size() * 2), body = info + 8, table = body + 0x14;
    const std::size_t channel = table + 12;
    w.bytes[body] = encoding;
    w.U32(body + 4, 32000);
    w.U32(body + 12, length);
    w.U32(table, 1);
    w.U32(table + 8, static_cast<uint32_t>(channel - table));
    w.U16(channel + 8, adpcm_type);
    w.U32(channel + 12, 16);
    for (std::size_t i = 0; i < adpcm.size(); i++)
    {
        w.U16(channel + 16 + i * 2, static_cast<uint16_t>(adpcm[i]));
    }
    Block(w, 0, 0x7000, "INFO", info, w.bytes.size() - info);

    const std::size_t at = w.Add(8);
    w.Add(data);
    Block(w, 1, 0x7001, "DATA", at, w.bytes.size() - at);

    return w.bytes;
}

// Builds a bank with `instruments` references to one instrument. Its 128 key entries share a key region. The key
// region's 128 velocity entries share a velocity region: 16512 regions per instrument from about 2 KB of data.
std::vector<uint8_t> SharedRegionsBank(const Format& f, uint32_t instruments)
{
    Writer w = BinaryFileHeader(f, "BNK", 1);
    const std::size_t info = w.Add(8 + 16), body = info + 8;
    const std::size_t waves = w.Add(4);
    const std::size_t table = w.Add(4 + instruments * 8);
    const std::size_t instrument = w.Add(8);
    const std::size_t keys = w.Add(4 + 128 * 8);
    const std::size_t key = w.Add(8);
    const std::size_t velocities = w.Add(4 + 128 * 8);
    w.Add(8); // the region: wave 0, with no options

    // INFO: references to the empty wave table and to the instrument table.
    w.U16(body, 0x0100);
    w.U32(body + 4, static_cast<uint32_t>(waves - body));
    w.U16(body + 8, 0x0101);
    w.U32(body + 12, static_cast<uint32_t>(table - body));
    w.U32(table, instruments);
    for (uint32_t i = 0; i < instruments; i++)
    {
        w.U16(table + 4 + i * 8, 0x5900);
        w.U32(table + 8 + i * 8, static_cast<uint32_t>(instrument - table));
    }

    // Points the reference at `from` to an index table at `at`. All 128 table entries point to `to`.
    auto index_table = [&](std::size_t from, std::size_t at, std::size_t to)
    {
        w.U16(from, 0x6002);
        w.U32(from + 4, static_cast<uint32_t>(at - from));
        w.bytes[at + 1] = 127;
        for (std::size_t k = 0; k < 128; k++)
        {
            w.U32(at + 8 + k * 8, static_cast<uint32_t>(to - at));
        }
    };
    index_table(instrument, keys, key);
    index_table(key, velocities, velocities + 4 + 128 * 8);
    Block(w, 0, 0x5800, "INFO", info, w.bytes.size() - info);

    return w.bytes;
}

// Builds a bank with one instrument. Its key table and its key region's velocity table are direct tables, and its one
// velocity region plays wave 0 with no options.
std::vector<uint8_t> DirectBank(const Format& f)
{
    Writer w = BinaryFileHeader(f, "BNK", 1);
    const std::size_t info = w.Add(8 + 16), body = info + 8;
    const std::size_t wave_table = w.Add(4);
    const std::size_t table = w.Add(12);
    const std::size_t instrument = w.Add(40);

    // INFO: references to the empty wave table and to the instrument table.
    w.U16(body, 0x0100);
    w.U32(body + 4, static_cast<uint32_t>(wave_table - body));
    w.U16(body + 8, 0x0101);
    w.U32(body + 12, static_cast<uint32_t>(table - body));
    w.U32(table, 1);
    w.U16(table + 4, 0x5900);
    w.U32(table + 8, static_cast<uint32_t>(instrument - table));

    // The instrument and the key region each reference a direct table 8 bytes ahead. Each table points another 8 bytes
    // ahead to the next region.
    for (const std::size_t at : {instrument, instrument + 16})
    {
        w.U16(at, 0x6000);
        w.U32(at + 4, 8);
        w.U32(at + 12, 8);
    }
    Block(w, 0, 0x5800, "INFO", info, w.bytes.size() - info);

    return w.bytes;
}

// Builds a bank with one instrument whose key table is a range table with the upper keys `upper_keys`. Its key regions
// have no velocity tables.
std::vector<uint8_t> RangeBank(const Format& f, const std::vector<uint8_t>& upper_keys)
{
    const std::size_t n = upper_keys.size(), refs = 4 + ((n + 3) & ~std::size_t{3});
    Writer w = BinaryFileHeader(f, "BNK", 1);
    const std::size_t info = w.Add(8 + 16), body = info + 8;
    const std::size_t wave_table = w.Add(4);
    const std::size_t table = w.Add(12);
    const std::size_t instrument = w.Add(8 + refs + n * 8);

    w.U16(body, 0x0100);
    w.U32(body + 4, static_cast<uint32_t>(wave_table - body));
    w.U16(body + 8, 0x0101);
    w.U32(body + 12, static_cast<uint32_t>(table - body));
    w.U32(table, 1);
    w.U16(table + 4, 0x5900);
    w.U32(table + 8, static_cast<uint32_t>(instrument - table));

    // The instrument references a range table 8 bytes ahead: a count, the upper keys and a null reference for each.
    w.U16(instrument, 0x6001);
    w.U32(instrument + 4, 8);
    const std::size_t range = instrument + 8;
    w.U32(range, static_cast<uint32_t>(n));
    for (std::size_t k = 0; k < n; k++)
    {
        w.bytes[range + 4 + k] = upper_keys[k];
        w.U32(range + refs + k * 8 + 4, 0xffffffff);
    }
    Block(w, 0, 0x5800, "INFO", info, w.bytes.size() - info);

    return w.bytes;
}

// Builds a sequence file with `commands` in DATA and `labels` (name, offset into DATA) in LABL.
std::vector<uint8_t> LabelledSequenceFile(const Format& f, const std::vector<uint8_t>& commands,
                                          const std::vector<std::pair<std::string, uint32_t>>& labels)
{
    Writer w = BinaryFileHeader(f, "SEQ", 2);
    const std::size_t data = w.Add(8);
    w.Add(commands);
    Block(w, 0, 0x5000, "DATA", data, w.bytes.size() - data);

    // LABL: a count and a reference to each label. A label holds a reference into DATA, its name's length and the name.
    const std::size_t labl = w.Add(12 + labels.size() * 8), b = labl + 8;
    w.U32(b, static_cast<uint32_t>(labels.size()));
    for (std::size_t i = 0; i < labels.size(); i++)
    {
        const std::string& name = labels[i].first;
        const std::size_t e = w.Add(12 + name.size() + 1);
        w.U16(b + 4 + i * 8, 0x5100);
        w.U32(b + 8 + i * 8, static_cast<uint32_t>(e - b));
        w.U16(e, 0x1f00);
        w.U32(e + 4, labels[i].second);
        w.U32(e + 8, static_cast<uint32_t>(name.size()));
        std::memcpy(w.bytes.data() + e + 12, name.data(), name.size());
    }
    Block(w, 1, 0x5001, "LABL", labl, w.bytes.size() - labl);

    return w.bytes;
}

// Builds a group with embedded files `held` (ID, contents) and references to non-embedded files `named`.
std::vector<uint8_t> Group(const Format& f, const std::vector<std::pair<uint32_t, std::string>>& held,
                           const std::vector<uint32_t>& named)
{
    Writer w = BinaryFileHeader(f, "GRP", 2);
    const std::size_t n = held.size() + named.size();
    const std::size_t info = w.Add(12 + n * 24), body = info + 8;
    w.U32(body, static_cast<uint32_t>(n));
    for (std::size_t i = 0; i < n; i++)
    {
        const std::size_t e = 4 + n * 8 + i * 16; // from the start of the table
        w.U16(body + 4 + i * 8, 0x7900);
        w.U32(body + 8 + i * 8, static_cast<uint32_t>(e));
        w.U32(body + e, i < held.size() ? held[i].first : named[i - held.size()]);
        w.U32(body + e + 8, 0xffffffff); // a null reference until a file is placed
        w.U32(body + e + 12, 0xffffffff);
    }
    Block(w, 0, 0x7800, "INFO", info, w.bytes.size() - info);

    const std::size_t file = w.Add(8);
    for (std::size_t i = 0; i < held.size(); i++)
    {
        const std::size_t e = body + 4 + n * 8 + i * 16;
        w.U16(e + 4, 0x1f00);
        w.U32(e + 8, static_cast<uint32_t>(w.Add(Bytes(held[i].second)) - file - 8));
        w.U32(e + 12, static_cast<uint32_t>(held[i].second.size()));
    }
    Block(w, 1, 0x7801, "FILE", file, w.bytes.size() - file);

    return w.bytes;
}

// Builds a sequence file containing `commands` in DATA.
std::vector<uint8_t> SequenceFile(const Format& f, const std::vector<uint8_t>& commands)
{
    Writer w = BinaryFileHeader(f, "SEQ", 1);
    const std::size_t data = w.Add(8);
    w.Add(commands);
    Block(w, 0, 0x5000, "DATA", data, w.bytes.size() - data);

    return w.bytes;
}

// A test file, either embedded or absent using one of the two supported location encodings.
struct ArchiveFile
{
    enum Place
    {
        kStored,
        kNullLocation, // a null reference instead of a location
        kNullOffset,   // a location with an offset of 0xFFFFFFFF
        kExternal      // a path outside the archive, held in `path`
    };

    Place place = kStored;
    std::vector<uint8_t> data;
    uint32_t extra_size = 0; // added to the size of a stored file in the file table
    std::string path = {};
};

// Test sequence metadata: sequence file, first-track offset and bank slot item IDs.
struct SequenceSound
{
    uint32_t file_id = 0;
    uint32_t start_offset = 0;
    std::vector<uint32_t> banks = {};
};

// Test archive tables: sequences, bank files, wave archives, group files and item names keyed by ID. Group file ID
// 0xFFFFFFFF means no file. Defaults allow partial designated initializers without warnings.
struct ArchiveLists
{
    std::vector<SequenceSound> sequences = {};
    std::vector<uint32_t> banks = {}, wave_archives = {}, groups = {};
    std::vector<std::pair<uint32_t, std::string>> names = {};
};

// Builds a sound archive in format `f` with `files` in order and metadata from `lists`.
std::vector<uint8_t> Archive(const Format& f, const std::vector<ArchiveFile>& files, const ArchiveLists& lists = {})
{
    Writer w = BinaryFileHeader(f, "SAR", lists.names.empty() ? 2 : 3);
    w.U32(0x08, f.version);

    // INFO: eight table references followed by their tables. Sound sets and players are empty; player settings are
    // zero.
    const std::size_t info = w.Add(8 + 8 * 8), body = info + 8;
    const uint16_t kTables[8] = {0x2100, 0x2104, 0x2101, 0x2103, 0x2105, 0x2102, 0x2106, 0x220b};
    std::size_t file_table = 0;
    for (int i = 0; i < 8; i++)
    {
        const std::size_t t = w.Add(4);
        w.U16(body + i * 8, kTables[i]);
        w.U32(body + i * 8 + 4, static_cast<uint32_t>(t - body));
        if (kTables[i] == 0x2100)
        {
            // Sound entry: file, volume, sequence settings at +28, and the pan option. Sequence settings point to a
            // bank table at +20, allocate track 0 and include the start-offset option.
            const std::vector<SequenceSound>& sequences = lists.sequences;
            w.U32(t, static_cast<uint32_t>(sequences.size()));
            const std::size_t refs = w.Add(sequences.size() * 8);
            for (std::size_t s = 0; s < sequences.size(); s++)
            {
                const SequenceSound& sound = sequences[s];
                const std::size_t e = w.Add(52 + sound.banks.size() * 4), q = e + 28;
                w.U16(refs + s * 8, 0x2200);
                w.U32(refs + s * 8 + 4, static_cast<uint32_t>(e - t));
                w.U32(e, sound.file_id);
                w.bytes[e + 8] = 127;
                w.U16(e + 12, 0x2203);
                w.U32(e + 16, static_cast<uint32_t>(q - e));
                w.U32(e + 20, 1 << 1);
                w.U32(q + 4, 20);
                w.U32(q + 8, 1);
                w.U32(q + 12, 1);
                w.U32(q + 16, sound.start_offset);
                w.U32(q + 20, static_cast<uint32_t>(sound.banks.size()));
                for (std::size_t b = 0; b < sound.banks.size(); b++)
                {
                    w.U32(q + 24 + b * 4, sound.banks[b]);
                }
            }
        }

        // Bank, wave archive and group entries contain file IDs with no options.
        const std::vector<uint32_t>* file_ids = kTables[i] == 0x2101   ? &lists.banks
                                                : kTables[i] == 0x2103 ? &lists.wave_archives
                                                : kTables[i] == 0x2105 ? &lists.groups
                                                                       : nullptr;
        if (file_ids)
        {
            w.U32(t, static_cast<uint32_t>(file_ids->size()));
            w.Add(file_ids->size() * 16);
            for (std::size_t k = 0; k < file_ids->size(); k++)
            {
                const std::size_t e = 4 + file_ids->size() * 8 + k * 8;
                w.U32(t + 8 + k * 8, static_cast<uint32_t>(e));
                w.U32(t + e, (*file_ids)[k]);
            }
        }
        if (kTables[i] == 0x2106)
        {
            file_table = t;
            w.U32(t, static_cast<uint32_t>(files.size()));
            w.Add(files.size() * 32);
        }
    }
    Block(w, 0, 0x2001, "INFO", info, w.bytes.size() - info);

    // FILE payload and matching locations in the file table.
    const std::size_t file = w.Add(8);
    for (std::size_t i = 0; i < files.size(); i++)
    {
        const std::size_t e = file_table + 4 + files.size() * 8 + i * 24; // location reference, options, location
        w.U16(file_table + 4 + i * 8, 0x220a);
        w.U32(file_table + 8 + i * 8, static_cast<uint32_t>(e - file_table));
        if (files[i].place == ArchiveFile::kNullLocation)
        {
            w.U32(e + 4, 0xffffffff);
            continue;
        }
        if (files[i].place == ArchiveFile::kExternal)
        {
            const std::size_t path = w.Add(files[i].path.size() + 1);
            std::memcpy(w.bytes.data() + path, files[i].path.data(), files[i].path.size());
            w.U16(e, 0x220d);
            w.U32(e + 4, static_cast<uint32_t>(path - e));
            continue;
        }

        w.U16(e, 0x220c);
        w.U32(e + 4, 12);
        w.U32(e + 16, 0xffffffff);
        w.U32(e + 20, 0xffffffff);
        if (files[i].place == ArchiveFile::kStored)
        {
            w.U16(e + 12, 0x1f00);
            w.U32(e + 16, static_cast<uint32_t>(w.Add(files[i].data) - file - 8));
            w.U32(e + 20, static_cast<uint32_t>(files[i].data.size()) + files[i].extra_size);
        }
    }
    Block(w, 1, 0x2002, "FILE", file, w.bytes.size() - file);

    // STRG: references to the name table and Patricia tree. Only the leaves' name-to-item mappings are read.
    if (!lists.names.empty())
    {
        const std::size_t count = lists.names.size();
        const std::size_t strg = w.Add(8 + 16), refs = strg + 8;
        const std::size_t table = w.Add(4 + count * 12), tree = w.Add(8 + count * 20);
        w.U16(refs, 0x2400);
        w.U32(refs + 4, static_cast<uint32_t>(table - refs));
        w.U16(refs + 8, 0x2401);
        w.U32(refs + 12, static_cast<uint32_t>(tree - refs));
        w.U32(table, static_cast<uint32_t>(count));
        w.U32(tree + 4, static_cast<uint32_t>(count));
        for (std::size_t i = 0; i < count; i++)
        {
            const std::string& name = lists.names[i].second;
            const std::size_t at = w.Add(name.size() + 1), e = table + 4 + i * 12, node = tree + 8 + i * 20;
            std::memcpy(w.bytes.data() + at, name.data(), name.size());
            w.U16(e, 0x1f01);
            w.U32(e + 4, static_cast<uint32_t>(at - table));
            w.U32(e + 8, static_cast<uint32_t>(name.size() + 1));
            w.U16(node, 1); // a leaf
            w.U32(node + 12, static_cast<uint32_t>(i));
            w.U32(node + 16, lists.names[i].first);
        }
        Block(w, 2, 0x2000, "STRG", strg, w.bytes.size() - strg);
    }

    w.U32(0x0c, static_cast<uint32_t>(w.bytes.size()));

    return w.bytes;
}

// Group test layout: file 0 is embedded directly; files 1 and 2 are groups stored after it. Files 3-5 have no direct
// location. Both groups hold different copies of file 3; only the second holds file 4; neither holds file 5. The first
// also references file 0 without embedding it. `first_group_magic` replaces the first group's magic when it's given.
std::vector<ArchiveFile> GroupTestFiles(const Format& f, const char* first_group_magic = nullptr)
{
    std::vector<ArchiveFile> files(6);
    files[0].data = Bytes("stored");
    files[1].data = Group(f, {{3, "first copy"}}, {0});
    if (first_group_magic)
    {
        std::memcpy(files[1].data.data(), first_group_magic, 4);
    }
    files[2].data = Group(f, {{3, "second copy"}, {4, std::string(64, 'x')}}, {});
    files[3].place = ArchiveFile::kNullLocation;
    files[4].place = ArchiveFile::kNullOffset;
    files[5].place = ArchiveFile::kNullLocation;

    return files;
}

void TestFilesKeptInGroupsAreFound(Flavor flavor)
{
    // The third group has no file.
    const Format f = FormatOf(flavor);

    const SoundArchive a =
        SoundArchive::Load(Archive(f, GroupTestFiles(f), {.groups = {1, 2, 0xffffffff}}), OptionsOf(flavor));

    const std::string on = " (" + NameOf(flavor) + ")";
    Check(Text(a.FileData(0)) == "stored" && Text(a.FileData(3)) == "first copy", "files in groups" + on);
    Check(Text(a.FileData(4)) == std::string(64, 'x'), "a file in the second group only" + on);
    Check(a.FileData(5).empty() && !a.FileMissing(5), "a file in no group" + on);
    Check(a.GroupErrors().empty() && !a.Truncated(), "no group errors" + on);
    Check(a.ExternalGroups() == std::vector<std::string>{"GROUP_2"}, "the group kept outside" + on);
}

void TestGroupsListTheFilesTheyLoad(Flavor flavor)
{
    // The first group holds a copy of file 3 and links to file 0 in the archive. The second holds copies of files 3
    // and 4. The third has no file.
    const Format f = FormatOf(flavor);

    const SoundArchive a =
        SoundArchive::Load(Archive(f, GroupTestFiles(f), {.groups = {1, 2, 0xffffffff}}), StrictOptionsOf(flavor));

    const std::string on = " (" + NameOf(flavor) + ")";
    Check(a.GroupFileIds(0) == std::vector<uint32_t>{3, 0} && a.GroupFileIds(1) == std::vector<uint32_t>{3, 4},
          "the files that each group loads" + on);
    Check(a.GroupFileIds(2).empty() && a.GroupFileIds(3).empty(), "a group kept outside, and no group" + on);
    Check(a.ReadOptions().flavor == flavor && a.ReadOptions().rules == Rules::kStopAtFirstProblem,
          "the options it was read with" + on);
}

void TestBrokenGroupIsReported(Flavor flavor)
{
    const Format f = FormatOf(flavor);

    const SoundArchive a =
        SoundArchive::Load(Archive(f, GroupTestFiles(f, "XGRP"), {.groups = {1, 2}}), OptionsOf(flavor));

    const std::string error = "group GROUP_0 is unreadable (bad magic, expected " + MagicOf(f, "GRP") +
                              "); omitting files stored only in this group";
    Check(Text(a.FileData(3)) == "second copy" && a.GroupErrors() == std::vector<std::string>{error},
          "a broken group (" + NameOf(flavor) + ")");
}

void TestBrokenGroupWhoseFilesAreElsewhereIsntReported(Flavor flavor)
{
    // Store file 5 directly. Then the second group holds a copy of every file stored in groups, and the broken first
    // group leaves out none.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files = GroupTestFiles(f, "XGRP");
    files[5] = {.data = Bytes("stored too")};

    const SoundArchive a = SoundArchive::Load(Archive(f, files, {.groups = {1, 2}}), OptionsOf(flavor));

    Check(Text(a.FileData(3)) == "second copy" && Text(a.FileData(5)) == "stored too" && a.GroupErrors().empty(),
          "a broken group whose files are elsewhere (" + NameOf(flavor) + ")");
}

void TestGroupPastTheEndLeavesItsFilesMissing(Flavor flavor)
{
    // Truncate halfway through file 4: the second group remains partly readable. Truncate at the group start: its
    // contents are unknown. Any file absent from the first group might be there.
    const Format f = FormatOf(flavor);
    const std::vector<uint8_t> bytes = Archive(f, GroupTestFiles(f), {.groups = {1, 2}});
    for (const std::size_t cut : {std::size_t{32}, GroupTestFiles(f)[2].data.size()})
    {
        const SoundArchive a = SoundArchive::Load(
            std::vector<uint8_t>(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut)), OptionsOf(flavor));

        const std::string on = " (" + NameOf(flavor) + ", " + std::to_string(cut) + " bytes cut)";
        Check(a.Truncated() && a.GroupErrors().empty(), "a truncated archive" + on);
        Check(Text(a.FileData(3)) == "first copy", "the first group's copy" + on);
        Check(a.FileData(4).empty() && a.FileMissing(4) && a.FileMissing(5), "files past the end" + on);
    }
}

void TestFilePastTheEndOfItsGroupIsReported(Flavor flavor)
{
    // Truncate the second group halfway through file 4 but leave the archive itself complete.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files = GroupTestFiles(f);
    files[2].data.resize(files[2].data.size() - 32);

    const SoundArchive a = SoundArchive::Load(Archive(f, files, {.groups = {1, 2}}), OptionsOf(flavor));

    const std::string on = " (" + NameOf(flavor) + ")";
    Check(Text(a.FileData(3)) == "first copy", "the first group's copy" + on);
    Check(a.FileData(4).empty() && !a.FileMissing(4) && !a.Truncated(), "a file cut short" + on);
    Check(a.GroupErrors() == std::vector<std::string>{"group GROUP_1: file 4 extends past the group; skipped"},
          "the error for a file cut short" + on);
}

void TestFileCutShortInOneGroupIsTakenFromAnother(Flavor flavor)
{
    // Truncate the first group's copy of file 3; the second group still has a complete copy.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files = GroupTestFiles(f);
    files[1].data.resize(files[1].data.size() - 5);

    const SoundArchive a = SoundArchive::Load(Archive(f, files, {.groups = {1, 2}}), OptionsOf(flavor));

    Check(Text(a.FileData(3)) == "second copy" && a.GroupErrors().empty(),
          "a file taken from another group (" + NameOf(flavor) + ")");
}

void TestFilePastTheEndOfAWholeArchiveIsntTruncation(Flavor flavor)
{
    // Give a file an oversized length without truncating the archive itself.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files = GroupTestFiles(f);
    files[0].extra_size = 1000000;

    const SoundArchive a = SoundArchive::Load(Archive(f, files, {.groups = {1, 2}}), OptionsOf(flavor));

    Check(a.FileData(0).empty() && a.FileMissing(0) && !a.Truncated() && Text(a.FileData(3)) == "first copy",
          "a file past the end of a whole archive (" + NameOf(flavor) + ")");
}

void TestDamagedNameTableLeavesSoundsNumbered(Flavor flavor)
{
    // Give the name tree of an archive more nodes than the archive holds. The archive still loads, and its sound goes
    // by number.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files(1);
    files[0].data = SequenceFile(f, {0x3c, 0x64, 0x18, 0xff});
    std::vector<uint8_t> bytes = Archive(f, files, {.sequences = {{0, 0}}, .names = {{0x01000000, "SEQ_A"}}});
    const xsflib::Reader r(bytes, f.big_endian);
    const std::size_t node_count = r.U32(0x30) + 24 + 16 + 4; // STRG, its one-entry string table, the tree's count
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(node_count), 4, uint8_t{0x7f});

    const SoundArchive archive = SoundArchive::Load(bytes, OptionsOf(flavor));

    Check(archive.NamesDamaged() && archive.Sounds().size() == 1 && archive.Sounds()[0].name == "SEQ_0",
          "a damaged name table (" + NameOf(flavor) + ")");
}

void TestTruncatedArchiveWhoseTablesCantBeReadIsReported(Flavor flavor)
{
    // Truncate within INFO's table references, then within the file's block table.
    const Format f = FormatOf(flavor);
    const std::vector<uint8_t> bytes = Archive(f, GroupTestFiles(f), {.groups = {1, 2}});
    auto message_for = [&](std::ptrdiff_t size)
    {
        return ErrorOf(
            [&] { SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), OptionsOf(flavor)); });
    };

    const std::string in_info = message_for(0x60);
    const std::string in_block_table = message_for(0x20);

    const std::string size = std::to_string(bytes.size());
    const std::string on = " (" + NameOf(flavor) + ")";
    Check(
        in_info == "truncated archive: 96 of " + size + " bytes; cannot read tables (read out of bounds at offset 96)",
        "an archive cut in INFO" + on);
    Check(in_block_table.starts_with("truncated archive: 32 of " + size +
                                     " bytes; cannot read tables (read out of bounds"),
          "an archive cut in its block table" + on);
}

void TestUnreadableGroupTableIsReported(Flavor flavor)
{
    // Make INFO's fifth reference (groups) point outside the archive. References start at 0x48, after the file and
    // block headers.
    const Format f = FormatOf(flavor);
    Writer w{Archive(f, GroupTestFiles(f), {.groups = {1, 2}}), f.big_endian};
    w.U32(0x48 + 4 * 8 + 4, 0x7ffffff0);

    const SoundArchive a = SoundArchive::Load(w.bytes, OptionsOf(flavor));

    const std::string error =
        "cannot read group table (read out of bounds at offset 2147483704); files stored only in groups may be missing";
    Check(Text(a.FileData(0)) == "stored" && a.FileData(3).empty() && !a.FileMissing(3) &&
              a.GroupErrors() == std::vector<std::string>{error},
          "an unreadable group table (" + NameOf(flavor) + ")");
}

void TestGroupTableFirstLocatesNoneOfADamagedGroup(Flavor flavor)
{
    // The group's second entry points outside the group. Reading the entries one at a time locates file 3 before the
    // damage. Reading the whole table first locates neither file.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files(3);
    Writer group{Group(f, {{1, "only copy"}, {2, "lost"}}, {}), f.big_endian};
    group.U32(0x48 + 8 + 8, 0x7ffffff0);
    files[0].data = group.bytes;
    files[1].place = ArchiveFile::kNullLocation;
    files[2].place = ArchiveFile::kNullLocation;
    const std::vector<uint8_t> bytes = Archive(f, files, {.groups = {0}});
    Options as_read = OptionsOf(flavor);
    as_read.group_table_first = false;
    Options table_first = as_read;
    table_first.group_table_first = true;

    const SoundArchive a = SoundArchive::Load(bytes, as_read);
    const SoundArchive b = SoundArchive::Load(bytes, table_first);

    const std::string on = " (" + NameOf(flavor) + ")";
    const std::string kError = "group GROUP_0 is unreadable (read out of bounds at offset ";
    Check(Text(a.FileData(1)) == "only copy" && a.FileData(2).empty(), "entries located as they're read" + on);
    Check(b.FileData(1).empty() && b.FileData(2).empty(), "a whole table read first" + on);
    Check(a.GroupErrors().size() == 1 && a.GroupErrors()[0].starts_with(kError) && a.GroupErrors() == b.GroupErrors(),
          "the damaged group's error" + on);
}

void TestNamesHaveNoControlCharacters(Flavor flavor)
{
    // Replace unsafe name characters with '?': newline, terminal escape, DEL, C1 NEL, Unicode line/paragraph separators
    // and invalid UTF-8 (lone byte, overlong encoding, surrogate and truncated character). Preserve valid é and emoji.
    const std::vector<std::pair<std::string, std::string>> kPieces = {
        {"Seq", "Seq"},           {"\n", "?"},
        {"\x1b[2J", "?[2J"},      {"\x7f", "?"},
        {"\xc2\x85", "?"},        {"\xe2\x80\xa8", "?"},
        {"\xe2\x80\xa9", "?"},    {"\xff", "?"},
        {"\xc0\xaf", "??"},       {"\xed\xa0\x80", "???"},
        {"\xc3\xa9", "\xc3\xa9"}, {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"},
        {"\xe2\x80", "??"}};
    std::string name, printable;
    for (const auto& [piece, shown] : kPieces)
    {
        name += piece;
        printable += shown;
    }

    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files(1);
    files[0].data = SequenceFile(f, {0xff});

    const SoundArchive archive = SoundArchive::Load(
        Archive(f, files, {.sequences = {{0, 0}}, .names = {{0x01000000, name}}}), OptionsOf(flavor));

    Check(archive.Sounds().size() == 1 && archive.Sounds()[0].name == printable,
          "printable names (" + NameOf(flavor) + ")");
}

void TestBankOfSharedRegionsIsReported(Flavor flavor)
{
    // Shared region table entries can expand a small bank into many regions. A larger version of this input could
    // exhaust memory.
    const std::vector<uint8_t> bank = SharedRegionsBank(FormatOf(flavor), 2);

    const std::string message = ErrorOf([&] { Bank::Parse(bank, OptionsOf(flavor)); });

    Check(message == "invalid region tables (region count exceeds bank size in bytes)",
          "a bank of shared regions (" + NameOf(flavor) + ")");
}

void TestGroupFilesBesideTheArchiveAreAdded(Flavor flavor)
{
    // The archive lists group GROUP_SE with no file, and file 0 with no location: only the group's file holds it.
    // Adding that file makes file 0 readable; a file that isn't a group is rejected. The archive keeps the group file.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files(1);
    files[0].place = ArchiveFile::kNullLocation;
    SoundArchive archive = SoundArchive::Load(
        Archive(f, files, {.groups = {0xffffffff}, .names = {{0x06000000, "GROUP_SE"}}}), OptionsOf(flavor));
    const bool missing_before = archive.FileData(0).empty() && !archive.FileMissing(0);
    const std::vector<uint8_t> group = Group(f, {{0, "kept beside"}}, {});

    archive.AddGroupFile(group);
    const std::string refused = ErrorOf([&] { archive.AddGroupFile(SequenceFile(f, {0xff})); });

    const std::string on = " (" + NameOf(flavor) + ")";
    Check(archive.ExternalGroups() == std::vector<std::string>{"GROUP_SE"} && missing_before,
          "a group kept beside the archive" + on);
    Check(Text(archive.FileData(0)) == "kept beside" && refused == "bad magic, expected " + MagicOf(f, "GRP"),
          "its file once added" + on);
    Check(archive.GroupFiles() == std::vector<std::vector<uint8_t>>{group}, "the group file kept" + on);
}

void TestSarcFilesAreRead()
{
    // A SARC in either byte order gives its files. A file that runs past the data is left out. Other data has no files.
    namespace sar_files = ntdlib::testing::sar_files;
    for (const bool big_endian : {true, false})
    {
        std::vector<uint8_t> sarc = sar_files::Sarc(big_endian, {Bytes("first"), Bytes("second")});
        const std::vector<SarcFile> files = SarcFiles(sarc);
        const std::string order = big_endian ? " (big-endian)" : " (little-endian)";
        Check(files.size() == 2 && Text(files[0].data) == "first" && Text(files[1].data) == "second" &&
                  files[0].name.empty() && files[1].name.empty(),
              "a SARC's files" + order);

        // The second file's end, in its node in SFAT, past the data.
        const auto end = static_cast<uint32_t>(sarc.size());
        big_endian ? StoreBe32(&sarc[0x14 + 0xC + 16 + 12], end) : StoreLe32(&sarc[0x14 + 0xC + 16 + 12], end);
        const std::vector<SarcFile> cut = SarcFiles(sarc);
        Check(cut.size() == 1 && Text(cut[0].data) == "first", "a file past a SARC's data left out" + order);
    }

    Check(SarcFiles(Bytes("not a SARC, though long enough")).empty(), "other data has no files");
    Check(SarcFiles(Bytes("SARC")).empty(), "a SARC whose header is cut short has no files");
}

void TestSarcNamesAreRead()
{
    // A file's name comes from the SFNT block, at the offset in words that its node gives. A file without a name, a
    // name that doesn't end before the data, a name past the names and a SARC without its SFNT block give empty names.
    // The files are read all the same.
    namespace sar_files = ntdlib::testing::sar_files;
    const std::size_t kNodes = 0x14 + 0xC;
    for (const bool big_endian : {true, false})
    {
        const std::string order = big_endian ? " (big-endian)" : " (little-endian)";
        const std::vector<uint8_t> sarc = sar_files::Sarc(
            big_endian, {{"Sound/GROUP_A.bfgrp", Bytes("A")}, {"", Bytes("none")}, {"B.bfgrp", Bytes("B")}});
        const std::vector<SarcFile> files = SarcFiles(sarc);
        Check(files.size() == 3 && files[0].name == "Sound/GROUP_A.bfgrp" && Text(files[0].data) == "A" &&
                  files[1].name.empty() && Text(files[1].data) == "none" && files[2].name == "B.bfgrp" &&
                  Text(files[2].data) == "B",
              "a SARC's names" + order);

        // The names and the padding after them, up to the data, with no NUL.
        std::vector<uint8_t> unended = sar_files::Sarc(big_endian, {{"N", Bytes("data")}});
        const std::size_t data = big_endian ? LoadBe32(&unended[0xC]) : LoadLe32(&unended[0xC]);
        std::fill(unended.begin() + static_cast<std::ptrdiff_t>(kNodes + 16 + 8),
                  unended.begin() + static_cast<std::ptrdiff_t>(data), uint8_t{'x'});
        const std::vector<SarcFile> running = SarcFiles(unended);
        Check(running.size() == 1 && running[0].name.empty() && Text(running[0].data) == "data",
              "a name that runs into the data left empty" + order);

        // The first file's name at word 0x100, past the names.
        std::vector<uint8_t> past = sarc;
        big_endian ? StoreBe32(&past[kNodes + 4], 0x01000100) : StoreLe32(&past[kNodes + 4], 0x01000100);
        const std::vector<SarcFile> beyond = SarcFiles(past);
        Check(beyond.size() == 3 && beyond[0].name.empty() && beyond[2].name == "B.bfgrp",
              "a name past the names left empty" + order);

        // The SFNT block's magic damaged.
        std::vector<uint8_t> no_sfnt = sarc;
        std::memcpy(&no_sfnt[kNodes + 3 * 16], "XXXX", 4);
        const std::vector<SarcFile> nameless = SarcFiles(no_sfnt);
        Check(nameless.size() == 3 && nameless[0].name.empty() && nameless[2].name.empty() &&
                  Text(nameless[2].data) == "B",
              "a SARC without its SFNT block has no names" + order);
    }
}

void TestGroupFilesMatchTheirArchive(Flavor flavor, Rules rules)
{
    // The archive's sequence is file 0, in the archive. Its bank and its wave archive are files 1 and 2. Only the group
    // GROUP_SE holds them. The group also holds file 3. No table names it.
    namespace sar_files = ntdlib::testing::sar_files;
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_SE", .file = 0, .banks = {0}}};
    spec.banks = {{"BANK_SE", 1}};
    spec.wave_archives = {{"WARC_SE", 2}};
    spec.players = {{"PLAYER_SE", 1}};
    spec.groups = {{"GROUP_SE", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(flavor, {0xff}, {}), "", false}, {{}, "", true}, {{}, "", true}, {{}, "", true}};
    const SoundArchive archive = SoundArchive::Load(sar_files::Archive(flavor, spec), {flavor, {}, false, rules});
    const auto file = [flavor](const char* kind)
    {
        return Bytes(MagicOf(FormatOf(flavor), kind) + " file");
    };
    const auto group = [flavor](const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& items)
    {
        return sar_files::Group(flavor, items);
    };
    const std::string on = " (" + NameOf(flavor) + (rules == Rules::kStopAtFirstProblem ? ", strict)" : ")");

    Check(archive.MatchesGroupFile(group({{1, file("BNK")}, {2, file("WAR")}, {3, Bytes("data")}})),
          "the archive's group" + on);
    Check(archive.MatchesGroupFile(group({{0, spec.files[0].bytes}, {1, file("BNK")}})),
          "a group with a copy of a file that the archive holds" + on);
    Check(!archive.MatchesGroupFile(group({{1, file("WAR")}, {2, file("WAR")}})), "a wave archive for the bank" + on);
    Check(!archive.MatchesGroupFile(group({{1, file("BNK")}, {4, file("BNK")}})),
          "a file past the archive's file table" + on);
    Check(!archive.MatchesGroupFile(group({{3, Bytes("data")}})), "a group of files that no table names" + on);
    Check(!archive.MatchesGroupFile(spec.files[0].bytes), "a file that isn't a group" + on);
}

void TestArchivesOfOtherConsolesAreRefused()
{
    // With kReadPastDamage, each flavor rejects the other consoles' archives with a message that names the archive's
    // kind.
    auto message_for = [](std::vector<uint8_t> bytes, Flavor flavor)
    {
        return ErrorOf([&] { SoundArchive::Load(std::move(bytes), OptionsOf(flavor)); });
    };
    auto with_magic = [](std::vector<uint8_t> bytes, const char* magic)
    {
        std::memcpy(bytes.data(), magic, 4);
        return bytes;
    };
    const std::vector<uint8_t> ctr = Archive(FormatOf(Flavor::kCtr), {});
    const std::vector<uint8_t> cafe = Archive(FormatOf(Flavor::kCafe), {});
    const std::vector<uint8_t> nx = Archive(FormatOf(Flavor::kNx), {});

    Check(message_for(cafe, Flavor::kCtr) == "unsupported FSAR archive (Wii U/Switch); expected CSAR (3DS)",
          "a Wii U archive on the 3DS");
    Check(message_for(with_magic(ctr, "RSAR"), Flavor::kCtr) == "bad magic, expected CSAR", "a Wii archive on the 3DS");
    Check(
        message_for(with_magic(cafe, "CSAR"), Flavor::kCafe) == "unsupported CSAR archive (3DS); expected FSAR (Wii U)",
        "a 3DS archive on the Wii U");
    Check(
        message_for(with_magic(cafe, "RSAR"), Flavor::kCafe) == "unsupported RSAR archive (Wii); expected FSAR (Wii U)",
        "a Wii archive on the Wii U");
    Check(message_for(nx, Flavor::kCafe) == "unsupported little-endian FSAR archive (Switch); expected a Wii U archive",
          "a Switch archive on the Wii U");
    Check(message_for(with_magic(nx, "CSAR"), Flavor::kNx) == "unsupported CSAR archive (3DS); expected FSAR (Switch)",
          "a 3DS archive on the Switch");
    Check(message_for(with_magic(nx, "RSAR"), Flavor::kNx) == "unsupported RSAR archive (Wii); expected FSAR (Switch)",
          "a Wii archive on the Switch");
    Check(message_for(cafe, Flavor::kNx) == "unsupported big-endian FSAR archive (Wii U); expected a Switch archive",
          "a Wii U archive on the Switch");
    Check(message_for(ctr, Flavor::kCtr).empty() && message_for(cafe, Flavor::kCafe).empty() &&
              message_for(nx, Flavor::kNx).empty(),
          "each console's archive");
}

void TestByteOrderMarksAreRead()
{
    // The Wii U and the Switch read a file in the byte order that its mark gives. The 3DS reads little-endian files
    // only. A file that's too short names its magic with the right article.
    const std::vector<int16_t> kSamples = {1, -2, 300, -32768};
    const Format kBig = {'F', true, 0};
    const Format kLittle = {'F', false, 0};
    const Format kCtrBig = {'C', true, 0};

    for (const Flavor flavor : {Flavor::kCafe, Flavor::kNx})
    {
        const Pcm from_big = DecodeWave(Pcm16Wav(kBig, 4, kSamples), OptionsOf(flavor));
        const Pcm from_little = DecodeWave(Pcm16Wav(kLittle, 4, kSamples), OptionsOf(flavor));

        Check(from_big.channels.size() == 1 && from_big.channels[0] == kSamples && from_big.sample_rate == 32000 &&
                  from_little.channels.size() == 1 && from_little.channels[0] == kSamples,
              "waves in either byte order (" + NameOf(flavor) + ")");
    }

    const std::string refused = ErrorOf([&] { DecodeWave(Pcm16Wav(kCtrBig, 4, kSamples), OptionsOf(Flavor::kCtr)); });
    const std::vector<uint8_t> kShort(8, 0);
    const std::string short_ctr = ErrorOf([&] { Wave::Parse(kShort, OptionsOf(Flavor::kCtr)); });
    const std::string short_cafe = ErrorOf([&] { Wave::Parse(kShort, OptionsOf(Flavor::kCafe)); });

    Check(refused == "bad CWAV: unsupported byte order", "a big-endian wave on the 3DS");
    Check(short_ctr == "too short to be a CWAV file" && short_cafe == "too short to be an FWAV file",
          "files that are too short");
}

void TestPcm16WaveDecodes(Flavor flavor)
{
    const Pcm pcm = DecodeWave(Pcm16Wav(FormatOf(flavor), 4, {1, -2, 300, -32768}), OptionsOf(flavor));

    Check(pcm.channels.size() == 1 && !pcm.loop && pcm.Length() == 4 &&
              pcm.channels[0] == std::vector<int16_t>({1, -2, 300, -32768}),
          "a PCM16 wave (" + NameOf(flavor) + ")");
}

void TestWaveLongerThanItsDataIsReported(Flavor flavor)
{
    // Header claims 2^32 - 1 samples; data contains four.
    const std::vector<uint8_t> wav = Pcm16Wav(FormatOf(flavor), 0xffffffff, {1, 2, 3, 4});

    const std::string message = ErrorOf([&] { DecodeWave(wav, OptionsOf(flavor)); });

    Check(message == "wave data shorter than the declared sample count",
          "a wave longer than its data (" + NameOf(flavor) + ")");
}

void TestWaveDecodesItsFirstTwoChannels(Flavor flavor)
{
    // Like nw::snd and nn::atk, ignore channels after the first two, even if the header gives thousands.
    const Pcm pcm = DecodeWave(Pcm16Wav(FormatOf(flavor), 4, {1, -2, 300, -32768}, 3), OptionsOf(flavor));

    Check(pcm.channels.size() == 2 && pcm.channels[1] == std::vector<int16_t>({1, -2, 300, -32768}),
          "a wave's first two channels (" + NameOf(flavor) + ")");
}

void TestBadWaveNamesItsFormat()
{
    const std::vector<uint8_t> kNotAWave = SequenceFile(FormatOf(Flavor::kCtr), {0xff});

    const std::string ctr = ErrorOf([&] { DecodeWave(kNotAWave, OptionsOf(Flavor::kCtr)); });
    const std::string nx = ErrorOf([&] { DecodeWave(kNotAWave, OptionsOf(Flavor::kNx)); });

    Check(ctr == "bad CWAV: bad magic, expected CWAV" && nx == "bad FWAV: bad magic, expected FWAV",
          "a file that isn't a wave");
}

void TestImaAdpcmDecodesOnlyOnThe3ds()
{
    // IMA-ADPCM (encoding 3) from sample 100 and step index 10. Only the 3DS has it. The Wii U and the Switch don't
    // support the encoding.
    const std::vector<uint8_t> kData = {0x17, 0x9f, 0x40, 0x08};
    const std::vector<int16_t> kInfo = {100, 10};

    const Pcm ctr = DecodeWave(CodedWav(FormatOf(Flavor::kCtr), 3, 8, kData, 0x0301, kInfo), OptionsOf(Flavor::kCtr));
    const std::string cafe = ErrorOf(
        [&] { DecodeWave(CodedWav(FormatOf(Flavor::kCafe), 3, 8, kData, 0x0301, kInfo), OptionsOf(Flavor::kCafe)); });
    const std::string nx = ErrorOf(
        [&] { DecodeWave(CodedWav(FormatOf(Flavor::kNx), 3, 8, kData, 0x0301, kInfo), OptionsOf(Flavor::kNx)); });

    Check(ctr.channels.size() == 1 && ctr.channels[0] == wave::DecodeImaAdpcm(kData, 8, 100, 10),
          "IMA-ADPCM on the 3DS");
    Check(cafe == "unknown wave encoding" && nx == "unknown wave encoding", "IMA-ADPCM on the Wii U and the Switch");
}

void TestSarFilesBuildThe3dsFiles()
{
    // sar_files builds a 3DS archive whose sequence plays a bank. The bank's wave is an IMA-ADPCM wave in a wave
    // archive. The sequence sets its channels' priority to 80, with the release priority fix.
    namespace sar_files = ntdlib::testing::sar_files;
    const std::vector<uint8_t> kData = {0x17, 0x9f, 0x40, 0x08};
    const Options options = OptionsOf(Flavor::kCtr);

    sar_files::WaveSpec wave;
    wave.encoding = 3;
    wave.loop_end = 8;
    wave.channels = {kData};
    const std::vector<uint8_t> wave_file = sar_files::Wave(Flavor::kCtr, wave);
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, sar_files::Region{}}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::SoundSpec sequence;
    sequence.name = "SEQ_CTR";
    sequence.banks = {0};
    sequence.channel_priority = 80 | 1 << 8;
    sar_files::ArchiveSpec spec;
    spec.sounds = {sequence};
    spec.banks = {{"BANK_CTR", 1}};
    spec.wave_archives = {{"WARC_CTR", 2}};
    spec.players = {{"PLAYER_CTR", 1}};
    spec.files = {{sar_files::Sequence(Flavor::kCtr, {0xFF}, {{"SEQ_CTR", 0}}), "", false},
                  {sar_files::Bank(Flavor::kCtr, bank), "", false},
                  {sar_files::WaveArchive(Flavor::kCtr, {wave_file}), "", false}};
    const std::vector<uint8_t> file = sar_files::Archive(Flavor::kCtr, spec);

    const SoundArchive archive = SoundArchive::Load(file, options);
    const Bank read_bank = Bank::Parse(archive.FileData(1), options);
    const WaveArchive read_waves = WaveArchive::Parse(archive.FileData(2), options);
    const Pcm pcm = DecodeWave(read_waves.waves.at(0), options);

    Check(Text(std::span(file).first(4)) == "CSAR" && archive.Version() == 0x02030000, "the 3DS's magic and version");
    Check(archive.Sounds().size() == 1 && archive.Sounds()[0].name == "SEQ_CTR" &&
              archive.Sounds()[0].type == SoundType::kSequence && archive.Sounds()[0].banks.size() == 1 &&
              archive.Banks().size() == 1 && archive.Banks()[0].name == "BANK_CTR",
          "the 3DS archive's sound and bank");
    Check(archive.Sounds()[0].channel_priority == 80 && archive.Sounds()[0].release_priority_fix,
          "the 3DS sequence's channel priority");
    Check(read_bank.waves.size() == 1 && read_bank.instruments.size() == 1 && read_bank.instruments[0],
          "the 3DS's bank");
    Check(pcm.channels.size() == 1 && pcm.channels[0] == wave::DecodeImaAdpcm(kData, 8, 100, 10),
          "the 3DS's IMA-ADPCM wave");
}

void TestWaveSoundFilesAreRead(Flavor flavor)
{
    // Two wave sounds: one with its settings, and one without them whose second track has no events. The archive gives
    // each sound's place in the file, in both rules.
    namespace sar_files = ntdlib::testing::sar_files;
    sar_files::Region pitched;
    pitched.wave = 1;
    pitched.original_key = 72;
    sar_files::Region panned;
    panned.pan = 100;
    sar_files::WaveSoundSpec set;
    set.pan = 20;
    set.pitch = 1.5f;
    set.adshr = std::array<uint8_t, 5>{1, 2, 3, 4, 5};
    set.tracks = {{{0.0f, 0.0f, 0}}};
    set.notes = {pitched};
    sar_files::WaveSoundSpec plain;
    plain.tracks = {{{0.0f, 1.0f, 1}, {0.5f, 0.25f, 0}}, {}};
    plain.notes = {sar_files::Region{}, panned};
    sar_files::WaveSoundFileSpec wsd;
    wsd.waves = {{0x05000000, 3}, {0x05000001, 0}};
    wsd.sounds = {set, plain};

    sar_files::SoundSpec first;
    first.name = "WSD_PLAIN";
    first.kind = sar_files::SoundSpec::kWaveSound;
    first.wave_sound_index = 1;
    sar_files::SoundSpec second = first;
    second.name = "WSD_SET";
    second.wave_sound_index = 0;
    sar_files::ArchiveSpec spec;
    spec.sounds = {first, second};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::WaveSoundFile(flavor, wsd), "", false}};
    const std::vector<uint8_t> bytes = sar_files::Archive(flavor, spec);

    const SoundArchive lenient = SoundArchive::Load(bytes, OptionsOf(flavor));
    const SoundArchive strict = SoundArchive::Load(bytes, StrictOptionsOf(flavor));
    const WaveSoundFile read = WaveSoundFile::Parse(lenient.FileData(0), OptionsOf(flavor));
    const WaveSoundFile read_strictly = WaveSoundFile::Parse(strict.FileData(0), StrictOptionsOf(flavor));

    const std::string console = NameOf(flavor);
    for (const SoundArchive* archive : {&lenient, &strict})
    {
        Check(archive->Sounds().size() == 2 && archive->Sounds()[0].type == SoundType::kWaveSound &&
                  archive->Sounds()[0].wave_sound_index == 1 && archive->Sounds()[1].wave_sound_index == 0,
              console + ": the wave sounds' places in their file");
    }
    Check(read.waves.size() == 2 && read.waves[0].wave_archive_item == 0x05000000 && read.waves[0].index == 3 &&
              read.waves[1].wave_archive_item == 0x05000001 && read.waves[1].index == 0,
          console + ": the wave sound file's waves");
    Check(read.sounds.size() == 2, console + ": the wave sound file's sounds");
    if (read.sounds.size() == 2)
    {
        const WaveSoundFile::WaveSound& a = read.sounds[0];
        const WaveSoundFile::WaveSound& b = read.sounds[1];
        Check(a.pan == 20 && a.pitch == 1.5f && a.adshr.attack == 1 && a.adshr.decay == 2 && a.adshr.sustain == 3 &&
                  a.adshr.hold == 4 && a.adshr.release == 5,
              console + ": a wave sound's settings");
        Check(a.tracks.size() == 1 && a.tracks[0].size() == 1 && a.tracks[0][0].note == 0 && a.notes.size() == 1 &&
                  a.notes[0].wave_id_index == 1 && a.notes[0].original_key == 72,
              console + ": a wave sound's track and note");
        Check(b.pan == 64 && b.pitch == 1.0f && b.adshr.attack == 127 && b.adshr.release == 127,
              console + ": a wave sound's default settings");
        Check(b.tracks.size() == 2 && b.tracks[0].size() == 2 && b.tracks[1].empty() && b.tracks[0][0].length == 1.0f &&
                  b.tracks[0][0].note == 1 && b.tracks[0][1].position == 0.5f && b.tracks[0][1].length == 0.25f &&
                  b.tracks[0][1].note == 0,
              console + ": a wave sound's note events");
        Check(b.notes.size() == 2 && b.notes[0].wave_id_index == 0 && b.notes[0].pan == 64 && b.notes[1].pan == 100 &&
                  b.notes[1].original_key == 60,
              console + ": a wave sound's notes");
    }
    Check(read_strictly.sounds.size() == read.sounds.size() && read_strictly.waves.size() == read.waves.size(),
          console + ": a wave sound file read strictly");
    Check(!ErrorOf([&] { WaveSoundFile::Parse(sar_files::Bank(flavor, {}), OptionsOf(flavor)); }).empty(),
          console + ": a bank isn't a wave sound file");
}

void TestWaveSoundFileOfSharedTablesIsReported()
{
    // One wave sound whose table of tracks names the same track 100 times. That track's table of events names the same
    // event 100 times. The 10,100 entries outnumber the file's bytes.
    namespace sar_files = ntdlib::testing::sar_files;
    sar_files::Bytes info(false);
    info.Add(16);
    const std::size_t waves = info.Add(4);
    info.Ref(0, 0x0100, waves, 0);
    const std::size_t sounds = info.Add(12);
    info.Ref(8, 0x0101, sounds, 0);
    info.U32(sounds, 1);
    const std::size_t data = info.Add(24);
    info.Ref(sounds + 4, 0x4900, data, sounds);
    const std::size_t settings = info.Options({});
    info.Ref(data, 0x4901, settings, data);
    const std::size_t tracks = info.Add(4 + 8 * 100);
    info.Ref(data + 8, 0x0101, tracks, data);
    info.U32(tracks, 100);
    const std::size_t track = info.Add(8);
    const std::size_t events = info.Add(4 + 8 * 100);
    info.Ref(track, 0x0101, events, track);
    info.U32(events, 100);
    const std::size_t event = info.Add(16);
    for (std::size_t i = 0; i < 100; ++i)
    {
        info.Ref(tracks + 4 + 8 * i, 0x4903, track, tracks);
        info.Ref(events + 4 + 8 * i, 0x4904, event, events);
    }
    const std::size_t notes = info.Add(4);
    info.Ref(data + 16, 0x0101, notes, data);
    const std::vector<uint8_t> file = sar_files::File(false, "FWSD", 0x00010100, {{0x6800, "INFO", info.data}});

    const std::string message = ErrorOf([&] { WaveSoundFile::Parse(file, OptionsOf(Flavor::kNx)); });

    Check(file.size() < 10100 && message.find("more entries than bytes") != std::string::npos,
          "a wave sound file of shared tables: " + message);
}

void TestWaveSoundFileWithoutTablesIsReported()
{
    // An INFO block whose two references are empty.
    namespace sar_files = ntdlib::testing::sar_files;
    sar_files::Bytes info(false);
    info.Add(16);
    const std::vector<uint8_t> file = sar_files::File(false, "FWSD", 0x00010100, {{0x6800, "INFO", info.data}});

    const std::string message = ErrorOf([&] { WaveSoundFile::Parse(file, OptionsOf(Flavor::kNx)); });

    Check(message.find("without its tables") != std::string::npos, "a wave sound file without its tables: " + message);
}

void TestDspAdpcmSumsAsTheConsoleDoes()
{
    // A frame at the largest scale, after two full-scale samples, with both coefficients at their largest. The sum
    // passes 32 bits. The 3DS sums in 64 bits. AX on the Wii U and the Switch's renderer wrap at 32 bits.
    const std::vector<uint8_t> kData = {0x0f, 0x77, 0x77};
    std::vector<int16_t> info(19, 0);
    info[0] = 32767;
    info[1] = 32767;
    info[17] = 32767; // yn1
    info[18] = 32767; // yn2
    std::array<int16_t, 16> coefs{};
    coefs[0] = coefs[1] = 32767;
    const std::vector<int16_t> wide = wave::DecodeDspAdpcm(kData, 4, coefs, 32767, 32767, wave::DspAdpcmSum::kWide);
    const std::vector<int16_t> wrap = wave::DecodeDspAdpcm(kData, 4, coefs, 32767, 32767, wave::DspAdpcmSum::kWrap);
    Check(wide != wrap, "the two sums differ on this frame");

    for (const Flavor flavor : kFlavors)
    {
        const Pcm pcm = DecodeWave(CodedWav(FormatOf(flavor), 2, 4, kData, 0x0300, info), OptionsOf(flavor));

        Check(pcm.channels.size() == 1 && pcm.channels[0] == (flavor == Flavor::kCtr ? wide : wrap),
              "DSP-ADPCM's sum (" + NameOf(flavor) + ")");
    }
}

void TestStrictRulesRefuseOtherArchivesInTheirWords()
{
    // The 3DS's player checks no size first. Its reads past the end say so in its words.
    auto message_for = [](std::vector<uint8_t> bytes, Flavor flavor)
    {
        return ErrorOf([&] { SoundArchive::Load(std::move(bytes), StrictOptionsOf(flavor)); });
    };
    auto with_magic = [](std::vector<uint8_t> bytes, const char* magic)
    {
        std::memcpy(bytes.data(), magic, 4);
        return bytes;
    };
    const std::vector<uint8_t> ctr = Archive(FormatOf(Flavor::kCtr), {});
    const std::vector<uint8_t> cafe = Archive(FormatOf(Flavor::kCafe), {});
    const std::vector<uint8_t> nx = Archive(FormatOf(Flavor::kNx), {});

    Check(message_for(cafe, Flavor::kCtr) == "a Wii U or Switch sound archive (FSAR), not a 3DS one (CSAR)",
          "a Wii U archive (kStopAtFirstProblem, 3DS)");
    Check(message_for(with_magic(ctr, "RSAR"), Flavor::kCtr) == "bad magic, expected CSAR",
          "a Wii archive (kStopAtFirstProblem, 3DS)");
    Check(message_for(Archive({'C', true, 0x02030000}, {}), Flavor::kCtr) == "unsupported byte order",
          "a big-endian archive (kStopAtFirstProblem, 3DS)");
    Check(message_for({'C', 'S'}, Flavor::kCtr) == "the data ends before offset 0 (the file is cut short or damaged)",
          "two bytes (kStopAtFirstProblem, 3DS)");
    Check(message_for(with_magic(cafe, "CSAR"), Flavor::kCafe) == "a 3DS sound archive (CSAR), not a Wii U one",
          "a 3DS archive (kStopAtFirstProblem, Wii U)");
    Check(message_for(with_magic(cafe, "RSAR"), Flavor::kCafe) == "a Wii sound archive (RSAR), not a Wii U one",
          "a Wii archive (kStopAtFirstProblem, Wii U)");
    Check(message_for(with_magic(nx, "CSAR"), Flavor::kNx) == "a 3DS sound archive (CSAR), not a Switch one",
          "a 3DS archive (kStopAtFirstProblem, Switch)");
    Check(message_for(with_magic(nx, "RSAR"), Flavor::kNx) == "a Wii sound archive (RSAR), not a Switch one",
          "a Wii archive (kStopAtFirstProblem, Switch)");
    Check(message_for({'F', 'S', 'A', 'R'}, Flavor::kCafe) == "too short for FSAR",
          "four bytes (kStopAtFirstProblem, Wii U)");
    Check(message_for(nx, Flavor::kCafe).empty() && message_for(cafe, Flavor::kNx).empty(),
          "kStopAtFirstProblem reads either byte order");
}

void TestStrictRulesReportMissingBlocksTheirWay()
{
    // A wave whose DATA block has another type, an archive whose INFO block has another type, and an archive whose
    // table of sounds has another type. Each one throws FormatError. The message doesn't depend on the standard
    // library. DecodeWave adds the format's name to the message of a FormatError.
    std::vector<uint8_t> wav[3], sar[3], no_sounds[3];
    for (const Flavor flavor : kFlavors)
    {
        const auto i = static_cast<std::size_t>(flavor);
        Writer w{Pcm16Wav(FormatOf(flavor), 4, {1, 2, 3, 4}), FormatOf(flavor).big_endian};
        w.U16(0x14 + 12, 0x7009);
        wav[i] = w.bytes;
        Writer a{Archive(FormatOf(flavor), {}), FormatOf(flavor).big_endian};
        a.U16(0x14, 0x2009);
        sar[i] = a.bytes;
        Writer t{Archive(FormatOf(flavor), {}), FormatOf(flavor).big_endian};
        t.U16(0x48, 0x2109); // the INFO block's first reference: the table of sounds
        no_sounds[i] = t.bytes;
    }

    const std::string ctr_wave = ErrorOf([&] { Wave::Parse(wav[0], StrictOptionsOf(Flavor::kCtr)); });
    const std::string ctr_archive = ErrorOf([&] { SoundArchive::Load(sar[0], StrictOptionsOf(Flavor::kCtr)); });
    const std::string cafe_wave = ErrorOf([&] { Wave::Parse(wav[1], StrictOptionsOf(Flavor::kCafe)); });
    const std::string nx_decoded = ErrorOf([&] { DecodeWave(wav[2], StrictOptionsOf(Flavor::kNx)); });
    const std::string nx_archive = ErrorOf([&] { SoundArchive::Load(sar[2], StrictOptionsOf(Flavor::kNx)); });
    const std::string lenient_wave = ErrorOf([&] { Wave::Parse(wav[1], OptionsOf(Flavor::kCafe)); });

    Check(ctr_wave == "a wave (CWAV) has no DATA block", "a wave without DATA (kStopAtFirstProblem, 3DS)");
    Check(ctr_archive == "the sound archive has no INFO block", "an archive without INFO (kStopAtFirstProblem, 3DS)");
    Check(cafe_wave == "no DATA block", "a wave without DATA (kStopAtFirstProblem, Wii U): " + cafe_wave);
    Check(nx_decoded == "bad FWAV: no DATA block", "DecodeWave of a wave without DATA (kStopAtFirstProblem, Switch)");
    Check(nx_archive == "no INFO or FILE block", "an archive without INFO (kStopAtFirstProblem, Switch)");
    Check(lenient_wave == "no DATA block", "a wave without DATA (kReadPastDamage)");
    for (const Flavor flavor : kFlavors)
    {
        const std::string message =
            ErrorOf([&] { SoundArchive::Load(no_sounds[static_cast<std::size_t>(flavor)], StrictOptionsOf(flavor)); });
        Check(message == "the sound archive's INFO block has no table of type 0x2100",
              "an archive without a table of sounds (kStopAtFirstProblem, " + NameOf(flavor) + "): " + message);
    }
}

void TestStrictRulesKeepTheArchivesNames(Flavor flavor)
{
    // kStopAtFirstProblem keeps a name's control characters, and leaves an unnamed bank unnamed.
    const Format f = FormatOf(flavor);
    std::vector<ArchiveFile> files(2);
    files[0].data = SequenceFile(f, {0xff});
    files[1].data = DirectBank(f);
    const std::vector<uint8_t> bytes =
        Archive(f, files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .names = {{0x01000000, "Seq\n1"}}});

    const SoundArchive strict = SoundArchive::Load(bytes, StrictOptionsOf(flavor));
    const SoundArchive lenient = SoundArchive::Load(bytes, OptionsOf(flavor));

    const std::string on = " (" + NameOf(flavor) + ")";
    Check(strict.Sounds()[0].name == "Seq\n1" && lenient.Sounds()[0].name == "Seq?1", "a name's line break" + on);
    Check(strict.Banks()[0].name.empty() && lenient.Banks()[0].name == "BANK_0", "an unnamed bank" + on);
    Check(strict.FindSound("Seq\n1") == 0u && !strict.FindSound("SEQ_0"), "FindSound" + on);
}

void TestStrictRulesCountRegionsAsTheyComeToThem(Flavor flavor)
{
    const std::vector<uint8_t> bank = SharedRegionsBank(FormatOf(flavor), 2);

    const std::string message = ErrorOf([&] { Bank::Parse(bank, StrictOptionsOf(flavor)); });

    Check(message == "the bank has more regions than bytes",
          "a bank of shared regions with kStopAtFirstProblem (" + NameOf(flavor) + ")");
}

void TestKeyBoundsFollowTheRules(Flavor flavor)
{
    // A direct key table reaches any key with kStopAtFirstProblem, and 127 with kReadPastDamage. A range table's next
    // bound after 255 wraps to 0 with kReadPastDamage only.
    const Format f = FormatOf(flavor);

    const Bank direct_strict = Bank::Parse(DirectBank(f), StrictOptionsOf(flavor));
    const Bank direct_lenient = Bank::Parse(DirectBank(f), OptionsOf(flavor));
    const Bank range_strict = Bank::Parse(RangeBank(f, {255, 10}), StrictOptionsOf(flavor));
    const Bank range_lenient = Bank::Parse(RangeBank(f, {255, 10}), OptionsOf(flavor));

    const std::string on = " (" + NameOf(flavor) + ")";
    const KeyRegion& strict_key = direct_strict.instruments[0]->keys[0];
    const KeyRegion& lenient_key = direct_lenient.instruments[0]->keys[0];
    Check(strict_key.hi == KeyRegion::kAnyValue && lenient_key.hi == 127, "a direct key table" + on);
    Check(strict_key.velocities[0].hi == KeyRegion::kAnyValue && lenient_key.velocities[0].hi == KeyRegion::kAnyValue,
          "a direct velocity table" + on);
    Check(range_strict.instruments[0]->keys[1].lo == 256 && range_lenient.instruments[0]->keys[1].lo == 0,
          "a range table's bound after 255" + on);
}

void TestStrictRulesReadTheirFields()
{
    // On the Wii U and the Switch, kStopAtFirstProblem reads labels and external paths. On every console, it reads the
    // ADPCM state at the loop, and takes a file past the archive's end as truncation.
    const Format cafe = FormatOf(Flavor::kCafe);
    const std::vector<uint8_t> sequence = LabelledSequenceFile(cafe, {0xff, 0xff}, {{"SEQ_MAIN", 1}});
    std::vector<int16_t> info(22, 0);
    info[16] = 0x34;
    info[19] = 0x56;
    info[20] = -7;
    info[21] = 9;
    const std::vector<uint8_t> wave = CodedWav(cafe, 2, 14, std::vector<uint8_t>(8, 0), 0x0300, info);
    std::vector<ArchiveFile> files(3);
    files[0].data = Bytes("stored");
    files[0].extra_size = 1000000;
    files[1].place = ArchiveFile::kExternal;
    files[1].path = "stream/BGM.bfstm";
    files[2].data = Group(cafe, {}, {});

    const Sequence labelled = Sequence::Parse(sequence, StrictOptionsOf(Flavor::kCafe));
    const Sequence plain = Sequence::Parse(sequence, OptionsOf(Flavor::kCafe));
    const Wave strict_wave = Wave::Parse(wave, StrictOptionsOf(Flavor::kCafe));
    const Wave lenient_wave = Wave::Parse(wave, OptionsOf(Flavor::kCafe));
    const SoundArchive archive =
        SoundArchive::Load(Archive(cafe, files, {.groups = {2, 0xffffffff}}), StrictOptionsOf(Flavor::kCafe));

    Check(labelled.labels.size() == 1 && labelled.labels[0].name == "SEQ_MAIN" && labelled.labels[0].offset == 1 &&
              plain.labels.empty(),
          "a sequence's labels");
    const DspAdpcmInfo& adpcm = *strict_wave.channels[0].adpcm;
    Check(adpcm.pred_scale == 0x34 && adpcm.loop_pred_scale == 0x56 && adpcm.loop_yn1 == -7 && adpcm.loop_yn2 == 9 &&
              lenient_wave.channels[0].adpcm->loop_pred_scale == 0,
          "the ADPCM state at the loop");
    Check(archive.Truncated() && archive.FileData(0).empty() && archive.FileCount() == 3, "a file past the end");
    Check(archive.ExternalPath(1) == "stream/BGM.bfstm" && archive.ExternalPath(0).empty(), "an external path");
    Check(archive.Groups().size() == 2 && archive.Groups()[1].file_id == 0xffffffff && archive.Version() == 0x00020200,
          "the groups and the version");
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestFilesKeptInGroupsAreFound(flavor);
        TestBrokenGroupIsReported(flavor);
        TestBrokenGroupWhoseFilesAreElsewhereIsntReported(flavor);
        TestGroupPastTheEndLeavesItsFilesMissing(flavor);
        TestFilePastTheEndOfItsGroupIsReported(flavor);
        TestFileCutShortInOneGroupIsTakenFromAnother(flavor);
        TestFilePastTheEndOfAWholeArchiveIsntTruncation(flavor);
        TestDamagedNameTableLeavesSoundsNumbered(flavor);
        TestTruncatedArchiveWhoseTablesCantBeReadIsReported(flavor);
        TestUnreadableGroupTableIsReported(flavor);
        TestGroupTableFirstLocatesNoneOfADamagedGroup(flavor);
        TestNamesHaveNoControlCharacters(flavor);
        TestBankOfSharedRegionsIsReported(flavor);
        TestGroupFilesBesideTheArchiveAreAdded(flavor);
        TestGroupFilesMatchTheirArchive(flavor, Rules::kReadPastDamage);
        TestGroupFilesMatchTheirArchive(flavor, Rules::kStopAtFirstProblem);
        TestPcm16WaveDecodes(flavor);
        TestWaveLongerThanItsDataIsReported(flavor);
        TestWaveDecodesItsFirstTwoChannels(flavor);
        TestStrictRulesKeepTheArchivesNames(flavor);
        TestStrictRulesCountRegionsAsTheyComeToThem(flavor);
        TestKeyBoundsFollowTheRules(flavor);
        TestWaveSoundFilesAreRead(flavor);
    }

    // Only the Wii U's and the Switch's readers list the groups.
    TestGroupsListTheFilesTheyLoad(Flavor::kCafe);
    TestGroupsListTheFilesTheyLoad(Flavor::kNx);
    TestArchivesOfOtherConsolesAreRefused();
    TestSarcFilesAreRead();
    TestSarcNamesAreRead();
    TestByteOrderMarksAreRead();
    TestBadWaveNamesItsFormat();
    TestImaAdpcmDecodesOnlyOnThe3ds();
    TestSarFilesBuildThe3dsFiles();
    TestDspAdpcmSumsAsTheConsoleDoes();
    TestWaveSoundFileOfSharedTablesIsReported();
    TestWaveSoundFileWithoutTablesIsReported();
    TestStrictRulesRefuseOtherArchivesInTheirWords();
    TestStrictRulesReportMissingBlocksTheirWay();
    TestStrictRulesReadTheirFields();

    return xsflib::testing::Summary("sar");
}
