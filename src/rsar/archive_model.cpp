// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/archive_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/reference.h"
#include "sar/file_bytes.h"
#include "sar/format_ids.h"
#include "sar/name_tree.h"

namespace ntdlib::rsar
{
namespace
{

constexpr std::size_t kHeaderSize = 0x40;
constexpr std::size_t kBlockAlignment = 32;
constexpr std::size_t kFileBlockHeader = 0x20; // the FILE block's magic, its size and zeros
constexpr std::size_t kTreeOffsets = 20;       // SYMB's offsets of its string table and its four trees
constexpr std::size_t kInfoReferences = 0x30;  // INFO's references to its five tables and the player's settings
constexpr std::size_t kSoundEntrySize = 0x2C;
constexpr std::size_t kSettings3dSize = 0xC;
constexpr std::size_t kSequenceDetailsSize = 0x14;
constexpr std::size_t kStreamDetailsSize = 0xC;
constexpr std::size_t kWaveSoundDetailsSize = 0x10;
constexpr std::size_t kBankEntrySize = 0xC;
constexpr std::size_t kPlayerEntrySize = 0x10;
constexpr std::size_t kFileEntrySize = 0x1C;
constexpr std::size_t kPositionSize = 8;
constexpr std::size_t kGroupEntrySize = 0x28;
constexpr std::size_t kItemSize = 0x18;
constexpr std::size_t kSettingsSize = 0x14; // seven counts, two bytes of padding and four reserved bytes
constexpr std::size_t kFileAlignment = 32;
constexpr uint16_t kOldestVersion = 0x0101;
constexpr uint16_t kNewestVersion = 0x0104;

using sar::detail::kNone;
using sar::detail::OtherLayout;
using sar::detail::Out;
using sar::detail::TreeName;

// A reference to an offset from INFO's payload, with the type of its data.
void PutReference(Out& out, std::size_t at, uint8_t data_type, std::size_t target, std::size_t base)
{
    out.U8(at, 1);
    out.U8(at + 1, data_type);
    out.U32(at + 4, static_cast<uint32_t>(target - base));
}

// A table of `count` references at the end of `out`, with the reference at `ref` to it. Returns where its references
// start.
std::size_t PutTable(Out& out, std::size_t ref, std::size_t count, std::size_t base)
{
    const std::size_t table = out.Zeros(4 + 8 * count);
    out.U32(table, static_cast<uint32_t>(count));
    PutReference(out, ref, 0, table, base);

    return table + 4;
}

std::size_t DetailsSize(SoundType type)
{
    switch (type)
    {
    case SoundType::kSequence:
        return kSequenceDetailsSize;
    case SoundType::kStream:
        return kStreamDetailsSize;
    case SoundType::kWaveSound:
        return kWaveSoundDetailsSize;
    case SoundType::kUnknown:
        break;
    }

    throw std::invalid_argument("a sound of an unknown kind");
}

// Throws std::invalid_argument for a name of an item of `model` with a zero byte.
void CheckNames(const ArchiveModel& model)
{
    auto check_name = [](const std::string& name)
    {
        if (name.find('\0') != std::string::npos)
        {
            throw std::invalid_argument("a name with a zero byte");
        }
    };
    for (const SoundModel& sound : model.sounds)
    {
        check_name(sound.name);
    }
    for (const BankModel& bank : model.banks)
    {
        check_name(bank.name);
    }
    for (const PlayerModel& player : model.players)
    {
        check_name(player.name);
    }
    for (const GroupModel& group : model.groups)
    {
        check_name(group.name);
    }
}

// Throws std::invalid_argument for a model that BuildArchive can't lay out.
void Check(const ArchiveModel& model)
{
    if (model.version < kOldestVersion || model.version > kNewestVersion)
    {
        throw std::invalid_argument("an archive of version " + std::to_string(model.version >> 8) + "." +
                                    std::to_string(model.version & 0xFF));
    }
    for (std::size_t s = 0; s < model.sounds.size(); ++s)
    {
        const SoundModel& sound = model.sounds[s];
        DetailsSize(sound.type);
        if (sound.file_id >= model.files.size())
        {
            throw std::invalid_argument("sound " + std::to_string(s) + " names a file that the archive lacks");
        }
        if (sound.player_id >= model.players.size())
        {
            throw std::invalid_argument("sound " + std::to_string(s) + " names a player that the archive lacks");
        }
        if (sound.type == SoundType::kSequence && sound.bank >= model.banks.size())
        {
            throw std::invalid_argument("sound " + std::to_string(s) + " names a bank that the archive lacks");
        }
    }
    for (std::size_t b = 0; b < model.banks.size(); ++b)
    {
        if (model.banks[b].file_id >= model.files.size())
        {
            throw std::invalid_argument("bank " + std::to_string(b) + " names a file that the archive lacks");
        }
    }
    for (std::size_t g = 0; g < model.groups.size(); ++g)
    {
        for (const uint32_t id : model.groups[g].file_ids)
        {
            if (id >= model.files.size())
            {
                throw std::invalid_argument("group " + std::to_string(g) + " names a file that the archive lacks");
            }
            if (model.files[id].path)
            {
                throw std::invalid_argument("group " + std::to_string(g) + " names a file beside the archive");
            }
        }
    }
    for (std::size_t f = 0; f < model.files.size(); ++f)
    {
        const FileModel& file = model.files[f];
        if (file.path && (!file.bytes.empty() || !file.wave_data.empty()))
        {
            throw std::invalid_argument("file " + std::to_string(f) + " lies beside the archive and holds bytes");
        }
        if (file.bytes.size() % kFileAlignment != 0 || file.wave_data.size() % kFileAlignment != 0)
        {
            throw std::invalid_argument("file " + std::to_string(f) + " has a size that isn't a multiple of 32 bytes");
        }
    }

    CheckNames(model);
}

// The groups of `model`'s archive: its groups with names, then the last group, which holds the files that the others
// don't hold, in the order of their IDs.
std::vector<GroupModel> AllGroups(const ArchiveModel& model)
{
    std::vector<GroupModel> groups = model.groups;
    std::vector<bool> held(model.files.size(), false);
    for (const GroupModel& group : model.groups)
    {
        for (const uint32_t id : group.file_ids)
        {
            held[id] = true;
        }
    }
    GroupModel& last = groups.emplace_back();
    for (uint32_t id = 0; id < model.files.size(); ++id)
    {
        if (!held[id] && !model.files[id].path)
        {
            last.file_ids.push_back(id);
        }
    }

    return groups;
}

// The string index of each name, in the string table's order: the players', the groups', the banks' and then the
// sounds'. An empty name has none.
struct Strings
{
    std::vector<std::string_view> table;
    std::vector<uint32_t> players, groups, banks, sounds;
};

Strings StringsOf(const ArchiveModel& model, const std::vector<GroupModel>& groups)
{
    Strings strings;
    auto add = [&](const std::string& name)
    {
        if (name.empty())
        {
            return kNone;
        }

        strings.table.push_back(name);
        return static_cast<uint32_t>(strings.table.size() - 1);
    };
    for (const PlayerModel& player : model.players)
    {
        strings.players.push_back(add(player.name));
    }
    for (const GroupModel& group : groups)
    {
        strings.groups.push_back(add(group.name));
    }
    for (const BankModel& bank : model.banks)
    {
        strings.banks.push_back(add(bank.name));
    }
    for (const SoundModel& sound : model.sounds)
    {
        strings.sounds.push_back(add(sound.name));
    }

    return strings;
}

// The names of a tree, in the order of their items. `indices` gives each item's string index.
std::vector<TreeName> TreeNames(const Strings& strings, const std::vector<uint32_t>& indices)
{
    std::vector<TreeName> names;
    for (uint32_t item = 0; item < indices.size(); ++item)
    {
        if (indices[item] != kNone)
        {
            names.push_back({strings.table[indices[item]], indices[item], item});
        }
    }

    return names;
}

void PutSymbols(Out& out, const ArchiveModel& model, const Strings& strings)
{
    const std::size_t block = out.Append(std::span(reinterpret_cast<const uint8_t*>("SYMB"), 4));
    out.Zeros(4);
    const std::size_t body = out.Size();
    out.Zeros(kTreeOffsets);
    const std::size_t table = out.Zeros(4 + 4 * strings.table.size());
    out.U32(body, static_cast<uint32_t>(table - body));
    out.U32(table, static_cast<uint32_t>(strings.table.size()));
    for (std::size_t i = 0; i < strings.table.size(); ++i)
    {
        const std::string_view name = strings.table[i];
        const std::size_t at = out.Append(std::span(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
        out.Zeros(1);
        out.U32(table + 4 + 4 * i, static_cast<uint32_t>(at - body));
    }
    if (model.aligned_trees)
    {
        out.Align(4);
    }

    const std::array<std::pair<const std::vector<uint32_t>*, const char*>, 4> trees = {{
        {&strings.sounds, "sound"},
        {&strings.players, "player"},
        {&strings.groups, "group"},
        {&strings.banks, "bank"},
    }};
    for (std::size_t t = 0; t < trees.size(); ++t)
    {
        const std::vector<TreeName> names = TreeNames(strings, *trees[t].first);
        const auto [root, nodes] =
            sar::detail::BuildTree(names, std::string("two ") + trees[t].second + " names alike: ");
        const std::size_t tree = sar::detail::AppendTree(out, root, nodes);
        out.U32(body + 4 + 4 * t, static_cast<uint32_t>(tree - body));
    }
    out.Align(kBlockAlignment);
    out.U32(block + 4, static_cast<uint32_t>(out.Size() - block));
}

// Where the entry of each group and of each of its items lies in INFO.
struct GroupPlaces
{
    std::size_t entry = 0;
    std::vector<std::size_t> items;
};

// Appends the entry of `sound`, with the string index `name`, then its details and its 3D settings. The reference at
// `reference` leads to the entry. INFO's references count from `base`.
void PutSound(Out& out, const SoundModel& sound, uint32_t name, std::size_t reference, std::size_t base)
{
    const std::size_t entry = out.Zeros(kSoundEntrySize);
    PutReference(out, reference, 0, entry, base);
    out.U32(entry, name);
    out.U32(entry + 4, sound.file_id);
    out.U32(entry + 8, sound.player_id);
    out.U8(entry + 0x14, sound.volume);
    out.U8(entry + 0x15, sound.player_priority);
    out.U8(entry + 0x16, static_cast<uint8_t>(sound.type));
    out.U8(entry + 0x17, sound.remote_filter);
    out.U32(entry + 0x20, sound.user_params[0]);
    out.U32(entry + 0x24, sound.user_params[1]);
    out.U8(entry + 0x28, sound.pan_mode);
    out.U8(entry + 0x29, sound.pan_curve);
    out.U8(entry + 0x2A, sound.actor_player_id);

    const std::size_t details = out.Zeros(DetailsSize(sound.type));
    PutReference(out, entry + 0x18, static_cast<uint8_t>(sound.type), details, base);
    switch (sound.type)
    {
    case SoundType::kSequence:
        out.U32(details, sound.start_offset);
        out.U32(details + 4, sound.bank);
        out.U32(details + 8, sound.allocate_track_flags);
        out.U8(details + 0xC, sound.channel_priority);
        out.U8(details + 0xD, sound.release_priority_fix ? 1 : 0);
        break;
    case SoundType::kStream:
        out.U32(details, sound.stream_start);
        out.U16(details + 4, sound.allocate_channel_count);
        out.U16(details + 6, sound.stream_track_flags);
        break;
    default:
        out.U32(details, static_cast<uint32_t>(sound.wave_sound_index));
        out.U32(details + 4, sound.allocate_track_count);
        out.U8(details + 8, sound.channel_priority);
        out.U8(details + 9, sound.release_priority_fix ? 1 : 0);
        break;
    }

    const std::size_t settings = out.Zeros(kSettings3dSize);
    PutReference(out, entry + 0xC, 0, settings, base);
    out.U32(settings, sound.settings_3d.flags);
    out.U8(settings + 4, sound.settings_3d.decay_curve);
    out.U8(settings + 5, sound.settings_3d.decay_ratio);
    out.U8(settings + 6, sound.settings_3d.doppler_factor);
}

// Appends the table of files of `model` and each file's entry, with its path and the places where `groups` hold it: a
// group and an item of the group. The reference at `reference` leads to the table. INFO's references count from
// `base`.
void PutFileEntries(Out& out, const ArchiveModel& model, const std::vector<GroupModel>& groups, std::size_t reference,
                    std::size_t base)
{
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> positions(model.files.size());
    for (uint32_t g = 0; g < groups.size(); ++g)
    {
        for (uint32_t i = 0; i < groups[g].file_ids.size(); ++i)
        {
            positions[groups[g].file_ids[i]].emplace_back(g, i);
        }
    }

    const std::size_t files = PutTable(out, reference, model.files.size(), base);
    for (std::size_t f = 0; f < model.files.size(); ++f)
    {
        const FileModel& file = model.files[f];
        const std::size_t entry = out.Zeros(kFileEntrySize);
        PutReference(out, files + 8 * f, 0, entry, base);
        out.U32(entry, file.path ? file.path_size : static_cast<uint32_t>(file.bytes.size()));
        out.U32(entry + 4, static_cast<uint32_t>(file.wave_data.size()));
        out.U32(entry + 8, kNone);
        if (file.path)
        {
            const std::size_t path =
                out.Append(std::span(reinterpret_cast<const uint8_t*>(file.path->data()), file.path->size()));
            if (model.terminated_paths)
            {
                out.Zeros(1);
            }
            out.Align(4);
            PutReference(out, entry + 0xC, 0, path, base);
        }

        const std::size_t places = PutTable(out, entry + 0x14, positions[f].size(), base);
        for (std::size_t p = 0; p < positions[f].size(); ++p)
        {
            const std::size_t place = out.Zeros(kPositionSize);
            PutReference(out, places + 8 * p, 0, place, base);
            out.U32(place, positions[f][p].first);
            out.U32(place + 4, positions[f][p].second);
        }
    }
}

// Appends the table of `groups` and each group's entry and items, with the groups' string indices from `strings`. The
// reference at `reference` leads to the table. INFO's references count from `base`. Returns where each entry and item
// lies.
std::vector<GroupPlaces> PutGroupEntries(Out& out, const std::vector<GroupModel>& groups, const Strings& strings,
                                         std::size_t reference, std::size_t base)
{
    std::vector<GroupPlaces> group_places;
    const std::size_t group_table = PutTable(out, reference, groups.size(), base);
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        GroupPlaces& places = group_places.emplace_back();
        places.entry = out.Zeros(kGroupEntrySize);
        PutReference(out, group_table + 8 * g, 0, places.entry, base);
        out.U32(places.entry, strings.groups[g]);
        out.U32(places.entry + 4, kNone);
        const std::size_t items = PutTable(out, places.entry + 0x20, groups[g].file_ids.size(), base);
        for (std::size_t i = 0; i < groups[g].file_ids.size(); ++i)
        {
            const std::size_t item = out.Zeros(kItemSize);
            PutReference(out, items + 8 * i, 0, item, base);
            out.U32(item, groups[g].file_ids[i]);
            places.items.push_back(item);
        }
    }

    return group_places;
}

std::vector<GroupPlaces> PutInfo(Out& out, const ArchiveModel& model, const Strings& strings,
                                 const std::vector<GroupModel>& groups)
{
    const std::size_t block = out.Append(std::span(reinterpret_cast<const uint8_t*>("INFO"), 4));
    out.Zeros(4);
    const std::size_t base = out.Size();
    const std::size_t refs = out.Zeros(kInfoReferences);

    const std::size_t sounds = PutTable(out, refs, model.sounds.size(), base);
    for (std::size_t s = 0; s < model.sounds.size(); ++s)
    {
        PutSound(out, model.sounds[s], strings.sounds[s], sounds + 8 * s, base);
    }

    const std::size_t banks = PutTable(out, refs + 8, model.banks.size(), base);
    for (std::size_t b = 0; b < model.banks.size(); ++b)
    {
        const std::size_t entry = out.Zeros(kBankEntrySize);
        PutReference(out, banks + 8 * b, 0, entry, base);
        out.U32(entry, strings.banks[b]);
        out.U32(entry + 4, model.banks[b].file_id);
    }

    const std::size_t players = PutTable(out, refs + 0x10, model.players.size(), base);
    for (std::size_t p = 0; p < model.players.size(); ++p)
    {
        const std::size_t entry = out.Zeros(kPlayerEntrySize);
        PutReference(out, players + 8 * p, 0, entry, base);
        out.U32(entry, strings.players[p]);
        out.U8(entry + 4, model.players[p].playable_sound_count);
        out.U32(entry + 8, model.players[p].heap_size);
    }

    PutFileEntries(out, model, groups, refs + 0x18, base);
    std::vector<GroupPlaces> group_places = PutGroupEntries(out, groups, strings, refs + 0x20, base);

    const PlayerSettings& player = model.player_settings;
    const std::size_t settings = out.Zeros(kSettingsSize);
    PutReference(out, refs + 0x28, 0, settings, base);
    const std::array<uint16_t, 7> counts = {player.sequence_sounds, player.sequence_tracks, player.stream_sounds,
                                            player.stream_tracks,   player.stream_channels, player.wave_sounds,
                                            player.wave_tracks};
    for (std::size_t c = 0; c < counts.size(); ++c)
    {
        out.U16(settings + 2 * c, counts[c]);
    }

    out.Align(kBlockAlignment);
    out.U32(block + 4, static_cast<uint32_t>(out.Size() - block));

    return group_places;
}

void PutFiles(Out& out, const ArchiveModel& model, const std::vector<GroupModel>& groups,
              const std::vector<GroupPlaces>& places)
{
    const std::size_t block = out.Append(std::span(reinterpret_cast<const uint8_t*>("FILE"), 4));
    out.Zeros(kFileBlockHeader - 4);
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        const std::size_t start = out.Size();
        for (std::size_t i = 0; i < groups[g].file_ids.size(); ++i)
        {
            const std::vector<uint8_t>& bytes = model.files[groups[g].file_ids[i]].bytes;
            out.U32(places[g].items[i] + 4, static_cast<uint32_t>(out.Size() - start));
            out.U32(places[g].items[i] + 8, static_cast<uint32_t>(bytes.size()));
            out.Append(bytes);
        }

        const std::size_t waves = out.Size();
        for (std::size_t i = 0; i < groups[g].file_ids.size(); ++i)
        {
            const std::vector<uint8_t>& wave_data = model.files[groups[g].file_ids[i]].wave_data;
            if (!wave_data.empty())
            {
                out.U32(places[g].items[i] + 0xC, static_cast<uint32_t>(out.Size() - waves));
                out.U32(places[g].items[i] + 0x10, static_cast<uint32_t>(wave_data.size()));
                out.Append(wave_data);
            }
        }

        if (out.Size() > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("an archive past 4 GB");
        }
        out.U32(places[g].entry + 0x10, static_cast<uint32_t>(start));
        out.U32(places[g].entry + 0x14, static_cast<uint32_t>(waves - start));
        out.U32(places[g].entry + 0x18, static_cast<uint32_t>(waves));
        out.U32(places[g].entry + 0x1C, static_cast<uint32_t>(out.Size() - waves));
    }
    out.U32(block + 4, static_cast<uint32_t>(out.Size() - block));
}

// The name at string index `index` of `strings`, or an empty name for kNone.
std::string NameAt(const std::vector<std::string>& strings, uint32_t index)
{
    if (index == kNone)
    {
        return {};
    }
    if (index >= strings.size())
    {
        throw OtherLayout("a string index past the string table");
    }

    return strings[index];
}

// The places of the entries of the table whose reference lies at `ref`. `base` is INFO's payload.
std::vector<std::size_t> EntriesOf(const xsflib::Reader& r, std::size_t ref, std::size_t base)
{
    const Reference table = ReadRef(r, ref);
    if (!table.IsOffset())
    {
        throw OtherLayout("a table that isn't there");
    }

    const std::size_t at = base + table.offset;
    const uint32_t count = r.U32(at);
    if (count > (r.Size() - at) / 8)
    {
        throw xsflib::FormatError("a table that runs past the end of the archive");
    }
    std::vector<std::size_t> entries;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Reference entry = ReadRef(r, at + 4 + 8 * std::size_t{i});
        if (!entry.IsOffset())
        {
            throw OtherLayout("a table's entry that isn't there");
        }
        entries.push_back(base + entry.offset);
    }

