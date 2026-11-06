// SPDX-License-Identifier: MIT

#include "ntdlib/n64/ead_bank.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

constexpr std::size_t kEarlyHeaderSize = 16;
constexpr std::size_t kInstrumentSize = 32;
constexpr std::size_t kDrumSize = 16;
constexpr std::size_t kEffectSize = 8;
constexpr std::size_t kEarlySampleSize = 20;
constexpr std::size_t kLaterSampleSize = 16;
constexpr std::size_t kLoopHeaderSize = 16;
constexpr std::size_t kLoopStateSize = 2 * kVadpcmFrameSamples;
constexpr std::size_t kBookHeaderSize = 8;
constexpr std::size_t kEnvelopePointSize = 4;
constexpr int32_t kMaxOrder = 16;
constexpr int32_t kMaxPredictors = 256;
constexpr std::size_t kObjectAlignment = 16;

class Parser
{
public:
    Parser(std::span<const uint8_t> data, EadEngine engine)
        : reader_(data, true), base_(engine == EadEngine::kEarly ? kEarlyHeaderSize : 0)
    {
        bank_.engine = engine;
        const auto bytes = reader_.Sub(0, data.size());
        bank_.image.assign(bytes.begin(), bytes.end());
    }

    EadBank Parse(std::size_t instrument_count, std::size_t drum_count, std::size_t effect_count)
    {
        // The table of offsets at the bank's start.
        const bool later = bank_.engine != EadEngine::kEarly;
        const std::size_t instruments_at = base_ + (later ? 8 : 4);
        bank_.drums_table_offset = reader_.U32(base_);
        if (later)
        {
            bank_.effects_offset = reader_.U32(base_ + 4);
        }

        bank_.program_instruments.reserve(instrument_count);
        for (std::size_t i = 0; i < instrument_count; i++)
        {
            const uint32_t offset = reader_.U32(instruments_at + i * 4);
            bank_.program_instruments.push_back(offset == 0 ? kNoObject : ParseInstrument(offset));
        }

        if (bank_.drums_table_offset != 0)
        {
            bank_.drum_notes.reserve(drum_count);
            for (std::size_t i = 0; i < drum_count; i++)
            {
                const uint32_t offset = reader_.U32(base_ + bank_.drums_table_offset + i * 4);
                bank_.drum_notes.push_back(offset == 0 ? kNoObject : ParseDrum(offset));
            }
        }

        if (bank_.effects_offset != 0)
        {
            bank_.effects.reserve(effect_count);
            for (std::size_t i = 0; i < effect_count; i++)
            {
                EadSoundEffect effect;
                effect.sound = ParseTuned(base_ + bank_.effects_offset + i * kEffectSize);
                bank_.effects.push_back(effect);
            }
        }

        return std::move(bank_);
    }

private:
    int32_t ParseInstrument(uint32_t offset)
    {
        const auto found = instruments_.find(offset);
        if (found != instruments_.end())
        {
            return found->second;
        }

        const std::size_t at = base_ + offset;
        reader_.Sub(at, kInstrumentSize);

        EadInstrument instrument;
        instrument.offset = offset;
        instrument.loaded = reader_.U8(at);
        instrument.normal_range_low = reader_.U8(at + 1);
        instrument.normal_range_high = reader_.U8(at + 2);
        instrument.release_rate = reader_.U8(at + 3);
        instrument.envelope = ParseEnvelope(reader_.U32(at + 4));
        instrument.low = ParseTuned(at + 8);
        instrument.normal = ParseTuned(at + 16);
        instrument.high = ParseTuned(at + 24);

        const auto index = static_cast<int32_t>(bank_.instruments.size());
        instruments_[offset] = index;
        bank_.instruments.push_back(instrument);

        return index;
    }

