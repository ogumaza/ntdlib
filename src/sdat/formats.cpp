// SPDX-License-Identifier: MIT

#include "ntdlib/sdat/formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::sdat
{

namespace
{

constexpr uint16_t kByteOrderMark = 0xFEFF;
constexpr std::size_t kFileHeaderBytes = 0x10;
constexpr std::size_t kBlockHeaderBytes = 8;
constexpr std::size_t kWaveParamBytes = 12;
constexpr std::size_t kRegionBytes = 12;
constexpr std::size_t kKinds = 8;
constexpr std::size_t kMaxNameBytes = 256;

// The block that follows a file's header, with its magic word. Returns the block's offset.
std::size_t FirstBlock(const xsflib::Reader& r, const char* magic)
{
    if (!r.Magic(kFileHeaderBytes, magic))
    {
        throw xsflib::FormatError(std::string("the file has no ") + magic + " block");
    }

    return kFileHeaderBytes;
}

void CheckRange(const xsflib::Reader& r, std::size_t offset, std::size_t size, const char* what)
{
    if (offset > r.Size() || size > r.Size() - offset)
    {
        throw xsflib::FormatError(std::string(what) + " lies past the end of the file");
    }
}

Region ReadRegion(const xsflib::Reader& r, std::size_t at, InstrumentType type)
{
    Region region;
    region.type = type;
    region.wave = r.U16(at);
    region.wave_archive = r.U16(at + 2);
    region.root_key = r.U8(at + 4);
    region.attack = r.U8(at + 5);
    region.decay = r.U8(at + 6);
    region.sustain = r.U8(at + 7);
    region.release = r.U8(at + 8);
    region.pan = r.U8(at + 9);

    return region;
}

// A region of a drum set or a key split: its type, a byte of padding and its parameters.
Region ReadTypedRegion(const xsflib::Reader& r, std::size_t at)
{
    Region region = ReadRegion(r, at + 2, static_cast<InstrumentType>(r.U8(at)));
    region.padding = r.U8(at + 1);

    return region;
}

// A name, or none for an offset of 0.
std::optional<std::string> OptionalName(const xsflib::Reader& symb, uint32_t offset)
{
    return offset == 0 ? std::nullopt : std::optional<std::string>(symb.CString(offset, kMaxNameBytes));
}

// The names of one kind of item, each or none.
std::vector<std::optional<std::string>> OptionalNames(const xsflib::Reader& symb, uint32_t list)
{
    std::vector<std::optional<std::string>> names;
    const uint32_t count = symb.U32(list);
    CheckRange(symb, list + 4, std::size_t{count} * 4, "a name list");
    for (uint32_t i = 0; i < count; i++)
    {
        names.push_back(OptionalName(symb, symb.U32(list + 4 + std::size_t{i} * 4)));
    }

    return names;
}

// The name at an index, or an empty name.
std::string NameAt(const std::vector<std::optional<std::string>>& names, std::size_t index)
{
    return index < names.size() ? names[index].value_or(std::string()) : std::string();
}

// The names, with an empty name for each that has none.
std::vector<std::string> NamesOrEmpty(const std::vector<std::optional<std::string>>& names)
{
    std::vector<std::string> plain;
    for (const std::optional<std::string>& name : names)
    {
        plain.push_back(name.value_or(std::string()));
    }
    return plain;
}

// The records of one kind of item: a count, then an offset from the INFO block's start for each record. Calls `read`
// with each record's offset in the archive.
template <typename Info, typename Read>
std::vector<std::optional<Info>> Records(const xsflib::Reader& r, std::size_t info, uint32_t list, Read read)
{
    std::vector<std::optional<Info>> records;
    const uint32_t count = r.U32(info + list);
    CheckRange(r, info + list + 4, std::size_t{count} * 4, "an INFO record list");
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t offset = r.U32(info + list + 4 + std::size_t{i} * 4);
        if (offset == 0)
        {
            records.emplace_back();
            continue;
        }
        records.push_back(read(r, info + offset));
    }

    return records;
}

SequenceInfo ReadSequenceInfo(const xsflib::Reader& r, std::size_t at)
{
    return {r.U32(at), r.U16(at + 4), r.U8(at + 6), r.U8(at + 7), r.U8(at + 8), r.U8(at + 9), r.U16(at + 10), ""};
}

SequenceArchiveInfo ReadSequenceArchiveInfo(const xsflib::Reader& r, std::size_t at)
{
    return {r.U32(at), "", {}};
}

BankInfo ReadBankInfo(const xsflib::Reader& r, std::size_t at)
{
    return {r.U32(at), {r.U16(at + 4), r.U16(at + 6), r.U16(at + 8), r.U16(at + 10)}, ""};
}

WaveArchiveInfo ReadWaveArchiveInfo(const xsflib::Reader& r, std::size_t at)
{
    return {r.U32(at), ""};
}

PlayerInfo ReadPlayerInfo(const xsflib::Reader& r, std::size_t at)
{
    return {r.U8(at), r.U8(at + 1), r.U16(at + 2), r.U32(at + 4), ""};
}

// A group: a count, then 8 bytes for each item.
GroupInfo ReadGroupInfo(const xsflib::Reader& r, std::size_t at)
{
    GroupInfo group;
    const uint32_t count = r.U32(at);
    CheckRange(r, at + 4, std::size_t{count} * 8, "a group");
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t item = at + 4 + std::size_t{i} * 8;
        group.items.push_back({r.U8(item), r.U8(item + 1), r.U16(item + 2), r.U32(item + 4)});
    }

    return group;
}