    return entries;
}

// The names in the string table of the SYMB block at `symb`. Sets `aligned_trees` when the trees don't start right
// after the last name.
std::vector<std::string> ReadNames(const xsflib::Reader& r, std::size_t symb, bool& aligned_trees)
{
    const std::size_t body = symb + std::size_t{8};
    const std::size_t table = body + r.U32(body);
    const uint32_t count = r.U32(table);
    if (count > (r.Size() - table) / 4)
    {
        throw xsflib::FormatError("a string table that runs past the end of the archive");
    }
    std::vector<std::string> strings;
    std::size_t strings_end = table + 4 + 4 * std::size_t{count};
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t at = body + r.U32(table + 4 + 4 * std::size_t{i});
        if (at >= r.Size())
        {
            throw xsflib::FormatError("a string past the end of the archive");
        }
        strings.push_back(r.CString(at, r.Size() - at));
        strings_end = std::max(strings_end, at + strings.back().size() + 1);
    }
    aligned_trees = body + r.U32(body + 4) != strings_end;

    return strings;
}

// The sound of the sound table's entry at `entry`, with its 3D settings and its details. `base` is INFO's payload.
SoundModel ReadSound(const xsflib::Reader& r, std::size_t base, const std::vector<std::string>& strings,
                     std::size_t entry)
{
    SoundModel sound;
    sound.name = NameAt(strings, r.U32(entry));
    sound.file_id = r.U32(entry + 4);
    sound.player_id = r.U32(entry + 8);
    sound.volume = r.U8(entry + 0x14);
    sound.player_priority = r.U8(entry + 0x15);
    const uint8_t type = r.U8(entry + 0x16);
    sound.remote_filter = r.U8(entry + 0x17);
    sound.user_params = {r.U32(entry + 0x20), r.U32(entry + 0x24)};
    sound.pan_mode = r.U8(entry + 0x28);
    sound.pan_curve = r.U8(entry + 0x29);
    sound.actor_player_id = r.U8(entry + 0x2A);
    if (type < 1 || type > 3)
    {
        throw OtherLayout("a sound of an unknown kind (" + std::to_string(type) + ")");
    }
    sound.type = static_cast<SoundType>(type);

    const Reference settings = ReadRef(r, entry + 0xC);
    const Reference details = ReadRef(r, entry + 0x18);
    if (!settings.IsOffset() || !details.IsOffset())
    {
        throw OtherLayout("a sound without its 3D settings or its details");
    }
    const std::size_t at = base + settings.offset;
    sound.settings_3d.flags = r.U32(at);
    sound.settings_3d.decay_curve = r.U8(at + 4);
    sound.settings_3d.decay_ratio = r.U8(at + 5);
    sound.settings_3d.doppler_factor = r.U8(at + 6);

    const std::size_t q = base + details.offset;
    switch (sound.type)
    {
    case SoundType::kSequence:
        sound.start_offset = r.U32(q);
        sound.bank = r.U32(q + 4);
        sound.allocate_track_flags = r.U32(q + 8);
        sound.channel_priority = r.U8(q + 0xC);
        sound.release_priority_fix = r.U8(q + 0xD) != 0;
        break;
    case SoundType::kStream:
        sound.stream_start = r.U32(q);
        sound.allocate_channel_count = r.U16(q + 4);
        sound.stream_track_flags = r.U16(q + 6);
        break;
    default:
        sound.wave_sound_index = r.S32(q);
        sound.allocate_track_count = r.U32(q + 4);
        sound.channel_priority = r.U8(q + 8);
        sound.release_priority_fix = r.U8(q + 9) != 0;
        break;
    }

    return sound;
}