    int32_t ParseDrum(uint32_t offset)
    {
        const auto found = drums_.find(offset);
        if (found != drums_.end())
        {
            return found->second;
        }

        const std::size_t at = base_ + offset;
        reader_.Sub(at, kDrumSize);

        EadDrum drum;
        drum.offset = offset;
        drum.release_rate = reader_.U8(at);
        drum.pan = reader_.U8(at + 1);
        drum.loaded = reader_.U8(at + 2);
        drum.sound = ParseTuned(at + 4);
        drum.envelope = ParseEnvelope(reader_.U32(at + 12));

        const auto index = static_cast<int32_t>(bank_.drums.size());
        drums_[offset] = index;
        bank_.drums.push_back(drum);

        return index;
    }

    EadTunedSample ParseTuned(std::size_t at)
    {
        EadTunedSample tuned;
        const uint32_t sample = reader_.U32(at);
        tuned.sample = sample == 0 ? kNoObject : ParseSample(sample);
        tuned.tuning = reader_.F32(at + 4);

        return tuned;
    }

    int32_t ParseSample(uint32_t offset)
    {
        const auto found = samples_.find(offset);
        if (found != samples_.end())
        {
            return found->second;
        }

        const std::size_t at = base_ + offset;
        const bool early = bank_.engine == EadEngine::kEarly;
        reader_.Sub(at, early ? kEarlySampleSize : kLaterSampleSize);

        EadSample sample;
        sample.offset = offset;
        if (early)
        {
            sample.loaded = reader_.U8(at + 1);
            sample.size = reader_.U32(at + 16);
        }
        else
        {
            const uint32_t fields = reader_.U32(at);
            sample.codec = static_cast<uint8_t>(fields >> 28);
            sample.medium = static_cast<uint8_t>((fields >> 26) & 3);
            sample.unknown_bit = ((fields >> 25) & 1) != 0;
            sample.loaded = static_cast<uint8_t>((fields >> 24) & 1);
            sample.size = fields & 0x00FFFFFF;
        }

        sample.address = reader_.U32(at + 4);
        sample.loop = ParseLoop(reader_.U32(at + 8));
        sample.book = ParseBook(reader_.U32(at + 12));

        const auto index = static_cast<int32_t>(bank_.samples.size());
        samples_[offset] = index;
        bank_.samples.push_back(sample);

        return index;
    }

    int32_t ParseLoop(uint32_t offset)
    {
        if (offset == 0)
        {
            return kNoObject;
        }

        const auto found = loops_.find(offset);
        if (found != loops_.end())
        {
            return found->second;
        }

        const std::size_t at = base_ + offset;

        BankLoop loop;
        loop.offset = offset;
        loop.start = reader_.U32(at);
        loop.end = reader_.U32(at + 4);
        loop.count = reader_.U32(at + 8);
        loop.sample_end = reader_.U32(at + 12);
        loop.has_state = loop.count != 0;
        if (loop.has_state)
        {
            reader_.Sub(at + kLoopHeaderSize, kLoopStateSize);
            for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
            {
                loop.state[i] = reader_.S16(at + kLoopHeaderSize + i * 2);
            }
        }

        const auto index = static_cast<int32_t>(bank_.loops.size());
        loops_[offset] = index;
        bank_.loops.push_back(loop);

        return index;
    }

    int32_t ParseBook(uint32_t offset)
    {
        if (offset == 0)
        {
            return kNoObject;
        }

        const auto found = books_.find(offset);
        if (found != books_.end())
        {
            return found->second;
        }

        const std::size_t at = base_ + offset;

        BankBook book;
        book.offset = offset;
        book.book.order = reader_.S32(at);
        book.book.predictor_count = reader_.S32(at + 4);
        if (book.book.order <= 0 || book.book.order > kMaxOrder || book.book.predictor_count <= 0 ||
            book.book.predictor_count > kMaxPredictors)
        {
            throw xsflib::FormatError("a book of a bank has an order of " + std::to_string(book.book.order) + " and " +
                                      std::to_string(book.book.predictor_count) + " predictors");
        }

        const std::size_t count = static_cast<std::size_t>(book.book.order) *
                                  static_cast<std::size_t>(book.book.predictor_count) * kVadpcmGroupSamples;
        reader_.Sub(at + kBookHeaderSize, count * 2);
        book.book.coefficients.reserve(count);
        for (std::size_t i = 0; i < count; i++)
        {
            book.book.coefficients.push_back(reader_.S16(at + kBookHeaderSize + i * 2));
        }

        const auto index = static_cast<int32_t>(bank_.books.size());
        books_[offset] = index;
        bank_.books.push_back(std::move(book));

        return index;
    }

