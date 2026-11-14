// SPDX-License-Identifier: MIT

// Sound archive and file readers (see formats.h).

#include "ntdlib/rsar/formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/utf8.h"

#include "ntdlib/rsar/reference.h"
#include "rsar/wave_infos.h"

namespace ntdlib::rsar
{
namespace
{

using detail::SampleAt;

// The longest name read from the string table.
constexpr std::size_t kMaxName = 1024;

// The place `offset` bytes after `base`, or the largest place when a 32-bit build can't hold the sum. A damaged offset
// then points past the end of the archive in every build, as it does in a 64-bit build, in place of wrapping round to a
// place inside it.
std::size_t PlaceAfter(std::size_t base, uint32_t offset)
{
    constexpr std::size_t kLast = std::numeric_limits<std::size_t>::max();

    return offset <= kLast - base ? base + offset : kLast;
}

// A place where the archive keeps a file: a group, and an item of the group.
struct FilePosition
{
    uint32_t group;
    uint32_t index;
};

bool Short(const ReadOptions& options)
{
    return options.wording == Wording::kShort;
}

// A big-endian reader of `data` with the wording's text for a read past the end. The reader checks a string's offset
// with either wording.
xsflib::Reader MakeReader(std::span<const uint8_t> data, const ReadOptions& options)
{
    xsflib::ReaderOptions reader;
    reader.bounds_message = Short(options) ? xsflib::BoundsMessage::kOutOfBounds : xsflib::BoundsMessage::kRunsPastEnd;
    reader.check_string_offset = true;

    return xsflib::Reader(data, true, reader);
}

// True if the 4 bytes at `off` are `magic`. With the short texts, a block past the end throws. With the sentences, it
// isn't there.
bool IsMagic(const xsflib::Reader& r, std::size_t off, const char* magic, const ReadOptions& options)
{
    if (!Short(options) && (off > r.Size() || r.Size() - off < 4))
    {
        return false;
    }

    return r.Magic(off, magic);
}

// Checks the file magic (RSAR, RWAV, etc.) and the Wii's byte order, and returns the file's version.
uint16_t CheckHeader(const xsflib::Reader& r, const char* magic, const ReadOptions& options)
{
    if (!Short(options))
    {
        if (!IsMagic(r, 0, magic, options))
        {
            throw xsflib::FormatError(std::string("not an ") + magic + " file");
        }
        if (r.U16(4) != 0xFEFF)
        {
            throw xsflib::FormatError(std::string("the ") + magic + " file isn't big-endian (Wii files are)");
        }

        return r.U16(6);
    }

    if (r.Size() < 0x10)
    {
        throw xsflib::FormatError(std::string("too short to be an ") + magic + " file");
    }
    if (!r.Magic(0, magic))
    {
        throw xsflib::FormatError(std::string("bad magic, expected ") + magic);
    }
    if (r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("unsupported byte order");
    }

    return r.U16(6);
}

// Reads a wave info at `offset` whose channels' sample offsets count from `channel_base`.
Wave ReadWaveInfo(const xsflib::Reader& info, std::size_t offset, std::span<const uint8_t> channel_base,
                  const ReadOptions& options)
{
    Wave w;
    const uint8_t encoding = info.U8(offset);
    if (encoding > 2)
    {
        throw xsflib::FormatError(Short(options) ? "unknown wave encoding " + std::to_string(encoding)
                                                 : "a wave has an unknown format (" + std::to_string(encoding) + ")");
    }

    // The loop start is the first sample's address, and the end is the last sample's address. ReadWaveParam (main.dol
    // 0x8004eca8) makes the end one past it.
    w.encoding = static_cast<WaveEncoding>(encoding);
    w.loop = info.U8(offset + 1) != 0;
    const uint8_t channels = info.U8(offset + 2);
    w.sample_rate = detail::ReadRate(info, offset + 3);
    w.loop_start_address = info.U32(offset + 8);
    w.loop_end_address = info.U32(offset + 0xC);
    w.loop_start = SampleAt(w.loop_start_address, w.encoding);
    w.loop_end = SampleAt(w.loop_end_address, w.encoding) + 1;

    // Each channel: its samples' offset, its ADPCM info's offset from the wave info (0 for none) and its volumes.
    const std::size_t table = offset + info.U32(offset + 0x10);
    for (uint8_t c = 0; c < channels; c++)
    {
        const std::size_t channel = offset + info.U32(table + c * 4u);
        const uint32_t samples = info.U32(channel);
        if (samples > channel_base.size())
        {
            throw xsflib::FormatError(Short(options)
                                          ? "wave channel " + std::to_string(c) + " starts past the wave data"
                                          : std::string("a wave's samples lie outside its wave data"));
        }

        WaveChannel ch;
        ch.data = channel_base.subspan(samples);
        if (options.playback_fields)
        {
            for (int v = 0; v < 4; v++)
            {
                ch.volumes[v] = info.U32(channel + 8 + v * 4u);
            }
        }

        // With playback_fields, every channel's whole ADPCM context is read. Without it, only what a decoder of
        // DSP-ADPCM waves uses is read.
        const uint32_t adpcm = info.U32(channel + 4);
        if (adpcm != 0 && (options.playback_fields || w.encoding == WaveEncoding::kDspAdpcm))
        {
            const std::size_t a = offset + adpcm;
            DspAdpcmInfo d;
            for (int k = 0; k < 16; k++)
            {
                d.coefs[k] = info.S16(a + k * 2u);
            }

            if (options.playback_fields)
            {
                d.gain = info.U16(a + 0x20);
                d.pred_scale = info.U16(a + 0x22);
            }
            d.yn1 = info.S16(a + 0x24);
            d.yn2 = info.S16(a + 0x26);
            if (options.playback_fields)
            {
                d.loop_pred_scale = info.U16(a + 0x28);
                d.loop_yn1 = info.S16(a + 0x2A);
                d.loop_yn2 = info.S16(a + 0x2C);
            }
            ch.adpcm = d;
        }

        w.channels.push_back(ch);
    }

    return w;
}

// Reads the SYMB block's string table at `symb`: a table of offsets, from the block's payload, to the names. Items
// refer to the names by index.
std::vector<std::string> ReadStrings(const xsflib::Reader& r, std::size_t symb)
{
    const std::size_t base = symb + 8;
    const std::size_t table = PlaceAfter(base, r.U32(base));
    const uint32_t count = r.U32(table);

    // A name takes a byte at least. A table of more entries than the archive has bytes is damaged.
    if (count > r.Size())
    {
        throw xsflib::FormatError("invalid string table");
    }

    std::vector<std::string> strings;
    for (uint32_t i = 0; i < count; i++)
    {
        strings.push_back(xsflib::PrintableName(r.CString(PlaceAfter(base, r.U32(table + 4 + i * 4)), kMaxName)));
    }

    return strings;
}

// The names of an archive's items, from the string table of the SYMB block at `symb`. Archives without names have no
// SYMB block. With printable names, the table is read whole, and a damaged table leaves the items unnamed without
// stopping the archive. Otherwise each name is read when an item requires it.
class Names
{
public:
    Names(const xsflib::Reader& r, uint32_t symb, uint32_t symb_size, const ReadOptions& options)
        : r_(r), base_(symb + std::size_t{8}), printable_(options.printable_names)
    {
        if (symb == 0 || symb_size == 0 || symb >= r.Size() || r.Size() - symb < 4 ||
            !IsMagic(r, symb, "SYMB", options))
        {
            return;
        }

        if (printable_)
        {
            try
            {
                strings_ = ReadStrings(r, symb);
            }
            catch (const xsflib::FormatError&)
            {
                damaged_ = true;
            }

            return;
        }

        const std::size_t table = PlaceAfter(base_, r.U32(base_));
        const uint32_t count = r.U32(table);
        for (uint32_t i = 0; i < count; i++)
        {
            offsets_.push_back(r.U32(table + 4 + i * std::size_t{4}));
        }
    }

