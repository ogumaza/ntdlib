// SPDX-License-Identifier: MIT

#include "ntdlib/sar/archive_model.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "sar/file_bytes.h"
#include "sar/stream_details.h"

namespace ntdlib::sar
{
namespace
{

constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kBlockCountField = 0x10;
constexpr std::size_t kBlockTable = 0x14;
constexpr std::size_t kBlockEntrySize = 12; // a type, two bytes of padding, an offset and a size
constexpr std::size_t kBlockHeaderSize = 8; // a block's magic and size
constexpr uint16_t kStringBlock = 0x2000;
constexpr uint16_t kInfoBlock = 0x2001;
constexpr uint16_t kFileBlock = 0x2002;
constexpr uint16_t kListReference = 0x0100; // a list of 32-bit values
constexpr uint16_t kDataReference = 0x1F00; // a file's place in the FILE block
constexpr uint16_t kStreamDetails = 0x2201;
constexpr uint16_t kWaveSoundDetails = 0x2202;
constexpr uint16_t kSequenceDetails = 0x2203;
constexpr uint16_t kWaveSoundSetDetails = 0x2205;
constexpr uint16_t kInternalFile = 0x220C; // from a file's entry to its place in the FILE block
constexpr uint16_t kExternalFile = 0x220D; // from a file's entry to its path
constexpr uint16_t kStreamExtension = 0x2210;
constexpr uint16_t kGroupInfoBlock = 0x7800;
constexpr uint16_t kGroupFileBlock = 0x7801;
constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr std::size_t kSettingsSize = 16; // a sound's 3D settings

// The options of a sound's entry, by their bits, and the options that an entry can hold.
constexpr int kNameBit = 0;
constexpr int kPanBit = 1;
constexpr int kPriorityBit = 2;
constexpr int kSinglePlayBit = 3;
constexpr int kSettingsBit = 8;
constexpr int kOption17Bit = 17;
constexpr int kFirstUserBit = 28;
constexpr uint32_t kKnownSoundOptions = 0xF002010F;

// The options of the other entries that the model holds: the string index, a wave archive's count of waves and a
// player's heap. A sequence sound's details hold its start and its channel priority, and a wave sound's details its
// channel priority.
constexpr uint32_t kKnownNameOptions = 1;
constexpr uint32_t kKnownWaveArchiveOptions = 3;
constexpr uint32_t kKnownPlayerOptions = 3;
constexpr uint32_t kKnownSequenceOptions = 3;
constexpr uint32_t kKnownWaveSoundOptions = 1;

using detail::kNothingVersion;
using detail::kSendsVersion;
using detail::Out;

// The value of each option of an entry, by its bit.
using OptionValues = std::map<int, uint32_t>;

xsflib::FormatError OtherLayout(const std::string& what)
{
    return xsflib::FormatError("an archive laid out another way: " + what);
}

std::size_t Align(std::size_t at, std::size_t alignment)
{
    return (at + alignment - 1) / alignment * alignment;
}

// Two bytes as the value of an option: `low` in its low byte and `high` in the next.
uint32_t Pair(uint8_t low, uint8_t high)
{
    return low | static_cast<uint32_t>(high) << 8;
}

uint8_t Low(uint32_t value)
{
    return static_cast<uint8_t>(value);
}

uint8_t High(uint32_t value)
{
    return static_cast<uint8_t>(value >> 8);
}

// Appends `values`, the options of an entry: the field of their bits, then each value in the order of the bits.
void AppendOptions(Out& out, const OptionValues& values)
{
    const std::size_t at = out.Zeros(4 + 4 * values.size());
    uint32_t bits = 0;
    std::size_t next = at + 4;
    for (const auto& [bit, value] : values)
    {
        bits |= 1u << bit;
        out.U32(next, value);
        next += 4;
    }
    out.U32(at, bits);
}

// The options at `at` of the entry that `r` reads. Throws xsflib::FormatError for bits outside `known`, and for
// options past the entry's end.
OptionValues ReadOptions(const xsflib::Reader& r, std::size_t at, uint32_t known)
{
    const uint32_t bits = r.U32(at);
    if ((bits & ~known) != 0)
    {
        throw OtherLayout("an entry with options that the model doesn't hold");
    }

    OptionValues values;
    std::size_t next = at + 4;
    for (int bit = 0; bit < 32; ++bit)
    {
        if (((bits >> bit) & 1) != 0)
        {
            values[bit] = r.U32(next);
            next += 4;
        }
    }

    return values;
}

// The value of option `bit` of `values`. Throws xsflib::FormatError for an entry without it. `what` names the option.
uint32_t Required(const OptionValues& values, int bit, const char* what)
{
    const auto it = values.find(bit);
    if (it == values.end())
    {
        throw OtherLayout(std::string("an entry without its ") + what);
    }

    return it->second;
}

// The options of an entry with the string index `index`, or without one.
OptionValues NameOption(std::optional<uint32_t> index)
{
    OptionValues values;
    if (index)
    {
        values[kNameBit] = *index;
    }

    return values;
}

// The string index in `values`, if they hold one.
std::optional<uint32_t> IndexOf(const OptionValues& values)
{
    const auto it = values.find(kNameBit);
    if (it == values.end())
    {
        return std::nullopt;
    }

    return it->second;
}

// The name of item `id`, whose options are `values`, in `strings`: empty for an item without a string index. Throws
// xsflib::FormatError for a string index past the string table, or one whose name belongs to another item.
std::string NameOf(const StringBlock& strings, const OptionValues& values, uint32_t id)
{
    const std::optional<uint32_t> index = IndexOf(values);
    if (!index)
    {
        return {};
    }
    if (*index >= strings.names.size() || strings.names[*index].item_id != id)
    {
        throw OtherLayout("a string index that names another item");
    }

    return strings.names[*index].name;
}

// Throws xsflib::FormatError if `built`, an entry that the model gives, isn't `entry`. `what` names the entry.
void CheckEntry(std::span<const uint8_t> built, std::span<const uint8_t> entry, const std::string& what)
{
    if (!std::ranges::equal(built, entry))
    {
        throw OtherLayout(what + " holds fields that the model doesn't hold");
    }
}

// The list of 32-bit values at `at` of the entry that `r` reads: its count, then the values.
std::vector<uint32_t> ReadList(const xsflib::Reader& r, std::size_t at)
{
    std::vector<uint32_t> values;
    const uint32_t count = r.U32(at);
    for (uint32_t i = 0; i < count; ++i)
    {
        values.push_back(r.U32(at + 4 + 4 * std::size_t{i}));
    }

    return values;
}

// Appends `values` as a list of 32-bit values: their count, then the values. Returns where the list starts.
std::size_t AppendList(Out& out, const std::vector<uint32_t>& values)
{
    const std::size_t at = out.Zeros(4 + 4 * values.size());
    out.U32(at, static_cast<uint32_t>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        out.U32(at + 4 + 4 * i, values[i]);
    }

    return at;
}

// A stream sound's details, as the tools write them in an archive of `version`. The fields after the sends follow
// SoundModel::stream_fields, and the extension follows the rest. Throws std::invalid_argument for such fields in a
// version whose stream sounds hold no sends, and for an extension without a reference to it.
std::vector<uint8_t> StreamDetails(const SoundModel& sound, uint32_t version, bool big_endian)
{
    const bool sends = version >= kSendsVersion;
    if (!sends && sound.stream_fields != StreamFields::kNone)
    {
        throw std::invalid_argument("fields after a stream sound's sends in a version without sends");
    }
    if (sound.stream_extension && sound.stream_fields == StreamFields::kNone)
    {
        throw std::invalid_argument("a stream sound's extension without a reference to it");
    }

    // The header of the version that holds every field gives the reference to no extension and the prefetch file. The
    // reference takes 8 bytes after the sends, and the prefetch file 4.
    std::vector<uint8_t> header = detail::StreamHeader(!sends ? version : kNothingVersion, big_endian);
    if (sound.stream_fields == StreamFields::kNone)
    {
        header.resize(header.size() - (sends ? 12 : 0));
    }
    else if (sound.stream_fields == StreamFields::kExtension)
    {
        header.resize(header.size() - 4);
    }
    Out out(big_endian);
    out.Append(detail::StreamDetailsOf(header, sound.stream, sends, big_endian));
    if (sound.stream_fields == StreamFields::kPrefetch)
    {
        out.U32(32, sound.prefetch_file_id);
    }

    // The extension.
    if (const std::optional<StreamExtension>& extension = sound.stream_extension)
    {
        const std::size_t at = out.Zeros(12);
        out.U32(at, extension->stream_type);
        out.U32(at + 4, extension->loop_start);
        out.U32(at + 8, extension->loop_end);
        out.Reference(24, kStreamExtension, static_cast<uint32_t>(at));
    }

    return out.Take();
}

// The entry of `sound`, with the string index `name` or without one, as the tools write it in an archive of
// `version`. Throws std::invalid_argument for a sound of an unknown kind, and the exceptions of StreamDetails.
std::vector<uint8_t> SoundEntry(const SoundModel& sound, std::optional<uint32_t> name, uint32_t version,
                                bool big_endian)
{
    Out out(big_endian);
    out.Zeros(20);
    out.U32(0, sound.file_id);
    out.U32(4, sound.player_id);
    out.U8(8, sound.volume);
    out.U8(9, sound.remote_filter);

    // The options. The 3D settings follow them.
    OptionValues options = NameOption(name);
    options[kPanBit] = Pair(sound.pan_mode, sound.pan_curve);
    options[kPriorityBit] = Pair(sound.player_priority, sound.actor_player_id);
    if (sound.single_play)
    {
        options[kSinglePlayBit] = *sound.single_play;
    }
    if (sound.option_17)
    {
        options[kOption17Bit] = *sound.option_17;
    }
    for (std::size_t i = 0; i < sound.user_options.size(); ++i)
    {
        if (sound.user_options[i])
        {
            options[kFirstUserBit + static_cast<int>(i)] = *sound.user_options[i];
        }
    }
    options[kSettingsBit] = 0;
    options[kSettingsBit] = static_cast<uint32_t>(out.Size() + 4 + 4 * options.size());
    AppendOptions(out, options);
    const std::size_t settings = out.Zeros(kSettingsSize);
    out.U32(settings, sound.settings_3d.flags);
    out.U32(settings + 4, std::bit_cast<uint32_t>(sound.settings_3d.decay_ratio));
    out.U8(settings + 8, sound.settings_3d.decay_curve);
    out.U8(settings + 9, sound.settings_3d.doppler_factor);

    // The details.
    switch (sound.type)
    {
    case SoundType::kSequence:
        {
            out.U32(out.Zeros(4), sound.after_3d);
            const std::size_t details = out.Zeros(12);
            out.U32(details + 8, sound.allocate_track_flags);
            AppendOptions(
                out, {{0, sound.start_offset}, {1, Pair(sound.channel_priority, sound.release_priority_fix ? 1 : 0)}});
            const std::size_t banks = AppendList(out, sound.banks);
            out.Reference(details, kListReference, static_cast<uint32_t>(banks - details));
            out.Reference(12, kSequenceDetails, static_cast<uint32_t>(details));
            break;
        }

    case SoundType::kWaveSound:
        {
            out.U32(out.Zeros(4), sound.after_3d);
            const std::size_t details = out.Zeros(8);
            out.U32(details, sound.wave_sound_index);
            out.U32(details + 4, 1);
            AppendOptions(out, {{0, Pair(sound.channel_priority, sound.release_priority_fix ? 1 : 0)}});
            out.Reference(12, kWaveSoundDetails, static_cast<uint32_t>(details));
            break;
        }

    case SoundType::kStream:
        {
            const std::size_t details = out.Append(StreamDetails(sound, version, big_endian));
            out.Reference(12, kStreamDetails, static_cast<uint32_t>(details));
            break;
        }

    default:
        throw std::invalid_argument("a sound of an unknown kind");
    }

    return out.Take();
}

// Reads a main send and a send to each aux bus at `at`.
void ReadSends(const xsflib::Reader& r, std::size_t at, uint8_t& main, std::array<uint8_t, 3>& aux)
{
    main = r.U8(at);
    for (std::size_t bus = 0; bus < aux.size(); ++bus)
    {
        aux[bus] = r.U8(at + 1 + bus);
    }
}

// The stream sound's details at `details` of the entry that `r` reads, in an archive of `version`.
StreamSoundInfo ReadStream(const xsflib::Reader& r, std::size_t details, uint32_t version)
{
    const bool sends = version >= kSendsVersion;
    StreamSoundInfo stream;
    stream.valid_tracks = r.U16(details);
    stream.channel_count = r.U16(details + 2);
    if (sends)
    {
        stream.pitch = std::bit_cast<float>(r.U32(details + 12));
        ReadSends(r, details + r.U32(details + 20), stream.main_send, stream.aux_sends);
    }

    // The table of tracks, and each track's info, channels and sends.
    const std::size_t table = details + r.U32(details + 8);
    const uint32_t count = r.U32(table);
    for (uint32_t t = 0; t < count; ++t)
    {
        const std::size_t info = table + r.U32(table + 8 + 8 * std::size_t{t});
        StreamTrackInfo track;
        track.volume = r.U8(info);
        track.pan = r.U8(info + 1);
        track.span = r.U8(info + 2);
        track.flags = r.U8(info + 3);
        const std::size_t indices = info + r.U32(info + 8);
        const uint32_t channels = r.U32(indices);
        for (uint32_t c = 0; c < channels; ++c)
        {
            track.channels.push_back(r.U8(indices + 4 + c));
        }
        if (sends)
        {
            track.low_pass = r.U8(info + 20);
            track.biquad_type = r.U8(info + 21);
            track.biquad_value = r.U8(info + 22);
            ReadSends(r, info + r.U32(info + 16), track.main_send, track.aux_sends);
        }
        stream.tracks.push_back(std::move(track));
    }

    return stream;
}

// The model of `entry`, the entry of sound `index` in an archive of `version` whose names are `strings`. Throws
// xsflib::FormatError for an entry that the model doesn't give back.
SoundModel ReadSound(std::span<const uint8_t> entry, const StringBlock& strings, uint32_t index, uint32_t version,
                     bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    SoundModel sound;
    sound.file_id = r.U32(0);
    sound.player_id = r.U32(4);
    sound.volume = r.U8(8);
    sound.remote_filter = r.U8(9);

    // The options and the 3D settings.
    const OptionValues options = ReadOptions(r, 20, kKnownSoundOptions);
    sound.name = NameOf(strings, options, ItemId(kSoundItem, index));
    const uint32_t pan = Required(options, kPanBit, "pan");
    sound.pan_mode = Low(pan);
    sound.pan_curve = High(pan);
    const uint32_t priorities = Required(options, kPriorityBit, "priorities");
    sound.player_priority = Low(priorities);
    sound.actor_player_id = High(priorities);
    if (const auto it = options.find(kSinglePlayBit); it != options.end())
    {
        sound.single_play = it->second;
    }
    if (const auto it = options.find(kOption17Bit); it != options.end())
    {
        sound.option_17 = it->second;
    }
    for (std::size_t i = 0; i < sound.user_options.size(); ++i)
    {
        if (const auto it = options.find(kFirstUserBit + static_cast<int>(i)); it != options.end())
        {
            sound.user_options[i] = it->second;
        }
    }
    const std::size_t settings = Required(options, kSettingsBit, "3D settings");
    sound.settings_3d.flags = r.U32(settings);
    sound.settings_3d.decay_ratio = std::bit_cast<float>(r.U32(settings + 4));
    sound.settings_3d.decay_curve = r.U8(settings + 8);
    sound.settings_3d.doppler_factor = r.U8(settings + 9);

    // The details.
    const std::size_t details = r.U32(16);
    switch (r.U16(12))
    {
    case kSequenceDetails:
        {
            sound.type = SoundType::kSequence;
            sound.after_3d = r.U32(settings + kSettingsSize);
            sound.allocate_track_flags = r.U32(details + 8);
            const OptionValues values = ReadOptions(r, details + 12, kKnownSequenceOptions);
            sound.start_offset = Required(values, 0, "start");
            const uint32_t channel = Required(values, 1, "channel priority");
            sound.channel_priority = Low(channel);
            sound.release_priority_fix = High(channel) != 0;
            sound.banks = ReadList(r, details + r.U32(details + 4));
            break;
        }

    case kWaveSoundDetails:
        {
            sound.type = SoundType::kWaveSound;
            sound.after_3d = r.U32(settings + kSettingsSize);
            sound.wave_sound_index = r.U32(details);
            const uint32_t channel =
                Required(ReadOptions(r, details + 8, kKnownWaveSoundOptions), 0, "channel priority");
            sound.channel_priority = Low(channel);
            sound.release_priority_fix = High(channel) != 0;
            break;
        }

    case kStreamDetails:
        {
            // The table of tracks follows the fields after the sends.
            sound.type = SoundType::kStream;
            sound.stream = ReadStream(r, details, version);
            const uint32_t table = r.U32(details + 8);
            sound.stream_fields = StreamFields::kNone;
            if (table >= 36)
            {
                sound.stream_fields = StreamFields::kPrefetch;
                sound.prefetch_file_id = r.U32(details + 32);
            }
            else if (table >= 32)
            {
                sound.stream_fields = StreamFields::kExtension;
            }
            if (table >= 32 && r.U16(details + 24) == kStreamExtension)
            {
                const std::size_t extension = details + r.U32(details + 28);
                sound.stream_extension = {r.U32(extension), r.U32(extension + 4), r.U32(extension + 8)};
            }
            break;
        }

    default:
        throw OtherLayout("a sound of an unknown kind");
    }

    CheckEntry(SoundEntry(sound, IndexOf(options), version, r.BigEndian()), entry,
               "the entry of sound " + std::to_string(index));

    return sound;
}

// The entry of `group`, with the string index `name` or without one.
std::vector<uint8_t> SoundGroupEntry(const SoundGroupModel& group, std::optional<uint32_t> name, bool big_endian)
{
    Out out(big_endian);
    out.Zeros(24);
    out.U32(0, group.first_sound);
    out.U32(4, group.last_sound);
    AppendOptions(out, NameOption(name));
    out.Reference(8, kListReference, static_cast<uint32_t>(AppendList(out, group.file_ids)));

    // A wave sound set's details: a reference to the list of its wave archives, and no options.
    if (group.wave_archives)
    {
        const std::size_t details = out.Zeros(12);
        const std::size_t list = AppendList(out, *group.wave_archives);
        out.Reference(details, kListReference, static_cast<uint32_t>(list - details));
        out.Reference(16, kWaveSoundSetDetails, static_cast<uint32_t>(details));
    }
    else
    {
        out.U32(20, kNone);
    }

    return out.Take();
}

// The model of `entry`, the entry of sound group `index` in an archive whose names are `strings`. Throws
// xsflib::FormatError for an entry that the model doesn't give back.
SoundGroupModel ReadSoundGroup(std::span<const uint8_t> entry, const StringBlock& strings, uint32_t index,
                               bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    SoundGroupModel group;
    group.first_sound = r.U32(0);
    group.last_sound = r.U32(4);
    const OptionValues options = ReadOptions(r, 24, kKnownNameOptions);
    group.name = NameOf(strings, options, ItemId(kSoundGroupItem, index));
    group.file_ids = ReadList(r, r.U32(12));
    if (r.U16(16) == kWaveSoundSetDetails)
    {
        const std::size_t details = r.U32(20);
        group.wave_archives = ReadList(r, details + r.U32(details + 4));
    }

    CheckEntry(SoundGroupEntry(group, IndexOf(options), big_endian), entry,
               "the entry of sound group " + std::to_string(index));

    return group;
}

// The entry of `bank`, with the string index `name` or without one.
std::vector<uint8_t> BankEntry(const BankModel& bank, std::optional<uint32_t> name, bool big_endian)
{
    Out out(big_endian);
    out.Zeros(12);
    out.U32(0, bank.file_id);
    AppendOptions(out, NameOption(name));
    out.Reference(4, kListReference, static_cast<uint32_t>(AppendList(out, bank.wave_archives)));

    return out.Take();
}

// The model of `entry`, the entry of bank `index` in an archive whose names are `strings`. Throws
// xsflib::FormatError for an entry that the model doesn't give back.
BankModel ReadBank(std::span<const uint8_t> entry, const StringBlock& strings, uint32_t index, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    BankModel bank;
    bank.file_id = r.U32(0);
    const OptionValues options = ReadOptions(r, 12, kKnownNameOptions);
    bank.name = NameOf(strings, options, ItemId(kBankItem, index));
    bank.wave_archives = ReadList(r, r.U32(8));

    CheckEntry(BankEntry(bank, IndexOf(options), big_endian), entry, "the entry of bank " + std::to_string(index));

    return bank;
}

// The entry of `wave_archive`, with the string index `name` or without one.
std::vector<uint8_t> WaveArchiveEntry(const WaveArchiveModel& wave_archive, std::optional<uint32_t> name,
                                      bool big_endian)
{
    Out out(big_endian);
    out.Zeros(8);
    out.U32(0, wave_archive.file_id);
    out.U8(4, wave_archive.load_individually ? 1 : 0);
    OptionValues options = NameOption(name);
    if (wave_archive.wave_count)
    {
        options[1] = *wave_archive.wave_count;
    }
    AppendOptions(out, options);

    return out.Take();
}

// The model of `entry`, the entry of wave archive `index` in an archive whose names are `strings`. Throws
// xsflib::FormatError for an entry that the model doesn't give back.
WaveArchiveModel ReadWaveArchive(std::span<const uint8_t> entry, const StringBlock& strings, uint32_t index,
                                 bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    WaveArchiveModel wave_archive;
    wave_archive.file_id = r.U32(0);
    wave_archive.load_individually = r.U8(4) != 0;
    const OptionValues options = ReadOptions(r, 8, kKnownWaveArchiveOptions);
    wave_archive.name = NameOf(strings, options, ItemId(kWaveArchiveItem, index));
    if (const auto it = options.find(1); it != options.end())
    {
        wave_archive.wave_count = it->second;
    }

    CheckEntry(WaveArchiveEntry(wave_archive, IndexOf(options), big_endian), entry,
               "the entry of wave archive " + std::to_string(index));

    return wave_archive;
}

// The entry of `group`, with the string index `name` or without one.
std::vector<uint8_t> GroupEntry(const GroupModel& group, std::optional<uint32_t> name, bool big_endian)
{
    Out out(big_endian);
    out.Zeros(4);
    out.U32(0, group.file_id);
    AppendOptions(out, NameOption(name));

    return out.Take();
}

// The model of `entry`, the entry of group `index` in an archive whose names are `strings`. Throws
// xsflib::FormatError for an entry that the model doesn't give back.
GroupModel ReadGroup(std::span<const uint8_t> entry, const StringBlock& strings, uint32_t index, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    GroupModel group;
    group.file_id = r.U32(0);
    const OptionValues options = ReadOptions(r, 4, kKnownNameOptions);
    group.name = NameOf(strings, options, ItemId(kGroupItem, index));

    CheckEntry(GroupEntry(group, IndexOf(options), big_endian), entry, "the entry of group " + std::to_string(index));

    return group;
}

// The entry of `player`, with the string index `name` or without one.
std::vector<uint8_t> PlayerEntry(const PlayerModel& player, std::optional<uint32_t> name, bool big_endian)
{
    Out out(big_endian);
    out.Zeros(4);
    out.U32(0, player.playable_sound_max);
    OptionValues options = NameOption(name);
    options[1] = player.heap_size;
    AppendOptions(out, options);

    return out.Take();
}

// The model of `entry`, the entry of player `index` in an archive whose names are `strings`. Throws
// xsflib::FormatError for an entry that the model doesn't give back.
PlayerModel ReadPlayer(std::span<const uint8_t> entry, const StringBlock& strings, uint32_t index, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    PlayerModel player;
    player.playable_sound_max = r.U32(0);
    const OptionValues options = ReadOptions(r, 4, kKnownPlayerOptions);
    player.name = NameOf(strings, options, ItemId(kPlayerItem, index));
    player.heap_size = Required(options, 1, "heap");

    CheckEntry(PlayerEntry(player, IndexOf(options), big_endian), entry,
               "the entry of player " + std::to_string(index));

    return player;
}

// The archive player's settings as INFO holds them.
std::array<uint8_t, 20> SettingsBytes(const PlayerSettings& settings, bool big_endian)
{
    Out out(big_endian);
    out.Zeros(20);
    const uint16_t counts[] = {settings.sequence_sounds, settings.sequence_tracks, settings.stream_sounds,
                               settings.stream_tracks,   settings.stream_channels, settings.wave_sounds,
                               settings.wave_tracks};
    for (std::size_t i = 0; i < std::size(counts); ++i)
    {
        out.U16(2 * i, counts[i]);
    }
    out.U8(14, settings.stream_buffer_times);

    const std::vector<uint8_t> bytes = out.Take();
    std::array<uint8_t, 20> result{};
    std::ranges::copy(bytes, result.begin());

    return result;
}

// The model of `bytes`, the archive player's settings. Throws xsflib::FormatError for settings that the model doesn't
// give back.
PlayerSettings ReadSettings(const std::array<uint8_t, 20>& bytes, bool big_endian)
{
    const xsflib::Reader r(bytes, big_endian);
    PlayerSettings settings;
    settings.sequence_sounds = r.U16(0);
    settings.sequence_tracks = r.U16(2);
    settings.stream_sounds = r.U16(4);
    settings.stream_tracks = r.U16(6);
    settings.stream_channels = r.U16(8);
    settings.wave_sounds = r.U16(10);
    settings.wave_tracks = r.U16(12);
    settings.stream_buffer_times = r.U8(14);

    CheckEntry(SettingsBytes(settings, big_endian), bytes, "the archive player's settings");

    return settings;
}

// The entry of `file`, whose place starts `offset` bytes after the end of the FILE block's size and takes `size`
// bytes. `group_file` gives whether the file is a group's file.
std::vector<uint8_t> FileEntry(const FileModel& file, uint32_t offset, uint32_t size, bool group_file, bool big_endian)
{
    Out out(big_endian);
    out.Zeros(12);
    if (file.place == FilePlace::kPath)
    {
        const auto* text = reinterpret_cast<const uint8_t*>(file.path.data());
        const std::size_t path = out.Append(std::span(text, file.path.size()));
        out.Zeros(1);
        out.Align(4);
        out.Reference(0, kExternalFile, static_cast<uint32_t>(path));
        return out.Take();
    }

    // The place, or none, then the groups that hold the file.
    const std::size_t location = out.Zeros(12);
    out.Reference(0, kInternalFile, static_cast<uint32_t>(location));
    if (file.place == FilePlace::kGroupsOnly)
    {
        out.U32(location + 4, kNone);
        out.U32(location + 8, kNone);
    }
    else
    {
        out.Reference(location, group_file ? 0 : kDataReference, offset);
        out.U32(location + 8, size);
    }
    const std::size_t groups = out.Zeros(8);
    const std::size_t list = AppendList(out, file.groups);
    out.Reference(groups, kListReference, static_cast<uint32_t>(list - location));

    return out.Take();
}

// Where the copy of file `file` lies in `group`, a group's file of the byte order that `big_endian` gives: its offset
// from the group's start and its size. Nothing for a group's file that holds no copy of the file. Throws
// xsflib::FormatError for bytes that aren't a group's file laid out as the tools lay one out (ntdlib/sar/groups.h).
std::optional<std::pair<std::size_t, std::size_t>> CopyPlace(std::span<const uint8_t> group, uint32_t file,
                                                             bool big_endian)
{
    const xsflib::Reader r(group, big_endian);
    if (!r.Magic(0, "FGRP") || r.U16(kBlockTable) != kGroupInfoBlock ||
        r.U16(kBlockTable + kBlockEntrySize) != kGroupFileBlock)
    {
        throw xsflib::FormatError("bytes that aren't a group's file laid out as the tools lay one out");
    }

    // INFO holds the count of files and a reference to each file's entry: its ID and the place of its copy in FILE.
    const std::size_t info = r.U32(kBlockTable + 4);
    const std::size_t copies = r.U32(kBlockTable + kBlockEntrySize + 4) + kBlockHeaderSize;
    const uint32_t count = r.U32(info + kBlockHeaderSize);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t entry = info + kBlockHeaderSize + r.U32(info + 16 + 8 * std::size_t{i});
        if (r.U32(entry) == file && r.U16(entry + 4) == kDataReference)
        {
            return std::pair<std::size_t, std::size_t>{copies + r.U32(entry + 8), r.U32(entry + 12)};
        }
    }

