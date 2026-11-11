// SPDX-License-Identifier: MIT

// The sound archive reader (see formats.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/reference.h"
#include "sar/reading.h"

namespace ntdlib::sar
{
namespace
{

using detail::BlockOf;
using detail::Blocks;
using detail::BlockTable;
using detail::OpenFile;
using detail::OptionParams;
using detail::ReadSends;
using detail::RequiredBlock;

// The positions of the STRG block's two parts at `strg_off`: its string table and its Patricia tree.
std::pair<std::size_t, std::size_t> StringParts(const xsflib::Reader& r, std::size_t strg_off)
{
    const std::size_t b = strg_off + 8;
    std::size_t st = 0, pt = 0;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        if (ref.type == 0x2400)
        {
            st = b + ref.offset;
        }
        else if (ref.type == 0x2401)
        {
            pt = b + ref.offset;
        }
    }

    return {st, pt};
}

// Reads the STRG block at `strg_off`, of length `strg_size`: a string table and a Patricia tree mapping names to item
// IDs. The strings lie in the block, and each names one item. So neither the strings nor the names can add up to more
// than the block. A crafted table could otherwise point every entry at one long string and take memory without limit.
// kReadPastDamage makes each string printable, and also bounds the strings by the archive's size. Its caller takes a
// throw for damaged names and shows no message.
std::map<uint32_t, std::string> ReadItemNames(const xsflib::Reader& r, std::size_t strg_off, std::size_t strg_size,
                                              Rules rules)
{
    const bool strict = rules == Rules::kStopAtFirstProblem;
    const auto [st, pt] = StringParts(r, strg_off);
    const std::size_t room = strict ? strg_size : std::min(strg_size, r.Size());
    const auto check_room = [&](std::size_t used, const char* strict_message)
    {
        if (used > room)
        {
            throw xsflib::FormatError(strict ? strict_message
                                             : "invalid string table (total string length exceeds block size)");
        }
    };

    std::vector<std::string> strings;
    std::size_t string_bytes = 0;
    const uint32_t n = r.U32(st);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = st + 4 + i * 12;
        std::string text = r.CString(st + r.U32(e + 4), r.U32(e + 8));
        strings.push_back(strict ? std::move(text) : xsflib::PrintableName(text));
        string_bytes += strings.back().size();
        check_room(string_bytes, "the strings don't fit in the string block");
    }

    std::map<uint32_t, std::string> item_names;
    std::size_t name_bytes = 0;
    const uint32_t nodes = r.U32(pt + 4);
    for (uint32_t i = 0; i < nodes; i++)
    {
        const std::size_t e = pt + 8 + i * 20;
        if (r.U16(e) & 1)
        {
            const uint32_t sid = r.U32(e + 12), iid = r.U32(e + 16);
            if (sid < strings.size())
            {
                name_bytes += strings[sid].size();
                check_room(name_bytes, "the names don't fit in the string block");
                item_names[iid] = strings[sid];
            }
        }
    }

    return item_names;
}

// A stream sound's details at `q`, in an FSAR archive of version `version`. The details start with the tracks that
// play, the number of channels and a reference to the table of tracks. From 2.1.0 on, the pitch and a reference to the
// sends follow. Details without a table of tracks give nothing more. A track's info holds its volume, its pan, its
// span, its flags and a reference to the indices of its channels. From 2.1.0 on, a reference to its sends follows, then
// the low-pass value and the biquad filter's type and value.
void ReadStreamDetails(const xsflib::Reader& r, std::size_t q, uint32_t version, StreamSoundInfo& stream)
{
    constexpr uint32_t kMostTracks = 8;
    constexpr uint32_t kMostChannels = 16;
    const bool sends = version >= 0x00020100;
    stream.valid_tracks = r.U16(q);
    stream.channel_count = r.U16(q + 2);
    const Reference table_ref = ReadRef(r, q + 4);
    if (table_ref.type != 0x0101)
    {
        return; // details without tracks hold nothing more
    }

    if (sends)
    {
        stream.pitch = r.F32(q + 12);
        const Reference send = ReadRef(r, q + 16);
        if (send.type == 0x220F)
        {
            ReadSends(r, q + send.offset, stream.main_send, stream.aux_sends);
        }
    }

    const std::size_t table = q + table_ref.offset;
    const uint32_t count = r.U32(table);
    for (uint32_t i = 0; i < count && i < kMostTracks; ++i)
    {
        const Reference track_ref = ReadRef(r, table + 4 + std::size_t{i} * 8);
        const std::size_t t = table + track_ref.offset;
        StreamTrackInfo& track = stream.tracks.emplace_back();
        track.volume = r.U8(t);
        track.pan = r.U8(t + 1);
        track.span = r.U8(t + 2);
        track.flags = r.U8(t + 3);
        const Reference indices = ReadRef(r, t + 4);
        if (indices.type == 0x0100)
        {
            const uint32_t channels = r.U32(t + indices.offset);
            for (uint32_t k = 0; k < channels && k < kMostChannels; ++k)
            {
                track.channels.push_back(r.U8(t + indices.offset + 4 + k));
            }
        }
        if (sends)
        {
            const Reference send = ReadRef(r, t + 12);
            if (send.type == 0x220F)
            {
                ReadSends(r, t + send.offset, track.main_send, track.aux_sends);
            }
            track.low_pass = r.U8(t + 20);
            track.biquad_type = r.U8(t + 21);
            track.biquad_value = r.U8(t + 22);
        }
    }
}

