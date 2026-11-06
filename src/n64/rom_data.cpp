// SPDX-License-Identifier: MIT

#include "ntdlib/n64/rom_data.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/midi.h"
#include "ntdlib/n64/seq_file.h"
#include "ntdlib/n64/vadpcm.h"

namespace ntdlib::n64
{

namespace
{

constexpr std::size_t kMaxDecodedSize = 0x4000000;
constexpr std::size_t kFileEntrySize = 16;
constexpr std::size_t kLastFileChecked = 64;
constexpr std::size_t kMinCodeSize = 0x80000;
constexpr std::size_t kMaxCodeSize = 0x400000;
constexpr std::size_t kTableHeaderSize = 16;
constexpr std::size_t kTableEntrySize = 16;
constexpr uint8_t kCartridgeMedium = 2;
constexpr std::size_t kLoopsChecked = 8;
constexpr std::size_t kLaterBanksChecked = 4;
constexpr uint8_t kFirstScriptCommand = 0x80;
constexpr std::array<uint8_t, 2> kRareMagic = {0x11, 0x72};
constexpr std::size_t kRareHeaderSize = kRareMagic.size();
constexpr std::size_t kRareTableHeaderSize = 4;
constexpr std::size_t kRareTableEntrySize = 8;

// The start of an image's file table: the entry of the boot code. That code starts at 0 and runs to 0x1060.
constexpr uint8_t kFileTableStart[] = {0, 0, 0, 0, 0, 0, 0x10, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x60};

// A decoder's three streams.
struct ThreeStreams
{
    std::size_t bits = 16;
    std::size_t references = 0;
    std::size_t literals = 0;
    uint8_t current = 0;
    int bits_left = 0;
};

bool NextBit(std::span<const uint8_t> data, ThreeStreams& streams)
{
    if (streams.bits_left == 0)
    {
        if (streams.bits >= data.size())
        {
            throw xsflib::FormatError("a compressed block's bits leave the data");
        }

        streams.current = data[streams.bits++];
        streams.bits_left = 8;
    }

    streams.bits_left--;

    return ((streams.current >> streams.bits_left) & 1) != 0;
}

uint8_t ByteAt(std::span<const uint8_t> data, std::size_t offset)
{
    if (offset >= data.size())
    {
        throw xsflib::FormatError("a compressed block leaves the data");
    }

    return data[offset];
}

void CopyBack(std::vector<uint8_t>& out, std::size_t distance, std::size_t count)
{
    if (distance > out.size())
    {
        throw xsflib::FormatError("a compressed block repeats bytes before its start");
    }

    for (std::size_t i = 0; i < count; i++)
    {
        out.push_back(out[out.size() - distance]);
    }
}

// A raw DEFLATE stream's bits, from the low bit of each byte up.
class DeflateBits
{
public:
    explicit DeflateBits(std::span<const uint8_t> data) : data_(data)
    {
    }

    uint32_t Take(int count)
    {
        uint32_t value = 0;
        for (int i = 0; i < count; i++)
        {
            if (bit_ == 0)
            {
                current_ = ByteAt(data_, place_++);
            }

            value |= static_cast<uint32_t>((current_ >> bit_) & 1) << i;
            bit_ = (bit_ + 1) & 7;
        }

        return value;
    }

    // A stored block's bytes start on the next whole byte.
    void Align()
    {
        bit_ = 0;
    }