    return std::nullopt;
}

// The places of the files of `model` in an archive whose FILE block starts at `block`: each file's start from the
// archive's start and its size, and the block's end.
struct FilePlaces
{
    std::vector<std::size_t> starts;
    std::vector<std::size_t> sizes;
    std::size_t end = 0;
};

// The places of the files of `model` in a FILE block at `block`, as the tools lay them out. Throws
// std::invalid_argument for a file of kCopy whose group isn't in the group table, whose group's file the FILE block
// doesn't hold, or whose group's file holds no copy of the file.
FilePlaces PlaceFiles(const ArchiveModel& model, std::size_t block)
{
    const std::size_t alignment = model.flavor == Flavor::kNx ? 64 : 32;
    const std::size_t body = block + kBlockHeaderSize;
    FilePlaces places;
    places.starts.assign(model.files.size(), 0);
    places.sizes.assign(model.files.size(), 0);

    // The files with bytes of their own, in the order of their IDs.
    std::size_t next = body;
    bool any = false;
    for (std::size_t i = 0; i < model.files.size(); ++i)
    {
        if (model.files[i].place == FilePlace::kBytes)
        {
            places.starts[i] = Align(next, alignment);
            places.sizes[i] = model.files[i].bytes.size();
            next = places.starts[i] + places.sizes[i];
            any = true;
        }
    }
    places.end = any ? next : Align(body, alignment);

    // The files in the places of their copies.
    for (std::size_t i = 0; i < model.files.size(); ++i)
    {
        const FileModel& file = model.files[i];
        if (file.place != FilePlace::kCopy)
        {
            continue;
        }

        if (file.copy_group >= model.groups.size())
        {
            throw std::invalid_argument("a copy in a group past the group table");
        }
        const uint32_t holder = model.groups[file.copy_group].file_id;
        if (holder >= model.files.size() || model.files[holder].place != FilePlace::kBytes)
        {
            throw std::invalid_argument("a copy in a group's file that the FILE block doesn't hold");
        }
        std::optional<std::pair<std::size_t, std::size_t>> copy;
        try
        {
            copy = CopyPlace(model.files[holder].bytes, static_cast<uint32_t>(i), model.flavor == Flavor::kCafe);
        }
        catch (const xsflib::FormatError& error)
        {
            throw std::invalid_argument(std::string("a copy in a group's file that can't be read: ") + error.what());
        }
        if (!copy || copy->first + copy->second > places.sizes[holder])
        {
            throw std::invalid_argument("a copy that the group's file doesn't hold");
        }
        places.starts[i] = places.starts[holder] + copy->first;
        places.sizes[i] = copy->second;
    }

    return places;
}

// The files of `info`, the INFO block of `archive`, whose groups are `groups`: their places, their bytes, their paths
// and their lists of groups. Throws xsflib::FormatError for a file's entry of an unknown kind, a file past the
// archive's end, and a file inside another file that no group holds.
std::vector<FileModel> ReadFiles(std::span<const uint8_t> archive, const InfoBlock& info,
                                 const std::vector<GroupModel>& groups, bool big_endian)
{
    // The FILE block, from the header's table of blocks.
    const xsflib::Reader header(archive, big_endian);
    std::optional<std::size_t> block;
    for (std::size_t i = 0; i < header.U16(kBlockCountField); ++i)
    {
        if (header.U16(kBlockTable + kBlockEntrySize * i) == kFileBlock)
        {
            block = header.U32(kBlockTable + kBlockEntrySize * i + 4);
        }
    }
    if (!block)
    {
        throw OtherLayout("an archive without a FILE block");
    }

    // Each file's entry: its path, or its place and its list of groups.
    struct Place
    {
        std::size_t start = 0;
        std::size_t size = 0;
        std::size_t file = 0;
    };
    std::vector<FileModel> files(info.files.size());
    std::vector<Place> places;
    for (std::size_t i = 0; i < info.files.size(); ++i)
    {
        const xsflib::Reader r(info.files[i], big_endian);
        FileModel& file = files[i];
        if (r.U16(0) == kExternalFile)
        {
            file.place = FilePlace::kPath;
            file.path = r.CString(r.U32(4), info.files[i].size());
            continue;
        }
        if (r.U16(0) != kInternalFile)
        {
            throw OtherLayout("a file's entry of an unknown kind");
        }

        const std::size_t location = r.U32(4);
        file.groups = ReadList(r, location + r.U32(location + 16));
        if (r.U32(location + 4) == kNone)
        {
            file.place = FilePlace::kGroupsOnly;
            continue;
        }
        places.push_back({*block + kBlockHeaderSize + r.U32(location + 4), r.U32(location + 8), i});
    }

    // The files with bytes of their own, and the copies in group files inside them.
    std::ranges::stable_sort(places, {}, &Place::start);
    const Place* holder = nullptr;
    for (const Place& place : places)
    {
        if (holder != nullptr && place.start < holder->start + holder->size)
        {
            const auto group = std::ranges::find(groups, holder->file, &GroupModel::file_id);
            if (group == groups.end())
            {
                throw OtherLayout("a file inside another file that no group holds");
            }
            files[place.file].place = FilePlace::kCopy;
            files[place.file].copy_group = static_cast<uint32_t>(group - groups.begin());
            continue;
        }

        if (place.start > archive.size() || place.size > archive.size() - place.start)
        {
            throw OtherLayout("a file past the archive's end");
        }
        const auto start = archive.begin() + static_cast<std::ptrdiff_t>(place.start);
        files[place.file].bytes.assign(start, start + static_cast<std::ptrdiff_t>(place.size));
        holder = &place;
    }

    return files;
}

} // namespace

