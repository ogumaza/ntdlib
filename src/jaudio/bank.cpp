// SPDX-License-Identifier: MIT

// JAudio2's banks (see bank.h). Four Swords Adventures' main.dol reads its banks with a parser for each version:
// version 1's parser at 0x8002f610 creates 'Rand' and 'Sens' effects for its instruments.

#include "ntdlib/jaudio/bank.h"

#include <algorithm>
#include <string>
#include <utility>

#include "jaudio/reader.h"
#include "xsflib/binary.h"

namespace ntdlib::jaudio
{
namespace
{

using detail::kReaderOptions;

// Drum sets of a version 0 bank are programs from 0xe4 on.
constexpr uint32_t kFirstDrumProgram = 0xe4;

// Version 0 banks: the instrument and drum set tables.
constexpr std::size_t kBankChunk = 0x20;
constexpr std::size_t kInstrumentTable = 0x24;
constexpr std::size_t kDrumTable = 0x3b4;

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
// second, then its volume and pitch.
VelocityRegion ReadVelocityRegion(const xsflib::Reader& r, std::size_t offset)
{
    return {r.U8(offset), static_cast<uint16_t>(r.U32(offset + 4) & 0xffff), r.F32(offset + 8), r.F32(offset + 12)};
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
Oscillator ReadOscillatorV0(const xsflib::Reader& r, std::size_t offset)
{
    Oscillator osc;
    osc.target = r.U8(offset);
    osc.rate = r.F32(offset + 4);
    if (const uint32_t attack = r.U32(offset + 8))
    {
        osc.attack = ReadPoints(r, attack);
    }
    if (const uint32_t release = r.U32(offset + 12))
    {
        osc.release = ReadPoints(r, release);
    }
    osc.scale = r.F32(offset + 16);
    osc.vertex = r.F32(offset + 20);

    return osc;
}

// Reads a version 0 instrument at `at`. `oscillator` gives the bank's index of the oscillator at an offset.
template <typename OscillatorAt>
Instrument ReadInstrumentV0(const xsflib::Reader& r, uint32_t at, const OscillatorAt& oscillator)
{
    Instrument inst;
    inst.volume = r.F32(at + 8);
    inst.pitch = r.F32(at + 12);
    for (int k = 0; k < 2; k++)
    {
        if (const uint32_t osc = r.U32(at + 16 + 4 * k))
        {
            inst.oscillators.push_back(oscillator(osc));
        }
    }

    // The effects keep the parser's order: the two random ones, then the two that follow the key or velocity.
    for (int k = 0; k < 2; k++)
    {
        if (const uint32_t e = r.U32(at + 24 + 4 * k))
        {
            inst.effects.push_back(RandomEffect(r.U8(e), r.F32(e + 4), r.F32(e + 8)));
        }
    }
    for (int k = 0; k < 2; k++)
    {
        if (const uint32_t e = r.U32(at + 32 + 4 * k))
        {
            inst.effects.push_back(SenseEffect(r.U8(e), r.U8(e + 1), r.U8(e + 2), r.F32(e + 4), r.F32(e + 8)));
        }
    }

    const uint32_t keys = r.U32(at + 40);
    for (uint32_t k = 0; k < keys; k++)
    {
        const uint32_t keymap = r.U32(at + 44 + 4 * k);
        KeyRegion region;
        region.high_key = r.U8(keymap);
        const uint32_t velocities = r.U32(keymap + 4);
        for (uint32_t v = 0; v < velocities; v++)
        {
            region.velocities.push_back(ReadVelocityRegion(r, r.U32(keymap + 8 + 4 * v)));
        }
        inst.keys.push_back(std::move(region));
    }

    return inst;
}

// Reads a version 0 drum set at `at`. A drum set of the second kind (PER2) adds a pan and a release for each key.
DrumSet ReadDrumSetV0(const xsflib::Reader& r, uint32_t at)
{
    const bool per2 = r.Magic(at, "PER2");
    if (!per2 && !r.Magic(at, "PERC"))
    {
        throw xsflib::FormatError("bank has a drum set of unknown kind");
    }

    DrumSet drums;
    for (int key = 0; key < 128; key++)
    {
        const uint32_t pmap = r.U32(at + 0x88 + 4 * key);
        if (pmap == 0)
        {
            continue;
        }

        Drum drum;
        drum.volume = r.F32(pmap);
        drum.pitch = r.F32(pmap + 4);
        if (per2)
        {
            drum.pan = static_cast<float>(static_cast<int8_t>(r.U8(at + 0x288 + key))) / 127.0f;
            drum.release = r.U16(at + 0x308 + 2 * key);
        }

        for (int k = 0; k < 2; k++)
        {
            if (const uint32_t e = r.U32(pmap + 8 + 4 * k))
            {
                drum.effects.push_back(RandomEffect(r.U8(e), r.F32(e + 4), r.F32(e + 8)));
            }
        }

        const uint32_t velocities = r.U32(pmap + 16);
        for (uint32_t v = 0; v < velocities; v++)
        {
            drum.velocities.push_back(ReadVelocityRegion(r, r.U32(pmap + 20 + 4 * v)));
        }

        drums.keys[static_cast<std::size_t>(key)] = std::move(drum);
    }

    return drums;
}

// Reads a version 0 bank: 128 instrument offsets and 12 drum set offsets (The Wind Waker's layout). Banks share
// oscillators between instruments by offset.
void ParseVersion0(const xsflib::Reader& r, Bank& bank)
{
    if (!r.Magic(kBankChunk, "BANK"))
    {
        throw xsflib::FormatError("bank has no BANK chunk");
    }

    std::map<uint32_t, std::size_t> oscillators; // by offset
    const auto oscillator = [&](uint32_t offset)
    {
        const auto [it, added] = oscillators.try_emplace(offset, bank.oscillators.size());
        if (added)
        {
            bank.oscillators.push_back(ReadOscillatorV0(r, offset));
        }

        return it->second;
    };

    for (uint32_t program = 0; program < 128; program++)
    {
        if (const uint32_t at = r.U32(kInstrumentTable + 4 * program))
        {
            bank.programs[program] = ReadInstrumentV0(r, at, oscillator);
        }
    }

    for (uint32_t set = 0; set < 12; set++)
    {
        if (const uint32_t at = r.U32(kDrumTable + 4 * set))
        {
            bank.programs[kFirstDrumProgram + set] = ReadDrumSetV0(r, at);
        }
    }
}

// Reads a version 1 effect: a 'Rand' or 'Sens' object at `offset` from the bank's start.
InstEffect ReadEffectV1(const xsflib::Reader& r, std::size_t offset)
{
    if (r.Magic(offset, "Rand"))
    {
        return RandomEffect(r.U8(offset + 4), r.F32(offset + 8), r.F32(offset + 12));
    }
    if (r.Magic(offset, "Sens"))
    {
        return SenseEffect(r.U8(offset + 4), r.U8(offset + 5), r.U8(offset + 6), r.F32(offset + 8), r.F32(offset + 12));
    }

    throw xsflib::FormatError("bank has an effect of unknown kind");
}

// Reads a version 1 oscillator at `at`. Its tables are offsets into the envelope chunk's data, which starts at
// `envelopes`.
Oscillator ReadOscillatorV1(const xsflib::Reader& r, std::size_t at, std::size_t envelopes)
{
    if (!r.Magic(at, "Osci"))
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

// Reads a version 1 instrument whose tag ("Inst") is at `at`. The bank has `oscillators` oscillators.
Instrument ReadInstrumentV1(const xsflib::Reader& r, std::size_t at, std::size_t oscillators)
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
        region.high_key = r.U8(at);
        const uint32_t velocities = r.U32(at + 4);
        at += 8;
        for (uint32_t v = 0; v < velocities; v++, at += 16)
        {
            region.velocities.push_back(ReadVelocityRegion(r, at));
        }
        inst.keys.push_back(std::move(region));
    }

    inst.volume = r.F32(at);
    inst.pitch = r.F32(at + 4);

    return inst;
}

// Reads a version 1 drum set whose tag ("Perc") is at `at`.
DrumSet ReadDrumSetV1(const xsflib::Reader& r, std::size_t at)
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
        if (!r.Magic(pmap, "Pmap"))
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
            drum.velocities.push_back(ReadVelocityRegion(r, pmap));
        }