    // The name of string `string_id`, or nothing for an item that has none. A printable name that's empty counts as
    // none.
    std::optional<std::string> Of(uint32_t string_id) const
    {
        if (printable_)
        {
            return string_id < strings_.size() && !strings_[string_id].empty() ? std::optional(strings_[string_id])
                                                                               : std::nullopt;
        }

        return string_id < offsets_.size() ? std::optional(r_.CString(PlaceAfter(base_, offsets_[string_id]), kMaxName))
                                           : std::nullopt;
    }

    // Whether the table of printable names is damaged.
    bool Damaged() const
    {
        return damaged_;
    }

private:
    const xsflib::Reader& r_;
    std::size_t base_;                 // the SYMB block's payload
    bool printable_;                   // whether the names are read whole: printable names
    std::vector<std::string> strings_; // the printable names
    std::vector<uint32_t> offsets_;    // without printable names, each name's offset from the payload
    bool damaged_ = false;
};

// INFO's tables, read through `r`. INFO starts with references, from its payload at `base`, to the sound, bank, player,
// file and group tables. Each table is a count and a reference to each entry.
struct InfoTables
{
    // Calls `read(index, place, entry)` for each entry of the table whose reference is at `table_ref`. With the
    // sentences, a table that isn't there reads as empty. With the short texts, it throws.
    template <typename Read>
    void ForEachEntry(std::size_t table_ref, const char* name, Read read) const
    {
        const Reference ref = ReadRef(r, table_ref);
        if (!ref.IsOffset())
        {
            if (!Short(options))
            {
                return;
            }

            throw xsflib::FormatError(std::string("no ") + name + " table");
        }

        const std::size_t t = base + ref.offset;
        const uint32_t n = r.U32(t);
        for (uint32_t i = 0; i < n; i++)
        {
            const Reference entry = ReadRef(r, t + 4 + i * std::size_t{8});
            read(i, base + entry.offset, entry);
        }
    }