    uint8_t TakeByte()
    {
        return ByteAt(data_, place_++);
    }

private:
    std::span<const uint8_t> data_;
    std::size_t place_ = 0;
    uint8_t current_ = 0;
    int bit_ = 0;
};

// A canonical Huffman code, from each symbol's length in bits. A code holds the symbols in order of length, and within
// a length in order of symbol.
class DeflateCode
{
public:
    explicit DeflateCode(std::span<const uint8_t> lengths)
    {
        for (const uint8_t length : lengths)
        {
            counts_[length]++;
        }

        counts_[0] = 0;
        std::array<uint16_t, kMaxBits + 1> starts = {};
        int left = 1;
        for (std::size_t length = 1; length <= kMaxBits; length++)
        {
            left = left * 2 - counts_[length];
            if (left < 0)
            {
                throw xsflib::FormatError("a DEFLATE code has too many symbols of a length");
            }

            starts[length] = static_cast<uint16_t>(starts[length - 1] + counts_[length - 1]);
        }

        symbols_.resize(lengths.size());
        for (std::size_t symbol = 0; symbol < lengths.size(); symbol++)
        {
            if (lengths[symbol] != 0)
            {
                symbols_[starts[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
            }
        }
    }

    int Decode(DeflateBits& bits) const
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (std::size_t length = 1; length <= kMaxBits; length++)
        {
            code |= static_cast<int>(bits.Take(1));
            const int count = counts_[length];
            if (code - first < count)
            {
                return symbols_[static_cast<std::size_t>(index + code - first)];
            }

            index += count;
            first = (first + count) * 2;
            code *= 2;
        }

        throw xsflib::FormatError("a DEFLATE stream holds a code that its table doesn't");
    }

private:
    static constexpr std::size_t kMaxBits = 15;

    std::array<uint16_t, kMaxBits + 1> counts_ = {};
    std::vector<uint16_t> symbols_;
};

void InflateCodes(DeflateBits& bits, const DeflateCode& symbols, const DeflateCode& distances,
                  std::vector<uint8_t>& out)
{
    static constexpr std::array<uint16_t, 29> kLengthBases = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                              15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                              67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<uint8_t, 29> kLengthBits = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::array<uint16_t, 30> kDistanceBases = {
        1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<uint8_t, 30> kDistanceBits = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (true)
    {
        const int symbol = symbols.Decode(bits);
        if (symbol < 256)
        {
            out.push_back(static_cast<uint8_t>(symbol));
        }
        else if (symbol == 256)
        {
            return;
        }
        else
        {
            const auto length_code = static_cast<std::size_t>(symbol - 257);
            if (length_code >= kLengthBases.size())
            {
                throw xsflib::FormatError("a DEFLATE stream holds an unknown length");
            }

            const std::size_t count = kLengthBases[length_code] + bits.Take(kLengthBits[length_code]);
            const auto distance_code = static_cast<std::size_t>(distances.Decode(bits));
            if (distance_code >= kDistanceBases.size())
            {
                throw xsflib::FormatError("a DEFLATE stream holds an unknown distance");
            }

            CopyBack(out, kDistanceBases[distance_code] + bits.Take(kDistanceBits[distance_code]), count);
        }

        if (out.size() > kMaxDecodedSize)
        {
            throw xsflib::FormatError("a DEFLATE stream decodes to more than 64 MiB");
        }
    }
}

void InflateDynamic(DeflateBits& bits, std::vector<uint8_t>& out)
{
    static constexpr std::array<uint8_t, 19> kLengthOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                             11, 4,  12, 3, 13, 2, 14, 1, 15};
    const std::size_t symbol_count = bits.Take(5) + 257;
    const std::size_t distance_count = bits.Take(5) + 1;
    const std::size_t length_count = bits.Take(4) + 4;
    std::array<uint8_t, 19> code_lengths = {};
    for (std::size_t i = 0; i < length_count; i++)
    {
        code_lengths[kLengthOrder[i]] = static_cast<uint8_t>(bits.Take(3));
    }

    const DeflateCode length_code(code_lengths);
    std::vector<uint8_t> lengths;
    lengths.reserve(symbol_count + distance_count);
    while (lengths.size() < symbol_count + distance_count)
    {
        const int symbol = length_code.Decode(bits);
        if (symbol < 16)
        {
            lengths.push_back(static_cast<uint8_t>(symbol));
            continue;
        }

        uint8_t value = 0;
        std::size_t repeat = 0;
        if (symbol == 16)
        {
            if (lengths.empty())
            {
                throw xsflib::FormatError("a DEFLATE table repeats a length before its first");
            }

            value = lengths.back();
            repeat = 3 + bits.Take(2);
        }
        else
        {
            repeat = symbol == 17 ? 3 + bits.Take(3) : 11 + bits.Take(7);
        }

        lengths.insert(lengths.end(), repeat, value);
    }

    if (lengths.size() != symbol_count + distance_count)
    {
        throw xsflib::FormatError("a DEFLATE table holds too many lengths");
    }

    const std::span<const uint8_t> all = lengths;
    InflateCodes(bits, DeflateCode(all.first(symbol_count)), DeflateCode(all.subspan(symbol_count)), out);
}

void InflateFixed(DeflateBits& bits, std::vector<uint8_t>& out)
{
    std::array<uint8_t, 288> symbols = {};
    std::fill(symbols.begin(), symbols.begin() + 144, uint8_t{8});
    std::fill(symbols.begin() + 144, symbols.begin() + 256, uint8_t{9});
    std::fill(symbols.begin() + 256, symbols.begin() + 280, uint8_t{7});
    std::fill(symbols.begin() + 280, symbols.end(), uint8_t{8});
    std::array<uint8_t, 30> distances = {};
    distances.fill(5);
    InflateCodes(bits, DeflateCode(symbols), DeflateCode(distances), out);
}

void InflateStored(DeflateBits& bits, std::vector<uint8_t>& out)
{
    bits.Align();
    const uint32_t size = bits.Take(16);
    const uint32_t check = bits.Take(16);
    if ((size ^ check) != 0xFFFF)
    {
        throw xsflib::FormatError("a stored DEFLATE block's sizes disagree");
    }

    for (uint32_t i = 0; i < size; i++)
    {
        out.push_back(bits.TakeByte());
    }
}

std::size_t DecodedSize(std::span<const uint8_t> data, const char* magic, std::size_t header_size)
{
    if (data.size() < header_size || std::memcmp(data.data(), magic, 4) != 0)
    {
        throw xsflib::FormatError(std::string("not ") + magic + " data");
    }

    const std::size_t size = xsflib::LoadBe32(data.data() + 4);
    if (size > kMaxDecodedSize)
    {
        throw xsflib::FormatError(std::string(magic) + " data decodes to more than 64 MiB");
    }

    return size;
}

// The first bit of a loop's frames in a wave: the frames up to the end of the frame that holds the loop's start.
bool LoopStateFits(std::span<const uint8_t> image, const BankFile& bank, const BankWave& wave, std::size_t table)
{
    const BankLoop& loop = bank.loops[static_cast<std::size_t>(wave.loop)];
    const VadpcmBook& book = bank.books[static_cast<std::size_t>(wave.book)].book;
    if (!book.Usable())
    {
        return false;
    }

    const std::size_t frames = loop.start / kVadpcmFrameSamples + 1;
    const std::size_t size = frames * kVadpcmFrameSize;
    if (table + wave.base + size > image.size())
    {
        return false;
    }

    VadpcmState state = {};
    try
    {
        DecodeVadpcm(image.subspan(table + wave.base, size), book, state);
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }

    return state == loop.state;
}

// A table of the later engine: its count, zero padding, and entries on the cartridge with rising addresses.
bool LooksLikeLaterTable(std::span<const uint8_t> data, std::size_t offset, std::size_t& count)
{
    count = xsflib::LoadBe16(data.data() + offset);
    if (count < 2 || count > 1024 || offset + kTableHeaderSize + count * kTableEntrySize > data.size())
    {
        return false;
    }

    if (xsflib::LoadBe32(data.data() + offset + 4) != 0 || xsflib::LoadBe32(data.data() + offset + 8) != 0 ||
        xsflib::LoadBe32(data.data() + offset + 12) != 0)
    {
        return false;
    }

    uint32_t previous = 0;
    for (std::size_t i = 0; i < count; i++)
    {
        const uint8_t* entry = data.data() + offset + kTableHeaderSize + i * kTableEntrySize;
        const uint32_t address = xsflib::LoadBe32(entry);
        const uint32_t size = xsflib::LoadBe32(entry + 4);
        if (entry[8] != kCartridgeMedium || size > kMaxDecodedSize)
        {
            return false;
        }

        if (size != 0)
        {
            if (address < previous)
            {
                return false;
            }

            previous = address;
        }
    }

    return true;
}

// The end of the bytes that a table's entries cover.
std::size_t LaterTableEnd(std::span<const uint8_t> data, std::size_t offset)
{
    const std::size_t count = xsflib::LoadBe16(data.data() + offset);
    std::size_t end = 0;
    for (std::size_t i = 0; i < count; i++)
    {
        const uint8_t* entry = data.data() + offset + kTableHeaderSize + i * kTableEntrySize;
        end =
            std::max<std::size_t>(end, xsflib::LoadBe32(entry) + static_cast<std::size_t>(xsflib::LoadBe32(entry + 4)));
    }

    return end;
}

// Whether a table's banks read as banks at `place`.
bool LaterBanksFit(std::span<const uint8_t> image, std::span<const uint8_t> table, std::size_t place)
{
    const std::size_t count = xsflib::LoadBe16(table.data());
    std::size_t checked = 0;
    for (std::size_t i = 0; i < count && checked < kLaterBanksChecked; i++)
    {
        const uint8_t* entry = table.data() + kTableHeaderSize + i * kTableEntrySize;
        const std::size_t start = xsflib::LoadBe32(entry);
        const std::size_t size = xsflib::LoadBe32(entry + 4);
        if (size == 0)
        {
            continue;
        }

        if (place + start + size > image.size() || size < 8)
        {
            return false;
        }

        // A bank starts with offsets that lie inside it, and a cheap look at them rejects most places.
        const uint32_t instruments = xsflib::LoadBe16(entry + 12) >> 8;
        const std::size_t words = std::min<std::size_t>(size / 4, 2 + instruments);
        bool any = instruments == 0;
        for (std::size_t word = 0; word < words; word++)
        {
            const uint32_t value = xsflib::LoadBe32(image.data() + place + start + word * 4);
            if (value >= size)
            {
                return false;
            }

            any = any || value != 0;
        }

        // A stretch of zeros reads as a bank without objects.
        if (!any)
        {
            return false;
        }

        try
        {
            EadBank::ParseLater(image.subspan(place + start, size), instruments, xsflib::LoadBe16(entry + 12) & 0xFF,
                                xsflib::LoadBe16(entry + 14));
        }
        catch (const xsflib::FormatError&)
        {
            return false;
        }

        checked++;
    }

    return checked != 0;
}

// Whether a table's sequences start with commands at `place`.
bool LaterSequencesFit(std::span<const uint8_t> image, std::span<const uint8_t> table, std::size_t place)
{
    const std::size_t count = xsflib::LoadBe16(table.data());
    for (std::size_t i = 0; i < count; i++)
    {
        const uint8_t* entry = table.data() + kTableHeaderSize + i * kTableEntrySize;
        const std::size_t start = xsflib::LoadBe32(entry);
        const std::size_t size = xsflib::LoadBe32(entry + 4);
        if (size == 0)
        {
            continue;
        }

        if (place + start + size > image.size() || image[place + start] < kFirstScriptCommand)
        {
            return false;
        }
    }

    return true;
}

// The end of a table of each sequence's banks: the place after the set that lies last.
std::size_t LaterBankSetsEnd(std::span<const uint8_t> bytes, std::size_t table, std::size_t sequence_count)
{
    std::size_t end = table + sequence_count * 2;
    for (std::size_t i = 0; i < sequence_count; i++)
    {
        const std::size_t place = table + xsflib::LoadBe16(bytes.data() + table + i * 2);
        if (place < bytes.size())
        {
            end = std::max(end, std::min(bytes.size(), place + 1 + bytes[place]));
        }
    }

    return end;
}

// A game without a file table may hold the tables in its image as they are, in the order of the banks, the samples and
// the sequences, and the three files in a row in the same order (Pokémon Stadium 2).
std::optional<LaterSoundData> FindPlainLaterSoundData(std::span<const uint8_t> image)
{
    for (std::size_t offset = 0; offset + kTableHeaderSize < image.size(); offset += 16)
    {
        std::size_t bank_count = 0;
        if (!LooksLikeLaterTable(image, offset, bank_count))
        {
            continue;
        }

        const std::size_t samples = offset + kTableHeaderSize + bank_count * kTableEntrySize;
        std::size_t sample_count = 0;
        if (samples + kTableHeaderSize >= image.size() || !LooksLikeLaterTable(image, samples, sample_count))
        {
            continue;
        }

        const std::size_t sequences = samples + kTableHeaderSize + sample_count * kTableEntrySize;
        std::size_t sequence_count = 0;
        if (sequences + kTableHeaderSize >= image.size() || !LooksLikeLaterTable(image, sequences, sequence_count))
        {
            continue;
        }

        const std::size_t tables_end = sequences + kTableHeaderSize + sequence_count * kTableEntrySize;
        const std::size_t banks_size = LaterTableEnd(image, offset);
        const std::size_t samples_size = LaterTableEnd(image, samples);
        for (std::size_t place = 0; place + banks_size + samples_size < image.size(); place += 16)
        {
            if (!LaterBanksFit(image, image.subspan(offset), place) ||
                !LaterSequencesFit(image, image.subspan(sequences), place + banks_size + samples_size))
            {
                continue;
            }

            LaterSoundData data;
            // The table of each sequence's banks follows the three tables.
            std::size_t sets_end = tables_end;
            if (tables_end + sequence_count * 2 <= image.size() &&
                xsflib::LoadBe16(image.data() + tables_end) == sequence_count * 2)
            {
                sets_end = LaterBankSetsEnd(image, tables_end, sequence_count);
                data.bank_set_table = tables_end - offset;
            }

            data.code.assign(image.begin() + static_cast<std::ptrdiff_t>(offset),
                             image.begin() + static_cast<std::ptrdiff_t>(sets_end));
            data.bank_table = 0;
            data.sample_table = samples - offset;
            data.sequence_table = sequences - offset;
            data.banks = place;
            data.samples = place + banks_size;
            data.sequences = place + banks_size + samples_size;

            return data;
        }
    }

    return std::nullopt;
}

} // namespace

std::vector<uint8_t> DecodeMio0(std::span<const uint8_t> data)
{
    const std::size_t size = DecodedSize(data, "MIO0", 16);

    ThreeStreams streams;
    streams.references = xsflib::LoadBe32(data.data() + 8);
    streams.literals = xsflib::LoadBe32(data.data() + 12);
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size)
    {
        if (NextBit(data, streams))
        {
            out.push_back(ByteAt(data, streams.literals++));
            continue;
        }

        const uint8_t high = ByteAt(data, streams.references++);
        const uint8_t low = ByteAt(data, streams.references++);
        CopyBack(out, (static_cast<std::size_t>(high & 0x0F) << 8 | low) + 1, (high >> 4) + 3U);
    }