// The files beside the archive: each one's path, and the size that its entry gives. Clears `terminated_paths` when a
// path whose length is a multiple of 4 ends without a zero.
void ReadFiles(const xsflib::Reader& r, std::size_t base, ArchiveModel& model)
{
    const std::vector<std::size_t> file_entries = EntriesOf(r, base + 0x18, base);
    model.files.resize(file_entries.size());
    for (std::size_t f = 0; f < file_entries.size(); ++f)
    {
        const std::size_t entry = file_entries[f];
        const Reference path = ReadRef(r, entry + 0xC);
        if (!path.IsOffset())
        {
            continue;
        }

        const std::size_t at = base + path.offset;
        FileModel& file = model.files[f];
        file.path = r.CString(at, r.Size() - at);
        file.path_size = r.U32(entry);
        const Reference places = ReadRef(r, entry + 0x14);
        if (file.path->size() % 4 == 0 && places.IsOffset() && base + places.offset == at + file.path->size())
        {
            model.terminated_paths = false;
        }
    }
}

// The player's settings: the counts of sounds, tracks and channels that the sound players allocate.
PlayerSettings ReadPlayerSettings(const xsflib::Reader& r, std::size_t base)
{
    const Reference settings = ReadRef(r, base + 0x28);
    if (!settings.IsOffset())
    {
        throw OtherLayout("an archive without the player's settings");
    }
    PlayerSettings player;
    const std::array<uint16_t*, 7> counts = {&player.sequence_sounds, &player.sequence_tracks, &player.stream_sounds,
                                             &player.stream_tracks,   &player.stream_channels, &player.wave_sounds,
                                             &player.wave_tracks};
    for (std::size_t c = 0; c < counts.size(); ++c)
    {
        *counts[c] = r.U16(base + settings.offset + 2 * c);
    }

    return player;
}