    int32_t ParseEnvelope(uint32_t offset)
    {
        if (offset == 0)
        {
            return kNoObject;
        }

        const auto found = envelopes_.find(offset);
        if (found != envelopes_.end())
        {
            return found->second;
        }

        EadEnvelope envelope;
        envelope.offset = offset;
        for (std::size_t at = base_ + offset; at + kEnvelopePointSize <= reader_.Size(); at += kEnvelopePointSize)
        {
            const EadEnvelope::Point point = {reader_.S16(at), reader_.S16(at + 2)};
            envelope.points.push_back(point);
            if (point.delay == kEnvelopeDisable || point.delay == kEnvelopeHang || point.delay == kEnvelopeGoto ||
                point.delay == kEnvelopeRestart)
            {
                break;
            }
        }

        const auto index = static_cast<int32_t>(bank_.envelopes.size());
        envelopes_[offset] = index;
        bank_.envelopes.push_back(std::move(envelope));

        return index;
    }

    xsflib::Reader reader_;
    std::size_t base_;
    EadBank bank_;
    std::map<uint32_t, int32_t> instruments_;
    std::map<uint32_t, int32_t> drums_;
    std::map<uint32_t, int32_t> samples_;
    std::map<uint32_t, int32_t> envelopes_;
    std::map<uint32_t, int32_t> loops_;
    std::map<uint32_t, int32_t> books_;
};

// The bank's bytes as the writer builds them, with offsets that count from the bank's base.
class Layout
{
public:
    Layout(std::vector<uint8_t> bytes, std::size_t base) : bytes_(std::move(bytes)), base_(base)
    {
    }

    uint32_t Place(std::size_t size)
    {
        const std::size_t offset = (bytes_.size() + kObjectAlignment - 1) / kObjectAlignment * kObjectAlignment;
        bytes_.resize(offset + size, 0);

        return static_cast<uint32_t>(offset - base_);
    }

    // A field of the part of the bank before the base.
    void HeaderU32(std::size_t offset, uint32_t value)
    {
        xsflib::StoreBe32(bytes_.data() + offset, value);
    }

    void U8(std::size_t offset, uint8_t value)
    {
        bytes_[base_ + offset] = value;
    }

    void U16(std::size_t offset, uint16_t value)
    {
        xsflib::StoreBe16(bytes_.data() + base_ + offset, value);
    }

    void U32(std::size_t offset, uint32_t value)
    {
        xsflib::StoreBe32(bytes_.data() + base_ + offset, value);
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
    std::size_t base_;
};

template <typename Object>
uint32_t OffsetOf(const std::vector<Object>& objects, int32_t index)
{
    return index == kNoObject ? 0 : objects[static_cast<std::size_t>(index)].offset;
}

} // namespace

EadBank EadBank::ParseEarly(std::span<const uint8_t> data)
{
    const xsflib::Reader reader(data, true);
    const uint32_t instruments = reader.U32(0);
    const uint32_t drums = reader.U32(4);
    if (instruments > 0xFF || drums > 0xFF)
    {
        throw xsflib::FormatError("an early bank's header holds " + std::to_string(instruments) + " instruments and " +
                                  std::to_string(drums) + " drums");
    }

    return Parser(data, EadEngine::kEarly).Parse(instruments, drums, 0);
}

EadBank EadBank::ParseLater(std::span<const uint8_t> data, std::size_t instrument_count, std::size_t drum_count,
                            std::size_t effect_count)
{
    return Parser(data, EadEngine::kLater).Parse(instrument_count, drum_count, effect_count);
}