// The type of the sound at the INFO entry at `p`, and the details of that type, from the entry's reference `detail`,
// in an archive of version `version`. A 3DS stream sound's details take the layout of FSAR 2.0.0, without sends, and
// from CSAR 2.3.2.0 on that of FSAR 2.2.0. Before CSAR 2.3.0.0 they hold no table of tracks.
void ReadSoundDetails(const xsflib::Reader& r, std::size_t p, const Reference& detail, bool ctr, uint32_t version,
                      SoundInfo& s)
{
    switch (detail.type)
    {
    case 0x2201:
        s.type = SoundType::kStream;
        ReadStreamDetails(r, p + detail.offset,
                          !ctr                    ? version
                          : version >= 0x02030200 ? 0x00020200
                                                  : 0x00020000,
                          s.stream);
        break;

    case 0x2202:
        {
            // The wave sound's details start with its index in its wave sound file and its tracks. The options that
            // follow give the channel's priority and the release priority fix (SoundArchiveFile::WaveSoundInfo's
            // GetChannelPriority and GetIsReleasePriorityFix). The 3DS's details hold them the same way.
            s.type = SoundType::kWaveSound;
            const std::size_t q = p + detail.offset;
            s.wave_sound_index = r.U32(q);
            const OptionParams so = OptionParams::Read(r, q + 8);
            if (so.values[0])
            {
                s.channel_priority = static_cast<uint8_t>(*so.values[0]);
                s.release_priority_fix = ((*so.values[0] >> 8) & 0xff) != 0;
            }
            break;
        }

    case 0x2203:
        {
            s.type = SoundType::kSequence;
            const std::size_t q = p + detail.offset;

            // nw::snd and nn::atk have four bank slots. A corrupt table may claim many more. A crafted archive could
            // otherwise give every sound one long list and take memory without limit.
            const Reference banks = ReadRef(r, q);
            const std::size_t bt = q + banks.offset;
            const uint32_t nb = r.U32(bt);
            for (uint32_t k = 0; k < nb && k < kBankSlots; k++)
            {
                s.banks.push_back(r.U32(bt + 4 + k * 4));
            }

            s.allocate_track_flags = r.U32(q + 8);
            const OptionParams so = OptionParams::Read(r, q + 12);
            if (so.values[0])
            {
                s.start_offset = *so.values[0];
            }

            if (so.values[1])
            {
                s.channel_priority = static_cast<uint8_t>(*so.values[1]);
                s.release_priority_fix = ((*so.values[1] >> 8) & 0xff) != 0;
            }
            break;
        }

    default:
        break;
    }
}

// A name from the sound's type and index, for sound `index` if the archive has no name for it.
void NameUnnamedSound(SoundInfo& s, uint32_t index)
{
    if (s.name.empty())
    {
        const char* prefix = s.type == SoundType::kSequence    ? "SEQ_"
                             : s.type == SoundType::kStream    ? "STRM_"
                             : s.type == SoundType::kWaveSound ? "WSD_"
                                                               : "SOUND_";
        s.name = prefix + std::to_string(index);
    }
}

// Reads sound `index` from the INFO entry at `p`, in an archive of version `version`.
SoundInfo ReadSound(const xsflib::Reader& r, std::size_t p, uint32_t index, std::string name, bool ctr,
                    uint32_t version)
{
    SoundInfo s;
    s.name = std::move(name);
    s.file_id = r.U32(p);
    s.volume = r.U8(p + 8);

    // Pan mode and curve share the second optional parameter.
    const OptionParams options = OptionParams::Read(r, p + 20);
    if (options.values[1])
    {
        s.pan_mode = static_cast<uint8_t>(*options.values[1]);
        s.pan_curve = static_cast<uint8_t>(*options.values[1] >> 8);
    }

    ReadSoundDetails(r, p, ReadRef(r, p + 12), ctr, version, s);
    NameUnnamedSound(s, index);

    return s;
}

// Reads sound `index` from the INFO entry at `p`, in an archive of version `version`, as kStopAtFirstProblem reads it:
// with the player, the player priority and the actor player.
SoundInfo ReadSoundStrictly(const xsflib::Reader& r, std::size_t p, uint32_t index, std::string name, bool ctr,
                            uint32_t version)
{
    SoundInfo s;
    s.name = std::move(name);
    s.file_id = r.U32(p);
    s.player_id = r.U32(p + 4);
    s.volume = r.U8(p + 8);
    const Reference detail = ReadRef(r, p + 12);
    const OptionParams opt = OptionParams::Read(r, p + 20);
    if (opt.values[1])
    {
        s.pan_mode = static_cast<uint8_t>(*opt.values[1]);
        s.pan_curve = static_cast<uint8_t>(*opt.values[1] >> 8);
    }

    if (opt.values[2])
    {
        s.player_priority = static_cast<uint8_t>(*opt.values[2]);
        s.actor_player_id = static_cast<uint8_t>(*opt.values[2] >> 8);
    }

    ReadSoundDetails(r, p, detail, ctr, version, s);
    NameUnnamedSound(s, index);

    return s;
}

