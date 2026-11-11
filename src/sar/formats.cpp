// SPDX-License-Identifier: MIT

// The readers of the files in a sound archive (see formats.h). sound_archive.cpp reads the archive.

#include "ntdlib/sar/formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/reference.h"
#include "sar/reading.h"

namespace ntdlib::sar
{
namespace
{

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
    const SoundArchiveKind kind =
        SoundArchiveKindOf(std::string_view(reinterpret_cast<const char*>(file.data()), file.size()));
    if (magic == "FSAR" && kind == SoundArchiveKind::kCsar)
    {
        throw xsflib::FormatError(strict ? "a 3DS sound archive (CSAR), not a " + console + " one"
                                         : "unsupported CSAR archive (3DS); expected FSAR (" + console + ")");
    }
    if (magic == "FSAR" && kind == SoundArchiveKind::kRsar)
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

} // namespace

namespace detail
{

xsflib::Reader OpenFile(std::span<const uint8_t> file, const char* kind, const Options& options)
{
    const std::string magic = std::string(options.flavor == Flavor::kCtr ? "C" : "F") + kind;

    return options.flavor == Flavor::kCtr ? OpenCtrFile(file, magic, options) : OpenCafeFile(file, magic, options);
}

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

std::pair<std::size_t, std::size_t> RequiredBlock(const Blocks& blocks, uint16_t type, const char* name)
{
    const auto it = blocks.find(type);
    if (it == blocks.end())
    {
        throw xsflib::FormatError(std::string("no ") + name + " block");
    }

    return it->second;
}

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

} // namespace detail

namespace
{

// A region table entry with no target. Notes in this region are silent.
constexpr std::size_t kNoTarget = static_cast<std::size_t>(-1);

using detail::BlockOf;
using detail::Blocks;
using detail::BlockTable;
using detail::OpenFile;
using detail::OptionParams;

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

// Where INFO's body at `body`, in a bank or a wave sound file, refers to its table of waves and to its table of
// instruments or wave sounds. Nothing for a table that it doesn't refer to.
std::pair<std::optional<std::size_t>, std::optional<std::size_t>> WaveAndItemTables(const xsflib::Reader& r,
                                                                                    std::size_t body)
{
    std::optional<std::size_t> waves;
    std::optional<std::size_t> items;
    for (int i = 0; i < 2; i++)
    {
        const Reference ref = ReadRef(r, body + i * 8);
        if (ref.type == 0x0100)
        {
            waves = body + ref.offset;
        }
        else if (ref.type == 0x0101)
        {
            items = body + ref.offset;
        }
    }

    return {waves, items};
}

// The table of waves at `table`: a count, then a wave archive's item ID and a wave's index for each wave.
std::vector<WaveId> ReadWaveIds(const xsflib::Reader& r, std::size_t table)
{
    std::vector<WaveId> waves;
    const uint32_t count = r.U32(table);
    for (uint32_t i = 0; i < count; i++)
    {
        waves.push_back({r.U32(table + 4 + i * 8), r.U32(table + 8 + i * 8)});
    }

    return waves;
}

// The instrument whose table of key regions is at `at`, in a bank of `file_size` bytes. `regions` counts the regions
// that the reader has come to. A bank has fewer regions than bytes. Shared table entries in a corrupt bank could
// otherwise allocate more regions than fit in memory. With kStopAtFirstProblem (`strict`), each region counts as the
// reader comes to it. With kReadPastDamage, a key region's regions count once they've been read.
Instrument ReadInstrument(const xsflib::Reader& r, std::size_t at, bool strict, std::size_t file_size,
                          std::size_t& regions)
{
    const auto count_region = [&]
    {
        if (++regions > file_size)
        {
            throw xsflib::FormatError("the bank has more regions than bytes");
        }
    };

    Instrument inst;
    for (auto [lo, hi, key_off] : ReadRegionTable(r, at, strict ? KeyRegion::kAnyValue : 127, !strict))
    {
        if (strict)
        {
            count_region();
        }

        KeyRegion k;
        k.lo = lo;
        k.hi = hi;

        // A direct velocity table's region covers every velocity. The game's lookup returns the region without
        // comparing the value: code.bin 0x490a24 on the 3DS, GetRegion (0x022BBA30) on the Wii U and Bank::NoteOn's
        // lookup (0xc02290) on the Switch. A note above velocity 127, through the velocity range command (0xb3) or a
        // velocity byte above 127, still plays that region.
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
            if (regions > file_size)
            {
                throw xsflib::FormatError("invalid region tables (region count exceeds bank size in bytes)");
            }
        }

        inst.keys.push_back(std::move(k));
    }