    out.resize(size);

    return out;
}

std::vector<uint8_t> DecodeYay0(std::span<const uint8_t> data)
{
    const std::size_t size = DecodedSize(data, "Yay0", 16);

    ThreeStreams streams;
    streams.references = xsflib::LoadBe32(data.data() + 8);
    streams.literals = xsflib::LoadBe32(data.data() + 12);
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size)
    {
        if (NextBit(data, streams))
        {
            out.push_back(ByteAt(data, streams.literals++));
            continue;
        }

        const uint8_t high = ByteAt(data, streams.references++);
        const uint8_t low = ByteAt(data, streams.references++);
        std::size_t count = high >> 4;
        count = count == 0 ? ByteAt(data, streams.literals++) + 18U : count + 2;
        CopyBack(out, (static_cast<std::size_t>(high & 0x0F) << 8 | low) + 1, count);
    }

    out.resize(size);

    return out;
}

std::vector<uint8_t> DecodeYaz0(std::span<const uint8_t> data)
{
    const std::size_t size = DecodedSize(data, "Yaz0", 16);
    std::vector<uint8_t> out;
    out.reserve(size);
    std::size_t position = 16;
    while (out.size() < size)
    {
        const uint8_t flags = ByteAt(data, position++);
        for (int bit = 7; bit >= 0 && out.size() < size; bit--)
        {
            if (((flags >> bit) & 1) != 0)
            {
                out.push_back(ByteAt(data, position++));
                continue;
            }

            const uint8_t high = ByteAt(data, position++);
            const uint8_t low = ByteAt(data, position++);
            std::size_t count = high >> 4;
            count = count == 0 ? ByteAt(data, position++) + 18U : count + 2;
            CopyBack(out, (static_cast<std::size_t>(high & 0x0F) << 8 | low) + 1, count);
        }
    }

    out.resize(size);

    return out;
}