// Reads a sound group from the INFO entry at `p`: the IDs of its first and last sounds, then a reference to the table
// of its file IDs. The table holds its count and the IDs.
SoundGroupInfo ReadSoundGroup(const xsflib::Reader& r, std::size_t p, std::string name)
{
    SoundGroupInfo g;
    g.name = std::move(name);
    g.first_sound = ItemNumber(r.U32(p));
    g.last_sound = ItemNumber(r.U32(p + 4));
    const Reference files = ReadRef(r, p + 8);
    if (files.type == 0x0100 && files.offset >= 0)
    {
        const std::size_t table = p + static_cast<std::size_t>(files.offset);
        const uint32_t count = r.U32(table);
        for (uint32_t i = 0; i < count; i++)
        {
            g.file_ids.push_back(r.U32(table + 4 + std::size_t{i} * 4));
        }
    }

    return g;
}

// The places of the INFO block's tables at `info_off`, by type. The block's eight references lead to the tables of
// sounds, sound groups, banks, wave archives, groups, players and files, and to the archive player's settings.
std::map<uint16_t, std::size_t> InfoTables(const xsflib::Reader& r, std::size_t info_off)
{
    const std::size_t b = info_off + 8;
    std::map<uint16_t, std::size_t> tables;
    for (int i = 0; i < 8; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        tables[ref.type] = b + ref.offset;
    }

    return tables;
}

// Calls `read` with the index and position of each entry of the INFO table at `table`.
template <typename Read>
void ForEachEntry(const xsflib::Reader& r, std::size_t table, Read read)
{
    const uint32_t n = r.U32(table);
    for (uint32_t i = 0; i < n; i++)
    {
        read(i, table + ReadRef(r, table + 4 + i * 8).offset);
    }
}

// Calls `read` with the index and position of each entry of the INFO table of `type` that `tables` lists. Throws
// xsflib::FormatError for an archive without the table, with a message that names it as `name`.
template <typename Read>
void ForEachTableEntry(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables, uint16_t type,
                       const char* name, Read read)
{
    const auto table = tables.find(type);
    if (table == tables.end())
    {
        throw xsflib::FormatError(std::string("no ") + name + " table");
    }

    ForEachEntry(r, table->second, read);
}

// As ForEachTableEntry, with kStopAtFirstProblem's message for an archive without the table.
template <typename Read>
void ForEachTableEntryStrictly(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables, uint16_t type,
                               Read read)
{
    const auto table = tables.find(type);
    if (table == tables.end())
    {
        throw xsflib::FormatError("the sound archive's INFO block has no table of type " + xsflib::Hex(type, 4, true));
    }

    ForEachEntry(r, table->second, read);
}

// The items' names, read by `rules` from the STRG block that `blocks` lists: a string table, and a Patricia tree that
// maps names to item IDs. None for an archive built without names, whose STRG offset is then 0xFFFFFFFF, and none for
// a STRG block that starts past the archive's `size` bytes.
std::map<uint32_t, std::string> ItemNamesOf(const xsflib::Reader& r, const Blocks& blocks, std::size_t size,
                                            Rules rules)
{
    const auto strg = blocks.find(0x2000);
    if (strg == blocks.end() || strg->second.first == 0xFFFFFFFF || strg->second.first >= size)
    {
        return {};
    }

    return ReadItemNames(r, strg->second.first, strg->second.second, rules);
}

// The name of item `item` in `names`, or an empty name.
std::string NameIn(const std::map<uint32_t, std::string>& names, uint32_t item)
{
    const auto it = names.find(item);

    return it == names.end() ? std::string() : it->second;
}

// Calls `use` with the file ID, and the start and size within the group file, of each file that group file `g` embeds.
// Each call comes as soon as the file's entry is read. A start past the group file's end comes out as it is, for the
// caller to check.
template <typename Use>
void ForEachGroupItem(const xsflib::Reader& g, Use use)
{
    const Blocks blocks = BlockTable(g);
    const auto [info_off, info_size] = RequiredBlock(blocks, 0x7800, "INFO");
    const auto [file_off, file_size] = RequiredBlock(blocks, 0x7801, "FILE");

    // INFO lists file IDs and locations in the group's FILE block. A null reference names a file stored directly in the
    // archive.
    const std::size_t t = info_off + 8;
    const uint32_t n = g.U32(t);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = t + ReadRef(g, t + 4 + i * 8).offset;
        const uint32_t id = g.U32(e);
        const Reference ref = ReadRef(g, e + 4);
        const uint32_t size = g.U32(e + 12);
        if (ref.type == 0x1F00)
        {
            use(id, static_cast<uint64_t>(file_off) + 8 + static_cast<uint32_t>(ref.offset), size);
        }
    }
}