    const xsflib::Reader& r;
    std::size_t base;
    const ReadOptions& options;
    uint16_t version;
    const Names& names;
};

// The sound of entry `i` of the sound table, at `p` (SoundArchiveFileReader::ReadSoundInfo, main.dol 0x800425a8, and
// ReadSeqSoundInfo, 0x800427cc). Before version 1.1 a data type gives the sound's type; from 1.1 a field does. Pan mode
// and curve came with version 1.2.
SoundInfo ReadSound(const InfoTables& tables, uint32_t i, std::size_t p, const Reference& entry)
{
    const xsflib::Reader& r = tables.r;
    const ReadOptions& options = tables.options;
    SoundInfo s;
    if (options.playback_fields)
    {
        s.bank = 0;
        s.allocate_track_flags = 0;
    }

    s.file_id = r.U32(p + 4);
    s.volume = r.U8(p + 0x14);
    const Reference details = ReadRef(r, p + 0x18);
    const uint8_t old_type = options.type_from_detail ? details.data_type : entry.data_type;
    const uint8_t type = tables.version >= 0x0101 ? r.U8(p + 0x16) : old_type;
    s.type = type == 1   ? SoundType::kSequence
             : type == 2 ? SoundType::kStream
             : type == 3 ? SoundType::kWaveSound
                         : SoundType::kUnknown;
    if (options.playback_fields)
    {
        s.player_id = r.U32(p + 8);
        s.player_priority = r.U8(p + 0x15);
        s.remote_filter = r.U8(p + 0x17);
        s.user_params[0] = r.U32(p + 0x20);
        s.user_params[1] = r.U32(p + 0x24);
    }
    if (tables.version >= 0x0102)
    {
        s.pan_mode = r.U8(p + 0x28);
        s.pan_curve = r.U8(p + 0x29);
    }

    const std::size_t q = tables.base + details.offset;
    if (s.type == SoundType::kSequence && details.IsOffset())
    {
        s.start_offset = r.U32(q);
        s.bank = r.U32(q + 4);
        s.allocate_track_flags = r.U32(q + 8);
        if (options.playback_fields)
        {
            s.channel_priority = r.U8(q + 0xC);
            s.release_priority_fix = tables.version >= 0x0103 && r.U8(q + 0xD) != 0;
        }
    }
    else if (s.type == SoundType::kWaveSound && details.IsOffset() && options.playback_fields)
    {
        s.sub_index = r.S32(q);
        s.allocate_track_flags = r.U32(q + 4);
        s.channel_priority = r.U8(q + 8);
        s.release_priority_fix = tables.version >= 0x0103 && r.U8(q + 9) != 0;
    }
    else if (s.type == SoundType::kStream && details.IsOffset() && options.playback_fields)
    {
        s.allocate_channels = r.U16(q + 4);
        s.allocate_track_flags = r.U16(q + 6);
    }

    // Generate a name from the sound's type and index if the archive has none.
    const char* prefix = s.type == SoundType::kSequence    ? "SEQ_"
                         : s.type == SoundType::kStream    ? "STRM_"
                         : s.type == SoundType::kWaveSound ? "WSD_"
                                                           : "SOUND_";
    s.name = tables.names.Of(r.U32(p)).value_or(prefix + std::to_string(i));

    return s;
}

// The bank of entry `i` of the bank table, at `p`. A bank without a name takes BANK_<index>.
BankInfo ReadBank(const InfoTables& tables, uint32_t i, std::size_t p)
{
    const std::string name = tables.names.Of(tables.r.U32(p)).value_or("BANK_" + std::to_string(i));

    return {name, tables.r.U32(p + 4)};
}

// The player of entry `i` of the player table, at `p`. A player without a name takes PLAYER_<index>.
PlayerInfo ReadPlayer(const InfoTables& tables, uint32_t i, std::size_t p)
{
    PlayerInfo player;
    player.sound_count = tables.r.U8(p + 4);
    player.heap_size = tables.r.U32(p + 8);
    player.name = tables.names.Of(tables.r.U32(p)).value_or("PLAYER_" + std::to_string(i));

    return player;
}

// The file table: for each file, the places in groups where the archive keeps it, a group and an item of the group. A
// file beside the archive has its path instead (SoundArchive::detail_OpenFileStream, main.dol 0x80041e60).
struct FileTable
{
    std::vector<std::vector<FilePosition>> positions; // by file id
    std::vector<std::optional<std::string>> paths;
};

FileTable ReadFileTable(const InfoTables& tables)
{
    const xsflib::Reader& r = tables.r;
    FileTable file_table;
    const auto read_file = [&](uint32_t, std::size_t p, const Reference&)
    {
        const Reference path = ReadRef(r, p + 0xC);
        file_table.paths.push_back(path.IsOffset() ? std::optional(r.CString(tables.base + path.offset, kMaxName))
                                                   : std::nullopt);
        std::vector<FilePosition>& places = file_table.positions.emplace_back();
        const auto read_position = [&](uint32_t, std::size_t q, const Reference&)
        {
            places.push_back({r.U32(q), r.U32(q + 4)});
        };
        tables.ForEachEntry(p + 0x14, "file position", read_position);
    };
    tables.ForEachEntry(tables.base + 0x18, "file", read_file);

    return file_table;
}

// Where a group holds its files, and after them their wave data, and the places of its items' entries. A group with
// an external file path is kept outside the archive (SoundArchiveFileReader::ReadGroupInfo, main.dol 0x80042ac4, and
// ReadGroupItemInfo, 0x80042bd4).
struct GroupPlaces
{
    bool internal = false;
    uint64_t offset = 0, wave_data_offset = 0;
    std::vector<std::size_t> items; // places of the items' entries
};

// The groups' places. Playback also keeps each group's sizes and items in `groups`. A group that ends past the archive
// sets `truncated`.
std::vector<GroupPlaces> ReadGroups(const InfoTables& tables, std::vector<SoundArchive::Group>& groups, bool& truncated)
{
    const xsflib::Reader& r = tables.r;
    std::vector<GroupPlaces> places;
    const auto read_group = [&](uint32_t, std::size_t p, const Reference&)
    {
        GroupPlaces& g = places.emplace_back();
        g.internal = !ReadRef(r, p + 8).IsOffset();
        g.offset = r.U32(p + 0x10);
        g.wave_data_offset = r.U32(p + 0x18);
        const auto read_item = [&](uint32_t, std::size_t q, const Reference&)
        {
            g.items.push_back(q);
        };
        tables.ForEachEntry(p + 0x20, "group item", read_item);
        if (!tables.options.playback_fields)
        {
            return;
        }

        SoundArchive::Group group;
        group.internal = g.internal;
        group.offset = r.U32(p + 0x10);
        group.size = r.U32(p + 0x14);
        group.wave_data_offset = r.U32(p + 0x18);
        group.wave_data_size = r.U32(p + 0x1C);
        for (const std::size_t q : g.items)
        {
            group.items.push_back({r.U32(q), r.U32(q + 4), r.U32(q + 8), r.U32(q + 0xC), r.U32(q + 0x10)});
        }

        const uint64_t end =
            std::max(uint64_t{group.offset} + group.size, uint64_t{group.wave_data_offset} + group.wave_data_size);
        if (group.internal && end > r.Size())
        {
            truncated = true;
        }

        groups.push_back(std::move(group));
    };
    tables.ForEachEntry(tables.base + 0x20, "group", read_group);

    return places;
}

// The region of a bank of `version` at `at`, as BankFileReader::ReadInstInfo reads it. A bank older than version 1.1
// has no volume or tune.
Region ReadRegion(const xsflib::Reader& r, std::size_t at, uint16_t version, const ReadOptions& options)
{
    Region region;
    region.wave_index = r.S32(at);
    region.attack = r.U8(at + 4);
    region.decay = r.U8(at + 5);
    region.sustain = r.U8(at + 6);
    region.release = r.U8(at + 7);
    region.hold = r.U8(at + 8);
    if (options.playback_fields)
    {
        region.wave_data_location = r.U8(at + 9);
    }
    region.note_off_type = r.U8(at + 0xA);
    region.key_group = r.U8(at + 0xB);
    region.original_key = r.U8(at + 0xC);
    region.pan = r.U8(at + 0xE);
    if (options.playback_fields)
    {
        region.surround_pan = r.U8(at + 0xF);
    }
    if (version >= 0x0101)
    {
        region.volume = r.U8(at + 0xD);
        region.tune = r.F32(at + 0x10);
    }

    return region;
}

// The keys of a bank's table, where its references start, and how many it holds.
struct TableShape
{
    std::vector<uint8_t> keys;
    std::size_t refs = 0;
    std::size_t entries = 0;
};

// The shape of a bank's table at `at`. A range table (`range`) holds a count and each entry's upper key, then
// references from a multiple of 4 bytes on. An index table holds its lowest and highest key, two padding bytes, then a
// reference for each key.
TableShape ReadTableShape(const xsflib::Reader& r, std::size_t at, bool range)
{
    TableShape shape;
    if (range)
    {
        shape.entries = r.U8(at);
        for (std::size_t i = 0; i < shape.entries; i++)
        {
            shape.keys.push_back(r.U8(at + 1 + i));
        }

        shape.refs = at + ((shape.entries + 1 + 3) & ~std::size_t{3});
    }
    else
    {
        const uint8_t lowest = r.U8(at);
        const uint8_t highest = r.U8(at + 1);
        shape.keys = {lowest, highest};
        shape.entries = highest >= lowest ? std::size_t{highest} - lowest + 1 : 0;
        shape.refs = at + 4;
    }

    return shape;
}

// Where each wave info of a version 1.1 bank lies: its WAVE block holds a reference to each, from the table's start.
// Nothing for a later bank, which has no WAVE block: its wave data is an RWAR. `file_size` is the bank's size. Throws
// xsflib::FormatError for a table of more waves than the bank has bytes, with short messages.
std::optional<std::vector<std::size_t>> WaveInfoPlaces(const xsflib::Reader& r, std::size_t file_size,
                                                       const ReadOptions& options)
{
    const uint32_t wave = r.U16(0x0E) >= 2 ? r.U32(0x18) : 0;
    if (wave == 0 || !IsMagic(r, wave, "WAVE", options))
    {
        return std::nullopt;
    }

    const std::size_t wave_base = wave + 8;
    const uint32_t waves = r.U32(wave_base);
    if (Short(options) && waves > file_size)
    {
        throw xsflib::FormatError("invalid wave table");
    }

    std::vector<std::size_t> places;
    for (uint32_t i = 0; i < waves; i++)
    {
        places.push_back(wave_base + ReadRef(r, wave_base + 4 + i * std::size_t{8}).offset);
    }

    return places;
}

// Reads the settings at `info` of a wave sound in a file of `version` into `sound`, as WsdFileReader::ReadWaveSoundInfo
// (0x8004f2ec) does: version 1.1 adds the pitch, the pan and the surround pan, and version 1.2 the sends. The builds
// from 2008 read the sends from every later version too.
void ReadWaveSoundSettings(const xsflib::Reader& r, std::size_t info, uint16_t version, WaveSoundFile::WaveSound& sound)
{
    if (version >= 0x0101)
    {
        sound.pitch = r.F32(info);
        sound.pan = r.U8(info + 4);
        sound.surround_pan = r.U8(info + 5);
    }
    if (version >= 0x0102)
    {
        for (std::size_t bus = 0; bus < sound.fx_sends.size(); bus++)
        {
            sound.fx_sends[bus] = r.U8(info + 6 + bus);
        }
        sound.main_send = r.U8(info + 9);
    }
}

// The note at `at` of a wave sound in a file of `version`, as ReadWaveSoundNoteInfo (0x8004f40c) reads it: version 1.1
// adds the note's pan, surround pan and pitch. The builds from 2008 also read a hold (Mario Kart Wii, 0x800adac0).
WaveSoundFile::Note ReadNote(const xsflib::Reader& r, std::size_t at, uint16_t version)
{
    WaveSoundFile::Note note;
    note.wave_index = r.S32(at);
    note.attack = r.U8(at + 4);
    note.decay = r.U8(at + 5);
    note.sustain = r.U8(at + 6);
    note.release = r.U8(at + 7);
    note.hold = r.U8(at + 8);
    note.original_key = r.U8(at + 0xC);
    note.volume = r.U8(at + 0xD);
    if (version >= 0x0101)
    {
        note.pan = r.U8(at + 0xE);
        note.surround_pan = r.U8(at + 0xF);
        note.pitch = r.F32(at + 0x10);
    }

    return note;
}

// Where each wave info of the WAVE block at `wave` lies, in a file of `version` whose notes play waves up to
// `highest_wave`. The block holds an offset to each wave info from the block's start (ReadWaveParam, 0x8004f51c). From
// version 1.1, a count comes first. Version 1.0's offsets follow the block's header. The notes then give how many there
// are. `file_size` is the file's size. Throws xsflib::FormatError for a table of more waves than the file has bytes,
// with short messages.
std::vector<std::size_t> WaveInfoTable(const xsflib::Reader& r, std::size_t wave, uint16_t version,
                                       int32_t highest_wave, std::size_t file_size, const ReadOptions& options)
{
    std::size_t table = wave + std::size_t{8};
    std::size_t waves = highest_wave >= 0 ? static_cast<std::size_t>(highest_wave) + 1 : 0;
    if (version >= 0x0101)
    {
        waves = r.U32(wave + 8);
        table += 4;
    }
    if (Short(options) && waves > file_size)
    {
        throw xsflib::FormatError("invalid wave table");
    }

    std::vector<std::size_t> places;
    for (std::size_t i = 0; i < waves; i++)
    {
        places.push_back(wave + std::size_t{r.U32(table + i * 4)});
    }

    return places;
}

} // namespace

Wave Wave::ParseFile(std::span<const uint8_t> rwav, const ReadOptions& options)
{
    // An INFO block holding the wave info, and a DATA block holding the samples. The channels' offsets count from the
    // samples. (The wave info's data offset points at the DATA block's header.)
    const xsflib::Reader r = MakeReader(rwav, options);
    CheckHeader(r, "RWAV", options);
    const uint32_t info = r.U32(0x10);
    const uint32_t data = r.U32(0x18);
    if (!IsMagic(r, info, "INFO", options) || !IsMagic(r, data, "DATA", options))
    {
        throw xsflib::FormatError(Short(options) ? "no INFO or DATA block" : "an RWAV file has no INFO or DATA block");
    }

    return ReadWaveInfo(r, info + 8, r.Sub(data + 8, rwav.size() - (data + 8)), options);
}

Wave Wave::ParseInfo(const xsflib::Reader& info, std::size_t offset, std::span<const uint8_t> wave_data,
                     const ReadOptions& options)
{
    const uint32_t data_offset = info.U32(offset + 0x14);
    if (data_offset > wave_data.size())
    {
        throw xsflib::FormatError(Short(options) ? "samples start past the wave data"
                                                 : "a wave's samples lie outside its wave data");
    }

    return ReadWaveInfo(info, offset, wave_data.subspan(data_offset), options);
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> file, const ReadOptions& options)
{
    // A TABL block lists each wave as a reference to it, from the DATA block's start, and its size.
    const xsflib::Reader r = MakeReader(file, options);
    CheckHeader(r, "RWAR", options);
    const uint32_t table = r.U32(0x10);
    const uint32_t data = r.U32(0x18);
    if (!IsMagic(r, table, "TABL", options) || !IsMagic(r, data, "DATA", options))
    {
        throw xsflib::FormatError(Short(options) ? "no TABL or DATA block" : "an RWAR file has no TABL or DATA block");
    }

    WaveArchive war;
    const uint32_t count = r.U32(table + 8);
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t e = table + 0xC + i * std::size_t{12};
        war.waves.push_back(r.Sub(data + std::size_t{ReadRef(r, e).offset}, r.U32(e + 8)));
    }