StreamPlayerInfo ReadStreamPlayerInfo(const xsflib::Reader& r, std::size_t at)
{
    StreamPlayerInfo player;
    player.channel_count = r.U8(at);
    for (std::size_t i = 0; i < player.channels.size(); i++)
    {
        player.channels[i] = r.U8(at + 1 + i);
    }
    for (std::size_t i = 0; i < player.reserved.size(); i++)
    {
        player.reserved[i] = r.U8(at + 17 + i);
    }

    return player;
}

StreamInfo ReadStreamInfo(const xsflib::Reader& r, std::size_t at)
{
    StreamInfo stream{r.U32(at), r.U8(at + 4), r.U8(at + 5), r.U8(at + 6), r.U8(at + 7), {}, ""};
    for (std::size_t i = 0; i < stream.reserved.size(); i++)
    {
        stream.reserved[i] = r.U8(at + 8 + i);
    }

    return stream;
}

// Gives each record the name at its index.
template <typename Info>
void NameRecords(std::vector<std::optional<Info>>& records, const std::vector<std::optional<std::string>>& names)
{
    for (std::size_t i = 0; i < records.size(); i++)
    {
        if (records[i])
        {
            records[i]->name = NameAt(names, i);
        }
    }
}

} // namespace

FileHeader FileHeader::Parse(std::span<const uint8_t> file, const char* magic)
{
    const xsflib::Reader r(file);
    if (!r.Magic(0, magic))
    {
        throw xsflib::FormatError(std::string("not an ") + magic + " file");
    }
    if (r.U16(4) != kByteOrderMark)
    {
        throw xsflib::FormatError(std::string("the ") + magic + " file's byte order mark isn't 0xFEFF");
    }

    FileHeader header{r.U16(6), r.U32(8), r.U16(12), r.U16(14)};
    if (header.size > file.size())
    {
        throw xsflib::FormatError(std::string("the ") + magic + " file is cut short");
    }

    return header;
}

Wave Wave::Parse(const xsflib::Reader& r, std::size_t at, std::size_t end)
{
    CheckRange(r, at, kWaveParamBytes, "a wave");
    if (end < at + kWaveParamBytes || end > r.Size())
    {
        throw xsflib::FormatError("a wave's samples lie past the end of the file");
    }

    Wave wave;
    wave.encoding = static_cast<WaveEncoding>(r.U8(at));
    wave.loop = r.U8(at + 1);
    wave.sample_rate = r.U16(at + 2);
    wave.timer = r.U16(at + 4);
    wave.loop_start = r.U16(at + 6);
    wave.loop_length = r.U32(at + 8);
    wave.data = r.Sub(at + kWaveParamBytes, end - at - kWaveParamBytes);

    return wave;
}

Wave Wave::ParseFile(std::span<const uint8_t> swav)
{
    const FileHeader header = FileHeader::Parse(swav, "SWAV");
    const xsflib::Reader r(swav.first(header.size));
    const std::size_t block = FirstBlock(r, "DATA");
    const uint32_t block_size = r.U32(block + 4);
    CheckRange(r, block, block_size, "the DATA block");

    return Parse(r, block + kBlockHeaderBytes, block + block_size);
}