// The groups, and the bytes of each file that they hold. Every copy of a file has to hold the same bytes. The last
// group has no name. The model leaves it out.
void ReadGroups(const xsflib::Reader& r, std::size_t base, const std::vector<std::string>& strings, ArchiveModel& model)
{
    const std::vector<std::size_t> group_entries = EntriesOf(r, base + 0x20, base);
    if (group_entries.empty())
    {
        throw OtherLayout("an archive without its last group");
    }
    std::vector<bool> read(model.files.size(), false);
    for (std::size_t g = 0; g < group_entries.size(); ++g)
    {
        const std::size_t entry = group_entries[g];
        if (ReadRef(r, entry + 8).IsOffset())
        {
            throw OtherLayout("a group beside the archive");
        }

        GroupModel group;
        group.name = NameAt(strings, r.U32(entry));
        const std::size_t files = r.U32(entry + 0x10);
        const std::size_t waves = r.U32(entry + 0x18);
        for (const std::size_t item : EntriesOf(r, entry + 0x20, base))
        {
            const uint32_t id = r.U32(item);
            if (id >= model.files.size())
            {
                throw OtherLayout("a group's item that names a file that the archive lacks");
            }
            const std::span<const uint8_t> bytes = r.Sub(files + r.U32(item + 4), r.U32(item + 8));
            const std::span<const uint8_t> wave_data = r.Sub(waves + r.U32(item + 0xC), r.U32(item + 0x10));
            FileModel& file = model.files[id];
            if (!read[id])
            {
                read[id] = true;
                file.bytes.assign(bytes.begin(), bytes.end());
                file.wave_data.assign(wave_data.begin(), wave_data.end());
            }
            else if (!std::ranges::equal(file.bytes, bytes) || !std::ranges::equal(file.wave_data, wave_data))
            {
                throw OtherLayout("copies of file " + std::to_string(id) + " that differ");
            }
            group.file_ids.push_back(id);
        }

        if (g + 1 < group_entries.size())
        {
            model.groups.push_back(std::move(group));
        }
        else if (!group.name.empty())
        {
            throw OtherLayout("a last group with a name");
        }
    }
}

