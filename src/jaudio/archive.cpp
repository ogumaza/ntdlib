// SPDX-License-Identifier: MIT

// JAudio2's start-up archive (see archive.h). Four Swords Adventures' main.dol reads it with an interpreter of its
// commands (0x8003a5e4).

#include "ntdlib/jaudio/archive.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "jaudio/reader.h"
#include "xsflib/binary.h"

namespace ntdlib::jaudio
{
namespace
{

using detail::kReaderOptions;

// The reader settings for `options`: the module's settings, with the options' message for a read past the end.
xsflib::ReaderOptions ReaderOptionsOf(const BaaOptions& options)
{
    xsflib::ReaderOptions reader = kReaderOptions;
    if (!options.past_end.empty())
    {
        reader.bounds_message = xsflib::BoundsMessage::kFixed;
        reader.fixed_message = options.past_end;
    }

    return reader;
}

// The number of argument bytes after a command's tag, or nothing for a command that isn't known.
std::optional<std::size_t> ArgumentBytes(const std::string& tag)
{
    if (tag == "ws  " || tag == "bms ")
    {
        return 12;
    }
    if (tag == "bnk " || tag == "bst " || tag == "bstn" || tag == "bsc " || tag == "bl_<" || tag == "vbnk")
    {
        return 8;
    }
    if (tag == "bmsa" || tag == "dsqb" || tag == "bsft" || tag == "sect" || tag == "bfca")
    {
        return 4;
    }
    if (tag == ">_bl")
    {
        return 0;
    }

    return std::nullopt;
}

// Walks the command list from "AA_<" to ">_AA" (JAUAudioArcInterpreter). For each command, calls visit(tag, at) with
// `at` the offset of the command's first argument, then moves past it. Throws FormatError for data that isn't a
// start-up archive and for a command that isn't known.
template <typename Visit>
void WalkCommands(const xsflib::Reader& r, const BaaOptions& options, const Visit& visit)
{
    if (r.Size() < options.min_size || !r.Magic(0, "AA_<"))
    {
        throw xsflib::FormatError(options.not_an_archive);
    }

    for (std::size_t at = 4;;)
    {
        const std::string tag(reinterpret_cast<const char*>(r.Sub(at, 4).data()), 4);
        at += 4;
        if (tag == ">_AA")
        {
            return;
        }

        const std::optional<std::size_t> arguments = ArgumentBytes(tag);
        if (!arguments)
        {
            throw xsflib::FormatError(options.unknown_command);
        }

        visit(tag, at);
        at += *arguments;
    }
}

// The block that a command's start and end arguments at `at` give. Throws FormatError for one that ends before it
// starts or lies past the end of the archive.
std::span<const uint8_t> Block(const xsflib::Reader& r, std::size_t at)
{
    const uint32_t start = r.U32(at), end = r.U32(at + 4);
    if (end < start)
    {
        throw xsflib::FormatError("archive has a chunk that ends before it starts");
    }

    return r.Sub(start, end - start);
}

// The archive from `start` to its end. A start past the end gives no data, and the first read of it fails.
std::span<const uint8_t> ToEnd(std::span<const uint8_t> baa, uint32_t start)
{
    return start <= baa.size() ? baa.subspan(start) : std::span<const uint8_t>();
}

// The sounds of the sound table that `table` starts with, and their names from the name table that `names` starts with.
// The table starts at `table_offset` in the archive. The table lists its sections, each section its groups, and each
// group its sounds. A sound's word holds its type in the top byte and the offset of its entry below. The name table has
// the same shape, with a name before each section's and group's list.
std::vector<SoundEntry> ReadSoundTable(const xsflib::Reader& table, uint32_t table_offset,
                                       const std::optional<xsflib::Reader>& names, const BaaOptions& options)
{
    const auto name = [&](uint32_t offset)
    {
        if (options.names_inside)
        {
            names->U8(offset);
        }

        return names->CString(offset, 0x100);
    };

    if (options.check_magic && !table.Magic(0, "BST "))
    {
        throw xsflib::FormatError(options.table_damaged);
    }

    const uint32_t root = table.U32(12);
    std::optional<uint32_t> names_root;
    if (names)
    {
        if (options.check_magic && !names->Magic(0, "BSTN"))
        {
            throw xsflib::FormatError(options.table_damaged);
        }

        names_root = names->U32(12);
    }

    std::vector<SoundEntry> sounds;
    const uint32_t sections = table.U32(root);
    for (uint32_t s = 0; s < sections && !(options.cap_counts && s >= 0x100); s++)
    {
        const uint32_t section = table.U32(root + 4 + 4 * s);
        if (options.skip_empty && section == 0)
        {
            continue;
        }

        std::optional<uint32_t> names_section;
        if (names && (!options.check_name_counts || s < names->U32(*names_root)))
        {
            names_section = names->U32(*names_root + 4 + 4 * s);
        }

        const std::string section_name =
            options.section_names && names_section ? name(names->U32(*names_section + 4)) : "";
        const uint32_t groups = table.U32(section);
        for (uint32_t g = 0; g < groups && !(options.cap_counts && g >= 0x100); g++)
        {
            const uint32_t group = table.U32(section + 4 + 4 * g);
            if (options.skip_empty && group == 0)
            {
                continue;
            }

            std::optional<uint32_t> names_group;
            if (names_section && (!options.check_name_counts || g < names->U32(*names_section)))
            {
                names_group = names->U32(*names_section + 8 + 4 * g);
            }

            const std::string group_name =
                options.section_names && names_group ? name(names->U32(*names_group + 4)) : "";
            const uint32_t items = table.U32(group);
            for (uint32_t i = 0; i < items && !(options.cap_counts && i >= 0x10000); i++)
            {
                const uint32_t word = table.U32(group + 8 + 4 * i);
                const uint32_t entry = word & 0xffffff;
                if (options.skip_empty && entry == 0)
                {
                    continue;
                }

                SoundEntry sound;
                sound.offset = table_offset + entry;
                sound.type = static_cast<uint8_t>(word >> 24);
                sound.section = static_cast<uint8_t>(s);
                sound.group = static_cast<uint8_t>(g);
                sound.index = static_cast<uint16_t>(i);
                // The volume is in 127ths.
                const ItemLayout layout = LayoutOf(sound.type);
                sound.volume = table.U8(entry + layout.volume);
                if (names_group && (!options.check_name_counts || i < names->U32(*names_group)))
                {
                    sound.name = name(names->U32(*names_group + 8 + 4 * i));
                }

                sound.section_name = section_name;
                sound.group_name = group_name;

                // A sound of a type that plays a file of the sequence archive names the file in the low half of the
                // word that ends with the file's number. The others play the collection's sequence at their place in
                // their group. The group is their category in the collection.
                if (options.sources && layout.file)
                {
                    sound.source = SoundEntry::Source::kArchive;
                    sound.resource = table.U32(entry + *layout.file - 2) & 0xffff;
                }
                else if (options.sources)
                {
                    sound.source = SoundEntry::Source::kCollection;
                    sound.resource = i;
                }

                sounds.push_back(std::move(sound));
            }
        }
    }

    return sounds;
}

} // namespace

std::vector<SoundEntry> ReadSounds(std::span<const uint8_t> baa, const BaaOptions& options)
{
    const xsflib::ReaderOptions reader_options = ReaderOptionsOf(options);
    const xsflib::Reader r(baa, true, reader_options);

    // The walk records each table's start, and without tables_reach_end its block. The tables are read once the walk is
    // over.
    std::optional<uint32_t> table_start, names_start;
    std::span<const uint8_t> table_block, names_block;
    const auto find_tables = [&](const std::string& tag, std::size_t at)
    {
        if (tag != "bst " && tag != "bstn")
        {
            return;
        }

        (tag == "bst " ? table_start : names_start) = r.U32(at);
        if (!options.tables_reach_end)
        {
            (tag == "bst " ? table_block : names_block) = Block(r, at);
        }
    };
    WalkCommands(r, options, find_tables);

    std::optional<xsflib::Reader> table, names;
    if (options.tables_reach_end)
    {
        if (table_start && *table_start)
        {
            table.emplace(ToEnd(baa, *table_start), true, reader_options);
        }
        if (names_start && *names_start)
        {
            names.emplace(ToEnd(baa, *names_start), true, reader_options);
        }
    }
    else
    {
        if (!table_block.empty())
        {
            table.emplace(table_block, true, reader_options);
        }
        if (!names_block.empty())
        {
            names.emplace(names_block, true, reader_options);
        }
    }

    if (!table)
    {
        if (!options.no_table.empty())
        {
            throw xsflib::FormatError(options.no_table);
        }

        return {};
    }

    std::vector<SoundEntry> sounds = ReadSoundTable(*table, *table_start, names, options);
    if (sounds.empty() && !options.no_sounds.empty())
    {
        throw xsflib::FormatError(options.no_sounds);
    }

    return sounds;
}

std::vector<std::string> WaveArchiveNames(std::span<const uint8_t> baa, const BaaOptions& options)
{
    const xsflib::ReaderOptions reader_options = ReaderOptionsOf(options);
    const xsflib::Reader r(baa, true, reader_options);
    std::vector<uint32_t> systems;
    const auto find_systems = [&](const std::string& tag, std::size_t at)
    {
        if (tag == "ws  ")
        {
            systems.push_back(r.U32(at + 4));
        }
    };
    WalkCommands(r, options, find_systems);

    // A wave system gives its WINF block at +0x10. WINF counts the wave groups and gives each group's offset. Both
    // offsets count from the wave system's start. A group starts with its wave archive's file name, in 0x70 bytes.
    std::vector<std::string> names;
    for (const uint32_t offset : systems)
    {
        const std::span<const uint8_t> data =
            options.tables_reach_end ? ToEnd(baa, offset)
                                     : r.Sub(offset, xsflib::Reader(r.Sub(offset, 8), true, reader_options).U32(4));
        const xsflib::Reader ws(data, true, reader_options);
        if (!ws.Magic(0, "WSYS"))
        {
            throw xsflib::FormatError(options.wave_system_damaged);
        }

        const uint32_t info = ws.U32(0x10);
        if (!ws.Magic(info, "WINF"))
        {
            throw xsflib::FormatError(options.wave_system_damaged);
        }

        const uint32_t groups = ws.U32(info + 4);
        for (uint32_t g = 0; g < groups && !(options.cap_counts && g >= 0x100); g++)
        {
            const uint32_t at = ws.U32(info + 8 + 4 * g);
            if (options.names_inside)
            {
                ws.U8(at);
            }

            const std::string name = ws.CString(at, 0x70);
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(name);
            }
        }
    }

