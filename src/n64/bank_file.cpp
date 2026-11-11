// SPDX-License-Identifier: MIT

#include "ntdlib/n64/bank_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

constexpr std::size_t kBanksOffset = 4;
constexpr std::size_t kBankHeaderSize = 12;
constexpr std::size_t kInstrumentHeaderSize = 16;
constexpr std::size_t kSoundSize = 16;
constexpr std::size_t kEnvelopeSize = 14;
constexpr std::size_t kKeyMapSize = 6;
constexpr std::size_t kWaveSize = 20;
constexpr std::size_t kRawLoopSize = 12;
constexpr std::size_t kVadpcmLoopSize = kRawLoopSize + 2 * kVadpcmFrameSamples;
constexpr std::size_t kBookHeaderSize = 8;
constexpr std::size_t kObjectAlignment = 8;
constexpr int32_t kMaxOrder = 16;
constexpr int32_t kMaxPredictors = 256;
constexpr int32_t kMinSampleRate = 1000;
constexpr int32_t kMaxSampleRate = 100000;

// The offsets of a file's objects, each object once, with its index in its list.
class Parser
{
public:
    explicit Parser(std::span<const uint8_t> data) : reader_(data, true)
    {
    }

    BankFile Parse()
    {
        if (reader_.Size() < kBanksOffset || reader_.U16(0) != kBankFileRevision)
        {
            throw xsflib::FormatError("not a bank file of revision B1");
        }

        const std::size_t count = reader_.U16(2);
        Extend(kBanksOffset + count * 4);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint32_t offset = reader_.U32(kBanksOffset + i * 4);
            if (offset == 0)
            {
                throw xsflib::FormatError("bank " + std::to_string(i) + " of a bank file has no offset");
            }

            file_.banks.push_back(ParseBank(offset));
        }

        const auto bytes = reader_.Sub(0, end_);
        file_.image.assign(bytes.begin(), bytes.end());

        return std::move(file_);
    }