// Throws an error of OtherLayout when the archive that `model` gives isn't `archive`.
void CheckRebuilt(const ArchiveModel& model, std::span<const uint8_t> archive)
{
    std::vector<uint8_t> built;
    try
    {
        built = BuildArchive(model);
    }
    catch (const std::invalid_argument& error)
    {
        throw OtherLayout(error.what());
    }
    const auto [ours, theirs] = std::ranges::mismatch(built, archive);
    if (ours != built.end() || theirs != archive.end())
    {
        throw OtherLayout("the model gives other bytes from offset " +
                          std::to_string(static_cast<std::size_t>(ours - built.begin())));
    }
}

} // namespace

ArchiveModel ReadArchiveModel(std::span<const uint8_t> archive)
{
    const xsflib::Reader r(archive, true);
    if (archive.size() < kHeaderSize || !r.Magic(0, "RSAR") || r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("not a sound archive of the Wii (RSAR)");
    }

    ArchiveModel model;
    model.version = r.U16(6);
    if (model.version < kOldestVersion || model.version > kNewestVersion)
    {
        throw OtherLayout("an archive of version " + std::to_string(model.version >> 8) + "." +
                          std::to_string(model.version & 0xFF));
    }
    const uint32_t symb = r.U32(0x10);
    const uint32_t info = r.U32(0x18);
    if (symb == 0 || r.U32(0x14) == 0)
    {
        throw OtherLayout("an archive without names");
    }
    if (!r.Magic(symb, "SYMB") || !r.Magic(info, "INFO"))
    {
        throw xsflib::FormatError("an archive without its SYMB or INFO block");
    }

    // The names, then the tables.
    const std::vector<std::string> strings = ReadNames(r, symb, model.aligned_trees);
    const std::size_t base = info + std::size_t{8};
    for (const std::size_t entry : EntriesOf(r, base, base))
    {
        model.sounds.push_back(ReadSound(r, base, strings, entry));
    }
    for (const std::size_t entry : EntriesOf(r, base + 8, base))
    {
        model.banks.push_back({NameAt(strings, r.U32(entry)), r.U32(entry + 4)});
    }
    for (const std::size_t entry : EntriesOf(r, base + 0x10, base))
    {
        model.players.push_back({NameAt(strings, r.U32(entry)), r.U8(entry + 4), r.U32(entry + 8)});
    }
    ReadFiles(r, base, model);
    model.player_settings = ReadPlayerSettings(r, base);
    ReadGroups(r, base, strings, model);

    // The archive that the model gives has to be `archive`.
    CheckRebuilt(model, archive);

    return model;
}