uint32_t Wave::Samples() const
{
    return SamplesInWords(encoding, loop_start + loop_length);
}

uint32_t Wave::LoopStartSample() const
{
    return SamplesInWords(encoding, loop_start);
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> swar)
{
    WaveArchive archive;
    archive.header = FileHeader::Parse(swar, "SWAR");
    const xsflib::Reader r(swar.first(archive.header.size));
    const std::size_t block = FirstBlock(r, "DATA");

    // The block's header, 32 bytes that the game fills at run time, the count, and each wave's offset.
    const uint32_t count = r.U32(block + 0x28);
    CheckRange(r, block + 0x2C, std::size_t{count} * 4, "the wave table");
    for (uint32_t i = 0; i < count; i++)
    {
        archive.offsets.push_back(r.U32(block + 0x2C + std::size_t{i} * 4));
    }

    // A wave's samples run to the next wave's offset, or to the end of the file.
    for (uint32_t i = 0; i < count; i++)
    {
        std::size_t end = r.Size();
        for (const uint32_t offset : archive.offsets)
        {
            if (offset > archive.offsets[i] && offset < end)
            {
                end = offset;
            }
        }
        archive.waves.push_back(Wave::Parse(r, archive.offsets[i], end));
    }

    return archive;
}

Bank Bank::Parse(std::span<const uint8_t> sbnk)
{
    Bank bank;
    bank.header = FileHeader::Parse(sbnk, "SBNK");
    const xsflib::Reader r(sbnk.first(bank.header.size));
    const std::size_t block = FirstBlock(r, "DATA");

    const uint32_t count = r.U32(block + 0x28);
    CheckRange(r, block + 0x2C, std::size_t{count} * 4, "the instrument table");
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t record = r.U32(block + 0x2C + std::size_t{i} * 4);
        Instrument instrument;
        instrument.type = static_cast<InstrumentType>(record & 0xFF);
        instrument.offset = record >> 8;
        const std::size_t at = instrument.offset;
        switch (instrument.type)
        {
        case InstrumentType::kNone:
            break;
        case InstrumentType::kDrumSet:
            {
                instrument.min_key = r.U8(at);
                instrument.max_key = r.U8(at + 1);
                if (instrument.max_key < instrument.min_key)
                {
                    throw xsflib::FormatError("a drum set's last key lies below its first");
                }
                for (int key = instrument.min_key; key <= instrument.max_key; key++)
                {
                    instrument.regions.push_back(
                        ReadTypedRegion(r, at + 2 + std::size_t(key - instrument.min_key) * kRegionBytes));
                }
                break;
            }

        case InstrumentType::kKeySplit:
            {
                for (std::size_t k = 0; k < instrument.split_keys.size(); k++)
                {
                    instrument.split_keys[k] = r.U8(at + k);
                }
                for (std::size_t k = 0; k < instrument.split_keys.size() && instrument.split_keys[k] != 0; k++)
                {
                    instrument.regions.push_back(ReadTypedRegion(r, at + 8 + k * kRegionBytes));
                }
                break;
            }

        default:
            instrument.region = ReadRegion(r, at, instrument.type);
            break;
        }
        bank.instruments.push_back(std::move(instrument));
    }

    return bank;
}

Sequence Sequence::Parse(std::span<const uint8_t> sseq)
{
    Sequence sequence;
    sequence.header = FileHeader::Parse(sseq, "SSEQ");
    const xsflib::Reader r(sseq.first(sequence.header.size));
    const std::size_t block = FirstBlock(r, "DATA");
    sequence.data_offset = r.U32(block + 8);
    CheckRange(r, sequence.data_offset, 0, "the sequence's bytecode");
    sequence.bytecode = r.Sub(sequence.data_offset, r.Size() - sequence.data_offset);

    return sequence;
}

SequenceArchive SequenceArchive::Parse(std::span<const uint8_t> ssar)
{
    SequenceArchive archive;
    archive.header = FileHeader::Parse(ssar, "SSAR");
    const xsflib::Reader r(ssar.first(archive.header.size));
    const std::size_t block = FirstBlock(r, "DATA");
    archive.data_offset = r.U32(block + 8);
    const uint32_t count = r.U32(block + 12);
    CheckRange(r, block + 16, std::size_t{count} * 12, "the sequence table");
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t at = block + 16 + std::size_t{i} * 12;
        archive.sequences.push_back(
            {r.U32(at), r.U16(at + 4), r.U8(at + 6), r.U8(at + 7), r.U8(at + 8), r.U8(at + 9), r.U16(at + 10)});
    }
    CheckRange(r, archive.data_offset, 0, "the sequences' bytecode");
    archive.bytecode = r.Sub(archive.data_offset, r.Size() - archive.data_offset);

    return archive;
}