private:
    void Extend(std::size_t end)
    {
        reader_.Sub(0, end);
        end_ = std::max(end_, end);
    }

    Bank ParseBank(uint32_t offset)
    {
        const std::size_t count = reader_.U16(offset);
        Extend(offset + kBankHeaderSize + count * 4);

        Bank bank;
        bank.offset = offset;
        bank.flags = reader_.U8(offset + 2);
        bank.padding = reader_.U8(offset + 3);
        bank.sample_rate = reader_.S32(offset + 4);
        bank.capacity = count;

        const uint32_t percussion = reader_.U32(offset + 8);
        bank.percussion = percussion == 0 ? kNoObject : ParseInstrument(percussion);
        bank.instruments.reserve(count);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint32_t instrument = reader_.U32(offset + kBankHeaderSize + i * 4);
            bank.instruments.push_back(instrument == 0 ? kNoObject : ParseInstrument(instrument));
        }

        return bank;
    }

    int32_t ParseInstrument(uint32_t offset)
    {
        const auto found = instruments_.find(offset);
        if (found != instruments_.end())
        {
            return found->second;
        }

        const std::size_t count = reader_.U16(offset + 14);
        Extend(offset + kInstrumentHeaderSize + count * 4);

        BankInstrument instrument;
        instrument.offset = offset;
        instrument.volume = reader_.U8(offset);
        instrument.pan = reader_.U8(offset + 1);
        instrument.priority = reader_.U8(offset + 2);
        instrument.flags = reader_.U8(offset + 3);
        instrument.tremolo_type = reader_.U8(offset + 4);
        instrument.tremolo_rate = reader_.U8(offset + 5);
        instrument.tremolo_depth = reader_.U8(offset + 6);
        instrument.tremolo_delay = reader_.U8(offset + 7);
        instrument.vibrato_type = reader_.U8(offset + 8);
        instrument.vibrato_rate = reader_.U8(offset + 9);
        instrument.vibrato_depth = reader_.U8(offset + 10);
        instrument.vibrato_delay = reader_.U8(offset + 11);
        instrument.bend_range = reader_.S16(offset + 12);
        instrument.capacity = count;
        instrument.sounds.reserve(count);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint32_t sound = reader_.U32(offset + kInstrumentHeaderSize + i * 4);
            if (sound == 0)
            {
                throw xsflib::FormatError("an instrument of a bank file has a sound without an offset");
            }

            instrument.sounds.push_back(ParseSound(sound));
        }

        const auto index = static_cast<int32_t>(file_.instruments.size());
        instruments_[offset] = index;
        file_.instruments.push_back(std::move(instrument));

        return index;
    }

    int32_t ParseSound(uint32_t offset)
    {
        const auto found = sounds_.find(offset);
        if (found != sounds_.end())
        {
            return found->second;
        }

        Extend(offset + kSoundSize);

        BankSound sound;
        sound.offset = offset;
        sound.envelope = ParseEnvelope(reader_.U32(offset));
        sound.key_map = ParseKeyMap(reader_.U32(offset + 4));
        sound.wave = ParseWave(reader_.U32(offset + 8));
        sound.pan = reader_.U8(offset + 12);
        sound.volume = reader_.U8(offset + 13);
        sound.flags = reader_.U8(offset + 14);

        const auto index = static_cast<int32_t>(file_.sounds.size());
        sounds_[offset] = index;
        file_.sounds.push_back(sound);

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

        Extend(offset + kEnvelopeSize);

        BankEnvelope envelope;
        envelope.offset = offset;
        envelope.attack_time = reader_.S32(offset);
        envelope.decay_time = reader_.S32(offset + 4);
        envelope.release_time = reader_.S32(offset + 8);
        envelope.attack_volume = reader_.U8(offset + 12);
        envelope.decay_volume = reader_.U8(offset + 13);

        const auto index = static_cast<int32_t>(file_.envelopes.size());
        envelopes_[offset] = index;
        file_.envelopes.push_back(envelope);

        return index;
    }

    int32_t ParseKeyMap(uint32_t offset)
    {
        if (offset == 0)
        {
            return kNoObject;
        }

        const auto found = key_maps_.find(offset);
        if (found != key_maps_.end())
        {
            return found->second;
        }

        Extend(offset + kKeyMapSize);

        BankKeyMap key_map;
        key_map.offset = offset;
        key_map.velocity_min = reader_.U8(offset);
        key_map.velocity_max = reader_.U8(offset + 1);
        key_map.key_min = reader_.U8(offset + 2);
        key_map.key_max = reader_.U8(offset + 3);
        key_map.key_base = reader_.U8(offset + 4);
        key_map.detune = static_cast<int8_t>(reader_.U8(offset + 5));

        const auto index = static_cast<int32_t>(file_.key_maps.size());
        key_maps_[offset] = index;
        file_.key_maps.push_back(key_map);

        return index;
    }

    int32_t ParseWave(uint32_t offset)
    {
        if (offset == 0)
        {
            return kNoObject;
        }

        const auto found = waves_.find(offset);
        if (found != waves_.end())
        {
            return found->second;
        }

        Extend(offset + kWaveSize);

        BankWave wave;
        wave.offset = offset;
        wave.base = reader_.U32(offset);
        wave.length = reader_.U32(offset + 4);
        wave.type = reader_.U8(offset + 8);
        wave.flags = reader_.U8(offset + 9);

        const uint32_t loop = reader_.U32(offset + 12);
        if (wave.type == kWaveVadpcm)
        {
            wave.loop = ParseLoop(loop, true);
            wave.book = ParseBook(reader_.U32(offset + 16));
        }
        else if (wave.type == kWaveRaw16)
        {
            wave.loop = ParseLoop(loop, false);
        }
        else
        {
            throw xsflib::FormatError("a wave of a bank file has the unknown type " + std::to_string(wave.type));
        }

        const auto index = static_cast<int32_t>(file_.waves.size());
        waves_[offset] = index;
        file_.waves.push_back(wave);

        return index;
    }

    int32_t ParseLoop(uint32_t offset, bool has_state)
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

        Extend(offset + (has_state ? kVadpcmLoopSize : kRawLoopSize));

        BankLoop loop;
        loop.offset = offset;
        loop.start = reader_.U32(offset);
        loop.end = reader_.U32(offset + 4);
        loop.count = reader_.U32(offset + 8);
        loop.has_state = has_state;
        if (has_state)
        {
            for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
            {
                loop.state[i] = reader_.S16(offset + kRawLoopSize + i * 2);
            }
        }

        const auto index = static_cast<int32_t>(file_.loops.size());
        loops_[offset] = index;
        file_.loops.push_back(loop);

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

        BankBook book;
        book.offset = offset;
        book.book.order = reader_.S32(offset);
        book.book.predictor_count = reader_.S32(offset + 4);
        if (book.book.order <= 0 || book.book.order > kMaxOrder || book.book.predictor_count <= 0 ||
            book.book.predictor_count > kMaxPredictors)
        {
            throw xsflib::FormatError("a book of a bank file has an order of " + std::to_string(book.book.order) +
                                      " and " + std::to_string(book.book.predictor_count) + " predictors");
        }

        const std::size_t count = static_cast<std::size_t>(book.book.order) *
                                  static_cast<std::size_t>(book.book.predictor_count) * kVadpcmGroupSamples;
        Extend(offset + kBookHeaderSize + count * 2);
        book.book.coefficients.reserve(count);
        for (std::size_t i = 0; i < count; i++)
        {
            book.book.coefficients.push_back(reader_.S16(offset + kBookHeaderSize + i * 2));
        }

        const auto index = static_cast<int32_t>(file_.books.size());
        books_[offset] = index;
        file_.books.push_back(std::move(book));

        return index;
    }

    xsflib::Reader reader_;
    BankFile file_;
    std::size_t end_ = 0;
    std::map<uint32_t, int32_t> instruments_;
    std::map<uint32_t, int32_t> sounds_;
    std::map<uint32_t, int32_t> envelopes_;
    std::map<uint32_t, int32_t> key_maps_;
    std::map<uint32_t, int32_t> waves_;
    std::map<uint32_t, int32_t> loops_;
    std::map<uint32_t, int32_t> books_;
};

