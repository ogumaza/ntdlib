// SPDX-License-Identifier: MIT

// Sound archive and file readers (see formats.h).

#include "ntdlib/sar/formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

#include "ntdlib/sar/reference.h"

namespace ntdlib::sar
{
namespace
{

// A region table entry with no target. Notes in this region are silent.
constexpr std::size_t kNoTarget = static_cast<std::size_t>(-1);

// Optional parameters: 32 flag bits followed by one 32-bit value per set bit.
struct OptionParams
{
    static OptionParams Read(const xsflib::Reader& r, std::size_t off)
    {
        OptionParams p;
        const uint32_t flags = r.U32(off);
        std::size_t q = off + 4;
        for (int bit = 0; bit < 32; bit++)
        {
            if (flags >> bit & 1)
            {
                p.values[bit] = r.U32(q);
                p.value_offset[bit] = q;
                q += 4;
            }
        }

        return p;
    }

    std::array<std::optional<uint32_t>, 32> values{};
    std::array<std::size_t, 32> value_offset{};
};

// Block offsets and sizes in a NintendoWare binary file, indexed by type.
using Blocks = std::map<uint16_t, std::pair<uint32_t, uint32_t>>;

// Checks a 3DS file's magic (CSAR, CWAV, etc.) and its byte order, and returns a little-endian reader. With
// kReadPastDamage, a file too short for the fixed header fails first. With kStopAtFirstProblem, no size check comes
// first, as in the 3DS's player: a file too short for its header fails as its first read past the end does. The rule
// sets word the refusal of a Wii U or Switch archive differently.
xsflib::Reader OpenCtrFile(std::span<const uint8_t> file, const std::string& magic, const Options& options)
{
    const bool strict = options.rules == Rules::kStopAtFirstProblem;
    const xsflib::Reader r(file, false, options.reader);
    if (!strict && r.Size() < 0x14)
    {
        throw xsflib::FormatError("too short to be a " + magic + " file");
    }
    if (magic == "CSAR" && r.Size() >= 4 && r.Magic(0, "FSAR"))
    {
        throw xsflib::FormatError(strict ? "a Wii U or Switch sound archive (FSAR), not a 3DS one (CSAR)"
                                         : "unsupported FSAR archive (Wii U/Switch); expected CSAR (3DS)");
    }
    if (!r.Magic(0, magic.c_str()))
    {
        throw xsflib::FormatError("bad magic, expected " + magic);
    }
    if (r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("unsupported byte order");
    }

    return r;
}

// Checks a Wii U or Switch file's size, its magic (FSAR, FWAV, etc.) and its byte order mark, and returns a reader in
// the file's byte order. The mark reads 0xFEFF in that order. The rule sets word the refusals of a short file and of
// another console's archive differently.
xsflib::Reader OpenCafeFile(std::span<const uint8_t> file, const std::string& magic, const Options& options)
{
    const bool strict = options.rules == Rules::kStopAtFirstProblem;
    const std::string console = options.flavor == Flavor::kNx ? "Switch" : "Wii U";
    const xsflib::Reader probe(file, true, options.reader);
    if (probe.Size() < 0x14)
    {
        throw xsflib::FormatError(strict ? "too short for " + magic : "too short to be an " + magic + " file");
    }
    if (magic == "FSAR" && probe.Magic(0, "CSAR"))
    {
        throw xsflib::FormatError(strict ? "a 3DS sound archive (CSAR), not a " + console + " one"
                                         : "unsupported CSAR archive (3DS); expected FSAR (" + console + ")");
    }
    if (magic == "FSAR" && probe.Magic(0, "RSAR"))
    {
        throw xsflib::FormatError(strict ? "a Wii sound archive (RSAR), not a " + console + " one"
                                         : "unsupported RSAR archive (Wii); expected FSAR (" + console + ")");
    }
    if (!probe.Magic(0, magic.c_str()))
    {
        throw xsflib::FormatError("bad magic, expected " + magic);
    }

    const uint16_t mark = probe.U16(4);
    if (mark != 0xFEFF && mark != 0xFFFE)
    {
        throw xsflib::FormatError("unknown byte order mark");
    }

    return xsflib::Reader(file, mark == 0xFEFF, options.reader);
}

// Checks the header of a file of kind `kind` (SAR, WAV, etc.) and returns a reader in the file's byte order. The magic
// is the kind with C before it on the 3DS, and F on the Wii U and the Switch.
xsflib::Reader OpenFile(std::span<const uint8_t> file, const char* kind, const Options& options)
{
    const std::string magic = std::string(options.flavor == Flavor::kCtr ? "C" : "F") + kind;

    return options.flavor == Flavor::kCtr ? OpenCtrFile(file, magic, options) : OpenCafeFile(file, magic, options);
}

// The block table of a NintendoWare binary file: {type, offset, size} entries after a 0x14-byte header.
Blocks BlockTable(const xsflib::Reader& r)
{
    const uint16_t count = r.U16(0x10);
    Blocks blocks;
    for (uint16_t i = 0; i < count; i++)
    {
        const std::size_t e = 0x14 + i * 12;
        blocks[r.U16(e)] = {r.U32(e + 4), r.U32(e + 8)};
    }

    return blocks;
}

// Returns a required block's offset and size as std::size_t. On 64-bit builds, derived offsets cannot wrap at 32 bits
// and point back into the file. On 32-bit builds they can. Corrupt files may then produce misleading errors.
std::pair<std::size_t, std::size_t> RequiredBlock(const Blocks& blocks, uint16_t type, const char* name)
{
    const auto it = blocks.find(type);
    if (it == blocks.end())
    {
        throw xsflib::FormatError(std::string("no ") + name + " block");
    }

    return it->second;
}

// Returns the offset and size of block `type`, named `name` (such as INFO), in a file that kStopAtFirstProblem's
// messages on the 3DS call `what` (such as "a wave (CWAV)"). A missing block throws FormatError. With
// kStopAtFirstProblem on the 3DS, the message names the file: "a wave (CWAV) has no INFO block". Otherwise the message
// is "no INFO block".
std::pair<std::size_t, std::size_t> BlockOf(const Blocks& blocks, uint16_t type, const char* name, const char* what,
                                            const Options& options)
{
    if (options.rules == Rules::kReadPastDamage || options.flavor != Flavor::kCtr)
    {
        return RequiredBlock(blocks, type, name);
    }

    const auto it = blocks.find(type);
    if (it == blocks.end())
    {
        throw xsflib::FormatError(std::string(what) + " has no " + name + " block");
    }

    return it->second;
}

// The envelope that the reference at `ref_at` points to.
Adshr ReadAdshr(const xsflib::Reader& r, std::size_t ref_at)
{
    const std::size_t a = ref_at + ReadRef(r, ref_at).offset;
    return {r.U8(a), r.U8(a + 1), r.U8(a + 2), r.U8(a + 3), r.U8(a + 4)};
}

// Reads a velocity region at `p`: its wave and optional playback parameters. A bank's velocity regions and a wave
// sound's notes share the layout.
VelocityRegion ReadVelocityRegion(const xsflib::Reader& r, std::size_t p)
{
    // nw::snd BankFileReader (code.bin 0x48e2f8 and accessors 0x490b28..0x490cc4 on the 3DS).
    VelocityRegion v;
    v.wave_id_index = r.U32(p);
    const OptionParams opt = OptionParams::Read(r, p + 4);

    if (opt.values[0])
    {
        v.original_key = static_cast<uint8_t>(*opt.values[0]);
    }

    if (opt.values[1])
    {
        v.volume = static_cast<uint8_t>(*opt.values[1]);
    }

    if (opt.values[2])
    {
        v.pan = static_cast<uint8_t>(*opt.values[2]);
    }

    if (opt.values[3])
    {
        v.pitch = r.F32(opt.value_offset[3]);
    }

    if (opt.values[4])
    {
        v.ignore_note_off = (*opt.values[4] & 0xff) != 0;
        v.key_group = static_cast<uint8_t>(*opt.values[4] >> 8);
        v.interpolation = static_cast<uint8_t>(*opt.values[4] >> 16);
    }

    if (opt.values[9])
    {
        // The value points to a {type, offset} reference. The ADSHR data is at that reference plus its offset.
        v.adshr = ReadAdshr(r, p + *opt.values[9]);
    }

    return v;
}

// Reads a direct, range or index region table. Returns each region's lower and upper bounds and its location, or
// kNoTarget. A direct table's one region reaches `direct_top`. With `wrap`, a range table's lower bounds are bytes, as
// with kReadPastDamage: the bound after an upper bound of 255 is 0.
std::vector<std::tuple<int, int, std::size_t>> ReadRegionTable(const xsflib::Reader& r, std::size_t p, int direct_top,
                                                               bool wrap)
{
    const Reference ref = ReadRef(r, p);
    const std::size_t q = p + ref.offset;

    auto target = [&](std::size_t ref_off, std::size_t base) -> std::size_t
    {
        const Reference t = ReadRef(r, ref_off);
        if (t.offset == -1 || t.type == 0x5903)
        {
            return kNoTarget;
        }

        return base + t.offset;
    };

    std::vector<std::tuple<int, int, std::size_t>> out;
    switch (ref.type)
    {
    case 0x6000: // direct
        out.emplace_back(0, direct_top, target(q, q));
        break;

    case 0x6001: // range: count, upper keys, refs
        {
            const uint32_t n = r.U32(q);
            const std::size_t refs = q + 4 + ((n + 3) & ~3u);
            int lo = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                const int hi = r.U8(q + 4 + i);
                out.emplace_back(lo, hi, target(refs + i * 8, q));
                lo = wrap ? static_cast<uint8_t>(hi + 1) : hi + 1;
            }
            break;
        }

    case 0x6002: // index: min, max, refs
        {
            const uint8_t mn = r.U8(q), mx = r.U8(q + 1);
            for (int k = mn; k <= mx; k++)
            {
                out.emplace_back(k, k, target(q + 4 + (k - mn) * 8, q));
            }
            break;
        }

    default:
        throw xsflib::FormatError("unknown region table type");
    }