    return names;
}

std::vector<WaveSystem> ReadWaveSystems(std::span<const uint8_t> baa, const BaaOptions& options)
{
    const xsflib::ReaderOptions reader_options = ReaderOptionsOf(options);
    const xsflib::Reader r(baa, true, reader_options);
    std::vector<std::pair<uint32_t, uint32_t>> commands; // each wave system's number and offset
    const auto find_systems = [&](const std::string& tag, std::size_t at)
    {
        if (tag == "ws  ")
        {
            commands.emplace_back(r.U32(at), r.U32(at + 4));
        }
    };
    WalkCommands(r, options, find_systems);

    std::vector<WaveSystem> systems;
    for (const auto& [number, offset] : commands)
    {
        const std::span<const uint8_t> data =
            options.tables_reach_end ? ToEnd(baa, offset)
                                     : r.Sub(offset, xsflib::Reader(r.Sub(offset, 8), true, reader_options).U32(4));
        systems.push_back(ReadWaveSystem(data, number));
    }

    return systems;
}

ItemLayout LayoutOf(uint8_t type)
{
    if (type & 0x40)
    {
        return {1, (type & 0xf0) == 0x60 ? std::optional<uint32_t>(2) : std::nullopt, (type & 0xf0) == 0x70};
    }

    return {2, type == 0x11 ? std::optional<uint32_t>(10) : std::nullopt};
}

