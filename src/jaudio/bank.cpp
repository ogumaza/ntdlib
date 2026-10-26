// SPDX-License-Identifier: MIT

// JAudio2's banks (see bank.h). Four Swords Adventures' main.dol reads its banks with a parser for each version:
// version 1's parser at 0x8002f610 creates 'Rand' and 'Sens' effects for its instruments.

#include "ntdlib/jaudio/bank.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "jaudio/bank_parse.h"
#include "jaudio/reader.h"

namespace ntdlib::jaudio
{
namespace
{

using detail::HasTag;
using detail::IsBigEndian;
using detail::kOffsetBytes;
using detail::kReaderOptions;
using detail::kWideOffsetBytes;
using detail::OffsetAt;
using detail::StartsWithReversed;
using detail::TagAt;

// Drum sets of a version 0 bank are programs from 0xe4 on.
constexpr uint32_t kFirstDrumProgram = 0xe4;

// Version 0 banks: the instrument table, 100 words, and the drum set table. A little-endian bank of version 0 holds 100
// more words before its drum table (the Switch's release of Pikmin 2: the bank's parser, 0x3d16b0 in its main). Its
// size leaves those words out. Its data then reaches 0x190 bytes past its size. A bank with 64-bit offsets widens each
// word of the tables to 8 bytes (the Switch's release of Pikmin: the bank's parser, 0x279930 in its main).
constexpr std::size_t kBankChunk = 0x20;
constexpr std::size_t kInstrumentTable = 0x24;
constexpr std::size_t kDrumTable = 0x3b4;
constexpr std::size_t kWideDrumTable = 0x544;
constexpr std::size_t kWideOffsetDrumTable = 0x744;

// A drum set of version 0: its tag, 0x84 bytes, then an offset for each key's drum. After the offsets, a PER2 drum set
// gives each key a pan in a byte, then a release in 16 bits.
constexpr uint32_t kDrumOffsets = 0x88;

// A version 0 bank's data in its byte order, the width of its offsets, and the set that gets the high halves of its
// waves' ids (or null).
struct BankV0
{
    const xsflib::Reader& r;
    uint32_t offset_bytes = kOffsetBytes;
    std::set<uint16_t>* wave_systems = nullptr;