    return war;
}

Bank Bank::Parse(std::span<const uint8_t> file, const ReadOptions& options)
{
    const xsflib::Reader r = MakeReader(file, options);
    Bank bank;
    bank.file_ = file;
    bank.options_ = options;
    const uint16_t version = CheckHeader(r, "RBNK", options);
    bank.version_ = version;
    const uint32_t data = r.U32(0x10);
    if (!IsMagic(r, data, "DATA", options))
    {
        throw xsflib::FormatError(Short(options) ? "no DATA block" : "an RBNK file has no DATA block");
    }

    // The instrument table: a count, then a reference for each program, from the table's start. Tables and regions that
    // several references share are read once. A bank whose tables all point at one entry then stays small.
    const std::size_t base = data + 8;
    std::map<std::pair<uint8_t, uint32_t>, uint32_t> seen;
    const auto read_node = [&](std::size_t ref_at, int depth, const auto& self) -> Node
    {
        const Reference ref = ReadRef(r, ref_at);
        if (!ref.IsOffset() || ref.data_type == kNone)
        {
            return Node{kNone, 0};
        }

        // NW4R selects at most two levels deep, a key and then a velocity, and has no other types.
        if (ref.data_type != kRegion && (depth >= 2 || ref.data_type > kIndexTable))
        {
            return Node{kNone, 0};
        }
        if (const auto known = seen.find({ref.data_type, ref.offset}); known != seen.end())
        {
            return Node{ref.data_type, known->second};
        }

        const std::size_t at = base + ref.offset;
        if (ref.data_type == kRegion)
        {
            const auto index = static_cast<uint32_t>(bank.regions_.size());
            bank.regions_.push_back(ReadRegion(r, at, version, options));
            seen[{ref.data_type, ref.offset}] = index;

            return Node{kRegion, index};
        }

        TableShape shape = ReadTableShape(r, at, ref.data_type == kRangeTable);
        Table table;
        table.type = ref.data_type;
        table.keys = std::move(shape.keys);
        const auto index = static_cast<uint32_t>(bank.tables_.size());
        seen[{ref.data_type, ref.offset}] = index;
        bank.tables_.push_back(table);
        for (std::size_t i = 0; i < shape.entries; i++)
        {
            const Node entry = self(shape.refs + i * 8, depth + 1, self);
            bank.tables_[index].entries.push_back(entry);
        }

        return Node{ref.data_type, index};
    };

    const uint32_t count = r.U32(base);
    if (Short(options) && count > file.size())
    {
        throw xsflib::FormatError("invalid instrument table");
    }

    for (uint32_t i = 0; i < count; i++)
    {
        bank.instruments_.push_back(read_node(base + 4 + i * std::size_t{8}, 0, read_node));
    }

    if (std::optional<std::vector<std::size_t>> places = WaveInfoPlaces(r, file.size(), options))
    {
        bank.wave_block_ = true;
        bank.wave_infos_ = std::move(*places);
    }

    return bank;
}