    return inst;
}

// Reads the settings at `settings` of a wave sound into `sound`: optional parameters, with the pan and the surround
// pan in the first, the pitch in the second, the filters in the third, the offset of the sends in the ninth, and the
// offset of a reference to the envelope in the tenth. The sends hold the main send, the number of aux sends and the
// aux sends. The reader reads the filters only with `filters`, and up to `most_aux_sends` aux sends.
void ReadWaveSoundSettings(const xsflib::Reader& r, std::size_t settings, bool filters, std::size_t most_aux_sends,
                           WaveSoundFile::WaveSound& sound)
{
    const OptionParams opt = OptionParams::Read(r, settings);
    if (opt.values[0])
    {
        sound.pan = static_cast<uint8_t>(*opt.values[0]);
        sound.span = static_cast<uint8_t>(*opt.values[0] >> 8);
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
    if (opt.values[8])
    {
        const std::size_t sends = settings + *opt.values[8];
        sound.main_send = r.U8(sends);
        const std::size_t count = std::min<std::size_t>(r.U8(sends + 1), most_aux_sends);
        for (std::size_t bus = 0; bus < count; ++bus)
        {
            sound.aux_sends[bus] = r.U8(sends + 2 + bus);
        }
    }
    if (opt.values[9])
    {
        sound.adshr = ReadAdshr(r, settings + *opt.values[9]);
    }
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
    const auto [wave_table, inst_table] = WaveAndItemTables(r, info_off + 8);

    Bank bank;
    bank.waves = ReadWaveIds(r, wave_table.value_or(0));
    std::size_t regions = 0;
    const std::size_t instruments = inst_table.value_or(0);
    const uint32_t ni = r.U32(instruments);
    for (uint32_t i = 0; i < ni; i++)
    {
        const Reference ref = ReadRef(r, instruments + 4 + i * 8);
        if (ref.type != 0x5900)
        {
            bank.instruments.emplace_back();
            continue;
        }

        bank.instruments.push_back(ReadInstrument(r, instruments + ref.offset, strict, file.size(), regions));
    }

    return bank;
}

WaveSoundFile WaveSoundFile::Parse(std::span<const uint8_t> file, const Options& options)
{
    const xsflib::Reader r = OpenFile(file, "WSD", options);
    const Blocks blocks = BlockTable(r);
    const auto [info_off, info_size] = BlockOf(blocks, 0x6800, "INFO", "a wave sound file (CWSD)", options);

    // INFO holds a reference to the table of the waves and one to the table of the wave sounds.
    const auto [wave_table, sound_table] = WaveAndItemTables(r, info_off + 8);
    if (!wave_table || !sound_table)
    {
        throw xsflib::FormatError("a wave sound file without its tables of waves and wave sounds");
    }

    WaveSoundFile wsd;
    wsd.waves = ReadWaveIds(r, *wave_table);

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

    // Each wave sound: references to its settings, its tracks and its notes. The Wii U's and the Switch's readers read
    // up to 3 aux sends, and the filters from version 1.1.0 on (WaveSoundFileReader, 0x022B95E4 in Animal Crossing:
    // amiibo Festival and 0xc285d0 in WarioWare: Get It Together!'s main). The 3DS's reader reads up to 2 aux sends,
    // and no filters (ReadWaveSoundInfo, 0x48f1ec in Pokémon X's code.bin).
    const bool ctr = options.flavor == Flavor::kCtr;
    const bool filters = !ctr && r.U32(0x08) >= 0x00010100;
    const std::size_t most_aux_sends = ctr ? 2 : 3;
    for (const std::size_t sound_at : table(*sound_table))
    {
        WaveSound sound;
        ReadWaveSoundSettings(r, sound_at + ReadRef(r, sound_at).offset, filters, most_aux_sends, sound);

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

} // namespace ntdlib::sar