// The file's bytes as the writer builds them, with the places of the objects that still need one.
class Layout
{
public:
    explicit Layout(std::vector<uint8_t> bytes) : bytes_(std::move(bytes))
    {
    }

    // The offset of an object of `size` bytes at the file's end.
    uint32_t Place(std::size_t size)
    {
        const std::size_t offset = (bytes_.size() + kObjectAlignment - 1) / kObjectAlignment * kObjectAlignment;
        bytes_.resize(offset + size, 0);

        return static_cast<uint32_t>(offset);
    }

    void Grow(std::size_t size)
    {
        if (bytes_.size() < size)
        {
            bytes_.resize(size, 0);
        }
    }

    void U8(std::size_t offset, uint8_t value)
    {
        bytes_[offset] = value;
    }

    void U16(std::size_t offset, uint16_t value)
    {
        xsflib::StoreBe16(bytes_.data() + offset, value);
    }

    void U32(std::size_t offset, uint32_t value)
    {
        xsflib::StoreBe32(bytes_.data() + offset, value);
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
};

// The offset that a pointer writes for an index, with 0 for no object.
template <typename Object>
uint32_t OffsetOf(const std::vector<Object>& objects, int32_t index)
{
    return index == kNoObject ? 0 : objects[static_cast<std::size_t>(index)].offset;
}

// Places an envelope at the end when it has no place, and writes its fields.
void WriteEnvelope(Layout& layout, BankEnvelope& envelope)
{
    if (envelope.offset == kNoOffset)
    {
        envelope.offset = layout.Place(kEnvelopeSize + 2);
    }

    layout.U32(envelope.offset, static_cast<uint32_t>(envelope.attack_time));
    layout.U32(envelope.offset + 4, static_cast<uint32_t>(envelope.decay_time));
    layout.U32(envelope.offset + 8, static_cast<uint32_t>(envelope.release_time));
    layout.U8(envelope.offset + 12, envelope.attack_volume);
    layout.U8(envelope.offset + 13, envelope.decay_volume);
}

// Places a key map at the end when it has no place, and writes its fields.
void WriteKeyMap(Layout& layout, BankKeyMap& key_map)
{
    if (key_map.offset == kNoOffset)
    {
        key_map.offset = layout.Place(kKeyMapSize);
    }

    layout.U8(key_map.offset, key_map.velocity_min);
    layout.U8(key_map.offset + 1, key_map.velocity_max);
    layout.U8(key_map.offset + 2, key_map.key_min);
    layout.U8(key_map.offset + 3, key_map.key_max);
    layout.U8(key_map.offset + 4, key_map.key_base);
    layout.U8(key_map.offset + 5, static_cast<uint8_t>(key_map.detune));
}

// Places a loop at the end when it has no place, and writes its fields and any VADPCM state.
void WriteLoop(Layout& layout, BankLoop& loop)
{
    if (loop.offset == kNoOffset)
    {
        loop.offset = layout.Place(loop.has_state ? kVadpcmLoopSize : kRawLoopSize);
    }

    layout.U32(loop.offset, loop.start);
    layout.U32(loop.offset + 4, loop.end);
    layout.U32(loop.offset + 8, loop.count);
    if (loop.has_state)
    {
        for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
        {
            layout.U16(loop.offset + kRawLoopSize + i * 2, static_cast<uint16_t>(loop.state[i]));
        }
    }
}

// Places a codebook at the end when it has no place, and writes its header and coefficients.
void WriteBook(Layout& layout, BankBook& book)
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

// Places a wave at the end when it has no place, and writes its fields. The loops and books of `placed` already have
// their places.
void WriteWave(Layout& layout, BankWave& wave, const BankFile& placed)
{
    if (wave.offset == kNoOffset)
    {
        wave.offset = layout.Place(kWaveSize);
    }

    layout.U32(wave.offset, wave.base);
    layout.U32(wave.offset + 4, wave.length);
    layout.U8(wave.offset + 8, wave.type);
    layout.U8(wave.offset + 9, wave.flags);
    layout.U32(wave.offset + 12, OffsetOf(placed.loops, wave.loop));
    if (wave.type == kWaveVadpcm)
    {
        layout.U32(wave.offset + 16, OffsetOf(placed.books, wave.book));
    }
}

// Places a sound at the end when it has no place, and writes its fields. The envelopes, key maps and waves of
// `placed` already have their places.
void WriteSound(Layout& layout, BankSound& sound, const BankFile& placed)
{
    if (sound.offset == kNoOffset)
    {
        sound.offset = layout.Place(kSoundSize);
    }

    layout.U32(sound.offset, OffsetOf(placed.envelopes, sound.envelope));
    layout.U32(sound.offset + 4, OffsetOf(placed.key_maps, sound.key_map));
    layout.U32(sound.offset + 8, OffsetOf(placed.waves, sound.wave));
    layout.U8(sound.offset + 12, sound.pan);
    layout.U8(sound.offset + 13, sound.volume);
    layout.U8(sound.offset + 14, sound.flags);
}

// Places an instrument at the end when it has no place or its list of sounds outgrew it, and writes its fields and
// sounds. The sounds of `placed` already have their places.
void WriteInstrument(Layout& layout, BankInstrument& instrument, const BankFile& placed)
{
    if (instrument.offset == kNoOffset || instrument.sounds.size() > instrument.capacity)
    {
        instrument.offset = layout.Place(kInstrumentHeaderSize + instrument.sounds.size() * 4);
    }

    layout.U8(instrument.offset, instrument.volume);
    layout.U8(instrument.offset + 1, instrument.pan);
    layout.U8(instrument.offset + 2, instrument.priority);
    layout.U8(instrument.offset + 3, instrument.flags);
    layout.U8(instrument.offset + 4, instrument.tremolo_type);
    layout.U8(instrument.offset + 5, instrument.tremolo_rate);
    layout.U8(instrument.offset + 6, instrument.tremolo_depth);
    layout.U8(instrument.offset + 7, instrument.tremolo_delay);
    layout.U8(instrument.offset + 8, instrument.vibrato_type);
    layout.U8(instrument.offset + 9, instrument.vibrato_rate);
    layout.U8(instrument.offset + 10, instrument.vibrato_depth);
    layout.U8(instrument.offset + 11, instrument.vibrato_delay);
    layout.U16(instrument.offset + 12, static_cast<uint16_t>(instrument.bend_range));
    layout.U16(instrument.offset + 14, static_cast<uint16_t>(instrument.sounds.size()));
    for (std::size_t i = 0; i < instrument.sounds.size(); i++)
    {
        layout.U32(instrument.offset + kInstrumentHeaderSize + i * 4, OffsetOf(placed.sounds, instrument.sounds[i]));
    }
}

// Places a bank at the end when it has no place or its list of instruments outgrew it, and writes its fields and
// instruments. The instruments of `placed` already have their places.
void WriteBank(Layout& layout, Bank& bank, const BankFile& placed)
{
    if (bank.offset == kNoOffset || bank.instruments.size() > bank.capacity)
    {
        bank.offset = layout.Place(kBankHeaderSize + bank.instruments.size() * 4);
    }

    layout.U16(bank.offset, static_cast<uint16_t>(bank.instruments.size()));
    layout.U8(bank.offset + 2, bank.flags);
    layout.U8(bank.offset + 3, bank.padding);
    layout.U32(bank.offset + 4, static_cast<uint32_t>(bank.sample_rate));
    layout.U32(bank.offset + 8, OffsetOf(placed.instruments, bank.percussion));
    for (std::size_t i = 0; i < bank.instruments.size(); i++)
    {
        layout.U32(bank.offset + kBankHeaderSize + i * 4, OffsetOf(placed.instruments, bank.instruments[i]));
    }
}

} // namespace