Bank Bank::Parse(std::span<const uint8_t> file, std::span<const uint8_t> wave_data, const ReadOptions& options)
{
    // The waves come from a version 1.1 bank's WAVE block, over the raw samples, or else from the RWAR that is a later
    // bank's wave data.
    Bank bank = Parse(file, options);
    if (bank.HasWaveBlock())
    {
        for (std::size_t i = 0; i < bank.wave_infos_.size(); i++)
        {
            bank.waves_.push_back(bank.ReadWave(i, wave_data));
        }
    }
    else if (!wave_data.empty())
    {
        for (const std::span<const uint8_t> rwav : WaveArchive::Parse(wave_data, options).waves)
        {
            bank.waves_.push_back(Wave::ParseFile(rwav, options));
        }
    }

    return bank;
}

const Bank::Node* Bank::Choose(const Node& node, int key) const
{
    // BankFileReader::GetReferenceToSubRegion (main.dol 0x80035f40).
    switch (node.type)
    {
    case kRegion:
        return &node;

    case kRangeTable:
        {
            const Table& table = tables_[node.index];
            for (std::size_t i = 0; i < table.entries.size(); i++)
            {
                if (key <= table.keys[i])
                {
                    return &table.entries[i];
                }
            }

            return nullptr;
        }

    case kIndexTable:
        {
            const Table& table = tables_[node.index];
            if (key < table.keys[0] || key > table.keys[1] || table.entries.empty())
            {
                return nullptr;
            }

            return &table.entries[static_cast<std::size_t>(key - table.keys[0])];
        }

    default:
        return nullptr;
    }
}