std::vector<uint8_t> DecodeRare(std::span<const uint8_t> data)
{
    if (data.size() < kRareHeaderSize || data[0] != kRareMagic[0] || data[1] != kRareMagic[1])
    {
        throw xsflib::FormatError("not a block of Rare's");
    }

    DeflateBits bits(data.subspan(kRareHeaderSize));
    std::vector<uint8_t> out;
    bool last = false;
    while (!last)
    {
        last = bits.Take(1) != 0;
        switch (bits.Take(2))
        {
        case 0:
            InflateStored(bits, out);
            break;
        case 1:
            InflateFixed(bits, out);
            break;
        case 2:
            InflateDynamic(bits, out);
            break;
        default:
            throw xsflib::FormatError("a DEFLATE stream holds an unknown block type");
        }
    }

    return out;
}

RareSequenceTable RareSequenceTable::Parse(std::span<const uint8_t> data)
{
    if (data.size() < kRareTableHeaderSize)
    {
        throw xsflib::FormatError("a table of Rare's sequences leaves the data");
    }

    const std::size_t count = xsflib::LoadBe16(data.data());
    if (kRareTableHeaderSize + count * kRareTableEntrySize > data.size())
    {
        throw xsflib::FormatError("a table of Rare's sequences leaves the data");
    }

    RareSequenceTable table;
    table.data = data;
    table.entries.reserve(count);
    for (std::size_t i = 0; i < count; i++)
    {
        const uint8_t* entry = data.data() + kRareTableHeaderSize + i * kRareTableEntrySize;
        table.entries.push_back({xsflib::LoadBe32(entry), xsflib::LoadBe16(entry + 4), xsflib::LoadBe16(entry + 6)});
        if (uint64_t{table.entries.back().offset} + table.entries.back().block_size > data.size())
        {
            throw xsflib::FormatError("a block of Rare's sequences leaves the data");
        }
    }

    return table;
}