Stream Stream::Parse(std::span<const uint8_t> strm)
{
    Stream stream;
    stream.header = FileHeader::Parse(strm, "STRM");
    const xsflib::Reader r(strm.first(stream.header.size));
    const std::size_t head = FirstBlock(r, "HEAD");
    stream.encoding = static_cast<WaveEncoding>(r.U8(head + 8));
    stream.loop = r.U8(head + 9);
    stream.channels = r.U8(head + 10);
    stream.head_padding = r.U8(head + 11);
    stream.sample_rate = r.U16(head + 12);
    stream.timer = r.U16(head + 14);
    stream.loop_start = r.U32(head + 16);
    stream.samples = r.U32(head + 20);
    stream.data_offset = r.U32(head + 24);
    stream.blocks = r.U32(head + 28);
    stream.block_bytes = r.U32(head + 32);
    stream.block_samples = r.U32(head + 36);
    stream.last_block_bytes = r.U32(head + 40);
    stream.last_block_samples = r.U32(head + 44);
    for (std::size_t i = 0; i < stream.reserved.size(); i++)
    {
        stream.reserved[i] = r.U8(head + 48 + i);
    }

    // The samples run from the data offset to the end of the DATA block. The DATA block follows the HEAD block.
    const std::size_t data = head + r.U32(head + 4);
    if (!r.Magic(data, "DATA"))
    {
        throw xsflib::FormatError("the STRM file has no DATA block");
    }
    const std::size_t data_end = data + r.U32(data + 4);
    if (stream.data_offset < data + kBlockHeaderBytes || stream.data_offset > data_end || data_end > r.Size())
    {
        throw xsflib::FormatError("the stream's samples lie outside its DATA block");
    }
    stream.data = r.Sub(stream.data_offset, data_end - stream.data_offset);
    if (stream.blocks > 0)
    {
        const uint64_t bytes = (uint64_t{stream.blocks} - 1) * stream.block_bytes * stream.channels +
                               uint64_t{stream.last_block_bytes} * stream.channels;
        if (bytes > stream.data.size())
        {
            throw xsflib::FormatError("the stream's blocks run past its DATA block");
        }
    }

    return stream;
}

std::span<const uint8_t> Stream::Block(uint32_t channel, uint32_t block) const
{
    // Each block holds each channel's part in turn. In the last block, each part takes last_block_bytes.
    const uint32_t bytes = block + 1 == blocks ? last_block_bytes : block_bytes;
    const std::size_t start = std::size_t{block} * channels * block_bytes + std::size_t{channel} * bytes;

    return data.subspan(start, bytes);
}