bool Bank::Plays(uint32_t program) const
{
    // Find's walk for every key and velocity at once: each entry of a key table that a key reaches, then each entry
    // of a velocity table that a velocity reaches.
    const auto plays = [this](const auto& self, const Node& node, int splits) -> bool
    {
        if (node.type == kRegion)
        {
            return regions_[node.index].wave_index >= 0;
        }

        std::set<const Node*> reached;
        for (int key = 0; splits > 0 && key <= 127; key++)
        {
            if (const Node* next = Choose(node, key))
            {
                reached.insert(next);
            }
        }

        return std::ranges::any_of(reached, [&](const Node* next) { return self(self, *next, splits - 1); });
    };

    return program < instruments_.size() && plays(plays, instruments_[program], 2);
}

const Region* Bank::Find(uint32_t program, int key, int velocity) const
{
    if (program >= instruments_.size())
    {
        return nullptr;
    }

    const Node* node = &instruments_[program];
    for (const int split : {key, velocity})
    {
        if (node->type == kNone)
        {
            return nullptr;
        }

        node = Choose(*node, split);
        if (!node)
        {
            return nullptr;
        }
    }

    if (node->type != kRegion || regions_[node->index].wave_index < 0)
    {
        return nullptr;
    }

    return &regions_[node->index];
}

Wave Bank::ReadWave(std::size_t index, std::span<const uint8_t> wave_data) const
{
    if (index >= wave_infos_.size())
    {
        throw xsflib::FormatError("no wave " + std::to_string(index) + " (the bank has " +
                                  std::to_string(wave_infos_.size()) + ")");
    }

    return Wave::ParseInfo(MakeReader(file_, options_), wave_infos_[index], wave_data, options_);
}