std::vector<uint8_t> RareSequenceTable::Sequence(std::size_t index) const
{
    const Entry& entry = entries.at(index);
    std::vector<uint8_t> out = DecodeRare(data.subspan(entry.offset, entry.block_size));
    if (out.size() != entry.size)
    {
        throw xsflib::FormatError("a block of Rare's sequences has the wrong size");
    }

    return out;
}

std::optional<std::size_t> FindRareSequences(std::span<const uint8_t> image)
{
    for (std::size_t place = 0; place + kRareTableHeaderSize + kRareTableEntrySize <= image.size(); place += 2)
    {
        const std::size_t count = xsflib::LoadBe16(image.data() + place);
        const std::size_t after_table = kRareTableHeaderSize + count * kRareTableEntrySize;
        if (count < 2 || place + after_table > image.size())
        {
            continue;
        }

        std::size_t expected = after_table;
        bool fits = true;
        for (std::size_t i = 0; i < count && fits; i++)
        {
            const uint8_t* entry = image.data() + place + kRareTableHeaderSize + i * kRareTableEntrySize;
            const std::size_t block = place + expected;
            fits = xsflib::LoadBe32(entry) == expected && block + kRareHeaderSize <= image.size() &&
                   image[block] == kRareMagic[0] && image[block + 1] == kRareMagic[1];
            expected += xsflib::LoadBe16(entry + 6);
        }

        if (fits)
        {
            return place;
        }
    }

    return std::nullopt;
}

