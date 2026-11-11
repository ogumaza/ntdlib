// SPDX-License-Identifier: MIT

#include "ntdlib/n64/rom_data.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/inflate.h"
#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/midi.h"
#include "ntdlib/n64/seq_file.h"
#include "ntdlib/n64/vadpcm.h"
#include "ntdlib/yaz0.h"

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

// The places and counts of the tables that read as files of the early engine, sorted by the revision of the file.
struct EarlyTables
{
    std::vector<std::pair<std::size_t, uint16_t>> banks;
    std::vector<std::pair<std::size_t, uint16_t>> samples;
    std::vector<std::pair<std::size_t, uint16_t>> sequences;
};

// Searches an image for the tables of the early engine on 16-byte boundaries.
EarlyTables FindEarlyTables(std::span<const uint8_t> image)
{
    EarlyTables tables;
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

        (revision == kEarlyBankFileRevision     ? tables.banks
         : revision == kEarlySampleFileRevision ? tables.samples
                                                : tables.sequences)
            .emplace_back(offset, count);
    }

    return tables;
}

// Sets the places of the first bank file and the first sample file with the same count. Returns false when no pair
// agrees.
bool PairEarlyBanks(const EarlyTables& tables, EarlySoundData& data)
{
    for (const auto& [bank_offset, bank_count] : tables.banks)
    {
        for (const auto& [sample_offset, sample_count] : tables.samples)
        {
            if (bank_count == sample_count)
            {
                data.banks = bank_offset;
                data.samples = sample_offset;
                return true;
            }
        }
    }

    return false;
}

// Whether the table of each sequence's banks fits at `offset`, with every bank number below `bank_count`.
bool EarlyBankSetsFit(std::span<const uint8_t> image, std::size_t offset, std::size_t sequence_count,
                      std::size_t bank_count)
{
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

    return good;
}

// The table of each sequence's banks: 16-bit offsets that start past the table, and for each a count and that many
// numbers of banks.
std::optional<std::size_t> FindEarlyBankSets(std::span<const uint8_t> image, std::size_t sequence_count,
                                             std::size_t bank_count)
{
    for (std::size_t offset = 0; offset + sequence_count * 2 < image.size(); offset += 16)
    {
        if (xsflib::LoadBe16(image.data() + offset) != sequence_count * 2)
        {
            continue;
        }

        if (EarlyBankSetsFit(image, offset, sequence_count, bank_count))
        {
            return offset;
        }
    }

    return std::nullopt;
}

// An entry of an image's file table: the places of a file in memory and on the image.
struct FileTableEntry
{
    uint32_t virtual_start;
    uint32_t virtual_end;
    uint32_t image_start;
    uint32_t image_end;
};

// Reads the entries of an image's file table up to the first empty one.
std::vector<FileTableEntry> ReadFileTable(std::span<const uint8_t> image, std::size_t table)
{
    std::vector<FileTableEntry> files;
    for (std::size_t offset = table; offset + kFileEntrySize <= image.size(); offset += kFileEntrySize)
    {
        const FileTableEntry file = {
            xsflib::LoadBe32(image.data() + offset), xsflib::LoadBe32(image.data() + offset + 4),
            xsflib::LoadBe32(image.data() + offset + 8), xsflib::LoadBe32(image.data() + offset + 12)};
        if (file.virtual_end == 0)
        {
            break;
        }

        files.push_back(file);
    }

    return files;
}

// Sets the offsets of the later engine's tables in a game's decoded code. Returns false when the code doesn't have
// exactly three tables.
bool FindLaterTablesInCode(LaterSoundData& data)
{
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
        return false;
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

    return true;
}

// The place on the image of the first plain file among the first `last` files whose size is at least `size_at_least`
// and less than 0x1000 bytes over it.
std::optional<std::size_t> FileOfSize(const std::vector<FileTableEntry>& files, std::size_t last,
                                      std::size_t size_at_least)
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
}

} // namespace

std::vector<uint8_t> DecodeRare(std::span<const uint8_t> data)
{
    if (data.size() < kRareHeaderSize || data[0] != kRareMagic[0] || data[1] != kRareMagic[1])
    {
        throw xsflib::FormatError("not a block of Rare's");
    }

    std::vector<uint8_t> out;
    std::size_t used = 0;
    std::string error;
    if (!DecodeDeflate(data.subspan(kRareHeaderSize), out, used, error))
    {
        throw xsflib::FormatError("a block of Rare's: " + error);
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

    // Waves whose loops start at the same place keep the order of the bank on every platform.
    std::stable_sort(waves.begin(), waves.end(), earlier);

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
    const EarlyTables tables = FindEarlyTables(image);
    if (tables.sequences.size() != 1)
    {
        return std::nullopt;
    }

    EarlySoundData data;
    data.sequences = tables.sequences.front().first;
    if (!PairEarlyBanks(tables, data))
    {
        return std::nullopt;
    }

    const std::size_t sequence_count = tables.sequences.front().second;
    const std::size_t bank_count = xsflib::LoadBe16(image.data() + data.banks + 2);
    data.bank_sets = FindEarlyBankSets(image, sequence_count, bank_count);

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
    const std::vector<FileTableEntry> files = ReadFileTable(image, table);
    if (files.size() < 8)
    {
        return std::nullopt;
    }

    // The game's code is the large compressed file among the first ones that holds the tables.
    const std::size_t last = std::min(files.size(), kLastFileChecked);
    for (std::size_t index = 0; index < last; index++)
    {
        const FileTableEntry& file = files[index];
        const std::size_t size = file.virtual_end - file.virtual_start;
        if (file.image_end == 0 || size < kMinCodeSize || size > kMaxCodeSize || file.image_end > image.size() ||
            file.image_start >= file.image_end)
        {
            continue;
        }

        LaterSoundData data;
        try
        {
            data.code = ntdlib::DecodeYaz0(image.subspan(file.image_start, file.image_end - file.image_start));
        }
        catch (const xsflib::FormatError&)
        {
            continue;
        }

        if (!FindLaterTablesInCode(data))
        {
            continue;
        }

        // The files lie in the same order right after the first files of the table: the first plain file whose size
        // fits each table's sum.
        const auto banks = FileOfSize(files, last, LaterTableEnd(data.code, data.bank_table));
        const auto sequences = FileOfSize(files, last, LaterTableEnd(data.code, data.sequence_table));
        const auto samples = FileOfSize(files, last, LaterTableEnd(data.code, data.sample_table));
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