    // The offset at `at`.
    uint32_t Offset(std::size_t at) const
    {
        return OffsetAt(r, at, offset_bytes);
    }
};

// Whether `file` holds a bank with 64-bit offsets: a tag in the big-endian order, and a BANK chunk whose tag is
// reversed. The Switch's release of Pikmin keeps its banks that way. Each bank's tag and size keep the big-endian
// order. Its other fields are little-endian.
bool HasWideOffsets(std::span<const uint8_t> file)
{
    return file.size() >= kBankChunk + 4 && !StartsWithReversed(file, "IBNK") &&
           StartsWithReversed(file.subspan(kBankChunk), "BANK");
}

// Reads an oscillator's table at `offset`: its points up to the first that holds, loops or stops (a mode above 10). The
// bank parsers stop there too (getOscTableEndPtr).
std::vector<OscPoint> ReadPoints(const xsflib::Reader& r, std::size_t offset)
{
    std::vector<OscPoint> points;
    for (;;)
    {
        const OscPoint p{r.S16(offset), r.S16(offset + 2), r.S16(offset + 4)};
        points.push_back(p);
        if (p.mode > 10)
        {
            return points;
        }

        offset += 6;
        if (points.size() > 0x10000)
        {
            throw xsflib::FormatError("oscillator table has no end");
        }
    }
}

// Reads a velocity region: its high velocity in the top byte of the first word, the wave id in the low half of the
// second, then its volume and pitch. `wave_systems`, unless it's null, gets the high half of the second word.
VelocityRegion ReadVelocityRegion(const xsflib::Reader& r, std::size_t offset, std::set<uint16_t>* wave_systems)
{
    const uint8_t high_velocity = r.U8(offset);
    const uint32_t wave = r.U32(offset + 4);
    if (wave_systems != nullptr)
    {
        wave_systems->insert(static_cast<uint16_t>(wave >> 16));
    }

    return {high_velocity, static_cast<uint16_t>(wave & 0xffff), r.F32(offset + 8), r.F32(offset + 12)};
}

InstEffect RandomEffect(uint8_t target, float base, float width)
{
    InstEffect e;
    e.kind = InstEffect::Kind::kRandom;
    e.target = target;
    e.base = base;
    e.width = width;

    return e;
}

InstEffect SenseEffect(uint8_t target, uint8_t trigger, uint8_t center, float low, float high)
{
    InstEffect e;
    e.kind = InstEffect::Kind::kSense;
    e.target = target;
    e.trigger = trigger;
    e.center = center;
    e.low = low;
    e.high = high;

    return e;
}

// A version 0 oscillator: its target, rate, tables (offsets from the bank's start), scale and vertex.
Oscillator ReadOscillatorV0(const BankV0& b, std::size_t offset)
{
    const xsflib::Reader& r = b.r;
    const uint32_t w = b.offset_bytes;
    Oscillator osc;
    osc.target = r.U8(offset);
    osc.rate = r.F32(offset + 4);
    if (const uint32_t attack = b.Offset(offset + 8))
    {
        osc.attack = ReadPoints(r, attack);
    }
    if (const uint32_t release = b.Offset(offset + 8 + w))
    {
        osc.release = ReadPoints(r, release);
    }
    osc.scale = r.F32(offset + 8 + 2 * w);
    osc.vertex = r.F32(offset + 12 + 2 * w);

    return osc;
}

// Reads a version 0 instrument at `at`. `oscillator` gives the bank's index of the oscillator at an offset. The word at
// +4 holds the instrument's flags in its low byte. The instrument's volume and pitch follow, then the offsets of two
// oscillators, two random effects and two effects that follow the key or velocity. The number of key regions follows,
// then an offset for each.
template <typename OscillatorAt>
Instrument ReadInstrumentV0(const BankV0& b, uint32_t at, const OscillatorAt& oscillator)
{
    const xsflib::Reader& r = b.r;
    const uint32_t w = b.offset_bytes;
    Instrument inst;
    inst.flags = static_cast<uint8_t>(r.U32(at + 4));
    inst.volume = r.F32(at + 8);
    inst.pitch = r.F32(at + 12);
    for (int k = 0; k < 2; k++)
    {
        if (const uint32_t osc = b.Offset(at + 16 + w * k))
        {
            inst.oscillators.push_back(oscillator(osc));
        }
    }

    // The effects keep the parser's order: the two random ones, then the two that follow the key or velocity.
    for (int k = 0; k < 2; k++)
    {
        if (const uint32_t e = b.Offset(at + 16 + 2 * w + w * k))
        {
            inst.effects.push_back(RandomEffect(r.U8(e), r.F32(e + 4), r.F32(e + 8)));
        }
    }
    for (int k = 0; k < 2; k++)
    {
        if (const uint32_t e = b.Offset(at + 16 + 4 * w + w * k))
        {
            inst.effects.push_back(SenseEffect(r.U8(e), r.U8(e + 1), r.U8(e + 2), r.F32(e + 4), r.F32(e + 8)));
        }
    }

    const uint32_t keys_at = at + 16 + 6 * w;
    const uint32_t keys = r.U32(keys_at);
    for (uint32_t k = 0; k < keys; k++)
    {
        const uint32_t keymap = b.Offset(keys_at + 4 + w * k);
        KeyRegion region;
        region.high_key = r.U8(keymap);
        const uint32_t velocities = r.U32(keymap + 4);
        for (uint32_t v = 0; v < velocities; v++)
        {
            region.velocities.push_back(ReadVelocityRegion(r, b.Offset(keymap + 8 + w * v), b.wave_systems));
        }
        inst.keys.push_back(std::move(region));
    }

    return inst;
}

// Reads a version 0 drum set at `at`. A drum set of the second kind (PER2) adds a pan and a release for each key. A
// key's drum gives its volume and pitch, the offsets of two random effects, and the number of its velocity regions
// with an offset for each.
DrumSet ReadDrumSetV0(const BankV0& b, uint32_t at)
{
    const xsflib::Reader& r = b.r;
    const uint32_t w = b.offset_bytes;
    const bool per2 = HasTag(r, at, "PER2");
    if (!per2 && !HasTag(r, at, "PERC"))
    {
        throw xsflib::FormatError("bank has a drum set of unknown kind");
    }

    const uint32_t pans = at + kDrumOffsets + 128 * w;
    const uint32_t releases = pans + 128;
    DrumSet drums;
    for (int key = 0; key < 128; key++)
    {
        const uint32_t pmap = b.Offset(at + kDrumOffsets + w * key);
        if (pmap == 0)
        {
            continue;
        }

        Drum drum;
        drum.volume = r.F32(pmap);
        drum.pitch = r.F32(pmap + 4);
        if (per2)
        {
            drum.pan = static_cast<float>(static_cast<int8_t>(r.U8(pans + key))) / 127.0f;
            drum.release = r.U16(releases + 2 * key);
        }

        for (int k = 0; k < 2; k++)
        {
            if (const uint32_t e = b.Offset(pmap + 8 + w * k))
            {
                drum.effects.push_back(RandomEffect(r.U8(e), r.F32(e + 4), r.F32(e + 8)));
            }
        }

        const uint32_t velocities = r.U32(pmap + 8 + 2 * w);
        for (uint32_t v = 0; v < velocities; v++)
        {
            drum.velocities.push_back(ReadVelocityRegion(r, b.Offset(pmap + 12 + 2 * w + w * v), b.wave_systems));
        }

        drums.keys[static_cast<std::size_t>(key)] = std::move(drum);
    }

    return drums;
}

// Reads a version 0 bank: 128 instrument offsets and 12 drum set offsets at `drum_table` (The Wind Waker's layout at
// kDrumTable). Banks share oscillators between instruments by offset.
void ParseVersion0(const BankV0& b, std::size_t drum_table, Bank& bank)
{
    if (!HasTag(b.r, kBankChunk, "BANK"))
    {
        throw xsflib::FormatError("bank has no BANK chunk");
    }

    std::map<uint32_t, std::size_t> oscillators; // by offset
    const auto oscillator = [&](uint32_t offset)
    {
        const auto [it, added] = oscillators.try_emplace(offset, bank.oscillators.size());
        if (added)
        {
            bank.oscillators.push_back(ReadOscillatorV0(b, offset));
        }

        return it->second;
    };

    for (uint32_t program = 0; program < 128; program++)
    {
        if (const uint32_t at = b.Offset(kInstrumentTable + b.offset_bytes * program))
        {
            bank.programs[program] = ReadInstrumentV0(b, at, oscillator);
        }
    }

    for (uint32_t set = 0; set < 12; set++)
    {
        if (const uint32_t at = b.Offset(drum_table + b.offset_bytes * set))
        {
            bank.programs[kFirstDrumProgram + set] = ReadDrumSetV0(b, at);
        }
    }
}

// Reads a version 1 effect: a 'Rand' or 'Sens' object at `offset` from the bank's start.
InstEffect ReadEffectV1(const xsflib::Reader& r, std::size_t offset)
{
    if (HasTag(r, offset, "Rand"))
    {
        return RandomEffect(r.U8(offset + 4), r.F32(offset + 8), r.F32(offset + 12));
    }
    if (HasTag(r, offset, "Sens"))
    {
        return SenseEffect(r.U8(offset + 4), r.U8(offset + 5), r.U8(offset + 6), r.F32(offset + 8), r.F32(offset + 12));
    }

    throw xsflib::FormatError("bank has an effect of unknown kind");
}

// Reads a version 1 oscillator at `at`. Its tables are offsets into the envelope chunk's data, which starts at
// `envelopes`.
Oscillator ReadOscillatorV1(const xsflib::Reader& r, std::size_t at, std::size_t envelopes)
{
    if (!HasTag(r, at, "Osci"))
    {
        throw xsflib::FormatError("bank has a damaged oscillator");
    }

    Oscillator osc;
    osc.target = r.U8(at + 4);
    osc.rate = r.F32(at + 8);
    osc.attack = ReadPoints(r, envelopes + r.U32(at + 12));
    osc.release = ReadPoints(r, envelopes + r.U32(at + 16));
    osc.release_is_attack = r.U32(at + 12) == r.U32(at + 16);
    osc.scale = r.F32(at + 20);
    osc.vertex = r.F32(at + 24);

    return osc;
}

// Reads a version 1 instrument whose tag ("Inst") is at `at`. The bank has `oscillators` oscillators. A key region
// starts with its high key in a byte. A little-endian bank stores that byte as the top byte of a 32-bit word.
// `wave_systems`, unless it's null, gets the high halves of the waves' ids.
Instrument ReadInstrumentV1(const xsflib::Reader& r, std::size_t at, std::size_t oscillators,
                            std::set<uint16_t>* wave_systems)
{
    Instrument inst;
    at += 4;
    const uint32_t oscs = r.U32(at);
    at += 4;
    for (uint32_t k = 0; k < oscs; k++, at += 4)
    {
        const uint32_t index = r.U32(at);
        if (index >= oscillators)
        {
            throw xsflib::FormatError("instrument names a missing oscillator");
        }

        inst.oscillators.push_back(index);
    }

    const uint32_t effects = r.U32(at);
    at += 4;
    for (uint32_t k = 0; k < effects; k++, at += 4)
    {
        inst.effects.push_back(ReadEffectV1(r, r.U32(at)));
    }

    const uint32_t keys = r.U32(at);
    at += 4;
    for (uint32_t k = 0; k < keys; k++)
    {
        KeyRegion region;
        region.high_key = r.U8(r.BigEndian() ? at : at + 3);
        const uint32_t velocities = r.U32(at + 4);
        at += 8;
        for (uint32_t v = 0; v < velocities; v++, at += 16)
        {
            region.velocities.push_back(ReadVelocityRegion(r, at, wave_systems));
        }
        inst.keys.push_back(std::move(region));
    }

    inst.volume = r.F32(at);
    inst.pitch = r.F32(at + 4);

    return inst;
}

// Reads a version 1 drum set whose tag ("Perc") is at `at`. `wave_systems`, unless it's null, gets the high halves of
// the waves' ids.
DrumSet ReadDrumSetV1(const xsflib::Reader& r, std::size_t at, std::set<uint16_t>* wave_systems)
{
    DrumSet drums;
    const uint32_t count = std::min<uint32_t>(r.U32(at + 4), 128);
    for (uint32_t key = 0; key < count; key++)
    {
        std::size_t pmap = r.U32(at + 8 + 4 * key);
        if (pmap == 0)
        {
            continue;
        }
        if (!HasTag(r, pmap, "Pmap"))
        {
            throw xsflib::FormatError("drum set has a damaged key");
        }

        Drum drum;
        drum.volume = r.F32(pmap + 4);
        drum.pitch = r.F32(pmap + 8);
        drum.pan = static_cast<float>(r.U8(pmap + 12)) / 127.0f;
        drum.release = r.U16(pmap + 14);
        pmap += 16;
        const uint32_t effects = r.U32(pmap);
        pmap += 4;
        for (uint32_t k = 0; k < effects; k++, pmap += 4)
        {
            drum.effects.push_back(ReadEffectV1(r, r.U32(pmap)));
        }

        const uint32_t velocities = r.U32(pmap);
        pmap += 4;
        for (uint32_t v = 0; v < velocities; v++, pmap += 16)
        {
            drum.velocities.push_back(ReadVelocityRegion(r, pmap, wave_systems));
        }

        drums.keys[key] = std::move(drum);
    }

    return drums;
}

// Reads a version 1 bank of `size` bytes at the start of `file`: chunks of envelopes (ENVT), oscillators (OSCT) and a
// list of instruments and drum sets (LIST), with the effects and drum maps they point to (Twilight Princess's layout,
// with Four Swords Adventures' effects). Each chunk starts inside the bank's size. A chunk's data can end past it. The
// game reads each chunk whole. `wave_systems`, unless it's null, gets the high halves of the waves' ids.
void ParseVersion1(std::span<const uint8_t> file, std::size_t size, bool big_endian, Bank& bank,
                   std::set<uint16_t>* wave_systems)
{
    const xsflib::Reader in_bank(file.first(size), big_endian, kReaderOptions);
    std::map<std::string, std::pair<std::size_t, uint32_t>> chunks; // tag: data offset and size
    std::size_t end = size;
    for (std::size_t at = 0x20; at + 8 <= size;)
    {
        const uint32_t chunk_size = in_bank.U32(at + 4);
        chunks.try_emplace(TagAt(in_bank, at), at + 8, chunk_size);
        end = std::max<std::size_t>(end, at + 8 + chunk_size);
        at = (at + 0xb + chunk_size) & ~std::size_t{3};
    }

    for (const char* tag : {"ENVT", "OSCT", "LIST"})
    {
        if (!chunks.contains(tag))
        {
            throw xsflib::FormatError(std::string("bank has no ") + tag + " chunk");
        }
    }

    // The chunks' data, to the end of the last chunk.
    const xsflib::Reader r(file.first(std::min(file.size(), end)), big_endian, kReaderOptions);
    const std::size_t envelopes = chunks["ENVT"].first;
    const std::size_t osct = chunks["OSCT"].first;
    const uint32_t oscillators = r.U32(osct);
    for (uint32_t i = 0; i < oscillators; i++)
    {
        bank.oscillators.push_back(ReadOscillatorV1(r, osct + 4 + 0x1c * i, envelopes));
    }

    const std::size_t list = chunks["LIST"].first;
    const uint32_t programs = r.U32(list);
    for (uint32_t program = 0; program < programs; program++)
    {
        const std::size_t at = r.U32(list + 4 + 4 * program);
        if (at == 0)
        {
            continue;
        }

        if (HasTag(r, at, "Inst"))
        {
            bank.programs[program] = ReadInstrumentV1(r, at, bank.oscillators.size(), wave_systems);
        }
        else if (HasTag(r, at, "Perc"))
        {
            bank.programs[program] = ReadDrumSetV1(r, at, wave_systems);
        }
        else
        {
            throw xsflib::FormatError("bank lists a program of unknown kind");
        }
    }
}

// Reads a bank as Bank::Parse does. `wave_systems`, unless it's null, gets the high halves of the waves' ids.
Bank ReadBank(std::span<const uint8_t> file, uint32_t wave_system, std::set<uint16_t>* wave_systems)
{
    const bool wide_offsets = HasWideOffsets(file);
    const xsflib::Reader r(file, !wide_offsets && IsBigEndian(file, "IBNK"), kReaderOptions);
    if (!HasTag(r, 0, "IBNK"))
    {
        throw xsflib::FormatError("bank has no IBNK header");
    }

    Bank bank;
    bank.number = r.U32(8);
    bank.version = r.U32(12);
    bank.wave_system = wave_system;
    const std::size_t size = std::min<std::size_t>(file.size(), r.U32(4));
    switch (bank.version)
    {
    case 0:
        if (wide_offsets)
        {
            // The big-endian size gives the bank's size with 32-bit offsets. The bank's data reaches past it.
            ParseVersion0({r, kWideOffsetBytes, wave_systems}, kWideOffsetDrumTable, bank);
        }
        else if (r.BigEndian())
        {
            const xsflib::Reader in_bank(file.first(size), true, kReaderOptions);
            ParseVersion0({in_bank, kOffsetBytes, wave_systems}, kDrumTable, bank);
        }
        else
        {
            const std::size_t wide_size =
                std::min<std::size_t>(file.size(), std::size_t{r.U32(4)} + kWideDrumTable - kDrumTable);
            const xsflib::Reader in_bank(file.first(wide_size), false, kReaderOptions);
            ParseVersion0({in_bank, kOffsetBytes, wave_systems}, kWideDrumTable, bank);
        }
        break;
    case 1:
        ParseVersion1(file, size, r.BigEndian(), bank, wave_systems);
        break;
    default:
        throw xsflib::FormatError("bank of unknown version " + std::to_string(bank.version));
    }

    return bank;
}

} // namespace

Bank Bank::Parse(std::span<const uint8_t> file, uint32_t wave_system)
{
    return ReadBank(file, wave_system, nullptr);
}

detail::ParsedBank detail::ParseBank(std::span<const uint8_t> file, uint32_t wave_system)
{
    ParsedBank parsed;
    parsed.bank = ReadBank(file, wave_system, &parsed.wave_systems);

    return parsed;
}

} // namespace ntdlib::jaudio