ArchiveModel ReadArchiveModel(std::span<const uint8_t> archive, Flavor flavor)
{
    if (flavor == Flavor::kCtr)
    {
        throw std::invalid_argument("an archive of the 3DS");
    }

    const bool big_endian = flavor == Flavor::kCafe;
    const Tables tables = TablesOf(archive, flavor);
    if (!tables.strings)
    {
        throw OtherLayout("an archive without names");
    }
    const StringBlock strings = StringBlock::Read(*tables.strings, flavor);
    const InfoBlock info = InfoBlock::Read(tables.info, flavor);

    // The items of each table.
    ArchiveModel model;
    model.flavor = flavor;
    model.version = xsflib::Reader(archive, big_endian).U32(8);
    model.player_settings = ReadSettings(info.player_settings, big_endian);
    for (uint32_t i = 0; i < info.sounds.size(); ++i)
    {
        model.sounds.push_back(ReadSound(info.sounds[i], strings, i, model.version, big_endian));
    }
    for (uint32_t i = 0; i < info.sound_groups.size(); ++i)
    {
        model.sound_groups.push_back(ReadSoundGroup(info.sound_groups[i], strings, i, big_endian));
    }
    for (uint32_t i = 0; i < info.banks.size(); ++i)
    {
        model.banks.push_back(ReadBank(info.banks[i], strings, i, big_endian));
    }
    for (uint32_t i = 0; i < info.wave_archives.size(); ++i)
    {
        model.wave_archives.push_back(ReadWaveArchive(info.wave_archives[i], strings, i, big_endian));
    }
    for (uint32_t i = 0; i < info.groups.size(); ++i)
    {
        model.groups.push_back(ReadGroup(info.groups[i], strings, i, big_endian));
    }
    for (uint32_t i = 0; i < info.players.size(); ++i)
    {
        model.players.push_back(ReadPlayer(info.players[i], strings, i, big_endian));
    }
    model.files = ReadFiles(archive, info, model.groups, big_endian);

    // The archive that the model gives has to be `archive`.
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
        throw OtherLayout("the tools lay its bytes out otherwise from byte " +
                          std::to_string(theirs - archive.begin()));
    }

    return model;
}