// The files that group file `g` embeds, from its whole table: each one's file ID, and its start and size within the
// group file.
std::vector<std::tuple<uint32_t, uint64_t, uint32_t>> GroupItems(const xsflib::Reader& g)
{
    std::vector<std::tuple<uint32_t, uint64_t, uint32_t>> items;
    auto add = [&](uint32_t id, uint64_t start, uint32_t size)
    {
        items.emplace_back(id, start, size);
    };
    ForEachGroupItem(g, add);

    return items;
}

// The files that group file `file` embeds, read as kStopAtFirstProblem reads them on the Wii U and the Switch: each
// one's file ID, and its bytes within the group file. An item whose bytes lie past the group file's end throws.
// MatchesGroupFile reads a 3DS group file this way too. ReadTablesStrictly and AddGroupFile walk the 3DS's group files
// as its player does (WalkCtrGroup).
std::vector<std::pair<uint32_t, std::span<const uint8_t>>> GroupItemsStrictly(std::span<const uint8_t> file,
                                                                              const Options& options)
{
    const xsflib::Reader g = OpenFile(file, "GRP", options);
    const Blocks blocks = BlockTable(g);
    if (!blocks.contains(0x7800) || !blocks.contains(0x7801))
    {
        throw xsflib::FormatError("no INFO or FILE block");
    }

    const uint32_t items = blocks.at(0x7800).first + 8;
    const uint32_t data = blocks.at(0x7801).first + 8;
    std::vector<std::pair<uint32_t, std::span<const uint8_t>>> out;
    const uint32_t n = g.U32(items);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = items + ReadRef(g, items + 4 + i * 8).offset;
        const Reference where = ReadRef(g, e + 4);
        if (where.type == 0x1F00)
        {
            out.push_back({g.U32(e), g.Sub(data + where.offset, g.U32(e + 12))});
        }
    }

    return out;
}

// Walks group file `file` as the 3DS's player does with kStopAtFirstProblem. It reads each item's file ID first. For an
// item whose copy the group file holds, it reads the copy's bytes only when `wants` takes the file ID, and passes the
// ID and the bytes to `take`. A copy whose bytes lie past the group file's end throws.
template <typename Wants, typename Take>
void WalkCtrGroup(std::span<const uint8_t> file, const Options& options, const Wants& wants, const Take& take)
{
    const xsflib::Reader g = OpenFile(file, "GRP", options);
    const Blocks blocks = BlockTable(g);
    const auto items = static_cast<uint32_t>(BlockOf(blocks, 0x7800, "INFO", "a group (CGRP)", options).first + 8);
    const auto data = static_cast<uint32_t>(BlockOf(blocks, 0x7801, "FILE", "a group (CGRP)", options).first + 8);
    const uint32_t n = g.U32(items);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = items + ReadRef(g, items + 4 + i * 8).offset;
        const uint32_t file_id = g.U32(e);
        const Reference where = ReadRef(g, e + 4);
        if (where.type == 0x1F00 && wants(file_id))
        {
            take(file_id, g.Sub(data + where.offset, g.U32(e + 12)));
        }
    }
}

} // namespace