Archive Archive::Parse(std::span<const uint8_t> sdat)
{
    Archive archive;
    archive.header = FileHeader::Parse(sdat, "SDAT");
    archive.bytes = sdat.first(archive.header.size);
    const xsflib::Reader r(archive.bytes);

    // The header gives each block's offset and size: SYMB, INFO, FAT and FILE. An archive without names has an offset
    // of 0 for SYMB.
    const uint32_t symb_offset = r.U32(0x10);
    const uint32_t symb_size = r.U32(0x14);
    const uint32_t info = r.U32(0x18);
    const uint32_t fat = r.U32(0x20);
    CheckRange(r, info, r.U32(0x1C), "the INFO block");
    CheckRange(r, fat, r.U32(0x24), "the FAT block");
    if (!r.Magic(info, "INFO") || !r.Magic(fat, "FAT "))
    {
        throw xsflib::FormatError("the SDAT file has no INFO or FAT block");
    }

    // The names, when the archive has a SYMB block. The sequence archives' list gives each archive's name and a list
    // of its sequences' names.
    Symbols& symbols = archive.symbols;
    archive.has_symbols = symb_offset != 0 && symb_size != 0;
    if (archive.has_symbols)
    {
        CheckRange(r, symb_offset, symb_size, "the SYMB block");
        const xsflib::Reader symb(r.Sub(symb_offset, symb_size));
        if (!symb.Magic(0, "SYMB"))
        {
            throw xsflib::FormatError("the SDAT file's SYMB block has another magic word");
        }
        for (std::size_t kind = 0; kind < kKinds; kind++)
        {
            const uint32_t list = symb.U32(8 + kind * 4);
            if (kind == 1)
            {
                const uint32_t count = symb.U32(list);
                CheckRange(symb, list + 4, std::size_t{count} * 8, "a name list");
                for (uint32_t i = 0; i < count; i++)
                {
                    const std::size_t at = list + 4 + std::size_t{i} * 8;
                    symbols.names[kind].push_back(OptionalName(symb, symb.U32(at)));
                    const uint32_t sub = symb.U32(at + 4);
                    symbols.sequence_names.push_back(
                        sub == 0 ? std::nullopt
                                 : std::optional<std::vector<std::optional<std::string>>>(OptionalNames(symb, sub)));
                }
                continue;
            }
            symbols.names[kind] = OptionalNames(symb, list);
        }
    }

    // The INFO block gives each kind's list of records.
    const auto list = [&](std::size_t kind)
    {
        return r.U32(info + 8 + kind * 4);
    };
    archive.sequences = Records<SequenceInfo>(r, info, list(0), ReadSequenceInfo);
    archive.sequence_archives = Records<SequenceArchiveInfo>(r, info, list(1), ReadSequenceArchiveInfo);
    archive.banks = Records<BankInfo>(r, info, list(2), ReadBankInfo);
    archive.wave_archives = Records<WaveArchiveInfo>(r, info, list(3), ReadWaveArchiveInfo);
    archive.players = Records<PlayerInfo>(r, info, list(4), ReadPlayerInfo);
    archive.groups = Records<GroupInfo>(r, info, list(5), ReadGroupInfo);
    archive.stream_players = Records<StreamPlayerInfo>(r, info, list(6), ReadStreamPlayerInfo);
    archive.streams = Records<StreamInfo>(r, info, list(7), ReadStreamInfo);

    // The names. A sequence archive's sequences take theirs from its list.
    for (std::size_t i = 0; i < archive.sequence_archives.size(); i++)
    {
        if (archive.sequence_archives[i] && i < symbols.sequence_names.size() && symbols.sequence_names[i])
        {
            archive.sequence_archives[i]->sequence_names = NamesOrEmpty(*symbols.sequence_names[i]);
        }
    }
    NameRecords(archive.sequences, symbols.names[0]);
    NameRecords(archive.sequence_archives, symbols.names[1]);
    NameRecords(archive.banks, symbols.names[2]);
    NameRecords(archive.wave_archives, symbols.names[3]);
    NameRecords(archive.players, symbols.names[4]);
    NameRecords(archive.groups, symbols.names[5]);
    NameRecords(archive.stream_players, symbols.names[6]);
    NameRecords(archive.streams, symbols.names[7]);

    // The FAT: a count, then 16 bytes for each file.
    const uint32_t count = r.U32(fat + 8);
    CheckRange(r, fat + 12, std::size_t{count} * 16, "the FAT");
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t at = fat + 12 + std::size_t{i} * 16;
        FileEntry entry{r.U32(at), r.U32(at + 4), r.U32(at + 8), r.U32(at + 12)};
        CheckRange(r, entry.offset, entry.size, "a file");
        archive.files.push_back(entry);
    }

    return archive;
}

std::span<const uint8_t> Archive::File(uint32_t id) const
{
    if (id >= files.size())
    {
        throw xsflib::FormatError("file " + std::to_string(id) + " lies past the FAT");
    }

    return bytes.subspan(files[id].offset, files[id].size);
}

std::vector<Archive> FindArchives(std::span<const uint8_t> file)
{
    constexpr std::array<uint8_t, 4> kMagic = {'S', 'D', 'A', 'T'};
    std::vector<Archive> found;
    for (std::size_t at = 0; at + kMagic.size() <= file.size(); at += 4)
    {
        if (!std::equal(kMagic.begin(), kMagic.end(), file.begin() + static_cast<std::ptrdiff_t>(at)))
        {
            continue;
        }

        try
        {
            found.push_back(Archive::Parse(file.subspan(at)));
        }
        catch (const xsflib::FormatError&)
        {
        }
    }

    return found;
}

} // namespace ntdlib::sdat