WaveSoundFile WaveSoundFile::Parse(std::span<const uint8_t> file, const ReadOptions& options)
{
    const xsflib::Reader r = MakeReader(file, options);
    WaveSoundFile wsd;
    wsd.file_ = file;
    wsd.options_ = options;
    const uint16_t version = CheckHeader(r, "RWSD", options);
    wsd.version_ = version;
    const uint32_t data = r.U32(0x10);
    if (!IsMagic(r, data, "DATA", options))
    {
        throw xsflib::FormatError(Short(options) ? "no DATA block" : "an RWSD file has no DATA block");
    }

    // The DATA block: a count, then a reference to each wave sound. Every reference in the block counts from the
    // count's place. A wave sound refers to its settings, its tracks and its table of notes.
    const std::size_t base = data + 8;
    const auto follow = [&](std::size_t ref_at)
    {
        const Reference ref = ReadRef(r, ref_at);
        if (!ref.IsOffset())
        {
            throw xsflib::FormatError(Short(options) ? "a wave sound reference isn't an offset"
                                                     : "a wave sound of an RWSD file refers to nothing");
        }

        return base + ref.offset;
    };
    const uint32_t count = r.U32(base);
    if (Short(options) && count > file.size())
    {
        throw xsflib::FormatError("invalid wave sound table");
    }

    int32_t highest_wave = -1;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t sound_at = follow(base + 4 + i * std::size_t{8});
        WaveSound sound;
        ReadWaveSoundSettings(r, follow(sound_at), version, sound);

        const std::size_t notes = follow(sound_at + 0x10);
        const uint32_t note_count = r.U32(notes);
        if (Short(options) && note_count > file.size())
        {
            throw xsflib::FormatError("invalid note table");
        }
        for (uint32_t j = 0; j < note_count; j++)
        {
            const Note note = ReadNote(r, follow(notes + 4 + j * std::size_t{8}), version);
            highest_wave = std::max(highest_wave, note.wave_index);
            sound.notes.push_back(note);
        }

        wsd.sounds_.push_back(std::move(sound));
    }

    // A file without a WAVE block has an RWAR for its wave data.
    const uint32_t wave = r.U32(0x18);
    if (wave != 0 && IsMagic(r, wave, "WAVE", options))
    {
        wsd.wave_block_ = wave;
        wsd.wave_infos_ = WaveInfoTable(r, wave, version, highest_wave, file.size(), options);
    }

    return wsd;
}

WaveSoundFile WaveSoundFile::Parse(std::span<const uint8_t> file, std::span<const uint8_t> wave_data,
                                   const ReadOptions& options)
{
    // The waves come from the WAVE block, over the raw samples, or else from the RWAR that is a later file's wave data.
    WaveSoundFile wsd = Parse(file, options);
    if (wsd.HasWaveBlock())
    {
        for (std::size_t i = 0; i < wsd.wave_infos_.size(); i++)
        {
            wsd.waves_.push_back(wsd.ReadWave(i, wave_data));
        }
    }
    else if (!wave_data.empty())
    {
        for (const std::span<const uint8_t> rwav : WaveArchive::Parse(wave_data, options).waves)
        {
            wsd.waves_.push_back(Wave::ParseFile(rwav, options));
        }
    }

    return wsd;
}

Wave WaveSoundFile::ReadWave(std::size_t index, std::span<const uint8_t> wave_data) const
{
    if (index >= wave_infos_.size())
    {
        throw xsflib::FormatError("no wave " + std::to_string(index) + " (the wave sound file has " +
                                  std::to_string(wave_infos_.size()) + ")");
    }

    return Wave::ParseInfo(MakeReader(file_, options_), wave_infos_[index], wave_data, options_);
}

Sequence Sequence::Parse(std::span<const uint8_t> file, const ReadOptions& options)
{
    // DATA holds the commands from an offset it gives, from the block's start. LABL holds an offset for each label,
    // from LABL's payload. Each offset leads to the label's data offset, name length and name.
    const xsflib::Reader r = MakeReader(file, options);
    CheckHeader(r, "RSEQ", options);
    const uint32_t data = r.U32(0x10);
    const uint32_t data_size = r.U32(0x14);
    if (!IsMagic(r, data, "DATA", options) || data_size < 0xC)
    {
        throw xsflib::FormatError(Short(options) ? "no DATA block" : "an RSEQ file has no DATA block");
    }

    const uint32_t commands = r.U32(data + 8);
    if (commands > data_size)
    {
        throw xsflib::FormatError(Short(options) ? "commands start past the DATA block"
                                                 : "an RSEQ file's commands lie outside its DATA block");
    }

    Sequence s;
    s.data = r.Sub(data + std::size_t{commands}, data_size - commands);
    const uint32_t labels = options.playback_fields && r.U16(0x0E) >= 2 ? r.U32(0x18) : 0;
    if (labels != 0 && IsMagic(r, labels, "LABL", options))
    {
        s.labels = detail::ReadLabels(r, labels);
    }

    return s;
}

SoundArchive SoundArchive::Load(std::vector<uint8_t> bytes, const ReadOptions& options)
{
    SoundArchive a;
    a.bytes_ = std::move(bytes);
    a.options_ = options;
    const xsflib::Reader r = MakeReader(a.bytes_, options);
    const bool short_texts = Short(options);

    // Other consoles' archives. Only the short texts check for the DS's.
    if (r.Size() >= 4 && r.Magic(0, "CSAR"))
    {
        throw xsflib::FormatError(short_texts ? "unsupported CSAR archive (3DS); expected RSAR (Wii)"
                                              : "a 3DS sound archive (CSAR), not a Wii one (RSAR)");
    }
    if (r.Size() >= 4 && r.Magic(0, "FSAR"))
    {
        throw xsflib::FormatError(short_texts ? "unsupported FSAR archive (Wii U/Switch); expected RSAR (Wii)"
                                              : "a Wii U or Switch sound archive (FSAR), not a Wii one (RSAR)");
    }
    if (short_texts && r.Size() >= 4 && r.Magic(0, "SDAT"))
    {
        throw xsflib::FormatError("unsupported SDAT archive (DS); expected RSAR (Wii)");
    }

    a.version_ = CheckHeader(r, "RSAR", options);

    // Read as much of a truncated archive as possible. Use the size in its header to detect truncation. With the short
    // texts, include that in the error if its tables cannot be read.
    const uint32_t declared_size = r.U32(0x08);
    a.truncated_ = declared_size > a.bytes_.size();
    try
    {
        a.ReadTables(r);
    }
    catch (const xsflib::FormatError& e)
    {
        if (!short_texts || !a.truncated_)
        {
            throw;
        }

        throw xsflib::FormatError("truncated archive: " + std::to_string(a.bytes_.size()) + " of " +
                                  std::to_string(declared_size) + " bytes; cannot read tables (" + e.what() + ")");
    }

    return a;
}