        drums.keys[key] = std::move(drum);
    }

    return drums;
}

// Reads a version 1 bank of `size` bytes at the start of `file`: chunks of envelopes (ENVT), oscillators (OSCT) and a
// list of instruments and drum sets (LIST), with the effects and drum maps they point to (Twilight Princess's layout,
// with Four Swords Adventures' effects). Each chunk starts inside the bank's size. A chunk's data can end past it. The
// game reads each chunk whole.
void ParseVersion1(std::span<const uint8_t> file, std::size_t size, Bank& bank)
{
    const xsflib::Reader in_bank(file.first(size), true, kReaderOptions);
    std::map<std::string, std::pair<std::size_t, uint32_t>> chunks; // tag: data offset and size
    std::size_t end = size;
    for (std::size_t at = 0x20; at + 8 <= size;)
    {
        const uint32_t chunk_size = in_bank.U32(at + 4);
        const std::string tag(reinterpret_cast<const char*>(in_bank.Sub(at, 4).data()), 4);
        chunks.try_emplace(tag, at + 8, chunk_size);
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
    const xsflib::Reader r(file.first(std::min(file.size(), end)), true, kReaderOptions);
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

        if (r.Magic(at, "Inst"))
        {
            bank.programs[program] = ReadInstrumentV1(r, at, bank.oscillators.size());
        }
        else if (r.Magic(at, "Perc"))
        {
            bank.programs[program] = ReadDrumSetV1(r, at);
        }
        else
        {
            throw xsflib::FormatError("bank lists a program of unknown kind");
        }
    }
}

} // namespace

Bank Bank::Parse(std::span<const uint8_t> file, uint32_t wave_system)
{
    const xsflib::Reader r(file, true, kReaderOptions);
    if (!r.Magic(0, "IBNK"))
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
        ParseVersion0(xsflib::Reader(file.first(size), true, kReaderOptions), bank);
        break;
    case 1:
        ParseVersion1(file, size, bank);
        break;
    default:
        throw xsflib::FormatError("bank of unknown version " + std::to_string(bank.version));
    }

    return bank;
}

} // namespace ntdlib::jaudio