SoundArchive SoundArchive::Load(std::vector<uint8_t> bytes, const Options& options)
{
    SoundArchive a;
    a.options_ = options;
    a.bytes_ = std::move(bytes);
    const xsflib::Reader r = OpenFile(a.bytes_, "SAR", options); // reports other archive formats separately
    if (options.rules == Rules::kStopAtFirstProblem)
    {
        a.ReadTablesStrictly(r);
        return a;
    }

    a.version_ = r.U32(0x08);
    if (options.flavor == Flavor::kCafe && !r.BigEndian())
    {
        throw xsflib::FormatError("unsupported little-endian FSAR archive (Switch); expected a Wii U archive");
    }
    if (options.flavor == Flavor::kNx && r.BigEndian())
    {
        throw xsflib::FormatError("unsupported big-endian FSAR archive (Wii U); expected a Switch archive");
    }

    // Read as much of a truncated archive as possible. Use the size in its header to detect truncation, and include
    // that in the error if its tables cannot be read.
    const uint32_t declared_size = r.U32(0x0C);
    a.truncated_ = declared_size > a.bytes_.size();
    try
    {
        a.ReadTables(r);
    }
    catch (const xsflib::FormatError& e)
    {
        if (!a.truncated_)
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
    const Blocks blocks = BlockTable(r);
    const auto [info_off, info_size] = RequiredBlock(blocks, 0x2001, "INFO");
    const auto [file_off, file_size] = RequiredBlock(blocks, 0x2002, "FILE");

    // A damaged STRG block leaves the sounds unnamed, rather than stopping the archive.
    std::map<uint32_t, std::string> item_names;
    try
    {
        item_names = ItemNamesOf(r, blocks, bytes_.size(), Rules::kReadPastDamage);
    }
    catch (const xsflib::FormatError&)
    {
        names_damaged_ = true;
    }

    const std::map<uint16_t, std::size_t> tables = InfoTables(r, info_off);
    ReadItems(r, tables, item_names);
    ReadFiles(r, tables, file_off);
    ReadGroups(r, tables, item_names);
}

void SoundArchive::ReadItems(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                             const std::map<uint32_t, std::string>& names)
{
    auto read_sound = [&](uint32_t i, std::size_t p)
    {
        sounds_.push_back(
            ReadSound(r, p, i, NameIn(names, ItemId(kSoundItem, i)), options_.flavor == Flavor::kCtr, version_));
    };
    ForEachTableEntry(r, tables, 0x2100, "sound", read_sound);

    // As ReadSound does for sounds, generate missing bank and wave archive names from their indexes.
    auto read_bank = [&](uint32_t i, std::size_t p)
    {
        const std::string name = NameIn(names, ItemId(kBankItem, i));
        banks_.push_back({name.empty() ? "BANK_" + std::to_string(i) : name, r.U32(p)});
    };
    ForEachTableEntry(r, tables, 0x2101, "bank", read_bank);

    auto read_wave_archive = [&](uint32_t i, std::size_t p)
    {
        const std::string name = NameIn(names, ItemId(kWaveArchiveItem, i));
        wave_archives_.push_back({name.empty() ? "WARC_" + std::to_string(i) : name, r.U32(p)});
    };
    ForEachTableEntry(r, tables, 0x2103, "wave archive", read_wave_archive);

    // A sound group names a set of sounds, such as the wave sounds of a wave sound file. A table that can't be read
    // leaves none.
    auto read_sound_group = [&](uint32_t i, std::size_t p)
    {
        sound_groups_.push_back(ReadSoundGroup(r, p, NameIn(names, ItemId(kSoundGroupItem, i))));
    };
    if (tables.contains(0x2104))
    {
        try
        {
            ForEachTableEntry(r, tables, 0x2104, "sound group", read_sound_group);
        }
        catch (const xsflib::FormatError&)
        {
            sound_groups_.clear();
        }
    }
}

void SoundArchive::ReadFiles(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                             std::size_t file_off)
{
    // Files can be in the FILE block (0x220C), external (0x220D, a file name), or have no location (a null reference or
    // offset 0xFFFFFFFF). Files without locations may be stored in groups.
    auto read_file = [&](uint32_t, std::size_t p)
    {
        FileEntry f;
        const Reference loc = ReadRef(r, p);
        if (loc.type == 0x220C)
        {
            const std::size_t q = p + loc.offset;
            const uint32_t off = r.U32(q + 4), size = r.U32(q + 8);
            if (off != 0xFFFFFFFF)
            {
                // Keep out-of-bounds file locations so that partial groups can still be read. Only the header
                // determines whether the archive is truncated; a corrupt file table can point outside a complete
                // archive too.
                const uint64_t start = static_cast<uint64_t>(file_off) + 8 + off;
                f.offset = static_cast<std::size_t>(std::min<uint64_t>(start, bytes_.size()));
                f.size = size;
                f.internal = start + size <= bytes_.size();
                f.missing = !f.internal;
            }
        }
        else if (loc.type == 0x220D)
        {
            f.external_path = r.CString(p + loc.offset, 0x400);
        }

        f.grouped = !f.internal && !f.missing && loc.type != 0x220D;
        files_.push_back(std::move(f));
    };
    ForEachTableEntry(r, tables, 0x2106, "file", read_file);
}

void SoundArchive::ReadGroups(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                              const std::map<uint32_t, std::string>& names)
{
    // Each group has a group file listing its contents. Use the first copy of a file found in a group. Copies can
    // differ because banks may reference wave archives specific to their group, but they play the same waves. A group
    // with file ID 0xFFFFFFFF is kept outside the archive (see ExternalGroups).
    std::vector<std::pair<std::string, uint32_t>> cut_short;
    std::vector<std::string> unreadable;
    auto read_group = [&](uint32_t i, std::size_t p)
    {
        const std::string name = NameIn(names, ItemId(kGroupItem, i));
        const std::string label = name.empty() ? "GROUP_" + std::to_string(i) : name;
        const uint32_t file_id = r.U32(p);
        if (file_id < files_.size())
        {
            ReadGroup(label, files_[file_id], cut_short, unreadable);
        }
        else if (file_id == kExternalGroupFile)
        {
            external_groups_.push_back(label);
        }
    };
    if (tables.contains(0x2105))
    {
        try
        {
            ForEachTableEntry(r, tables, 0x2105, "group", read_group);
        }
        catch (const xsflib::FormatError& e)
        {
            // Keep files from groups read before the bad entry.
            unreadable.push_back(std::string("cannot read group table (") + e.what() +
                                 "); files stored only in groups may be missing");
        }
    }

    // A group that can't be read leaves out the files stored only in it. If every file stored in groups has a copy in
    // the groups read, it left out none.
    if (std::ranges::any_of(files_, [](const FileEntry& f) { return f.grouped && !f.internal; }))
    {
        group_errors_.insert(group_errors_.end(), unreadable.begin(), unreadable.end());
    }

    // Omit truncated group files unless another group has a complete copy.
    for (const auto& [group, id] : cut_short)
    {
        if (!files_[id].internal)
        {
            group_errors_.push_back("group " + group + ": file " + std::to_string(id) +
                                    " extends past the group; skipped");
        }
    }

    // Missing groups may contain files absent from the groups we could read.
    if (groups_cut_)
    {
        for (FileEntry& f : files_)
        {
            f.missing = f.missing || (f.grouped && !f.internal);
        }
    }
}

void SoundArchive::ReadGroup(const std::string& name, const FileEntry& group,
                             std::vector<std::pair<std::string, uint32_t>>& cut_short,
                             std::vector<std::string>& unreadable)
{
    if (!group.internal && !group.missing)
    {
        return; // external group file
    }

    // Read the available portion of a group in a truncated archive.
    groups_cut_ = groups_cut_ || group.missing;
    const std::span<const uint8_t> data = std::span<const uint8_t>(bytes_).subspan(
        group.offset, std::min<std::size_t>(group.size, bytes_.size() - group.offset));
    try
    {
        const xsflib::Reader g = OpenFile(data, "GRP", options_);
        auto locate = [&](uint32_t id, uint64_t start, uint32_t size)
        {
            if (id >= files_.size() || !files_[id].grouped || files_[id].internal)
            {
                return;
            }

            if (start + size <= data.size())
            {
                files_[id].internal = true;
                files_[id].missing = false;
                files_[id].offset = group.offset + static_cast<std::size_t>(start);
                files_[id].size = size;
            }
            else if (group.missing)
            {
                files_[id].missing = true;
            }
            else
            {
                cut_short.emplace_back(name, id);
            }
        };

        if (options_.group_table_first)
        {
            for (const auto& [id, start, size] : GroupItems(g))
            {
                locate(id, start, size);
            }
        }
        else
        {
            ForEachGroupItem(g, locate);
        }
    }
    catch (const xsflib::FormatError& e)
    {
        if (!group.missing)
        {
            unreadable.push_back("group " + name + " is unreadable (" + e.what() +
                                 "); omitting files stored only in this group");
        }
    }
}

void SoundArchive::ReadTablesStrictly(const xsflib::Reader& r)
{
    // On the 3DS, the message names the block that the archive lacks. On the Wii U and the Switch, both blocks are
    // checked at once.
    const bool ctr = options_.flavor == Flavor::kCtr;
    const Blocks blocks = BlockTable(r);
    if (!ctr && (!blocks.contains(0x2001) || !blocks.contains(0x2002)))
    {
        throw xsflib::FormatError("no INFO or FILE block");
    }

    version_ = r.U32(0x08);
    const auto info_off = static_cast<uint32_t>(BlockOf(blocks, 0x2001, "INFO", "the sound archive", options_).first);
    const auto file_off = static_cast<uint32_t>(BlockOf(blocks, 0x2002, "FILE", "the sound archive", options_).first);
    if (r.U32(0x0C) > bytes_.size())
    {
        truncated_ = true;
    }

    const std::map<uint32_t, std::string> item_names =
        ItemNamesOf(r, blocks, bytes_.size(), Rules::kStopAtFirstProblem);
    const std::map<uint16_t, std::size_t> tables = InfoTables(r, info_off);
    ReadItemsStrictly(r, tables, item_names);
    ReadFilesStrictly(r, tables, file_off);
    ReadGroupsStrictly(r, tables, item_names);
}

void SoundArchive::ReadItemsStrictly(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                                     const std::map<uint32_t, std::string>& names)
{
    const bool ctr = options_.flavor == Flavor::kCtr;
    const auto read_sound = [&](uint32_t i, std::size_t p)
    {
        sounds_.push_back(ReadSoundStrictly(r, p, i, NameIn(names, ItemId(kSoundItem, i)), ctr, version_));
    };
    ForEachTableEntryStrictly(r, tables, 0x2100, read_sound);

    // Banks, wave archives, players and groups keep the names the archive gives them, empty ones included.
    const auto read_bank = [&](uint32_t i, std::size_t p)
    {
        banks_.push_back({NameIn(names, ItemId(kBankItem, i)), r.U32(p)});
    };
    ForEachTableEntryStrictly(r, tables, 0x2101, read_bank);

    const auto read_wave_archive = [&](uint32_t i, std::size_t p)
    {
        wave_archives_.push_back({NameIn(names, ItemId(kWaveArchiveItem, i)), r.U32(p)});
    };
    ForEachTableEntryStrictly(r, tables, 0x2103, read_wave_archive);

    const auto read_sound_group = [&](uint32_t i, std::size_t p)
    {
        sound_groups_.push_back(ReadSoundGroup(r, p, NameIn(names, ItemId(kSoundGroupItem, i))));
    };
    ForEachTableEntryStrictly(r, tables, 0x2104, read_sound_group);

    const auto read_player = [&](uint32_t i, std::size_t p)
    {
        players_.push_back({NameIn(names, ItemId(kPlayerItem, i)), r.U32(p)});
    };
    ForEachTableEntryStrictly(r, tables, 0x2102, read_player);

    // The Wii U's and the Switch's groups are listed before the files. ReadGroupsStrictly lists the 3DS's.
    if (!ctr)
    {
        const auto list_group = [&](uint32_t i, std::size_t p)
        {
            groups_.push_back({NameIn(names, ItemId(kGroupItem, i)), r.U32(p)});
        };
        ForEachTableEntryStrictly(r, tables, 0x2105, list_group);
    }
}

void SoundArchive::ReadFilesStrictly(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                                     std::size_t file_off)
{
    // Files can be in the FILE block (0x220C), or external (0x220D, a path that the players read). A file past the
    // archive's end means a truncated archive.
    const auto read_file = [&](uint32_t, std::size_t p)
    {
        FileEntry f;
        const Reference loc = ReadRef(r, p);
        if (loc.type == 0x220C)
        {
            const std::size_t q = p + loc.offset;
            const uint32_t off = r.U32(q + 4), size = r.U32(q + 8);
            if (off != 0xFFFFFFFF)
            {
                const uint64_t start = static_cast<uint64_t>(file_off) + 8 + off;
                if (start + size <= bytes_.size())
                {
                    f.internal = true;
                    f.offset = static_cast<uint32_t>(start);
                    f.size = size;
                }
                else
                {
                    truncated_ = true; // the file lies past the end of a truncated archive
                }
            }
        }
        else if (loc.type == 0x220D)
        {
            f.external_path = r.CString(p + loc.offset, 0x400);
        }

        files_.push_back(std::move(f));
    };
    ForEachTableEntryStrictly(r, tables, 0x2106, read_file);
}

void SoundArchive::ReadGroupsStrictly(const xsflib::Reader& r, const std::map<uint16_t, std::size_t>& tables,
                                      const std::map<uint32_t, std::string>& names)
{
    const bool ctr = options_.flavor == Flavor::kCtr;

    // A group file embeds copies of other files. Some files are only there: their entries have no location, or an
    // offset of 0xFFFFFFFF. A file that the archive holds already keeps that copy.
    const auto take_copy = [&](uint32_t file_id, std::span<const uint8_t> copy)
    {
        files_[file_id] = FileEntry{};
        files_[file_id].offset = static_cast<uint32_t>(copy.data() - bytes_.data());
        files_[file_id].size = static_cast<uint32_t>(copy.size());
        files_[file_id].internal = true;
    };

    // The Wii U's and the Switch's players read a group file's whole table first.
    const auto take_cafe_copies = [&](std::span<const uint8_t> group)
    {
        for (const auto& [file_id, embedded] : GroupItemsStrictly(group, options_))
        {
            if (file_id < files_.size() && !files_[file_id].internal)
            {
                take_copy(file_id, embedded);
            }
        }
    };

    // The 3DS's player reads each item's file ID first, and the bytes of the copies of the files that the archive
    // lacks.
    const auto take_ctr_copies = [&](std::span<const uint8_t> group)
    {
        const auto lacks = [&](uint32_t file_id)
        {
            return file_id < files_.size() && !files_[file_id].internal;
        };
        WalkCtrGroup(group, options_, lacks, take_copy);
    };

    // The Wii U's and the Switch's groups are listed before the files. The 3DS's are listed here.
    const auto read_group = [&](uint32_t i, std::size_t p)
    {
        const uint32_t group_file = r.U32(p);
        if (ctr)
        {
            groups_.push_back({NameIn(names, ItemId(kGroupItem, i)), group_file});
        }

        if (group_file >= files_.size() || !files_[group_file].internal)
        {
            return; // no group file, or one outside the archive
        }

        const std::span<const uint8_t> group =
            std::span<const uint8_t>(bytes_).subspan(files_[group_file].offset, files_[group_file].size);
        if (ctr)
        {
            take_ctr_copies(group);
        }
        else
        {
            take_cafe_copies(group);
        }
    };
    ForEachTableEntryStrictly(r, tables, 0x2105, read_group);
}

void SoundArchive::AddGroupFile(std::vector<uint8_t> bytes, std::string group)
{
    // kStopAtFirstProblem takes a group file's copy of every file that the archive lacks.
    if (options_.rules == Rules::kStopAtFirstProblem)
    {
        // Moving a vector keeps its buffer. Afterwards, the items' spans point into the archive's copy of the file. On
        // the 3DS, the first copy of a file that the archive lacks is the only one whose bytes are read.
        std::vector<std::pair<uint32_t, std::span<const uint8_t>>> items;
        if (options_.flavor == Flavor::kCtr)
        {
            std::set<uint32_t> taken;
            const auto lacks = [&](uint32_t file_id)
            {
                return file_id < files_.size() && !files_[file_id].internal && !taken.contains(file_id);
            };
            const auto take = [&](uint32_t file_id, std::span<const uint8_t> copy)
            {
                items.push_back({file_id, copy});
                taken.insert(file_id);
            };
            WalkCtrGroup(bytes, options_, lacks, take);
        }
        else
        {
            items = GroupItemsStrictly(bytes, options_);
        }
        group_files_.push_back(std::move(bytes));
        group_file_groups_.push_back(std::move(group));
        const std::vector<uint8_t>& file = group_files_.back();
        const uint32_t source = static_cast<uint32_t>(group_files_.size());
        for (const auto& [file_id, data] : items)
        {
            if (file_id < files_.size() && !files_[file_id].internal)
            {
                files_[file_id] = FileEntry{};
                files_[file_id].offset = static_cast<uint32_t>(data.data() - file.data());
                files_[file_id].size = static_cast<uint32_t>(data.size());
                files_[file_id].source = source;
                files_[file_id].internal = true;
            }
        }
        return;
    }

    // Moving a vector keeps its buffer. The offsets found here stay valid in the archive's copy.
    const auto items = GroupItems(OpenFile(bytes, "GRP", options_));
    group_files_.push_back(std::move(bytes));
    group_file_groups_.push_back(std::move(group));
    const std::vector<uint8_t>& file = group_files_.back();
    const uint32_t source = static_cast<uint32_t>(group_files_.size());

    for (const auto& [id, start, size] : items)
    {
        if (id < files_.size() && files_[id].grouped && !files_[id].internal && start + size <= file.size())
        {
            files_[id] = {static_cast<std::size_t>(start), size, source, true, false, true, {}};
        }
    }
}

bool SoundArchive::MatchesGroupFile(std::span<const uint8_t> bytes) const
{
    std::vector<std::pair<uint32_t, std::span<const uint8_t>>> items;
    try
    {
        if (options_.rules == Rules::kStopAtFirstProblem)
        {
            items = GroupItemsStrictly(bytes, options_);
        }
        else
        {
            for (const auto& [id, start, size] : GroupItems(OpenFile(bytes, "GRP", options_)))
            {
                if (start + size <= bytes.size())
                {
                    items.push_back({id, bytes.subspan(static_cast<std::size_t>(start), size)});
                }
            }
        }
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }

    // The signature that the archive's tables give each file ID.
    const std::string prefix = options_.flavor == Flavor::kCtr ? "C" : "F";
    std::map<uint32_t, std::string> kinds;
    for (const SoundInfo& sound : sounds_)
    {
        if (sound.type == SoundType::kSequence || sound.type == SoundType::kWaveSound)
        {
            kinds[sound.file_id] = prefix + (sound.type == SoundType::kSequence ? "SEQ" : "WSD");
        }
    }

    for (const BankInfo& bank : banks_)
    {
        kinds[bank.file_id] = prefix + "BNK";
    }

    for (const WaveArchiveInfo& wave_archive : wave_archives_)
    {
        kinds[wave_archive.file_id] = prefix + "WAR";
    }

    bool known = false;
    for (const auto& [id, data] : items)
    {
        if (id >= files_.size())
        {
            return false;
        }

        const auto kind = kinds.find(id);
        if (kind == kinds.end())
        {
            continue;
        }

        if (data.size() < 4 || std::memcmp(data.data(), kind->second.data(), 4) != 0)
        {
            return false;
        }

        known = true;
    }

    return known;
}

std::span<const uint8_t> SoundArchive::FileData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].internal)
    {
        return {};
    }

    const FileEntry& f = files_[file_id];
    const std::vector<uint8_t>& holder = f.source != 0 ? group_files_[f.source - 1] : bytes_;
    return std::span<const uint8_t>(holder).subspan(f.offset, f.size);
}