std::vector<uint8_t> BuildArchive(const ArchiveModel& model)
{
    Check(model);
    const std::vector<GroupModel> groups = AllGroups(model);
    const Strings strings = StringsOf(model, groups);

    Out out(true);
    out.Append(std::span(reinterpret_cast<const uint8_t*>("RSAR"), 4));
    out.Zeros(kHeaderSize - 4);
    out.U16(4, 0xFEFF);
    out.U16(6, model.version);
    out.U16(0xC, static_cast<uint16_t>(kHeaderSize));
    out.U16(0xE, 3);

    const std::size_t symb = out.Size();
    PutSymbols(out, model, strings);
    const std::size_t info = out.Size();
    const std::vector<GroupPlaces> places = PutInfo(out, model, strings, groups);
    const std::size_t file = out.Size();
    PutFiles(out, model, groups, places);

    out.U32(8, static_cast<uint32_t>(out.Size()));
    out.U32(0x10, static_cast<uint32_t>(symb));
    out.U32(0x14, static_cast<uint32_t>(info - symb));
    out.U32(0x18, static_cast<uint32_t>(info));
    out.U32(0x1C, static_cast<uint32_t>(file - info));
    out.U32(0x20, static_cast<uint32_t>(file));
    out.U32(0x24, static_cast<uint32_t>(out.Size() - file));

    return out.Take();
}