FoundFiles FindFiles(std::span<const uint8_t> image)
{
    FoundFiles found;
    for (std::size_t offset = 0; offset + 4 <= image.size(); offset++)
    {
        const uint8_t first = image[offset];
        if (first == 'M')
        {
            if (image[offset + 1] == 'T' && image[offset + 2] == 'h' && image[offset + 3] == 'd' &&
                StandardMidi::Matches(image.subspan(offset)))
            {
                found.standard_midi.push_back(offset);
            }
        }
        else if (offset % 2 == 0 && image[offset + 1] == '1')
        {
            if (first == 'S' && SeqFile::Matches(image.subspan(offset)))
            {
                found.sequence_banks.push_back(offset);
            }
            else if (first == 'B' && BankFile::Matches(image.subspan(offset)))
            {
                found.bank_files.push_back(offset);
            }
        }
    }

    return found;
}

std::optional<std::size_t> FindBankSamples(std::span<const uint8_t> image, const BankFile& bank)
{
    std::vector<const BankWave*> waves;
    for (const BankWave& wave : bank.waves)
    {
        if (wave.type == kWaveVadpcm && wave.loop != kNoObject && wave.book != kNoObject &&
            bank.loops[static_cast<std::size_t>(wave.loop)].count != 0)
        {
            waves.push_back(&wave);
        }
    }

    if (waves.empty())
    {
        return std::nullopt;
    }

    const auto earlier = [&](const BankWave* a, const BankWave* b)
    {
        return bank.loops[static_cast<std::size_t>(a->loop)].start <
               bank.loops[static_cast<std::size_t>(b->loop)].start;
    };
    std::sort(waves.begin(), waves.end(), earlier);

    // Most of the time, the data lies on a multiple of 8 bytes and the first wave rules out every other place.
    const std::size_t checks = std::min(waves.size(), kLoopsChecked);
    for (std::size_t table = 0; table + kVadpcmFrameSize < image.size(); table += 8)
    {
        if (!LoopStateFits(image, bank, *waves.front(), table))
        {
            continue;
        }

        const auto fits = [&](const BankWave* wave)
        {
            return LoopStateFits(image, bank, *wave, table);
        };
        const auto end = waves.begin() + static_cast<std::ptrdiff_t>(checks);
        const auto good = std::count_if(waves.begin(), end, fits);

        // A few loops of a game's banks hold states from other tools. The majority decides.
        if (static_cast<std::size_t>(good) * 2 > checks)
        {
            return table;
        }
    }

    return std::nullopt;
}