SoundArchive SoundArchive::Parse(std::vector<uint8_t> baa)
{
    SoundArchive archive;
    archive.data_ = std::move(baa);
    const BaaOptions options;
    const xsflib::Reader r(archive.data_, true, kReaderOptions);

    // Each command's arguments are offsets into the archive. A wave system is read as the walk reaches it.
    std::span<const uint8_t> bst, bstn;
    uint32_t bst_offset = 0;
    std::vector<std::pair<uint32_t, uint32_t>> banks; // wave system number, offset
    const auto read_command = [&](const std::string& tag, std::size_t at)
    {
        if (tag == "ws  ")
        {
            const uint32_t number = r.U32(at), offset = r.U32(at + 4);
            const xsflib::Reader ws(r.Sub(offset, 8), true, kReaderOptions);
            archive.wave_systems_.push_back(ReadWaveSystem(r.Sub(offset, ws.U32(4)), number));
        }
        else if (tag == "bnk ")
        {
            banks.emplace_back(r.U32(at), r.U32(at + 4));
        }
        else if (tag == "bst " || tag == "bstn" || tag == "bsc ")
        {
            const std::span<const uint8_t> chunk = Block(r, at);
            if (tag == "bst ")
            {
                bst = chunk;
                bst_offset = r.U32(at);
            }
            else if (tag == "bstn")
            {
                bstn = chunk;
            }
            else
            {
                archive.collection_offset_ = r.U32(at);
                archive.collection_size_ = chunk.size();
            }
        }
    };
    WalkCommands(r, options, read_command);

    // Banks: each plays the wave system that its chunk names. A bank's chunks can end past its size (see Bank::Parse).
    // Each bank therefore gets the rest of the archive.
    for (const auto& [wave_system, offset] : banks)
    {
        try
        {
            const std::size_t rest = r.Size() - std::min<std::size_t>(offset, r.Size());
            archive.banks_.push_back(Bank::Parse(r.Sub(offset, rest), wave_system));
        }
        catch (const xsflib::FormatError& e)
        {
            archive.warnings_.push_back("bank at offset " + std::to_string(offset) + ": " + e.what());
        }
    }

    // The sequence collection: "SC", a category count, its size, then each category's table of sequence offsets.
    if (archive.collection_size_ > 0)
    {
        const xsflib::Reader sc(archive.Collection(), true, kReaderOptions);
        if (sc.U8(0) != 'S' || sc.U8(1) != 'C')
        {
            throw xsflib::FormatError("archive's sequence collection has no SC header");
        }

        const uint16_t categories = sc.U16(2);
        for (uint16_t c = 0; c < categories; c++)
        {
            const uint32_t table = sc.U32(8 + 4 * c);
            std::vector<uint32_t> offsets;
            const uint32_t count = sc.U32(table);
            for (uint32_t i = 0; i < count; i++)
            {
                offsets.push_back(sc.U32(table + 4 + 4 * i));
            }
            archive.collection_.push_back(std::move(offsets));
        }
    }

    if (!bst.empty())
    {
        const std::optional<xsflib::Reader> names =
            bstn.empty() ? std::nullopt : std::optional<xsflib::Reader>(xsflib::Reader(bstn, true, kReaderOptions));
        archive.sounds_ = ReadSoundTable(xsflib::Reader(bst, true, kReaderOptions), bst_offset, names, options);
    }

    return archive;
}

const Bank* SoundArchive::FindBank(uint32_t number) const
{
    for (const Bank& bank : banks_)
    {
        if (bank.number == number)
        {
            return &bank;
        }
    }

    return nullptr;
}

const WaveSystem* SoundArchive::FindWaveSystem(uint32_t number) const
{
    for (const WaveSystem& ws : wave_systems_)
    {
        if (ws.number == number)
        {
            return &ws;
        }
    }

    return nullptr;
}

std::span<const uint8_t> SoundArchive::Collection() const
{
    return std::span<const uint8_t>(data_).subspan(collection_offset_, collection_size_);
}

std::optional<uint32_t> SoundArchive::CollectionSequence(uint32_t category, uint32_t index) const
{
    if (category >= collection_.size() || index >= collection_[category].size())
    {
        return std::nullopt;
    }

    return collection_[category][index];
}

} // namespace ntdlib::jaudio