std::vector<uint8_t> EadBank::Write() const
{
    const bool early = engine == EadEngine::kEarly;
    EadBank placed = *this;
    Layout layout(image, early ? kEarlyHeaderSize : 0);
    if (early)
    {
        // The counts in the header follow the lists.
        layout.HeaderU32(0, static_cast<uint32_t>(program_instruments.size()));
        layout.HeaderU32(4, static_cast<uint32_t>(drum_notes.size()));
    }

    for (EadEnvelope& envelope : placed.envelopes)
    {
        if (envelope.offset == kNoOffset)
        {
            envelope.offset = layout.Place(envelope.points.size() * kEnvelopePointSize);
        }

        for (std::size_t i = 0; i < envelope.points.size(); i++)
        {
            layout.U16(envelope.offset + i * kEnvelopePointSize, static_cast<uint16_t>(envelope.points[i].delay));
            layout.U16(envelope.offset + i * kEnvelopePointSize + 2, static_cast<uint16_t>(envelope.points[i].target));
        }
    }

    for (BankLoop& loop : placed.loops)
    {
        if (loop.offset == kNoOffset)
        {
            loop.offset = layout.Place(kLoopHeaderSize + (loop.has_state ? kLoopStateSize : 0));
        }

        layout.U32(loop.offset, loop.start);
        layout.U32(loop.offset + 4, loop.end);
        layout.U32(loop.offset + 8, loop.count);
        layout.U32(loop.offset + 12, loop.sample_end);
        if (loop.has_state)
        {
            for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
            {
                layout.U16(loop.offset + kLoopHeaderSize + i * 2, static_cast<uint16_t>(loop.state[i]));
            }
        }
    }

    for (BankBook& book : placed.books)
    {
        if (book.offset == kNoOffset)
        {
            book.offset = layout.Place(kBookHeaderSize + book.book.coefficients.size() * 2);
        }

        layout.U32(book.offset, static_cast<uint32_t>(book.book.order));
        layout.U32(book.offset + 4, static_cast<uint32_t>(book.book.predictor_count));
        for (std::size_t i = 0; i < book.book.coefficients.size(); i++)
        {
            layout.U16(book.offset + kBookHeaderSize + i * 2, static_cast<uint16_t>(book.book.coefficients[i]));
        }
    }

    for (EadSample& sample : placed.samples)
    {
        if (sample.offset == kNoOffset)
        {
            sample.offset = layout.Place(early ? kEarlySampleSize : kLaterSampleSize);
        }

        if (early)
        {
            layout.U8(sample.offset + 1, sample.loaded);
            layout.U32(sample.offset + 16, sample.size);
        }
        else
        {
            const uint32_t fields = (static_cast<uint32_t>(sample.codec) << 28) |
                                    (static_cast<uint32_t>(sample.medium & 3) << 26) |
                                    (static_cast<uint32_t>(sample.unknown_bit ? 1 : 0) << 25) |
                                    (static_cast<uint32_t>(sample.loaded & 1) << 24) | (sample.size & 0x00FFFFFF);
            layout.U32(sample.offset, fields);
        }

        layout.U32(sample.offset + 4, sample.address);
        layout.U32(sample.offset + 8, OffsetOf(placed.loops, sample.loop));
        layout.U32(sample.offset + 12, OffsetOf(placed.books, sample.book));
    }

    const auto write_tuned = [&](std::size_t at, const EadTunedSample& tuned)
    {
        layout.U32(at, OffsetOf(placed.samples, tuned.sample));
        layout.U32(at + 4, std::bit_cast<uint32_t>(tuned.tuning));
    };

    for (EadInstrument& instrument : placed.instruments)
    {
        if (instrument.offset == kNoOffset)
        {
            instrument.offset = layout.Place(kInstrumentSize);
        }

        layout.U8(instrument.offset, instrument.loaded);
        layout.U8(instrument.offset + 1, instrument.normal_range_low);
        layout.U8(instrument.offset + 2, instrument.normal_range_high);
        layout.U8(instrument.offset + 3, instrument.release_rate);
        layout.U32(instrument.offset + 4, OffsetOf(placed.envelopes, instrument.envelope));
        write_tuned(instrument.offset + 8, instrument.low);
        write_tuned(instrument.offset + 16, instrument.normal);
        write_tuned(instrument.offset + 24, instrument.high);
    }

    for (EadDrum& drum : placed.drums)
    {
        if (drum.offset == kNoOffset)
        {
            drum.offset = layout.Place(kDrumSize);
        }

        layout.U8(drum.offset, drum.release_rate);
        layout.U8(drum.offset + 1, drum.pan);
        layout.U8(drum.offset + 2, drum.loaded);
        write_tuned(drum.offset + 4, drum.sound);
        layout.U32(drum.offset + 12, OffsetOf(placed.envelopes, drum.envelope));
    }

    const std::size_t instruments_at = early ? 4 : 8;
    for (std::size_t i = 0; i < program_instruments.size(); i++)
    {
        layout.U32(instruments_at + i * 4, OffsetOf(placed.instruments, program_instruments[i]));
    }

    layout.U32(0, drums_table_offset);
    if (drums_table_offset != 0)
    {
        for (std::size_t i = 0; i < drum_notes.size(); i++)
        {
            layout.U32(drums_table_offset + i * 4, OffsetOf(placed.drums, drum_notes[i]));
        }
    }

    if (!early)
    {
        layout.U32(4, effects_offset);
        for (std::size_t i = 0; i < effects.size(); i++)
        {
            write_tuned(effects_offset + i * kEffectSize, effects[i].sound);
        }
    }

    return layout.Take();
}