void SoundArchive::ReadTables(const xsflib::Reader& r)
{
    const uint32_t symb = r.U32(0x10);
    const uint32_t symb_size = r.U32(0x14);
    const uint32_t info = r.U32(0x18);
    if (!IsMagic(r, info, "INFO", options_))
    {
        throw xsflib::FormatError(Short(options_) ? "no INFO block" : "the sound archive has no INFO block");
    }

    if (options_.playback_fields)
    {
        info_size_ = r.U32(0x1c);
    }

    // The items' names, then INFO's tables of sounds, banks and players.
    const Names names(r, symb, symb_size, options_);
    names_damaged_ = names.Damaged();
    const InfoTables tables{r, info + std::size_t{8}, options_, version_, names};
    const auto read_sound = [&](uint32_t i, std::size_t p, const Reference& entry)
    {
        sounds_.push_back(ReadSound(tables, i, p, entry));
    };
    tables.ForEachEntry(tables.base, "sound", read_sound);

    const auto read_bank = [&](uint32_t i, std::size_t p, const Reference&)
    {
        banks_.push_back(ReadBank(tables, i, p));
    };
    tables.ForEachEntry(tables.base + 8, "bank", read_bank);

    if (options_.playback_fields)
    {
        const auto read_player = [&](uint32_t i, std::size_t p, const Reference&)
        {
            players_.push_back(ReadPlayer(tables, i, p));
        };
        tables.ForEachEntry(tables.base + 0x10, "player", read_player);
    }

    // The files and the groups that hold them.
    FileTable file_table = ReadFileTable(tables);
    files_.resize(file_table.positions.size());
    for (std::size_t id = 0; id < files_.size(); id++)
    {
        files_[id].path = std::move(file_table.paths[id]);
    }

    const std::vector<GroupPlaces> groups = ReadGroups(tables, groups_, truncated_);

    // Use the first group that holds a file and its wave data inside the archive, or else the first that holds the
    // file. Copies in different groups are the same.
    const auto inside = [&](uint64_t offset, uint64_t size)
    {
        return offset <= bytes_.size() && size <= bytes_.size() - offset;
    };
    for (std::size_t id = 0; id < files_.size(); id++)
    {
        FileEntry& f = files_[id];
        for (const auto& [group, item] : file_table.positions[id])
        {
            if (group >= groups.size() || !groups[group].internal || item >= groups[group].items.size())
            {
                continue;
            }

            const GroupPlaces& g = groups[group];
            const std::size_t q = g.items[item];
            const uint64_t offset = g.offset + r.U32(q + 4);
            const uint32_t size = r.U32(q + 8);
            const uint64_t wave_offset = g.wave_data_offset + r.U32(q + 0xC);
            const uint32_t wave_size = r.U32(q + 0x10);
            const bool file_inside = inside(offset, size);
            const bool wave_inside = inside(wave_offset, wave_size);
            f.missing = f.missing || !file_inside;
            f.wave_data_missing = f.wave_data_missing || !wave_inside;
            if (file_inside && (!f.found || (wave_inside && !f.wave_data_found)))
            {
                f.found = true;
                f.offset = static_cast<std::size_t>(offset);
                f.size = size;
                f.wave_data_found = wave_inside;
                f.wave_data_offset = wave_inside ? static_cast<std::size_t>(wave_offset) : 0;
                f.wave_data_size = wave_inside ? wave_size : 0;
            }
        }

        f.missing = f.missing && !f.found;
        f.wave_data_missing = f.wave_data_missing && !f.wave_data_found;
    }
}

std::optional<std::string> SoundArchive::ExternalPath(uint32_t file_id) const
{
    return file_id < files_.size() ? files_[file_id].path : std::nullopt;
}

std::optional<uint32_t> SoundArchive::FindSound(const std::string& name) const
{
    for (std::size_t i = 0; i < sounds_.size(); i++)
    {
        if (sounds_[i].name == name)
        {
            return static_cast<uint32_t>(i);
        }
    }

    return std::nullopt;
}

std::span<const uint8_t> SoundArchive::FileData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].found)
    {
        return {};
    }

    return std::span<const uint8_t>(bytes_).subspan(files_[file_id].offset, files_[file_id].size);
}

std::span<const uint8_t> SoundArchive::WaveData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].wave_data_found)
    {
        return {};
    }

    return std::span<const uint8_t>(bytes_).subspan(files_[file_id].wave_data_offset, files_[file_id].wave_data_size);
}

std::optional<FileData> SoundArchive::File(uint32_t file_id) const
{
    // ReadTables found the first group that holds the file and its wave data inside the archive. Like the groups, a
    // file with its wave data is for playback alone (playback_fields).
    if (!options_.playback_fields || file_id >= files_.size() || !files_[file_id].wave_data_found)
    {
        return std::nullopt;
    }

    return rsar::FileData{FileData(file_id), WaveData(file_id)};
}

} // namespace ntdlib::rsar