std::vector<uint8_t> BuildArchive(const ArchiveModel& model)
{
    if (model.flavor == Flavor::kCtr)
    {
        throw std::invalid_argument("an archive of the 3DS");
    }

    // The names in the order of the tables, and the tables' entries.
    const bool big_endian = model.flavor == Flavor::kCafe;
    StringBlock strings;
    strings.flavor = model.flavor;
    const auto name = [&strings](const std::string& text, uint32_t type, std::size_t index)
    {
        std::optional<uint32_t> string;
        if (!text.empty())
        {
            string = static_cast<uint32_t>(strings.names.size());
            strings.names.push_back({text, ItemId(type, static_cast<uint32_t>(index))});
        }
        return string;
    };
    InfoBlock info;
    info.flavor = model.flavor;
    for (std::size_t i = 0; i < model.sounds.size(); ++i)
    {
        const SoundModel& sound = model.sounds[i];
        info.sounds.push_back(SoundEntry(sound, name(sound.name, kSoundItem, i), model.version, big_endian));
    }
    for (std::size_t i = 0; i < model.sound_groups.size(); ++i)
    {
        const SoundGroupModel& group = model.sound_groups[i];
        info.sound_groups.push_back(SoundGroupEntry(group, name(group.name, kSoundGroupItem, i), big_endian));
    }
    for (std::size_t i = 0; i < model.banks.size(); ++i)
    {
        info.banks.push_back(BankEntry(model.banks[i], name(model.banks[i].name, kBankItem, i), big_endian));
    }
    for (std::size_t i = 0; i < model.wave_archives.size(); ++i)
    {
        const WaveArchiveModel& wave_archive = model.wave_archives[i];
        info.wave_archives.push_back(
            WaveArchiveEntry(wave_archive, name(wave_archive.name, kWaveArchiveItem, i), big_endian));
    }
    for (std::size_t i = 0; i < model.groups.size(); ++i)
    {
        info.groups.push_back(GroupEntry(model.groups[i], name(model.groups[i].name, kGroupItem, i), big_endian));
    }
    for (std::size_t i = 0; i < model.players.size(); ++i)
    {
        info.players.push_back(PlayerEntry(model.players[i], name(model.players[i].name, kPlayerItem, i), big_endian));
    }
    info.player_settings = SettingsBytes(model.player_settings, big_endian);
    const std::vector<uint8_t> names = strings.Write();

    // The FILE block's place follows from the sizes of the blocks before it. A file's entry takes the same size at any
    // place.
    std::vector<bool> group_files(model.files.size());
    for (const GroupModel& group : model.groups)
    {
        if (group.file_id < group_files.size())
        {
            group_files[group.file_id] = true;
        }
    }
    for (std::size_t i = 0; i < model.files.size(); ++i)
    {
        info.files.push_back(FileEntry(model.files[i], 0, 0, group_files[i], big_endian));
    }
    const std::size_t block = kHeaderSize + names.size() + info.Write().size();
    const FilePlaces places = PlaceFiles(model, block);
    if (places.end > kNone)
    {
        throw std::invalid_argument("an archive past 4 GB");
    }
    for (std::size_t i = 0; i < model.files.size(); ++i)
    {
        const FilePlace place = model.files[i].place;
        const bool placed = place == FilePlace::kBytes || place == FilePlace::kCopy;
        const auto offset = static_cast<uint32_t>(placed ? places.starts[i] - block - kBlockHeaderSize : 0);
        const auto size = static_cast<uint32_t>(places.sizes[i]);
        info.files[i] = FileEntry(model.files[i], offset, size, group_files[i], big_endian);
    }
    const std::vector<uint8_t> tables = info.Write();

    // The header and its table of blocks, then STRG, INFO and the FILE block's magic and size.
    Out out(big_endian);
    const uint8_t kMagic[] = {'F', 'S', 'A', 'R'};
    out.Append(kMagic);
    out.Zeros(kHeaderSize - sizeof(kMagic));
    out.U16(4, 0xFEFF);
    out.U16(6, static_cast<uint16_t>(kHeaderSize));
    out.U32(8, model.version);
    out.U32(0x0C, static_cast<uint32_t>(places.end));
    out.U16(kBlockCountField, 3);
    const std::size_t blocks[][3] = {{kStringBlock, out.Append(names), names.size()},
                                     {kInfoBlock, out.Append(tables), tables.size()},
                                     {kFileBlock, block, places.end - block}};
    for (std::size_t i = 0; i < std::size(blocks); ++i)
    {
        out.U16(kBlockTable + kBlockEntrySize * i, static_cast<uint16_t>(blocks[i][0]));
        out.U32(kBlockTable + kBlockEntrySize * i + 4, static_cast<uint32_t>(blocks[i][1]));
        out.U32(kBlockTable + kBlockEntrySize * i + 8, static_cast<uint32_t>(blocks[i][2]));
    }
    const uint8_t kFileMagic[] = {'F', 'I', 'L', 'E'};
    out.Append(kFileMagic);
    out.U32(out.Zeros(4), static_cast<uint32_t>(places.end - block));

    // The files' bytes, with zeros between them.
    std::vector<uint8_t> archive = out.Take();
    archive.resize(places.end);
    for (std::size_t i = 0; i < model.files.size(); ++i)
    {
        if (model.files[i].place == FilePlace::kBytes)
        {
            std::ranges::copy(model.files[i].bytes, archive.begin() + static_cast<std::ptrdiff_t>(places.starts[i]));
        }
    }

    return archive;
}

} // namespace ntdlib::sar