EadTable EadTable::Parse(std::span<const uint8_t> data)
{
    const xsflib::Reader reader(data, true);

    EadTable table;
    const std::size_t count = reader.U16(0);
    table.medium_parameter = reader.U16(2);
    table.base_address = reader.U32(4);

    const auto padding = reader.Sub(8, 8);
    table.padding.assign(padding.begin(), padding.end());
    table.entries.reserve(count);
    reader.Sub(kHeaderSize, count * kEntrySize);
    for (std::size_t i = 0; i < count; i++)
    {
        const std::size_t at = kHeaderSize + i * kEntrySize;

        Entry entry;
        entry.address = reader.U32(at);
        entry.size = reader.U32(at + 4);
        entry.medium = reader.U8(at + 8);
        entry.cache_policy = reader.U8(at + 9);
        entry.data1 = reader.U16(at + 10);
        entry.data2 = reader.U16(at + 12);
        entry.data3 = reader.U16(at + 14);
        table.entries.push_back(entry);
    }

    return table;
}

std::vector<uint8_t> EadTable::Write() const
{
    std::vector<uint8_t> bytes(kHeaderSize + entries.size() * kEntrySize);
    xsflib::StoreBe16(bytes.data(), static_cast<uint16_t>(entries.size()));
    xsflib::StoreBe16(bytes.data() + 2, medium_parameter);
    xsflib::StoreBe32(bytes.data() + 4, base_address);
    for (std::size_t i = 0; i < padding.size() && i < 8; i++)
    {
        bytes[8 + i] = padding[i];
    }

    for (std::size_t i = 0; i < entries.size(); i++)
    {
        const std::size_t at = kHeaderSize + i * kEntrySize;
        xsflib::StoreBe32(bytes.data() + at, entries[i].address);
        xsflib::StoreBe32(bytes.data() + at + 4, entries[i].size);
        bytes[at + 8] = entries[i].medium;
        bytes[at + 9] = entries[i].cache_policy;
        xsflib::StoreBe16(bytes.data() + at + 10, entries[i].data1);
        xsflib::StoreBe16(bytes.data() + at + 12, entries[i].data2);
        xsflib::StoreBe16(bytes.data() + at + 14, entries[i].data3);
    }

    return bytes;
}

std::vector<std::vector<uint8_t>> ParseEarlyBankSets(std::span<const uint8_t> data, std::size_t sequence_count)
{
    const xsflib::Reader reader(data, true);
    std::vector<std::vector<uint8_t>> sets(sequence_count);
    for (std::size_t i = 0; i < sequence_count; i++)
    {
        const std::size_t at = reader.U16(i * 2);
        const std::size_t count = reader.U8(at);
        const auto banks = reader.Sub(at + 1, count);
        sets[i].assign(banks.begin(), banks.end());
    }

    return sets;
}

} // namespace ntdlib::n64