namespace
{

// Throws std::invalid_argument for a model past the first `sounds` sounds of `archive`, a file past its file table,
// and a name that another sound holds.
void CheckNewSound(const ArchiveModel& archive, std::size_t sounds, uint32_t model, uint32_t file_id,
                   const std::string& name)
{
    if (model >= sounds)
    {
        throw std::invalid_argument("a model past the sound table");
    }
    if (file_id >= archive.files.size())
    {
        throw std::invalid_argument("a file past the file table");
    }

    const auto same_name = [&name](const SoundModel& sound)
    {
        return sound.name == name;
    };
    if (!name.empty() && std::ranges::any_of(archive.sounds, same_name))
    {
        throw std::invalid_argument("a name that another sound holds: " + name);
    }
}

// A new sound with the fields of `model` that a sound of every kind holds: its player, volume, player priority, remote
// filter, user parameters, pan mode, pan curve, actor player and 3D settings.
SoundModel CopiedSound(const SoundModel& model)
{
    SoundModel sound;
    sound.player_id = model.player_id;
    sound.volume = model.volume;
    sound.player_priority = model.player_priority;
    sound.remote_filter = model.remote_filter;
    sound.user_params = model.user_params;
    sound.pan_mode = model.pan_mode;
    sound.pan_curve = model.pan_curve;
    sound.actor_player_id = model.actor_player_id;
    sound.settings_3d = model.settings_3d;

    return sound;
}

} // namespace