BankFile BankFile::Parse(std::span<const uint8_t> data)
{
    return Parser(data).Parse();
}

bool BankFile::Matches(std::span<const uint8_t> data)
{
    if (data.size() < kBanksOffset || xsflib::LoadBe16(data.data()) != kBankFileRevision)
    {
        return false;
    }

    const std::size_t count = xsflib::LoadBe16(data.data() + 2);
    if (count == 0 || kBanksOffset + count * 4 > data.size())
    {
        return false;
    }

    // The offsets of the banks lie on multiples of 4 bytes, past the table. Bad data rarely gets this far.
    for (std::size_t i = 0; i < count; i++)
    {
        const uint32_t offset = xsflib::LoadBe32(data.data() + kBanksOffset + i * 4);
        if (offset % 4 != 0 || offset < kBanksOffset + count * 4 || offset > data.size())
        {
            return false;
        }
    }

    try
    {
        const BankFile file = Parse(data);
        const auto plausible = [](const Bank& bank)
        {
            return bank.sample_rate >= kMinSampleRate && bank.sample_rate <= kMaxSampleRate;
        };

        return !file.sounds.empty() && std::all_of(file.banks.begin(), file.banks.end(), plausible);
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }
}

std::vector<uint8_t> BankFile::Write() const
{
    // Objects without a place, and lists that outgrew theirs, get a place at the end. The copy keeps this file as it
    // is.
    BankFile placed = *this;
    Layout layout(image);
    layout.Grow(kBanksOffset + banks.size() * 4);

    for (BankEnvelope& envelope : placed.envelopes)
    {
        WriteEnvelope(layout, envelope);
    }

    for (BankKeyMap& key_map : placed.key_maps)
    {
        WriteKeyMap(layout, key_map);
    }

    for (BankLoop& loop : placed.loops)
    {
        WriteLoop(layout, loop);
    }

    for (BankBook& book : placed.books)
    {
        WriteBook(layout, book);
    }

    for (BankWave& wave : placed.waves)
    {
        WriteWave(layout, wave, placed);
    }

    for (BankSound& sound : placed.sounds)
    {
        WriteSound(layout, sound, placed);
    }

    for (BankInstrument& instrument : placed.instruments)
    {
        WriteInstrument(layout, instrument, placed);
    }

    layout.U16(0, kBankFileRevision);
    layout.U16(2, static_cast<uint16_t>(placed.banks.size()));
    for (std::size_t b = 0; b < placed.banks.size(); b++)
    {
        Bank& bank = placed.banks[b];
        WriteBank(layout, bank, placed);
        layout.U32(kBanksOffset + b * 4, bank.offset);
    }

    return layout.Take();
}

} // namespace ntdlib::n64