    return out;
}

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

// A send value at `at`: the main send, then a send to each aux bus.
void ReadSends(const xsflib::Reader& r, std::size_t at, uint8_t& main_send, std::array<uint8_t, 3>& aux_sends)
{
    main_send = r.U8(at);
    for (std::size_t bus = 0; bus < aux_sends.size(); ++bus)
    {
        aux_sends[bus] = r.U8(at + 1 + bus);
    }
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

// The type of the sound at the INFO entry at `p`, and the details of that type, from the entry's reference `detail`. A
// stream sound's details are read only in FSAR archives, of version `version`.
void ReadSoundDetails(const xsflib::Reader& r, std::size_t p, const Reference& detail, bool ctr, uint32_t version,
                      SoundInfo& s)
{
    switch (detail.type)
    {
    case 0x2201:
        s.type = SoundType::kStream;
        if (!ctr)
        {
            ReadStreamDetails(r, p + detail.offset, version, s.stream);
        }
        break;

    case 0x2202:
        {
            // The wave sound's details start with its index in its wave sound file and its tracks. On the Wii U and the
            // Switch, the options that follow give the channel's priority and the release priority fix
            // (SoundArchiveFile::WaveSoundInfo's GetChannelPriority and GetIsReleasePriorityFix).
            s.type = SoundType::kWaveSound;
            const std::size_t q = p + detail.offset;
            s.wave_sound_index = r.U32(q);
            if (!ctr)
            {
                const OptionParams so = OptionParams::Read(r, q + 8);
                if (so.values[0])
                {
                    s.channel_priority = static_cast<uint8_t>(*so.values[0]);
                    s.release_priority_fix = ((*so.values[0] >> 8) & 0xff) != 0;
                }
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
// AddGroupFile and MatchesGroupFile read a 3DS group file this way too. ReadTablesStrictly walks the 3DS's groups as
// its player does.
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

} // namespace

Wave Wave::Parse(std::span<const uint8_t> file, const Options& options)
{
    const xsflib::Reader r = OpenFile(file, "WAV", options);
    const Blocks blocks = BlockTable(r);
    const auto [info_off, info_size] = BlockOf(blocks, 0x7000, "INFO", "a wave (CWAV)", options);
    const auto [data_off, data_size] = BlockOf(blocks, 0x7001, "DATA", "a wave (CWAV)", options);
    const bool strict = options.rules == Rules::kStopAtFirstProblem;

    const std::size_t b = info_off + 8;
    Wave w;
    w.encoding = static_cast<WaveEncoding>(r.U8(b));
    w.big_endian = r.BigEndian();
    w.loop = r.U8(b + 1) != 0;
    w.sample_rate = r.U32(b + 4);
    w.loop_start = r.U32(b + 8);
    w.loop_end = r.U32(b + 12);

    const std::size_t table = b + 0x14;
    const uint32_t nch = r.U32(table);
    for (uint32_t i = 0; i < nch; i++)
    {
        const Reference ref = ReadRef(r, table + 4 + i * 8);
        const std::size_t ci = table + ref.offset;
        const Reference sample_ref = ReadRef(r, ci);
        const Reference adpcm_ref = ReadRef(r, ci + 8);
        WaveChannel ch;
        const std::size_t sample_off = data_off + 8 + sample_ref.offset;
        ch.data = r.Sub(sample_off, data_off + data_size - sample_off);

        if (adpcm_ref.type == 0x0300)
        {
            const std::size_t a = ci + adpcm_ref.offset;
            DspAdpcmInfo info;
            for (int k = 0; k < 16; k++)
            {
                info.coefs[k] = r.S16(a + k * 2);
            }

            // kStopAtFirstProblem also reads the first frame's predictor and scale, and the decoder's state at the
            // loop.
            if (strict)
            {
                info.pred_scale = r.U16(a + 32);
            }
            info.yn1 = r.S16(a + 34);
            info.yn2 = r.S16(a + 36);
            if (strict)
            {
                info.loop_pred_scale = r.U16(a + 38);
                info.loop_yn1 = r.S16(a + 40);
                info.loop_yn2 = r.S16(a + 42);
            }
            ch.adpcm = info;
        }
        else if (adpcm_ref.type == 0x0301 && options.flavor == Flavor::kCtr)
        {
            const std::size_t a = ci + adpcm_ref.offset;
            ImaAdpcmInfo info;
            info.sample = r.S16(a);
            info.step_index = r.U8(a + 2);
            ch.ima = info;
        }

        w.channels.push_back(ch);
    }

    return w;
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> file, const Options& options)
{
    const xsflib::Reader r = OpenFile(file, "WAR", options);
    const Blocks blocks = BlockTable(r);
    const auto [info_off, info_size] = BlockOf(blocks, 0x6800, "INFO", "a wave archive (CWAR)", options);
    const auto [file_off, file_size] = BlockOf(blocks, 0x6801, "FILE", "a wave archive (CWAR)", options);

    WaveArchive war;
    const uint32_t n = r.U32(info_off + 8);
    for (uint32_t i = 0; i < n; i++)
    {
        const std::size_t e = info_off + 12 + i * 12;
        const uint32_t off = r.U32(e + 4);
        const uint32_t size = r.U32(e + 8);
        war.waves.push_back(r.Sub(file_off + 8 + off, size));
    }

    return war;
}

const VelocityRegion* Instrument::Find(int key, int velocity) const
{
    for (const auto& k : keys)
    {
        if (key < k.lo || key > k.hi)
        {
            continue;
        }

        // The first matching key region applies even if none of its velocity ranges match.
        for (const auto& v : k.velocities)
        {
            if (velocity >= v.lo && velocity <= v.hi)
            {
                return v.region ? &*v.region : nullptr;
            }
        }

        return nullptr;
    }

    return nullptr;
}

Bank Bank::Parse(std::span<const uint8_t> file, const Options& options)
{
    const xsflib::Reader r = OpenFile(file, "BNK", options);
    const Blocks blocks = BlockTable(r);
    const auto [info_off, info_size] = BlockOf(blocks, 0x5800, "INFO", "a bank (CBNK)", options);
    const bool strict = options.rules == Rules::kStopAtFirstProblem;

    const std::size_t b = info_off + 8;
    std::size_t wave_table = 0, inst_table = 0;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        if (ref.type == 0x0100)
        {
            wave_table = b + ref.offset;
        }
        else if (ref.type == 0x0101)
        {
            inst_table = b + ref.offset;
        }
    }

    Bank bank;
    const uint32_t nw = r.U32(wave_table);
    for (uint32_t i = 0; i < nw; i++)
    {
        bank.waves.push_back({r.U32(wave_table + 4 + i * 8), r.U32(wave_table + 8 + i * 8)});
    }

    // A bank has fewer regions than bytes. Shared table entries in a corrupt bank could otherwise allocate more regions
    // than fit in memory. With kStopAtFirstProblem, each region counts as the reader comes to it. With kReadPastDamage,
    // a key region's regions count once they've been read.
    std::size_t regions = 0;
    const auto count_region = [&]
    {
        if (++regions > file.size())
        {
            throw xsflib::FormatError("the bank has more regions than bytes");
        }
    };

    const uint32_t ni = r.U32(inst_table);
    for (uint32_t i = 0; i < ni; i++)
    {
        const Reference ref = ReadRef(r, inst_table + 4 + i * 8);
        if (ref.type != 0x5900)
        {
            bank.instruments.emplace_back();
            continue;
        }

        Instrument inst;
        for (auto [lo, hi, key_off] :
             ReadRegionTable(r, inst_table + ref.offset, strict ? KeyRegion::kAnyValue : 127, !strict))
        {
            if (strict)
            {
                count_region();
            }

            KeyRegion k;
            k.lo = lo;
            k.hi = hi;

            // A direct velocity table's region covers every velocity. The game's lookup returns the region without
            // comparing the value: code.bin 0x490a24 on the 3DS, GetRegion (0x022BBA30) on the Wii U and
            // Bank::NoteOn's lookup (0xc02290) on the Switch. A note above velocity 127, through the velocity range
            // command (0xb3) or a velocity byte above 127, still plays that region.
            if (key_off != kNoTarget)
            {
                for (auto [vlo, vhi, vel_off] : ReadRegionTable(r, key_off, KeyRegion::kAnyValue, !strict))
                {
                    if (strict)
                    {
                        count_region();
                    }

                    KeyRegion::Vel v{vlo, vhi, std::nullopt};
                    if (vel_off != kNoTarget)
                    {
                        v.region = ReadVelocityRegion(r, vel_off);
                    }

                    k.velocities.push_back(v);
                }
            }

            if (!strict)
            {
                regions += 1 + k.velocities.size();
                if (regions > file.size())
                {
                    throw xsflib::FormatError("invalid region tables (region count exceeds bank size in bytes)");
                }
            }

            inst.keys.push_back(std::move(k));
        }

        bank.instruments.push_back(std::move(inst));
    }

    return bank;
}

WaveSoundFile WaveSoundFile::Parse(std::span<const uint8_t> file, const Options& options)
{
    const xsflib::Reader r = OpenFile(file, "WSD", options);
    const Blocks blocks = BlockTable(r);
    const auto [info_off, info_size] = BlockOf(blocks, 0x6800, "INFO", "a wave sound file (CWSD)", options);

    // INFO holds a reference to the table of the waves and one to the table of the wave sounds.
    const std::size_t b = info_off + 8;
    std::optional<std::size_t> wave_table;
    std::optional<std::size_t> sound_table;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, b + i * 8);
        if (ref.type == 0x0100)
        {
            wave_table = b + ref.offset;
        }
        else if (ref.type == 0x0101)
        {
            sound_table = b + ref.offset;
        }
    }
    if (!wave_table || !sound_table)
    {
        throw xsflib::FormatError("a wave sound file without its tables of waves and wave sounds");
    }

    WaveSoundFile wsd;
    const uint32_t waves = r.U32(*wave_table);
    for (uint32_t i = 0; i < waves; i++)
    {
        wsd.waves.push_back({r.U32(*wave_table + 4 + i * 8), r.U32(*wave_table + 8 + i * 8)});
    }

    // A table of references: a count, then each reference, from the table's start. Tables can share their entries. Each
    // entry counts, and a file has fewer of them than bytes.
    std::size_t entries = 0;
    const auto table = [&](std::size_t at)
    {
        std::vector<std::size_t> targets;
        const uint32_t count = r.U32(at);
        for (uint32_t i = 0; i < count; i++)
        {
            if (++entries > file.size())
            {
                throw xsflib::FormatError("the wave sound file has more entries than bytes");
            }
            targets.push_back(at + ReadRef(r, at + 4 + i * 8).offset);
        }

        return targets;
    };

    // Each wave sound: references to its settings, its tracks and its notes. The settings are optional parameters: the
    // pan and the surround pan in the first, the pitch in the second, the filters in the third, the offset of the sends
    // in the ninth, and the offset of a reference to the envelope in the tenth. The sends hold the main send, the
    // number of aux sends and the aux sends. The Wii U's and the Switch's readers read up to 3 aux sends, and the
    // filters from version 1.1.0 on (WaveSoundFileReader, 0x022B95E4 in Animal Crossing: amiibo Festival and 0xc285d0
    // in WarioWare: Get It Together!'s main).
    const bool ctr = options.flavor == Flavor::kCtr;
    const bool filters = !ctr && r.U32(0x08) >= 0x00010100;
    for (const std::size_t sound_at : table(*sound_table))
    {
        WaveSound sound;
        const std::size_t settings = sound_at + ReadRef(r, sound_at).offset;
        const OptionParams opt = OptionParams::Read(r, settings);
        if (opt.values[0])
        {
            sound.pan = static_cast<uint8_t>(*opt.values[0]);
            if (!ctr)
            {
                sound.span = static_cast<uint8_t>(*opt.values[0] >> 8);
            }
        }
        if (opt.values[1])
        {
            sound.pitch = r.F32(opt.value_offset[1]);
        }
        if (opt.values[2] && filters)
        {
            sound.low_pass = static_cast<uint8_t>(*opt.values[2]);
            sound.biquad_type = static_cast<uint8_t>(*opt.values[2] >> 8);
            sound.biquad_value = static_cast<uint8_t>(*opt.values[2] >> 16);
        }
        if (opt.values[8] && !ctr)
        {
            const std::size_t sends = settings + *opt.values[8];
            sound.main_send = r.U8(sends);
            const std::size_t count = std::min<std::size_t>(r.U8(sends + 1), sound.aux_sends.size());
            for (std::size_t bus = 0; bus < count; ++bus)
            {
                sound.aux_sends[bus] = r.U8(sends + 2 + bus);
            }
        }
        if (opt.values[9])
        {
            sound.adshr = ReadAdshr(r, settings + *opt.values[9]);
        }

        // A track holds a reference to the table of its note events. An event holds its position, its length and its
        // note.
        for (const std::size_t track_at : table(sound_at + ReadRef(r, sound_at + 8).offset))
        {
            std::vector<NoteEvent> events;
            for (const std::size_t event : table(track_at + ReadRef(r, track_at).offset))
            {
                events.push_back({r.F32(event), r.F32(event + 4), r.U32(event + 8)});
            }
            sound.tracks.push_back(std::move(events));
        }
        for (const std::size_t note : table(sound_at + ReadRef(r, sound_at + 16).offset))
        {
            sound.notes.push_back(ReadVelocityRegion(r, note));
        }

        wsd.sounds.push_back(std::move(sound));
    }

    return wsd;
}

Sequence Sequence::Parse(std::span<const uint8_t> file, const Options& options)
{
    const xsflib::Reader r = OpenFile(file, "SEQ", options);
    const Blocks blocks = BlockTable(r);
    const auto [data_off, data_size] = BlockOf(blocks, 0x5000, "DATA", "a sequence (CSEQ)", options);

    Sequence s;
    s.data = r.Sub(data_off + 8, data_size - 8);
    s.big_endian = r.BigEndian();

    // On the Wii U and the Switch, kStopAtFirstProblem reads LABL: a table of references to labels, each a reference
    // into DATA, the name's length and the name.
    const auto labels = blocks.find(0x5001);
    if (options.rules == Rules::kStopAtFirstProblem && options.flavor != Flavor::kCtr && labels != blocks.end())
    {
        const std::size_t b = labels->second.first + 8;
        const uint32_t n = r.U32(b);
        for (uint32_t i = 0; i < n; i++)
        {
            const std::size_t e = b + ReadRef(r, b + 4 + i * 8).offset;
            const Reference at = ReadRef(r, e);
            const uint32_t length = r.U32(e + 8);
            s.labels.push_back({r.CString(e + 12, length), static_cast<uint32_t>(at.offset)});
        }
    }

    return s;
}

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

    // Archives without names have a STRG offset of 0xFFFFFFFF. A damaged STRG block leaves the sounds unnamed, rather
    // than stopping the archive.
    std::map<uint32_t, std::string> item_names;
    const auto strg = blocks.find(0x2000);
    if (strg != blocks.end() && strg->second.first != 0xFFFFFFFF && strg->second.first < bytes_.size())
    {
        try
        {
            item_names = ReadItemNames(r, strg->second.first, strg->second.second, Rules::kReadPastDamage);
        }
        catch (const xsflib::FormatError&)
        {
            names_damaged_ = true;
        }
    }

    auto name_of = [&](uint32_t item) -> std::string
    {
        const auto it = item_names.find(item);
        return it == item_names.end() ? std::string() : it->second;
    };

    const std::map<uint16_t, std::size_t> tables = InfoTables(r, info_off);

    // Calls `read` with each table entry's index and position.
    auto for_each_entry = [&](uint16_t type, const char* name, auto read)
    {
        const auto table = tables.find(type);
        if (table == tables.end())
        {
            throw xsflib::FormatError(std::string("no ") + name + " table");
        }

        ForEachEntry(r, table->second, read);
    };

    auto read_sound = [&](uint32_t i, std::size_t p)
    {
        sounds_.push_back(
            ReadSound(r, p, i, name_of(ItemId(kSoundItem, i)), options_.flavor == Flavor::kCtr, version_));
    };
    for_each_entry(0x2100, "sound", read_sound);

    // As ReadSound does for sounds, generate missing bank and wave archive names from their indexes.
    auto read_bank = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(ItemId(kBankItem, i));
        banks_.push_back({name.empty() ? "BANK_" + std::to_string(i) : name, r.U32(p)});
    };
    for_each_entry(0x2101, "bank", read_bank);

