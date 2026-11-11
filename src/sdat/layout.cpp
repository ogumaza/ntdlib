// SPDX-License-Identifier: MIT

#include "ntdlib/sdat/layout.h"

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

#include "ntdlib/rounding.h"
#include "ntdlib/sdat/formats.h"
#include "sdat/writer.h"

namespace ntdlib::sdat
{

namespace
{

constexpr std::size_t kKinds = 8;
constexpr std::size_t kHeaderSize = 0x40;
constexpr std::size_t kBlockHeaderSize = 0x40; // SYMB's and INFO's: the magic word, the size, 8 lists and 24 zeros
constexpr std::size_t kFileAlignment = 32;
constexpr uint16_t kByteOrderMark = 0xFEFF;

using detail::Writer;

// A list of the SYMB or INFO block: a count, then a reference to each item. Returns where the references start.
std::size_t List(Writer& w, std::size_t count, std::size_t reference_size)
{
    w.U32(static_cast<uint32_t>(count));
    const std::size_t references = w.Size();
    w.PadTo(references + count * reference_size);
    return references;
}

void WriteName(Writer& w, const std::string& name)
{
    w.Bytes({reinterpret_cast<const uint8_t*>(name.data()), name.size()});
    w.U8(0);
}

// The SYMB block. Its size counts the zeros up to the next multiple of 4 bytes. Returns its size without them. The
// archive's header gives that size.
std::size_t WriteSymbols(Writer& w, const Symbols& symbols)
{
    const std::size_t start = w.Size();
    w.Magic("SYMB");
    w.U32(0);
    w.PadTo(start + kBlockHeaderSize);

    // The lists, the sequence archives' lists of their sequences after the list of the sequence archives.
    std::array<std::size_t, kKinds> lists{};
    std::vector<std::optional<std::size_t>> sequence_lists(symbols.sequence_names.size());
    for (std::size_t kind = 0; kind < kKinds; kind++)
    {
        w.Put32(start + 8 + 4 * kind, w.Size() - start);
        lists[kind] = List(w, symbols.names[kind].size(), kind == 1 ? 8 : 4);
        if (kind != 1)
        {
            continue;
        }
        for (std::size_t i = 0; i < symbols.sequence_names.size() && i < symbols.names[1].size(); i++)
        {
            if (symbols.sequence_names[i])
            {
                w.Put32(lists[1] + 8 * i + 4, w.Size() - start);
                sequence_lists[i] = List(w, symbols.sequence_names[i]->size(), 4);
            }
        }
    }

    // The names, in the lists' order.
    const auto name = [&w, start](std::size_t reference, const std::optional<std::string>& text)
    {
        if (text)
        {
            w.Put32(reference, w.Size() - start);
            WriteName(w, *text);
        }
    };
    for (std::size_t kind = 0; kind < kKinds; kind++)
    {
        for (std::size_t i = 0; i < symbols.names[kind].size(); i++)
        {
            name(lists[kind] + (kind == 1 ? 8 : 4) * i, symbols.names[kind][i]);
            if (kind == 1 && i < sequence_lists.size() && sequence_lists[i])
            {
                for (std::size_t j = 0; j < symbols.sequence_names[i]->size(); j++)
                {
                    name(*sequence_lists[i] + 4 * j, (*symbols.sequence_names[i])[j]);
                }
            }
        }
    }

    const std::size_t size = w.Size() - start;
    w.PadTo(AlignUp(w.Size(), 4));
    w.Put32(start + 4, w.Size() - start);
    return size;
}

// A kind's list in the INFO block, then its records.
template <typename Info, typename Write>
void WriteRecords(Writer& w, std::size_t start, std::size_t kind, const std::vector<std::optional<Info>>& records,
                  Write write)
{
    w.Put32(start + 8 + 4 * kind, w.Size() - start);
    const std::size_t references = List(w, records.size(), 4);
    for (std::size_t i = 0; i < records.size(); i++)
    {
        if (records[i])
        {
            w.Put32(references + 4 * i, w.Size() - start);
            write(*records[i]);
        }
    }
}

void WriteInfo(Writer& w, const ArchiveModel& model)
{
    const std::size_t start = w.Size();
    w.Magic("INFO");
    w.U32(0);
    w.PadTo(start + kBlockHeaderSize);

    WriteRecords(w, start, 0, model.sequences,
                 [&w](const SequenceInfo& info)
                 {
                     w.U32(info.file_id);
                     w.U16(info.bank);
                     w.U8(info.volume);
                     w.U8(info.channel_priority);
                     w.U8(info.player_priority);
                     w.U8(info.player);
                     w.U16(info.reserved);
                 });
    WriteRecords(w, start, 1, model.sequence_archives, [&w](const SequenceArchiveInfo& info) { w.U32(info.file_id); });
    WriteRecords(w, start, 2, model.banks,
                 [&w](const BankInfo& info)
                 {
                     w.U32(info.file_id);
                     for (const uint16_t wave_archive : info.wave_archives)
                     {
                         w.U16(wave_archive);
                     }
                 });
    WriteRecords(w, start, 3, model.wave_archives, [&w](const WaveArchiveInfo& info) { w.U32(info.file_id); });
    WriteRecords(w, start, 4, model.players,
                 [&w](const PlayerInfo& info)
                 {
                     w.U8(info.max_sequences);
                     w.U8(info.padding);
                     w.U16(info.channels);
                     w.U32(info.heap_size);
                 });
    WriteRecords(w, start, 5, model.groups,
                 [&w](const GroupInfo& info)
                 {
                     w.U32(static_cast<uint32_t>(info.items.size()));
                     for (const GroupItem& item : info.items)
                     {
                         w.U8(item.type);
                         w.U8(item.load);
                         w.U16(item.padding);
                         w.U32(item.index);
                     }
                 });
    WriteRecords(w, start, 6, model.stream_players,
                 [&w](const StreamPlayerInfo& info)
                 {
                     w.U8(info.channel_count);
                     w.Bytes(info.channels);
                     w.Bytes(info.reserved);
                 });
    WriteRecords(w, start, 7, model.streams,
                 [&w](const StreamInfo& info)
                 {
                     w.U32(info.file_id);
                     w.U8(info.volume);
                     w.U8(info.player_priority);
                     w.U8(info.player);
                     w.U8(info.flags);
                     w.Bytes(info.reserved);
                 });

    w.Put32(start + 4, w.Size() - start);
}

} // namespace

ArchiveModel ReadArchiveModel(std::span<const uint8_t> sdat)
{
    const Archive archive = Archive::Parse(sdat);
    ArchiveModel model;
    model.version = archive.header.version;
    if (archive.has_symbols)
    {
        model.symbols = archive.symbols;
    }
    model.sequences = archive.sequences;
    model.sequence_archives = archive.sequence_archives;
    model.banks = archive.banks;
    model.wave_archives = archive.wave_archives;
    model.players = archive.players;
    model.groups = archive.groups;
    model.stream_players = archive.stream_players;
    model.streams = archive.streams;
    for (uint32_t id = 0; id < archive.files.size(); id++)
    {
        const std::span<const uint8_t> file = archive.File(id);
        model.files.emplace_back(file.begin(), file.end());
    }

    const std::vector<uint8_t> built = BuildArchive(model);
    if (!std::equal(built.begin(), built.end(), archive.bytes.begin(), archive.bytes.end()))
    {
        const auto differ = std::mismatch(built.begin(), built.end(), archive.bytes.begin(), archive.bytes.end());
        throw xsflib::FormatError("the SDAT file isn't laid out as the SDK's tools lay it out: it differs at offset " +
                                  std::to_string(differ.second - archive.bytes.begin()));
    }

    return model;
}

std::vector<uint8_t> BuildArchive(const ArchiveModel& model)
{
    Writer w;
    w.Magic("SDAT");
    w.U16(kByteOrderMark);
    w.U16(model.version);
    w.U32(0);
    w.U16(static_cast<uint16_t>(kHeaderSize));
    w.U16(model.symbols ? 4 : 3);
    w.PadTo(kHeaderSize);

    // The header's blocks: SYMB, INFO, FAT and FILE, each an offset and a size. An archive without names gives SYMB 0
    // for both. SYMB's size leaves out the zeros after it.
    const auto block = [&w](std::size_t index, std::size_t start, std::size_t size)
    {
        w.Put32(0x10 + 8 * index, start);
        w.Put32(0x14 + 8 * index, size);
    };
    if (model.symbols)
    {
        const std::size_t symb = w.Size();
        const std::size_t size = WriteSymbols(w, *model.symbols);
        block(0, symb, size);
    }

    const std::size_t info = w.Size();
    WriteInfo(w, model);
    block(1, info, w.Size() - info);

    // The FAT: each file's offset and size, and 8 bytes of zeros.
    const std::size_t fat = w.Size();
    w.Magic("FAT ");
    w.U32(static_cast<uint32_t>(12 + 16 * model.files.size()));
    const std::size_t entries = List(w, model.files.size(), 16);
    block(2, fat, w.Size() - fat);

    // FILE: the file count, then the files.
    const std::size_t files = w.Size();
    w.Magic("FILE");
    w.U32(0);
    w.U32(static_cast<uint32_t>(model.files.size()));
    for (std::size_t id = 0; id < model.files.size(); id++)
    {
        w.PadTo(AlignUp(w.Size(), kFileAlignment));
        w.Put32(entries + 16 * id, w.Size());
        w.Put32(entries + 16 * id + 4, model.files[id].size());
        w.Bytes(model.files[id]);
    }
    w.PadTo(AlignUp(w.Size(), kFileAlignment));
    w.Put32(files + 4, w.Size() - files);
    block(3, files, w.Size() - files);

    w.Put32(8, w.Size());
    return w.Take();
}

} // namespace ntdlib::sdat