std::optional<EarlySoundData> FindEarlySoundData(std::span<const uint8_t> image)
{
    std::vector<std::pair<std::size_t, uint16_t>> banks;
    std::vector<std::pair<std::size_t, uint16_t>> samples;
    std::vector<std::pair<std::size_t, uint16_t>> sequences;
    for (std::size_t offset = 0; offset + 16 <= image.size(); offset += 16)
    {
        if (image[offset] != 0)
        {
            continue;
        }

        const uint16_t revision = xsflib::LoadBe16(image.data() + offset);
        if (revision < kEarlyBankFileRevision || revision > kEarlySequenceFileRevision)
        {
            continue;
        }

        const uint16_t count = xsflib::LoadBe16(image.data() + offset + 2);

        // The first entry of the table lies just past it.
        const uint32_t first = xsflib::LoadBe32(image.data() + offset + 4);
        if (count < 2 || count > 512 || first < 4U + count * 8U || first > 4U + count * 8U + 16U ||
            !SeqFile::Matches(image.subspan(offset), revision))
        {
            continue;
        }

        (revision == kEarlyBankFileRevision     ? banks
         : revision == kEarlySampleFileRevision ? samples
                                                : sequences)
            .emplace_back(offset, count);
    }

    if (sequences.size() != 1)
    {
        return std::nullopt;
    }

    EarlySoundData data;
    data.sequences = sequences.front().first;
    bool paired = false;
    for (const auto& [bank_offset, bank_count] : banks)
    {
        for (const auto& [sample_offset, sample_count] : samples)
        {
            if (!paired && bank_count == sample_count)
            {
                data.banks = bank_offset;
                data.samples = sample_offset;
                paired = true;
            }
        }
    }

    if (!paired)
    {
        return std::nullopt;
    }

    // The table of each sequence's banks: 16-bit offsets that start past the table, and for each a count and that many
    // numbers of banks.
    const std::size_t sequence_count = sequences.front().second;
    const std::size_t bank_count = xsflib::LoadBe16(image.data() + data.banks + 2);
    for (std::size_t offset = 0; offset + sequence_count * 2 < image.size(); offset += 16)
    {
        if (xsflib::LoadBe16(image.data() + offset) != sequence_count * 2)
        {
            continue;
        }

        bool good = true;
        std::size_t expected = sequence_count * 2;
        for (std::size_t i = 0; i < sequence_count && good; i++)
        {
            const std::size_t at = offset + expected;
            good = xsflib::LoadBe16(image.data() + offset + i * 2) == expected && at < image.size() && image[at] > 0 &&
                   image[at] < 16 && at + 1 + image[at] <= image.size();
            if (good)
            {
                const uint8_t count = image[at];
                for (uint8_t j = 0; j < count && good; j++)
                {
                    good = image[at + 1 + j] < bank_count;
                }

                expected += 1U + count;
            }
        }

        if (good)
        {
            data.bank_sets = offset;
            break;
        }
    }

    return data;
}