std::optional<std::size_t> SoundArchive::FileOffset(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].internal || files_[file_id].source != 0)
    {
        return std::nullopt;
    }

    return files_[file_id].offset;
}

std::string SoundArchive::ExternalPath(uint32_t file_id) const
{
    return file_id < files_.size() ? files_[file_id].external_path : "";
}

std::vector<uint32_t> SoundArchive::GroupFileIds(std::size_t group) const
{
    const std::span<const uint8_t> file =
        group < groups_.size() ? FileData(groups_[group].file_id) : std::span<const uint8_t>();
    if (file.empty())
    {
        return {};
    }

    // INFO lists each file's ID, then where the group's FILE block holds the file. A null reference there links to the
    // archive's copy.
    const xsflib::Reader g = OpenFile(file, "GRP", options_);
    const std::size_t table = RequiredBlock(BlockTable(g), 0x7800, "INFO").first + 8;
    const uint32_t count = g.U32(table);
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < count; i++)
    {
        ids.push_back(g.U32(table + ReadRef(g, table + 4 + i * 8).offset));
    }

    return ids;
}

std::optional<uint32_t> SoundArchive::FindSound(const std::string& name) const
{
    for (uint32_t i = 0; i < sounds_.size(); i++)
    {
        if (sounds_[i].name == name)
        {
            return i;
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::sar