    auto read_wave_archive = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(ItemId(kWaveArchiveItem, i));
        wave_archives_.push_back({name.empty() ? "WARC_" + std::to_string(i) : name, r.U32(p)});
    };
    for_each_entry(0x2103, "wave archive", read_wave_archive);

    // A sound group names a set of sounds, such as the wave sounds of a wave sound file. A table that can't be read
    // leaves none.
    auto read_sound_group = [&](uint32_t i, std::size_t p)
    {
        sound_groups_.push_back(ReadSoundGroup(r, p, name_of(ItemId(kSoundGroupItem, i))));
    };
    if (tables.contains(0x2104))
    {
        try
        {
            for_each_entry(0x2104, "sound group", read_sound_group);
        }
        catch (const xsflib::FormatError&)
        {
            sound_groups_.clear();
        }
    }

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
        else if (loc.type == 0x220D && options_.flavor != Flavor::kCtr)
        {
            f.external_path = r.CString(p + loc.offset, 0x400);
        }

        f.grouped = !f.internal && !f.missing && loc.type != 0x220D;
        files_.push_back(f);
    };
    for_each_entry(0x2106, "file", read_file);

    // Each group has a group file listing its contents. Use the first copy of a file found in a group. Copies can
    // differ because banks may reference wave archives specific to their group, but they play the same waves. A group
    // with file ID 0xFFFFFFFF is kept outside the archive (see ExternalGroups).
    std::vector<std::pair<std::string, uint32_t>> cut_short;
    std::vector<std::string> unreadable;
    auto read_group = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(ItemId(kGroupItem, i));
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
            for_each_entry(0x2105, "group", read_group);
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

    // STRG: a string table, and a Patricia tree that maps names to item IDs. An archive may be built without it (no
    // names; the block's offset is then 0xFFFFFFFF).
    std::map<uint32_t, std::string> item_names;
    const auto strg = blocks.find(0x2000);
    if (strg != blocks.end() && strg->second.first != 0xFFFFFFFF && strg->second.first < bytes_.size())
    {
        item_names = ReadItemNames(r, strg->second.first, strg->second.second, Rules::kStopAtFirstProblem);
    }

    const auto name_of = [&](uint32_t item) -> std::string
    {
        auto it = item_names.find(item);
        return it == item_names.end() ? std::string() : it->second;
    };

    const std::map<uint16_t, std::size_t> tables = InfoTables(r, info_off);

    // Calls `fn` with each table entry's index and position. A missing table throws FormatError.
    const auto for_each = [&](uint16_t type, auto fn)
    {
        const auto table = tables.find(type);
        if (table == tables.end())
        {
            throw xsflib::FormatError("the sound archive's INFO block has no table of type " +
                                      xsflib::Hex(type, 4, true));
        }

        ForEachEntry(r, table->second, fn);
    };

    const auto read_sound = [&](uint32_t i, std::size_t p)
    {
        SoundInfo s;
        s.name = name_of(ItemId(kSoundItem, i));
        s.file_id = r.U32(p);
        if (!ctr)
        {
            s.player_id = r.U32(p + 4);
        }
        s.volume = r.U8(p + 8);
        const Reference detail = ReadRef(r, p + 12);
        const OptionParams opt = OptionParams::Read(r, p + 20);
        if (opt.values[1])
        {
            s.pan_mode = static_cast<uint8_t>(*opt.values[1]);
            s.pan_curve = static_cast<uint8_t>(*opt.values[1] >> 8);
        }

        if (!ctr && opt.values[2])
        {
            s.player_priority = static_cast<uint8_t>(*opt.values[2]);
            s.actor_player_id = static_cast<uint8_t>(*opt.values[2] >> 8);
        }

        ReadSoundDetails(r, p, detail, ctr, version_, s);
        NameUnnamedSound(s, i);
        sounds_.push_back(std::move(s));
    };
    for_each(0x2100, read_sound);

    // Banks, wave archives, players and groups keep the names the archive gives them, empty ones included.
    const auto read_bank = [&](uint32_t i, std::size_t p)
    {
        banks_.push_back({name_of(ItemId(kBankItem, i)), r.U32(p)});
    };
    for_each(0x2101, read_bank);

    const auto read_wave_archive = [&](uint32_t i, std::size_t p)
    {
        wave_archives_.push_back({name_of(ItemId(kWaveArchiveItem, i)), r.U32(p)});
    };
    for_each(0x2103, read_wave_archive);

    const auto read_sound_group = [&](uint32_t i, std::size_t p)
    {
        sound_groups_.push_back(ReadSoundGroup(r, p, name_of(ItemId(kSoundGroupItem, i))));
    };
    for_each(0x2104, read_sound_group);

    if (!ctr)
    {
        const auto read_player = [&](uint32_t i, std::size_t p)
        {
            players_.push_back({name_of(ItemId(kPlayerItem, i)), r.U32(p)});
        };
        for_each(0x2102, read_player);

        const auto list_group = [&](uint32_t i, std::size_t p)
        {
            groups_.push_back({name_of(ItemId(kGroupItem, i)), r.U32(p)});
        };
        for_each(0x2105, list_group);
    }

    // Files can be in the FILE block (0x220C), or external (0x220D, a path that the Wii U's and the Switch's players
    // read). A file past the archive's end means a truncated archive.
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
        else if (loc.type == 0x220D && !ctr)
        {
            f.external_path = r.CString(p + loc.offset, 0x400);
        }

        files_.push_back(f);
    };
    for_each(0x2106, read_file);

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

    // The 3DS's player reads each item's file ID first, and the bytes of the items it takes.
    const auto take_ctr_copies = [&](std::span<const uint8_t> group)
    {
        const xsflib::Reader g = OpenFile(group, "GRP", options_);
        const Blocks group_blocks = BlockTable(g);
        const auto items =
            static_cast<uint32_t>(BlockOf(group_blocks, 0x7800, "INFO", "a group (CGRP)", options_).first + 8);
        const auto data =
            static_cast<uint32_t>(BlockOf(group_blocks, 0x7801, "FILE", "a group (CGRP)", options_).first + 8);
        const uint32_t n = g.U32(items);
        for (uint32_t i = 0; i < n; i++)
        {
            const std::size_t e = items + ReadRef(g, items + 4 + i * 8).offset;
            const uint32_t file_id = g.U32(e);
            const Reference where = ReadRef(g, e + 4);
            if (where.type != 0x1F00 || file_id >= files_.size() || files_[file_id].internal)
            {
                continue; // not embedded, or the archive holds it already
            }

            take_copy(file_id, g.Sub(data + where.offset, g.U32(e + 12)));
        }
    };

    const auto read_group = [&](uint32_t, std::size_t p)
    {
        const uint32_t group_file = r.U32(p);
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
    for_each(0x2105, read_group);
}

void SoundArchive::AddGroupFile(std::vector<uint8_t> bytes)
{
    // kStopAtFirstProblem takes a group file's copy of every file that the archive lacks.
    if (options_.rules == Rules::kStopAtFirstProblem)
    {
        // Moving a vector keeps its buffer. Afterwards, the items' spans point into the archive's copy of the file.
        const auto items = GroupItemsStrictly(bytes, options_);
        group_files_.push_back(std::move(bytes));
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