ArchiveModel WithNewWaveSounds(ArchiveModel archive, std::span<const NewWaveSound> sounds)
{
    const std::size_t count = archive.sounds.size();
    for (const NewWaveSound& new_sound : sounds)
    {
        CheckNewSound(archive, count, new_sound.model, new_sound.file_id, new_sound.name);
        const SoundModel& model = archive.sounds[new_sound.model];
        if (model.type != SoundType::kWaveSound && model.type != SoundType::kSequence)
        {
            throw std::invalid_argument("a model that isn't a wave sound or a sequence sound");
        }

        SoundModel sound = CopiedSound(model);
        sound.name = new_sound.name;
        sound.type = SoundType::kWaveSound;
        sound.file_id = new_sound.file_id;
        sound.channel_priority = model.channel_priority;
        sound.release_priority_fix = model.release_priority_fix;
        sound.wave_sound_index = new_sound.index;
        sound.allocate_track_count = model.type == SoundType::kWaveSound ? model.allocate_track_count : 1;
        archive.sounds.push_back(std::move(sound));
    }

    return archive;
}

ArchiveModel WithNewStreamSound(ArchiveModel archive, const NewStreamSound& sound)
{
    CheckNewSound(archive, archive.sounds.size(), sound.model, sound.file_id, sound.name);

    SoundModel stream = CopiedSound(archive.sounds[sound.model]);
    stream.name = sound.name;
    stream.type = SoundType::kStream;
    stream.file_id = sound.file_id;
    stream.stream_start = sound.stream_start;
    stream.allocate_channel_count = sound.allocate_channel_count;
    stream.stream_track_flags = sound.stream_track_flags;
    archive.sounds.push_back(std::move(stream));

    return archive;
}

} // namespace ntdlib::rsar