std::optional<LaterSoundData> FindLaterSoundData(std::span<const uint8_t> image)
{
    // The file table: the image's first search for the boot code's entry that lies past the boot code.
    const auto found = std::search(image.begin(), image.end(), std::begin(kFileTableStart), std::end(kFileTableStart));
    if (found == image.end())
    {
        return FindPlainLaterSoundData(image);
    }

    const auto table = static_cast<std::size_t>(found - image.begin());

    // The entries of the three files follow the first ones: the banks, the sequences and the samples, by their order on
    // the image. Their places are read below, from the game's code.
    struct File
    {
        uint32_t virtual_start;
        uint32_t virtual_end;
        uint32_t image_start;
        uint32_t image_end;
    };
    std::vector<File> files;
    for (std::size_t offset = table; offset + kFileEntrySize <= image.size(); offset += kFileEntrySize)
    {
        const File file = {xsflib::LoadBe32(image.data() + offset), xsflib::LoadBe32(image.data() + offset + 4),
                           xsflib::LoadBe32(image.data() + offset + 8), xsflib::LoadBe32(image.data() + offset + 12)};
        if (file.virtual_end == 0)
        {
            break;
        }

        files.push_back(file);
    }

    if (files.size() < 8)
    {
        return std::nullopt;
    }

    // The game's code is the large compressed file among the first ones that holds the tables.
    const std::size_t last = std::min(files.size(), kLastFileChecked);
    for (std::size_t index = 0; index < last; index++)
    {
        const File& file = files[index];
        const std::size_t size = file.virtual_end - file.virtual_start;
        if (file.image_end == 0 || size < kMinCodeSize || size > kMaxCodeSize || file.image_end > image.size() ||
            file.image_start >= file.image_end)
        {
            continue;
        }

        LaterSoundData data;
        try
        {
            data.code = DecodeYaz0(image.subspan(file.image_start, file.image_end - file.image_start));
        }
        catch (const xsflib::FormatError&)
        {
            continue;
        }

        // The three tables lie in a row in the code: the banks', the sequences' and the samples'.
        std::vector<std::size_t> tables;
        for (std::size_t offset = 0; offset + kTableHeaderSize < data.code.size(); offset += 16)
        {
            std::size_t count = 0;
            if (LooksLikeLaterTable(data.code, offset, count))
            {
                tables.push_back(offset);
            }
        }

        if (tables.size() != 3)
        {
            continue;
        }

        data.bank_table = tables[0];
        data.sequence_table = tables[1];
        data.sample_table = tables[2];

        // The table of each sequence's banks lies between the tables of banks and sequences.
        std::size_t bank_count = 0;
        LooksLikeLaterTable(data.code, data.bank_table, bank_count);
        std::size_t sequence_count = 0;
        LooksLikeLaterTable(data.code, data.sequence_table, sequence_count);
        const std::size_t sets = data.bank_table + kTableHeaderSize + bank_count * kTableEntrySize;
        if (sets + sequence_count * 2 <= data.sequence_table)
        {
            data.bank_set_table = sets;
        }

        // The files lie in the same order right after the first files of the table: the first plain file whose size
        // fits each table's sum.
        const auto end_of = [&](std::size_t offset)
        {
            const std::size_t count = xsflib::LoadBe16(data.code.data() + offset);
            std::size_t end = 0;
            for (std::size_t i = 0; i < count; i++)
            {
                const uint8_t* entry = data.code.data() + offset + kTableHeaderSize + i * kTableEntrySize;
                end = std::max<std::size_t>(
                    end, xsflib::LoadBe32(entry) + static_cast<std::size_t>(xsflib::LoadBe32(entry + 4)));
            }

            return end;
        };
        const auto file_of_size = [&](std::size_t size_at_least) -> std::optional<std::size_t>
        {
            for (std::size_t i = 0; i < last; i++)
            {
                const std::size_t file_size = files[i].virtual_end - files[i].virtual_start;
                if (files[i].image_end == 0 && file_size >= size_at_least && file_size < size_at_least + 0x1000)
                {
                    return files[i].image_start;
                }
            }

            return std::nullopt;
        };
        const auto banks = file_of_size(end_of(data.bank_table));
        const auto sequences = file_of_size(end_of(data.sequence_table));
        const auto samples = file_of_size(end_of(data.sample_table));
        if (!banks || !sequences || !samples)
        {
            continue;
        }

        data.banks = *banks;
        data.sequences = *sequences;
        data.samples = *samples;

        return data;
    }

    return std::nullopt;
}

} // namespace ntdlib::n64
