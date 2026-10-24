// SPDX-License-Identifier: MIT

// Tests of the model of a sound archive of the 3DS, the Wii U or the Switch and of its builder
// (ntdlib/sar/archive_model.h), in each console's byte order. The tests build archives from models made from nothing.
// The bytes that they expect follow from the layout that the header describes, worked out by hand. Each archive has to
// read back as its model, and ntdlib's reader has to read the model's values in it. An archive of
// ntdlib/testing/sar_tools.h has to read as a model and come back byte for byte.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;
using sar::ArchiveModel;
using sar::FilePlace;
using sar::Flavor;
using sar::InfoBlock;
using sar::ItemId;
using sar::SoundModel;
using sar::StreamFields;

constexpr Flavor kFlavors[] = {Flavor::kCafe, Flavor::kNx};
constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr uint32_t kAll = 0xFFFFFFFF; // the bits of every part of an item that a group loads

// The bytes of file 3: the copy in group 0's file.
const std::vector<uint8_t> kBankCopy(20, 0xF3);

// The files of EveryItem whose bytes the FILE block holds. File 3's copy lies inside file 8.
constexpr uint32_t kPlaced[] = {0, 1, 2, 4, 6, 8};

std::size_t AlignmentOf(Flavor flavor)
{
    return flavor == Flavor::kNx ? 64 : 32;
}

std::size_t Align(std::size_t at, std::size_t alignment)
{
    return (at + alignment - 1) / alignment * alignment;
}

uint16_t Load16(std::span<const uint8_t> bytes, std::size_t at, Flavor flavor)
{
    return xsflib::Reader(bytes, flavor == Flavor::kCafe).U16(at);
}

uint32_t Load32(std::span<const uint8_t> bytes, std::size_t at, Flavor flavor)
{
    return xsflib::Reader(bytes, flavor == Flavor::kCafe).U32(at);
}

sar::SoundArchive Load(const std::vector<uint8_t>& archive, Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return sar::SoundArchive::Load(archive, options);
}

InfoBlock InfoOf(std::span<const uint8_t> archive, Flavor flavor)
{
    return InfoBlock::Read(sar::TablesOf(archive, flavor).info, flavor);
}

template <class Error, class Call>
bool Throws(Call call)
{
    return !ErrorOf<Error>(call).empty();
}

// The version of the tests' archives: CSAR 2.0.0.0 on the 3DS, FSAR 2.2.0 on the Wii U, and 2.5.0 on the Switch.
uint32_t VersionOf(Flavor flavor)
{
    if (flavor == Flavor::kCtr)
    {
        return 0x02000000;
    }
    return flavor == Flavor::kNx ? 0x00020500 : 0x00020200;
}

// `size` zero bytes in the byte order of `flavor`, for an entry written by hand.
sar_files::Bytes Entry(Flavor flavor, std::size_t size)
{
    sar_files::Bytes entry(flavor == Flavor::kCafe);
    entry.Add(size);

    return entry;
}

// A sequence sound with a value in each field, and with every option that a sound's entry can hold.
SoundModel SequenceSound()
{
    SoundModel sound;
    sound.name = "SEQ_A";
    sound.volume = 100;
    sound.remote_filter = 5;
    sound.pan_mode = 1;
    sound.pan_curve = 2;
    sound.player_priority = 70;
    sound.actor_player_id = 1;
    sound.single_play = 0x00010005;
    sound.settings_3d = {.flags = 3, .decay_ratio = 0.25f, .decay_curve = 2, .doppler_factor = 3};
    sound.option_17 = 9;
    sound.user_options = {0x1234, std::nullopt, std::nullopt, 0x5678};
    sound.after_3d = 7;
    sound.banks = {ItemId(sar::kBankItem, 0), sar::kNoItem};
    sound.allocate_track_flags = 3;
    sound.start_offset = 12;
    sound.channel_priority = 80;
    sound.release_priority_fix = true;

    return sound;
}

// The entry of SequenceSound with string index 0. The options run from 20 to 56: the string index, the pan, the
// priorities, the single play, the offset of the 3D settings, option 17, and options 28 and 31. The 3D settings take 16
// bytes at 56, and the value after them follows at 72. The details start at 76 with the reference to the list of banks
// and the tracks. Their options follow at 88, and the list at 100.
std::vector<uint8_t> SequenceSoundEntry(Flavor flavor)
{
    sar_files::Bytes entry = Entry(flavor, 112);
    entry.U32(4, ItemId(sar::kPlayerItem, 0));
    entry.U8(8, 100);
    entry.U8(9, 5);
    entry.U16(12, 0x2203);
    entry.U32(16, 76);
    entry.U32(20, 0x9002010F);
    entry.U32(28, 0x0201);
    entry.U32(32, 0x0146);
    entry.U32(36, 0x00010005);
    entry.U32(40, 56);
    entry.U32(44, 9);
    entry.U32(48, 0x1234);
    entry.U32(52, 0x5678);
    entry.U32(56, 3);
    entry.U32(60, 0x3E800000); // 0.25
    entry.U8(64, 2);
    entry.U8(65, 3);
    entry.U32(72, 7);
    entry.U16(76, 0x0100);
    entry.U32(80, 24);
    entry.U32(84, 3);
    entry.U32(88, 3);
    entry.U32(92, 12);
    entry.U32(96, 0x0150);
    entry.U32(100, 2);
    entry.U32(104, ItemId(sar::kBankItem, 0));
    entry.U32(108, kNone);

    return entry.data;
}

// A file's entry: a reference to the location 12 bytes on, no options, then the location and a reference to the list
// of the groups that hold the file. The location holds a reference of `type` with the offset after the FILE block's
// size, and the size. The 3DS's entries end with the location.
std::vector<uint8_t> FileEntry(Flavor flavor, uint16_t type, uint32_t offset, uint32_t size,
                               const std::vector<uint32_t>& groups = {})
{
    const bool ctr = flavor == Flavor::kCtr;
    sar_files::Bytes entry = Entry(flavor, ctr ? 24 : 36 + 4 * groups.size());
    entry.U16(0, 0x220C);
    entry.U32(4, 12);
    entry.U16(12, type);
    entry.U32(16, offset);
    entry.U32(20, size);
    if (ctr)
    {
        return entry.data;
    }
    entry.U16(24, 0x0100);
    entry.U32(28, 20);
    entry.U32(32, static_cast<uint32_t>(groups.size()));
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        entry.U32(36 + 4 * i, groups[i]);
    }

    return entry.data;
}

// The archive player's settings in the test of the layout, as INFO holds them: seven 16-bit counts, then a byte.
std::array<uint8_t, 20> SettingsBytes(Flavor flavor)
{
    sar_files::Bytes bytes = Entry(flavor, 20);
    const uint16_t kCounts[] = {10, 20, 3, 6, 12, 30, 40};
    for (std::size_t i = 0; i < std::size(kCounts); ++i)
    {
        bytes.U16(2 * i, kCounts[i]);
    }
    bytes.U8(14, 2);

    std::array<uint8_t, 20> settings{};
    std::ranges::copy(bytes.data, settings.begin());

    return settings;
}

// The header of an archive of `version` that ends at `end`, with the STRG block of `strings` bytes at 64, INFO after
// it, and the FILE block at `block`.
std::vector<uint8_t> Header(Flavor flavor, uint32_t version, std::size_t end, std::size_t strings, std::size_t info,
                            std::size_t block)
{
    sar_files::Bytes header = Entry(flavor, 64);
    std::memcpy(header.data.data(), flavor == Flavor::kCtr ? "CSAR" : "FSAR", 4);
    header.U16(4, 0xFEFF);
    header.U16(6, 64);
    header.U32(8, version);
    header.U32(0x0C, static_cast<uint32_t>(end));
    header.U16(0x10, 3);
    header.U16(0x14, 0x2000);
    header.U32(0x18, 64);
    header.U32(0x1C, static_cast<uint32_t>(strings));
    header.U16(0x20, 0x2001);
    header.U32(0x24, static_cast<uint32_t>(64 + strings));
    header.U32(0x28, static_cast<uint32_t>(info));
    header.U16(0x2C, 0x2002);
    header.U32(0x30, static_cast<uint32_t>(block));
    header.U32(0x34, static_cast<uint32_t>(end - block));

    return header.data;
}

// Checks that `model` builds `expected`, and that `expected` reads as `model`.
void CheckBoth(const ArchiveModel& model, const std::vector<uint8_t>& expected)
{
    XSFLIB_CHECK(sar::BuildArchive(model) == expected);
    XSFLIB_CHECK(sar::ReadArchiveModel(expected, model.flavor) == model);
}

// An archive without items or files. Its FILE block ends at the first multiple of the alignment after the block's
// size.
void TestEmpty(Flavor flavor)
{
    ArchiveModel model;
    model.flavor = flavor;
    model.version = VersionOf(flavor);

    sar::StringBlock strings;
    strings.flavor = flavor;
    const std::vector<uint8_t> names = strings.Write();
    InfoBlock info;
    info.flavor = flavor;
    sar_files::Bytes settings = Entry(flavor, 20);
    const uint16_t kCounts[] = {64, 64, 4, 4, 8, 64, 64};
    for (std::size_t i = 0; i < std::size(kCounts); ++i)
    {
        settings.U16(2 * i, kCounts[i]);
    }
    settings.U8(14, 1);
    std::ranges::copy(settings.data, info.player_settings.begin());
    const std::vector<uint8_t> tables = info.Write();
    XSFLIB_CHECK(tables.size() == 128);

    const std::size_t block = 64 + names.size() + tables.size();
    const std::size_t end = Align(block + 8, AlignmentOf(flavor));
    std::vector<uint8_t> expected = Header(flavor, model.version, end, names.size(), tables.size(), block);
    expected.insert(expected.end(), names.begin(), names.end());
    expected.insert(expected.end(), tables.begin(), tables.end());
    expected.resize(end);
    std::memcpy(expected.data() + block, "FILE", 4);
    sar_files::Bytes size = Entry(flavor, 4);
    size.U32(0, static_cast<uint32_t>(end - block));
    std::ranges::copy(size.data, expected.begin() + static_cast<std::ptrdiff_t>(block + 4));

    CheckBoth(model, expected);
}

// An archive with a sequence sound, a bank and two files, laid out by hand. The header takes 64 bytes. STRG follows
// it, then INFO and the FILE block. The first file starts at the first multiple of the alignment after the FILE
// block's size. The second one starts at the first multiple after the first one's end, and the archive ends with it.
void TestLayout(Flavor flavor)
{
    ArchiveModel model;
    model.flavor = flavor;
    model.version = VersionOf(flavor);
    model.player_settings = {.sequence_sounds = 10,
                             .sequence_tracks = 20,
                             .stream_sounds = 3,
                             .stream_tracks = 6,
                             .stream_channels = 12,
                             .wave_sounds = 30,
                             .wave_tracks = 40,
                             .stream_buffer_times = 2};
    model.sounds = {SequenceSound()};
    model.banks = {{.name = "BANK_A", .file_id = 1}};
    model.files = {{.bytes = std::vector<uint8_t>(40, 0xF0)}, {.bytes = std::vector<uint8_t>(8, 0xF1)}};

    // STRG names the items in the order of the tables. The bank's entry holds its file and a reference to its empty
    // list of wave archives at 20, after its string index.
    sar::StringBlock strings;
    strings.flavor = flavor;
    strings.names = {{"SEQ_A", ItemId(sar::kSoundItem, 0)}, {"BANK_A", ItemId(sar::kBankItem, 0)}};
    const std::vector<uint8_t> names = strings.Write();
    sar_files::Bytes bank = Entry(flavor, 24);
    bank.U32(0, 1);
    bank.U16(4, 0x0100);
    bank.U32(8, 20);
    bank.U32(12, 1);
    bank.U32(16, 1);
    InfoBlock info;
    info.flavor = flavor;
    info.sounds = {SequenceSoundEntry(flavor)};
    info.banks = {bank.data};
    info.files = {FileEntry(flavor, 0x1F00, 0, 40), FileEntry(flavor, 0x1F00, 0, 8)};
    info.player_settings = SettingsBytes(flavor);

    // The files' places follow from the blocks' sizes. A file's entry takes the same size at any place.
    const std::size_t block = 64 + names.size() + info.Write().size();
    const std::size_t start0 = Align(block + 8, AlignmentOf(flavor));
    const std::size_t start1 = Align(start0 + 40, AlignmentOf(flavor));
    const std::size_t end = start1 + 8;
    info.files = {FileEntry(flavor, 0x1F00, static_cast<uint32_t>(start0 - block - 8), 40),
                  FileEntry(flavor, 0x1F00, static_cast<uint32_t>(start1 - block - 8), 8)};
    const std::vector<uint8_t> tables = info.Write();
    XSFLIB_CHECK(names.size() % 32 == 0 && tables.size() % 32 == 0);

    std::vector<uint8_t> expected = Header(flavor, model.version, end, names.size(), tables.size(), block);
    expected.insert(expected.end(), names.begin(), names.end());
    expected.insert(expected.end(), tables.begin(), tables.end());
    XSFLIB_CHECK(expected.size() == block);
    expected.resize(end);
    std::memcpy(expected.data() + block, "FILE", 4);
    sar_files::Bytes size = Entry(flavor, 4);
    size.U32(0, static_cast<uint32_t>(end - block));
    std::ranges::copy(size.data, expected.begin() + static_cast<std::ptrdiff_t>(block + 4));
    std::fill_n(expected.begin() + static_cast<std::ptrdiff_t>(start0), 40, 0xF0);
    std::fill_n(expected.begin() + static_cast<std::ptrdiff_t>(start1), 8, 0xF1);

    CheckBoth(model, expected);
}

// The details of stream sound STRM_D: a track that plays both of the stream's channels, with a value in each field.
sar::StreamSoundInfo StreamDetails()
{
    sar::StreamTrackInfo track;
    track.channels = {0, 1};
    track.volume = 100;
    track.pan = 32;
    track.span = 5;
    track.flags = 1;
    track.main_send = 110;
    track.aux_sends = {1, 2, 3};
    track.low_pass = 60;
    track.biquad_type = 2;
    track.biquad_value = 50;

    sar::StreamSoundInfo stream;
    stream.valid_tracks = 1;
    stream.channel_count = 2;
    stream.pitch = 1.5f;
    stream.main_send = 120;
    stream.aux_sends = {10, 20, 30};
    stream.tracks = {track};

    return stream;
}

// The details of a stream of one channel and one track, with the values that a stream sound holds by default.
sar::StreamSoundInfo MonoStream()
{
    sar::StreamSoundInfo stream;
    stream.valid_tracks = 1;
    stream.channel_count = 1;
    stream.tracks.resize(1);
    stream.tracks[0].channels = {0};

    return stream;
}

// Group 0's file: a copy of file 3, BANK_B's file, and a link to file 2, BANK_A's file. It lists both banks. Its INFO
// block starts at 64 and its FILE block at 128. The copy starts at the first multiple of the alignment after the FILE
// block's size: 192 on the Switch, and 160 on the Wii U.
sar_tools::GroupSpec GroupA()
{
    return {.files = {{3, kBankCopy}, {2, std::nullopt}},
            .items = {{ItemId(sar::kBankItem, 1), kAll}, {ItemId(sar::kBankItem, 0), kAll}}};
}

std::size_t CopyOffset(Flavor flavor)
{
    return flavor == Flavor::kNx ? 192 : 160;
}

// A model with items of each kind, and with files of each place. Sound 1, sound group 1, wave archive 1 and player 1
// have no names. Sound 3 plays a stream beside the archive, and names a prefetch file and an extension. Sound 4 holds
// a reference to no extension, and sound 5 neither field. Sound group 0 is a wave sound set. Group 0's file holds a
// copy of file 3, which gives file 3's place, and links to file 2. Group 1 lies beside the archive, and so does file
// 7, wave archive 1's file.
ArchiveModel EveryItem(Flavor flavor)
{
    SoundModel plain;
    SoundModel wave;
    wave.name = "WSD_C";
    wave.type = sar::SoundType::kWaveSound;
    wave.file_id = 1;
    wave.volume = 90;
    wave.wave_sound_index = 1;
    wave.channel_priority = 70;
    SoundModel stream;
    stream.name = "STRM_D";
    stream.type = sar::SoundType::kStream;
    stream.file_id = 5;
    stream.stream = StreamDetails();
    stream.stream_extension = sar::StreamExtension{.stream_type = 1, .loop_start = 100, .loop_end = 2000};
    stream.prefetch_file_id = 6;
    SoundModel reference = stream;
    reference.name = "STRM_E";
    reference.stream = MonoStream();
    reference.stream_fields = StreamFields::kExtension;
    reference.stream_extension.reset();
    reference.prefetch_file_id = sar::kNoItem;
    SoundModel bare = reference;
    bare.name = "STRM_F";
    bare.stream_fields = StreamFields::kNone;

    ArchiveModel model;
    model.flavor = flavor;
    model.version = VersionOf(flavor);
    model.sounds = {SequenceSound(), plain, wave, stream, reference, bare};
    model.sound_groups = {{.name = "WSDSET_A",
                           .first_sound = ItemId(sar::kSoundItem, 2),
                           .last_sound = ItemId(sar::kSoundItem, 2),
                           .file_ids = {1},
                           .wave_archives = std::vector<uint32_t>{ItemId(sar::kWaveArchiveItem, 0)}},
                          {}};
    model.banks = {{.name = "BANK_A", .file_id = 2},
                   {.name = "BANK_B", .file_id = 3, .wave_archives = {ItemId(sar::kWaveArchiveItem, 0)}}};
    model.wave_archives = {{.name = "WARC_A", .file_id = 4, .load_individually = true, .wave_count = 3},
                           {.file_id = 7}};
    model.groups = {{.name = "GROUP_A", .file_id = 8}, {.name = "GROUP_B", .file_id = sar::kExternalGroupFile}};
    model.players = {{.name = "PLAYER_A", .playable_sound_max = 2, .heap_size = 0x1000}, {}};
    const uint32_t group_a = ItemId(sar::kGroupItem, 0);
    model.files = {{.bytes = std::vector<uint8_t>(40, 0xF0)},
                   {.bytes = std::vector<uint8_t>(24, 0xF1)},
                   {.bytes = std::vector<uint8_t>(16, 0xF2), .groups = {group_a}},
                   {.place = FilePlace::kCopy, .copy_group = 0, .groups = {group_a}},
                   {.bytes = std::vector<uint8_t>(64, 0xF4)},
                   {.place = FilePlace::kPath, .path = "stream/STRM_D.bfstm"},
                   {.bytes = std::vector<uint8_t>(12, 0xF6)},
                   {.place = FilePlace::kGroupsOnly, .groups = {ItemId(sar::kGroupItem, 1)}},
                   {.bytes = sar_tools::GroupInArchive(flavor, GroupA())}};

    return model;
}

// The entries of EveryItem, worked out by hand. The string indices count up in the order of the tables, and an item
// without a name holds none.
void TestEntries(Flavor flavor)
{
    const ArchiveModel model = EveryItem(flavor);
    const std::vector<uint8_t> archive = sar::BuildArchive(model);
    XSFLIB_CHECK(sar::ReadArchiveModel(archive, flavor) == model);
    const InfoBlock info = InfoOf(archive, flavor);
    XSFLIB_CHECK(info.sounds.size() == 6 && info.sound_groups.size() == 2 && info.banks.size() == 2 &&
                 info.wave_archives.size() == 2 && info.groups.size() == 2 && info.players.size() == 2 &&
                 info.files.size() == 9);
    if (info.sounds.size() != 6 || info.sound_groups.size() != 2 || info.banks.size() != 2 ||
        info.wave_archives.size() != 2 || info.groups.size() != 2 || info.players.size() != 2 || info.files.size() != 9)
    {
        return;
    }

    // The names.
    const sar::StringBlock strings = sar::StringBlock::Read(*sar::TablesOf(archive, flavor).strings, flavor);
    const std::vector<sar::ItemName> names = {
        {"SEQ_A", ItemId(sar::kSoundItem, 0)},        {"WSD_C", ItemId(sar::kSoundItem, 2)},
        {"STRM_D", ItemId(sar::kSoundItem, 3)},       {"STRM_E", ItemId(sar::kSoundItem, 4)},
        {"STRM_F", ItemId(sar::kSoundItem, 5)},       {"WSDSET_A", ItemId(sar::kSoundGroupItem, 0)},
        {"BANK_A", ItemId(sar::kBankItem, 0)},        {"BANK_B", ItemId(sar::kBankItem, 1)},
        {"WARC_A", ItemId(sar::kWaveArchiveItem, 0)}, {"GROUP_A", ItemId(sar::kGroupItem, 0)},
        {"GROUP_B", ItemId(sar::kGroupItem, 1)},      {"PLAYER_A", ItemId(sar::kPlayerItem, 0)}};
    XSFLIB_CHECK(strings.names == names);
    XSFLIB_CHECK(info.sounds[0] == SequenceSoundEntry(flavor));

    // Sound 1, without a name: the pan, the priorities and the offset of the 3D settings from 20, the 3D settings at
    // 36, the value after them at 52, and the details from 56. The list of banks at 80 is empty.
    sar_files::Bytes plain = Entry(flavor, 84);
    plain.U32(4, ItemId(sar::kPlayerItem, 0));
    plain.U8(8, 127);
    plain.U16(12, 0x2203);
    plain.U32(16, 56);
    plain.U32(20, 0x106);
    plain.U32(28, 64);
    plain.U32(32, 36);
    plain.U32(36, 0xF);
    plain.U32(40, 0x3F000000); // 0.5
    plain.U8(44, 1);
    plain.U16(56, 0x0100);
    plain.U32(60, 24);
    plain.U32(64, 1);
    plain.U32(68, 3);
    plain.U32(76, 64);
    XSFLIB_CHECK(info.sounds[1] == plain.data);

    // Sound 2, a wave sound: its details at 60 hold its index, a 1 and its channel priority.
    sar_files::Bytes wave = Entry(flavor, 76);
    wave.U32(0, 1);
    wave.U32(4, ItemId(sar::kPlayerItem, 0));
    wave.U8(8, 90);
    wave.U16(12, 0x2202);
    wave.U32(16, 60);
    wave.U32(20, 0x107);
    wave.U32(24, 1);
    wave.U32(32, 64);
    wave.U32(36, 40);
    wave.U32(40, 0xF);
    wave.U32(44, 0x3F000000);
    wave.U8(48, 1);
    wave.U32(60, 1);
    wave.U32(64, 1);
    wave.U32(68, 1);
    wave.U32(72, 70);
    XSFLIB_CHECK(info.sounds[2] == wave.data);

    // Sound 3, a stream sound: no value after the 3D settings, and the details from 56. The table of tracks follows 36
    // bytes of fields, at 92. The track's info follows it at 104, the indices of its channels at 128 and its sends at
    // 136. The sound's sends follow at 144, and the extension at 152.
    sar_files::Bytes stream = Entry(flavor, 164);
    stream.U32(0, 5);
    stream.U32(4, ItemId(sar::kPlayerItem, 0));
    stream.U8(8, 127);
    stream.U16(12, 0x2201);
    stream.U32(16, 56);
    stream.U32(20, 0x107);
    stream.U32(24, 2);
    stream.U32(32, 64);
    stream.U32(36, 40);
    stream.U32(40, 0xF);
    stream.U32(44, 0x3F000000);
    stream.U8(48, 1);
    stream.U16(56, 1);
    stream.U16(58, 2);
    stream.U16(60, 0x0101);
    stream.U32(64, 36);
    stream.U32(68, 0x3FC00000); // 1.5
    stream.U16(72, 0x220F);
    stream.U32(76, 88);
    stream.U16(80, 0x2210);
    stream.U32(84, 96);
    stream.U32(88, 6);
    stream.U32(92, 1);
    stream.U16(96, 0x220E);
    stream.U32(100, 12);
    const uint8_t kTrack[] = {100, 32, 5, 1};
    std::ranges::copy(kTrack, stream.data.begin() + 104);
    stream.U16(108, 0x0100);
    stream.U32(112, 24);
    stream.U16(116, 0x220F);
    stream.U32(120, 32);
    const uint8_t kFilters[] = {60, 2, 50};
    std::ranges::copy(kFilters, stream.data.begin() + 124);
    stream.U32(128, 2);
    stream.U8(133, 1);
    const uint8_t kSends[] = {110, 1, 2, 3, 0, 0, 0, 0, 120, 10, 20, 30};
    std::ranges::copy(kSends, stream.data.begin() + 136);
    stream.U32(152, 1);
    stream.U32(156, 100);
    stream.U32(160, 2000);
    XSFLIB_CHECK(info.sounds[3] == stream.data);

    // Sound 4: the table follows 32 bytes of fields. The reference after the sends leads to nothing.
    const std::vector<uint8_t>& reference = info.sounds[4];
    XSFLIB_CHECK(reference.size() == 148 && Load32(reference, 64, flavor) == 32 && Load16(reference, 80, flavor) == 0 &&
                 Load32(reference, 84, flavor) == kNone && Load32(reference, 88, flavor) == 1);

    // Sound 5: the table follows 24 bytes of fields, at 80. The track's info follows at 92, the index of its channel at
    // 116 and its sends at 124. The sound's sends follow at 132.
    sar_files::Bytes bare = Entry(flavor, 140);
    bare.U32(0, 5);
    bare.U32(4, ItemId(sar::kPlayerItem, 0));
    bare.U8(8, 127);
    bare.U16(12, 0x2201);
    bare.U32(16, 56);
    bare.U32(20, 0x107);
    bare.U32(24, 4);
    bare.U32(32, 64);
    bare.U32(36, 40);
    bare.U32(40, 0xF);
    bare.U32(44, 0x3F000000);
    bare.U8(48, 1);
    bare.U16(56, 1);
    bare.U16(58, 1);
    bare.U16(60, 0x0101);
    bare.U32(64, 24);
    bare.U32(68, 0x3F800000); // 1
    bare.U16(72, 0x220F);
    bare.U32(76, 76);
    bare.U32(80, 1);
    bare.U16(84, 0x220E);
    bare.U32(88, 12);
    bare.U8(92, 127);
    bare.U8(93, 64);
    bare.U16(96, 0x0100);
    bare.U32(100, 24);
    bare.U16(104, 0x220F);
    bare.U32(108, 32);
    bare.U8(112, 64);
    bare.U32(116, 1);
    bare.U8(124, 127);
    bare.U8(132, 127);
    XSFLIB_CHECK(info.sounds[5] == bare.data);

    // The sound groups. A wave sound set's details at 40 hold a reference to the list of its wave archives, and a
    // zero. A sound group without them holds a reference to nothing.
    sar_files::Bytes set = Entry(flavor, 60);
    set.U32(0, ItemId(sar::kSoundItem, 2));
    set.U32(4, ItemId(sar::kSoundItem, 2));
    set.U16(8, 0x0100);
    set.U32(12, 32);
    set.U16(16, 0x2205);
    set.U32(20, 40);
    set.U32(24, 1);
    set.U32(28, 5);
    set.U32(32, 1);
    set.U32(36, 1);
    set.U16(40, 0x0100);
    set.U32(44, 12);
    set.U32(52, 1);
    set.U32(56, ItemId(sar::kWaveArchiveItem, 0));
    XSFLIB_CHECK(info.sound_groups[0] == set.data);
    sar_files::Bytes empty = Entry(flavor, 32);
    empty.U32(0, kNone);
    empty.U32(4, kNone);
    empty.U16(8, 0x0100);
    empty.U32(12, 28);
    empty.U32(20, kNone);
    XSFLIB_CHECK(info.sound_groups[1] == empty.data);

    // A bank's list of wave archives, a wave archive's choice and count of waves, a group's file and a player's heap.
    sar_files::Bytes bank = Entry(flavor, 28);
    bank.U32(0, 3);
    bank.U16(4, 0x0100);
    bank.U32(8, 20);
    bank.U32(12, 1);
    bank.U32(16, 7);
    bank.U32(20, 1);
    bank.U32(24, ItemId(sar::kWaveArchiveItem, 0));
    XSFLIB_CHECK(info.banks[1] == bank.data);
    sar_files::Bytes wave_archive = Entry(flavor, 20);
    wave_archive.U32(0, 4);
    wave_archive.U8(4, 1);
    wave_archive.U32(8, 3);
    wave_archive.U32(12, 8);
    wave_archive.U32(16, 3);
    XSFLIB_CHECK(info.wave_archives[0] == wave_archive.data);
    sar_files::Bytes unnamed = Entry(flavor, 12);
    unnamed.U32(0, 7);
    XSFLIB_CHECK(info.wave_archives[1] == unnamed.data);
    sar_files::Bytes group = Entry(flavor, 12);
    group.U32(0, kNone);
    group.U32(4, 1);
    group.U32(8, 10);
    XSFLIB_CHECK(info.groups[1] == group.data);
    sar_files::Bytes player = Entry(flavor, 16);
    player.U32(0, 2);
    player.U32(4, 3);
    player.U32(8, 11);
    player.U32(12, 0x1000);
    XSFLIB_CHECK(info.players[0] == player.data);
    sar_files::Bytes other = Entry(flavor, 12);
    other.U32(0, 1);
    other.U32(4, 2);
    XSFLIB_CHECK(info.players[1] == other.data);

    // The files with bytes of their own follow one another in the order of their IDs: files 0, 1, 2, 4, 6 and 8. File
    // 3's place is its copy in file 8. The reference of group 0's file, file 8, has type 0.
    const std::size_t block = Load32(archive, 0x30, flavor);
    const std::size_t alignment = AlignmentOf(flavor);
    std::vector<std::size_t> starts(9);
    starts[0] = Align(block + 8, alignment);
    starts[1] = Align(starts[0] + 40, alignment);
    starts[2] = Align(starts[1] + 24, alignment);
    starts[4] = Align(starts[2] + 16, alignment);
    starts[6] = Align(starts[4] + 64, alignment);
    starts[8] = Align(starts[6] + 12, alignment);
    starts[3] = starts[8] + CopyOffset(flavor);
    const std::size_t group_size = model.files[8].bytes.size();
    XSFLIB_CHECK(archive.size() == starts[8] + group_size && Load32(archive, 0x0C, flavor) == archive.size());
    const auto offset = [&](std::size_t file)
    {
        return static_cast<uint32_t>(starts[file] - block - 8);
    };
    const uint32_t group_a = ItemId(sar::kGroupItem, 0);
    XSFLIB_CHECK(info.files[0] == FileEntry(flavor, 0x1F00, offset(0), 40));
    XSFLIB_CHECK(info.files[1] == FileEntry(flavor, 0x1F00, offset(1), 24));
    XSFLIB_CHECK(info.files[2] == FileEntry(flavor, 0x1F00, offset(2), 16, {group_a}));
    XSFLIB_CHECK(info.files[3] == FileEntry(flavor, 0x1F00, offset(3), 20, {group_a}));
    XSFLIB_CHECK(info.files[4] == FileEntry(flavor, 0x1F00, offset(4), 64));
    XSFLIB_CHECK(info.files[6] == FileEntry(flavor, 0x1F00, offset(6), 12));
    XSFLIB_CHECK(info.files[7] == FileEntry(flavor, 0, kNone, kNone, {ItemId(sar::kGroupItem, 1)}));
    XSFLIB_CHECK(info.files[8] == FileEntry(flavor, 0, offset(8), static_cast<uint32_t>(group_size)));

    // File 5's entry holds its path after the reference, with a zero byte, up to a multiple of 4 bytes.
    sar_files::Bytes path = Entry(flavor, 32);
    path.U16(0, 0x220D);
    path.U32(4, 12);
    const std::string kText = "stream/STRM_D.bfstm";
    std::ranges::copy(kText, path.data.begin() + 12);
    XSFLIB_CHECK(info.files[5] == path.data);

    // The bytes of the files, with zeros between them.
    std::vector<uint8_t> files(archive.size() - (block + 8));
    for (const uint32_t file : kPlaced)
    {
        std::ranges::copy(model.files[file].bytes,
                          files.begin() + static_cast<std::ptrdiff_t>(starts[file] - block - 8));
    }
    XSFLIB_CHECK(std::ranges::equal(std::span(archive).subspan(block + 8), files));
    XSFLIB_CHECK(std::ranges::equal(std::span(archive).subspan(starts[3], kBankCopy.size()), kBankCopy));
}

// ntdlib's reader reads the values of EveryItem from its archive.
void TestRead(Flavor flavor)
{
    const ArchiveModel model = EveryItem(flavor);
    const sar::SoundArchive read = Load(sar::BuildArchive(model), flavor);
    XSFLIB_CHECK(read.Version() == model.version);

    const std::vector<sar::SoundInfo>& sounds = read.Sounds();
    XSFLIB_CHECK(sounds.size() == 6);
    if (sounds.size() == 6)
    {
        const sar::SoundInfo& a = sounds[0];
        XSFLIB_CHECK(a.name == "SEQ_A" && a.type == sar::SoundType::kSequence && a.file_id == 0 && a.volume == 100);
        XSFLIB_CHECK(a.pan_mode == 1 && a.pan_curve == 2 && a.player_id == ItemId(sar::kPlayerItem, 0) &&
                     a.player_priority == 70 && a.actor_player_id == 1);
        XSFLIB_CHECK(a.banks == model.sounds[0].banks && a.allocate_track_flags == 3 && a.start_offset == 12 &&
                     a.channel_priority == 80 && a.release_priority_fix);
        const sar::SoundInfo& plain = sounds[1];
        XSFLIB_CHECK(plain.type == sar::SoundType::kSequence && plain.volume == 127 && plain.banks.empty() &&
                     plain.allocate_track_flags == 1 && plain.channel_priority == 64 && !plain.release_priority_fix);
        const sar::SoundInfo& wave = sounds[2];
        XSFLIB_CHECK(wave.name == "WSD_C" && wave.type == sar::SoundType::kWaveSound && wave.file_id == 1 &&
                     wave.volume == 90 && wave.wave_sound_index == 1 && wave.channel_priority == 70);
        XSFLIB_CHECK(sounds[3].name == "STRM_D" && sounds[3].type == sar::SoundType::kStream &&
                     sounds[3].file_id == 5 && sounds[3].stream == StreamDetails());
        XSFLIB_CHECK(sounds[4].name == "STRM_E" && sounds[4].stream == MonoStream());
        XSFLIB_CHECK(sounds[5].name == "STRM_F" && sounds[5].stream == MonoStream());
    }

    // The other tables.
    const std::vector<sar::SoundGroupInfo>& sets = read.SoundGroups();
    XSFLIB_CHECK(sets.size() == 2 && sets[0].name == "WSDSET_A" && sets[0].first_sound == 2 &&
                 sets[0].last_sound == 2 && sets[0].file_ids == std::vector<uint32_t>({1}));
    XSFLIB_CHECK(sets.size() == 2 && sets[1].name.empty() && sets[1].file_ids.empty());
    XSFLIB_CHECK(read.Banks().size() == 2 && read.Banks()[0].name == "BANK_A" && read.Banks()[0].file_id == 2 &&
                 read.Banks()[1].name == "BANK_B" && read.Banks()[1].file_id == 3);
    XSFLIB_CHECK(read.WaveArchives().size() == 2 && read.WaveArchives()[0].name == "WARC_A" &&
                 read.WaveArchives()[0].file_id == 4 && read.WaveArchives()[1].file_id == 7);
    XSFLIB_CHECK(read.Groups().size() == 2 && read.Groups()[0].name == "GROUP_A" && read.Groups()[0].file_id == 8 &&
                 read.Groups()[1].name == "GROUP_B" && read.Groups()[1].file_id == sar::kExternalGroupFile);
    XSFLIB_CHECK(read.Players().size() == 2 && read.Players()[0].name == "PLAYER_A" &&
                 read.Players()[0].playable_sound_max == 2 && read.Players()[1].playable_sound_max == 1);

    // The files, the copy in group 0's file, the path and the file that only the group beside the archive holds.
    XSFLIB_CHECK(read.FileCount() == 9);
    for (const uint32_t file : kPlaced)
    {
        XSFLIB_CHECK(std::ranges::equal(read.FileData(file), model.files[file].bytes));
    }
    XSFLIB_CHECK(std::ranges::equal(read.FileData(3), kBankCopy));
    XSFLIB_CHECK(read.FileData(5).empty() && read.ExternalPath(5) == "stream/STRM_D.bfstm");
    XSFLIB_CHECK(read.FileData(7).empty());
    XSFLIB_CHECK(read.GroupFileIds(0) == std::vector<uint32_t>({3, 2}));
}

// A stream sound's fields in each version. In 2.0.0, a stream sound holds no sends, and its table of tracks follows 16
// bytes of fields. From 2.1.0 on, it follows 24 bytes without the fields after the sends, 32 with the reference to an
// extension, and 36 with the prefetch file too. A track's info takes 12 bytes in 2.0.0, and 24 with its sends later.
// The extension follows the sound's sends.
void TestStreamVersions(Flavor flavor)
{
    struct Case
    {
        uint32_t version;
        StreamFields fields;
        bool extension;
        std::size_t table;
    };
    const Case kCases[] = {
        {0x00020000, StreamFields::kNone, false, 16},      {0x00020100, StreamFields::kNone, false, 24},
        {0x00020100, StreamFields::kExtension, false, 32}, {0x00020100, StreamFields::kExtension, true, 32},
        {0x00020200, StreamFields::kPrefetch, false, 36},  {0x00020200, StreamFields::kPrefetch, true, 36},
        {0x00020500, StreamFields::kPrefetch, true, 36}};
    for (const Case& c : kCases)
    {
        SoundModel sound;
        sound.name = "STRM_A";
        sound.type = sar::SoundType::kStream;
        sound.stream = MonoStream();
        sound.stream_fields = c.fields;
        if (c.extension)
        {
            sound.stream_extension = sar::StreamExtension{.stream_type = 2, .loop_start = 7, .loop_end = 900};
        }
        ArchiveModel model;
        model.flavor = flavor;
        model.version = c.version;
        model.sounds = {sound};
        model.files = {{.place = FilePlace::kPath, .path = "stream/STRM_A.bfstm"}};

        const std::vector<uint8_t> archive = sar::BuildArchive(model);
        XSFLIB_CHECK(sar::ReadArchiveModel(archive, flavor) == model);
        const sar::SoundArchive read = Load(archive, flavor);
        XSFLIB_CHECK(read.Sounds().size() == 1 && read.Sounds()[0].stream == MonoStream());

        const std::vector<uint8_t> entry = InfoOf(archive, flavor).sounds.at(0);
        const std::size_t details = Load32(entry, 16, flavor);
        const std::size_t table = details + c.table;
        const bool sends = c.version >= 0x00020100;
        XSFLIB_CHECK(Load32(entry, details + 8, flavor) == c.table && Load32(entry, table + 8, flavor) == 12);
        XSFLIB_CHECK(Load32(entry, table + 12 + 8, flavor) == (sends ? 24u : 12u));
        if (c.fields != StreamFields::kNone)
        {
            XSFLIB_CHECK(Load16(entry, details + 24, flavor) == (c.extension ? 0x2210 : 0));
        }
        if (c.fields == StreamFields::kPrefetch)
        {
            XSFLIB_CHECK(Load32(entry, details + 32, flavor) == kNone);
        }

        // The entry ends with the sound's sends, or with the extension after them.
        const std::size_t end = sends ? details + Load32(entry, details + 20, flavor) + 8 : entry.size();
        if (c.extension)
        {
            XSFLIB_CHECK(Load32(entry, details + 28, flavor) == end - details && entry.size() == end + 12);
            XSFLIB_CHECK(Load32(entry, end, flavor) == 2 && Load32(entry, end + 4, flavor) == 7 &&
                         Load32(entry, end + 8, flavor) == 900);
        }
        else
        {
            XSFLIB_CHECK(entry.size() == end);
            if (c.fields != StreamFields::kNone)
            {
                XSFLIB_CHECK(Load32(entry, details + 28, flavor) == kNone);
            }
        }
    }
}

// An archive of ntdlib/testing/sar_tools.h reads as a model, and comes back byte for byte. Its entries hold what the
// tools write. Its group GROUP_B's file holds a copy of file 0, and the file table gives the group's file a reference
// of type 0. Its stream sound plays the file outside the archive.
void TestToolsArchive(Flavor flavor)
{
    sar_files::SoundSpec a;
    a.name = "SEQ_A";
    a.volume = 90;
    a.pan = 0x0101;
    a.priority = 0x0230;
    a.banks = {0};
    a.allocate_track_flags = 3;
    a.start_offset = 5;
    a.channel_priority = 0x0150;
    sar_files::SoundSpec b;
    b.name = "SEQ_B";
    b.banks = {0, kNone};
    b.start_offset = 9;
    sar_files::SoundSpec c;
    c.name = "WSD_C";
    c.kind = sar_files::SoundSpec::kWaveSound;
    c.file = 1;
    c.wave_sound_index = 2;
    sar_files::SoundSpec d;
    d.name = "STRM_D";
    d.kind = sar_files::SoundSpec::kStream;
    d.file = 4;
    d.stream = StreamDetails();

    sar_files::ArchiveSpec spec;
    spec.sounds = {a, b, c, d};
    spec.banks = {{"BANK_A", 2}};
    spec.wave_archives = {{"", 3}};
    spec.groups = {{"GROUP_A", sar::kExternalGroupFile}, {"GROUP_B", 5}};
    spec.players = {{"PLAYER_A", 4}};
    for (uint8_t i = 0; i < 3; ++i)
    {
        sar_files::FileSpec file;
        file.bytes.assign(40u - 8u * i, static_cast<uint8_t>(0xF0 + i));
        spec.files.push_back(file);
    }
    spec.files[0].groups = {1};
    sar_files::FileSpec only;
    only.group_only = true;
    only.groups = {0};
    sar_files::FileSpec outside;
    outside.external = "stream/STRM.bfstm";
    sar_files::FileSpec group;
    group.bytes = sar_tools::GroupInArchive(flavor, {.files = {{0, spec.files[0].bytes}}, .items = {{0x01000000, 1}}});
    spec.files.push_back(only);
    spec.files.push_back(outside);
    spec.files.push_back(group);

    const std::vector<uint8_t> archive = sar_tools::Archive(flavor, spec);
    const ArchiveModel model = sar::ReadArchiveModel(archive, flavor);
    XSFLIB_CHECK(sar::BuildArchive(model) == archive);
    XSFLIB_CHECK(model.version == 0x00020200 && model.sounds.size() == 4 && model.files.size() == 6);
    if (model.sounds.size() != 4 || model.files.size() != 6)
    {
        return;
    }

    const SoundModel& sound = model.sounds[0];
    XSFLIB_CHECK(sound.name == "SEQ_A" && sound.volume == 90 && sound.pan_mode == 1 && sound.pan_curve == 1 &&
                 sound.player_priority == 0x30 && sound.actor_player_id == 2);
    XSFLIB_CHECK(sound.banks == std::vector<uint32_t>({ItemId(sar::kBankItem, 0)}) && sound.allocate_track_flags == 3 &&
                 sound.start_offset == 5 && sound.channel_priority == 0x50 && sound.release_priority_fix);
    XSFLIB_CHECK(model.sounds[1].banks == std::vector<uint32_t>({ItemId(sar::kBankItem, 0), kNone}));
    XSFLIB_CHECK(model.sounds[2].type == sar::SoundType::kWaveSound && model.sounds[2].wave_sound_index == 2 &&
                 model.sounds[2].file_id == 1);
    XSFLIB_CHECK(model.sounds[3].type == sar::SoundType::kStream && model.sounds[3].file_id == 4 &&
                 model.sounds[3].stream == StreamDetails() &&
                 model.sounds[3].stream_fields == StreamFields::kPrefetch && !model.sounds[3].stream_extension &&
                 model.sounds[3].prefetch_file_id == sar::kNoItem);
    XSFLIB_CHECK(model.banks == std::vector<sar::BankModel>({{.name = "BANK_A", .file_id = 2}}));
    XSFLIB_CHECK(model.wave_archives == std::vector<sar::WaveArchiveModel>({{.file_id = 3}}));
    XSFLIB_CHECK(model.groups == std::vector<sar::GroupModel>({{.name = "GROUP_A", .file_id = sar::kExternalGroupFile},
                                                               {.name = "GROUP_B", .file_id = 5}}));
    XSFLIB_CHECK(model.players == std::vector<sar::PlayerModel>({{.name = "PLAYER_A", .playable_sound_max = 4}}));
    XSFLIB_CHECK(model.player_settings == sar::PlayerSettings{.stream_tracks = 32});
    XSFLIB_CHECK(model.files[0].place == FilePlace::kBytes && model.files[0].bytes == spec.files[0].bytes);
    XSFLIB_CHECK(model.files[3].place == FilePlace::kGroupsOnly &&
                 model.files[3].groups == std::vector<uint32_t>({ItemId(sar::kGroupItem, 0)}));
    XSFLIB_CHECK(model.files[4].place == FilePlace::kPath && model.files[4].path == "stream/STRM.bfstm");
    XSFLIB_CHECK(model.files[5].place == FilePlace::kBytes && model.files[5].bytes == spec.files[5].bytes);
    XSFLIB_CHECK(model.files[0].groups == std::vector<uint32_t>({ItemId(sar::kGroupItem, 1)}));
}

// The 3DS's versions of CSAR, with a sound of each kind and a file of each place where the model knows the version's
// layout of them. Each sound below holds a name, a pan, priorities and its 3D settings at 40. Its details start at 56,
// or at 60 after the value that follows the 3D settings. A wave sound of 2.0.0.0 holds sends after its details'
// options, at the offset of option 8. From 2.3.1.0 on, it holds none. A stream sound of 2.0.0.0 holds the value after
// its 3D settings too. Before 2.3.0.0, its details hold the tracks that play and the count of channels alone. 2.3.0.0
// adds the table of tracks after options that hold the extension's values. From 2.3.2.0 on, the details take the
// layout of FSAR 2.2.0 with sends of 4 bytes. A file's entry lists no groups. A file without a place has an entry that
// refers to nothing before 2.3.0.0, and an internal file's entry whose place refers to nothing from 2.3.2.0 on. The
// builder refuses a sound or a file whose layout in the version the model doesn't know, and the reader refuses an
// archive that holds one.
void TestCtrVersions()
{
    constexpr Flavor kCtr = Flavor::kCtr;
    struct Case
    {
        uint32_t version;
        bool wave_sounds;
        bool stream_sounds;
        bool without_place;
    };
    const Case kCases[] = {{0x02000000, true, true, true},
                           {0x02020000, false, true, true},
                           {0x02030000, false, true, false},
                           {0x02030100, true, false, false},
                           {0x02030200, true, true, true}};
    std::vector<uint8_t> first;
    for (const Case& c : kCases)
    {
        SoundModel wave;
        wave.name = "WSD_B";
        wave.type = sar::SoundType::kWaveSound;
        wave.file_id = 2;
        wave.after_3d = 6;
        wave.wave_sound_index = 3;
        wave.channel_priority = 70;

        SoundModel stream;
        stream.name = "STRM_C";
        stream.type = sar::SoundType::kStream;
        stream.file_id = 3;
        stream.stream_fields = StreamFields::kNone;
        if (c.version < 0x02030000)
        {
            stream.after_3d = c.version == 0x02000000 ? 5 : 0;
            stream.stream.valid_tracks = 3;
            stream.stream.channel_count = 4;
        }
        else if (c.version == 0x02030000)
        {
            stream.stream = MonoStream();
            stream.stream_options = 0b101;
            stream.stream_extension = sar::StreamExtension{.stream_type = 1, .loop_end = 900};
        }
        else
        {
            stream.stream = StreamDetails();
            stream.stream_fields = StreamFields::kPrefetch;
            stream.stream_extension = sar::StreamExtension{.stream_type = 2, .loop_start = 7, .loop_end = 900};
        }

        ArchiveModel model;
        model.flavor = kCtr;
        model.version = c.version;
        model.sounds = {SequenceSound()};
        model.banks = {{.name = "BANK_A", .file_id = 1}};
        model.files = {{.bytes = std::vector<uint8_t>(40, 0xF0)},
                       {.bytes = std::vector<uint8_t>(24, 0xF1)},
                       {.bytes = std::vector<uint8_t>(16, 0xF2)},
                       {.place = FilePlace::kPath, .path = "stream/STRM_C.bcstm"}};
        const sar::FileModel kWithoutPlace = {.place = FilePlace::kGroupsOnly};
        const auto refused = [&model](auto change)
        {
            ArchiveModel changed = model;
            change(changed);
            return Throws<std::invalid_argument>([&] { sar::BuildArchive(changed); });
        };
        XSFLIB_CHECK(c.wave_sounds || refused([&](ArchiveModel& changed) { changed.sounds.push_back(wave); }));
        XSFLIB_CHECK(c.stream_sounds || refused([&](ArchiveModel& changed) { changed.sounds.push_back(stream); }));
        XSFLIB_CHECK(c.without_place ||
                     refused([&](ArchiveModel& changed) { changed.files.push_back(kWithoutPlace); }));
        if (c.wave_sounds)
        {
            model.sounds.push_back(wave);
        }
        if (c.stream_sounds)
        {
            model.sounds.push_back(stream);
        }
        if (c.without_place)
        {
            model.files.push_back(kWithoutPlace);
        }

        const std::vector<uint8_t> archive = sar::BuildArchive(model);
        XSFLIB_CHECK(sar::ReadArchiveModel(archive, kCtr) == model);
        if (c.version == 0x02000000)
        {
            first = archive;
        }
        const sar::SoundArchive read = Load(archive, kCtr);
        const sar::SoundArchive lenient = sar::SoundArchive::Load(archive, sar::Options{.flavor = kCtr});
        XSFLIB_CHECK(read.ExternalPath(3) == "stream/STRM_C.bcstm" && read.ExternalPath(0).empty());
        XSFLIB_CHECK(lenient.ExternalPath(3) == "stream/STRM_C.bcstm");
        XSFLIB_CHECK(read.Sounds().size() == model.sounds.size());
        for (std::size_t i = 0; i < read.Sounds().size() && i < model.sounds.size(); ++i)
        {
            XSFLIB_CHECK(read.Sounds()[i].type == model.sounds[i].type &&
                         read.Sounds()[i].file_id == model.sounds[i].file_id);
        }
        XSFLIB_CHECK(std::ranges::equal(read.FileData(0), model.files[0].bytes));
        const InfoBlock info = InfoOf(archive, kCtr);
        XSFLIB_CHECK(info.files.at(0).size() == 24 && Load16(info.files[0], 12, kCtr) == 0x1F00);

        if (c.wave_sounds)
        {
            const std::vector<uint8_t>& entry = info.sounds.at(1);
            const std::size_t details = Load32(entry, 16, kCtr);
            XSFLIB_CHECK(details == 60 && Load32(entry, 56, kCtr) == 6 && Load32(entry, details, kCtr) == 3);
            XSFLIB_CHECK(Load32(entry, details + 12, kCtr) == 70);
            if (c.version == 0x02000000)
            {
                const uint8_t kSends[] = {0x7F, 2, 0, 0, 0, 0, 0, 0};
                XSFLIB_CHECK(Load32(entry, details + 8, kCtr) == 0x101 && Load32(entry, details + 16, kCtr) == 20);
                XSFLIB_CHECK(entry.size() == details + 28 &&
                             std::ranges::equal(std::span(entry).subspan(details + 20), kSends));
            }
            else
            {
                XSFLIB_CHECK(Load32(entry, details + 8, kCtr) == 1 && entry.size() == details + 16);
            }
        }

        if (c.stream_sounds)
        {
            const std::vector<uint8_t>& entry = info.sounds.at(c.wave_sounds ? 2 : 1);
            const std::size_t details = Load32(entry, 16, kCtr);
            XSFLIB_CHECK(details == (c.version == 0x02000000 ? 60u : 56u));
            if (c.version == 0x02000000)
            {
                XSFLIB_CHECK(Load32(entry, 56, kCtr) == 5);
            }
            if (c.version < 0x02030000)
            {
                XSFLIB_CHECK(entry.size() == details + 8 && Load16(entry, details, kCtr) == 3 &&
                             Load16(entry, details + 2, kCtr) == 4);
            }
            else if (c.version == 0x02030000)
            {
                // The options at 12 hold the type of the stream and the loop's end. The table follows at 24. Its
                // track's info takes 12 bytes, without sends.
                XSFLIB_CHECK(Load32(entry, details + 8, kCtr) == 24 && Load32(entry, details + 12, kCtr) == 0b101);
                XSFLIB_CHECK(Load32(entry, details + 16, kCtr) == 1 && Load32(entry, details + 20, kCtr) == 900);
                XSFLIB_CHECK(Load32(entry, details + 24, kCtr) == 1 && Load32(entry, details + 32, kCtr) == 12);
                XSFLIB_CHECK(entry.size() == details + 24 + 12 + 12 + 8);
            }
            else
            {
                // The table follows the prefetch file at 36. The track's info starts 12 bytes later, and its channels'
                // indices take 8 bytes after it. Its sends follow at 80, the sound's at 84 and the extension at 88.
                XSFLIB_CHECK(Load32(entry, details + 8, kCtr) == 36 && Load32(entry, details + 32, kCtr) == kNone);
                XSFLIB_CHECK(Load32(entry, details + 48 + 16, kCtr) == 32 && Load32(entry, details + 20, kCtr) == 84);
                XSFLIB_CHECK(Load32(entry, details + 28, kCtr) == 88 && entry.size() == details + 100);
                const uint8_t kSends[] = {110, 1, 2, 3, 120, 10, 20, 30};
                XSFLIB_CHECK(std::ranges::equal(std::span(entry).subspan(details + 80, 8), kSends));
            }
        }

        if (c.without_place)
        {
            const std::vector<uint8_t>& entry = info.files.at(4);
            if (c.version < 0x02030000)
            {
                XSFLIB_CHECK(entry.size() == 12 && Load16(entry, 0, kCtr) == 0 && Load32(entry, 4, kCtr) == kNone &&
                             Load32(entry, 8, kCtr) == 0);
            }
            else
            {
                XSFLIB_CHECK(entry.size() == 24 && Load16(entry, 0, kCtr) == 0x220C && Load32(entry, 4, kCtr) == 12 &&
                             Load16(entry, 12, kCtr) == 0 && Load32(entry, 16, kCtr) == kNone &&
                             Load32(entry, 20, kCtr) == kNone);
            }
            XSFLIB_CHECK(read.FileData(4).empty());
        }
    }

    // Stream sounds that the builder refuses: tracks or options in a version without them, options without an
    // extension or with an unknown bit, fields after the sends before 2.3.2.0, and a version whose layout the model
    // doesn't know. The model doesn't know a wave sound of that version either.
    const auto refused = [](uint32_t version, auto change)
    {
        SoundModel sound;
        sound.type = sar::SoundType::kStream;
        sound.stream_fields = StreamFields::kNone;
        if (version == 0x02030000)
        {
            sound.stream = MonoStream();
        }
        change(sound);
        ArchiveModel model;
        model.flavor = kCtr;
        model.version = version;
        model.sounds = {sound};
        model.files = {{.place = FilePlace::kPath, .path = "stream/STRM.bcstm"}};
        return Throws<std::invalid_argument>([&] { sar::BuildArchive(model); });
    };
    XSFLIB_CHECK(!refused(0x02000000, [](SoundModel&) {}) && !refused(0x02030000, [](SoundModel&) {}));
    XSFLIB_CHECK(refused(0x02000000, [](SoundModel& sound) { sound.stream = MonoStream(); }));
    XSFLIB_CHECK(refused(0x02020000, [](SoundModel& sound) { sound.stream_extension.emplace(); }));
    XSFLIB_CHECK(refused(0x02020000, [](SoundModel& sound) { sound.stream_options = 1; }));
    XSFLIB_CHECK(refused(0x02030000, [](SoundModel& sound) { sound.stream_options = 1; }));
    XSFLIB_CHECK(refused(0x02030000,
                         [](SoundModel& sound)
                         {
                             sound.stream_options = 8;
                             sound.stream_extension.emplace();
                         }));
    XSFLIB_CHECK(refused(0x02030200,
                         [](SoundModel& sound)
                         {
                             sound.stream_options = 1;
                             sound.stream_extension.emplace();
                         }));
    XSFLIB_CHECK(refused(0x02020000, [](SoundModel& sound) { sound.stream_fields = StreamFields::kExtension; }));
    XSFLIB_CHECK(refused(0x02010000, [](SoundModel&) {}));
    XSFLIB_CHECK(refused(0x02010000, [](SoundModel& sound) { sound.type = sar::SoundType::kWaveSound; }));

    // The reader refuses the archive of 2.0.0.0 as an archive of 2.2.0.0: the model doesn't know the wave sounds of
    // 2.2.0.0.
    first.at(8 + 2) = 0x02;
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::ReadArchiveModel(first, kCtr); }) ==
                 "an archive laid out another way: a wave sound of a version of the 3DS whose layout the model "
                 "doesn't know");
}

// An archive of the 3DS from ntdlib/testing/sar_tools.h reads as a model, and comes back byte for byte. sar_tools.h
// lays it out as CSAR 2.3.0.0. Its group's file holds a copy of file 0, and the file table gives the group's file a
// reference of type 0. The 3DS's file entries list no groups.
void TestCtrToolsArchive()
{
    constexpr Flavor kCtr = Flavor::kCtr;
    sar_files::SoundSpec a;
    a.name = "SEQ_A";
    a.volume = 90;
    a.banks = {0};
    a.start_offset = 5;
    sar_files::SoundSpec b;
    b.name = "SEQ_B";
    b.banks = {0, kNone};
    b.start_offset = 9;

    sar_files::ArchiveSpec spec;
    spec.sounds = {a, b};
    spec.banks = {{"BANK_A", 1}};
    spec.wave_archives = {{"", 2}};
    spec.groups = {{"GROUP_A", 4}};
    spec.players = {{"PLAYER_A", 4}};
    for (uint8_t i = 0; i < 3; ++i)
    {
        sar_files::FileSpec file;
        file.bytes.assign(40u - 8u * i, static_cast<uint8_t>(0xF0 + i));
        spec.files.push_back(file);
    }
    spec.files[0].groups = {0};
    sar_files::FileSpec outside;
    outside.external = "stream/STRM.bcstm";
    sar_files::FileSpec group;
    group.bytes = sar_tools::GroupInArchive(kCtr, {.files = {{0, spec.files[0].bytes}}});
    spec.files.push_back(outside);
    spec.files.push_back(group);

    const std::vector<uint8_t> archive = sar_tools::Archive(kCtr, spec);
    const ArchiveModel model = sar::ReadArchiveModel(archive, kCtr);
    XSFLIB_CHECK(sar::BuildArchive(model) == archive);
    XSFLIB_CHECK(model.version == 0x02030000 && model.sounds.size() == 2 && model.files.size() == 5);
    if (model.sounds.size() != 2 || model.files.size() != 5)
    {
        return;
    }
    XSFLIB_CHECK(model.sounds[0].name == "SEQ_A" && model.sounds[0].volume == 90 && model.sounds[0].start_offset == 5);
    XSFLIB_CHECK(model.sounds[1].banks == std::vector<uint32_t>({ItemId(sar::kBankItem, 0), kNone}));
    XSFLIB_CHECK(model.groups == std::vector<sar::GroupModel>({{.name = "GROUP_A", .file_id = 4}}));
    XSFLIB_CHECK(model.files[0].place == FilePlace::kBytes && model.files[0].bytes == spec.files[0].bytes &&
                 model.files[0].groups.empty());
    XSFLIB_CHECK(model.files[3].place == FilePlace::kPath && model.files[3].path == "stream/STRM.bcstm");
    XSFLIB_CHECK(model.files[4].place == FilePlace::kBytes && model.files[4].bytes == spec.files[4].bytes);
}

void TestRefusals()
{
    // An archive of the Switch is an archive of neither the 3DS nor the Wii U.
    const std::vector<uint8_t> good = sar::BuildArchive(EveryItem(Flavor::kNx));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::ReadArchiveModel(good, Flavor::kCtr); }) ==
                 "not a sound archive of the console (CSAR)");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sar::ReadArchiveModel(good, Flavor::kCafe); }) ==
                 "not a sound archive of the console (FSAR)");

    // Models that the builder refuses: a sound of an unknown kind, fields after the sends in a version without sends,
    // an extension without a reference to it, the options of a 3DS stream sound, copies in a group past the table, in
    // a group beside the archive, in a file that isn't a group's file and in a group's file without the copy or cut
    // short, and two names alike.
    const auto refused = [](auto change)
    {
        ArchiveModel model = EveryItem(Flavor::kNx);
        change(model);
        return Throws<std::invalid_argument>([&] { sar::BuildArchive(model); });
    };
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.sounds[1].type = sar::SoundType::kUnknown; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.version = 0x00020000; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.sounds[3].stream_fields = StreamFields::kNone; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.sounds[3].stream_options = 1; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.files[3].copy_group = 2; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.files[3].copy_group = 1; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.groups[0].file_id = 7; }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.groups[0].file_id = 0; }));
    XSFLIB_CHECK(refused(
        [](ArchiveModel& model)
        {
            model.files[8].bytes = sar_tools::GroupInArchive(Flavor::kNx, {.files = {{2, std::nullopt}}, .items = {}});
        }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.files[8].bytes.resize(CopyOffset(Flavor::kNx) + 10); }));
    XSFLIB_CHECK(refused([](ArchiveModel& model) { model.banks[1].name = "BANK_A"; }));

    // Archives laid out another way: a byte in the header's padding, a byte between two files, a byte after the last
    // file, an entry with an option that the model doesn't hold, a byte in an entry's padding, and no names.
    const auto unread = [](const std::vector<uint8_t>& archive)
    {
        return ErrorOf<xsflib::FormatError>([&] { sar::ReadArchiveModel(archive, Flavor::kNx); });
    };
    XSFLIB_CHECK(unread(good).empty());
    std::vector<uint8_t> changed = good;
    changed[0x3F] = 1;
    XSFLIB_CHECK(unread(changed) ==
                 "an archive laid out another way: the tools lay its bytes out otherwise from byte 63");
    changed = good;
    const std::size_t block = Load32(good, 0x30, Flavor::kNx);
    const std::size_t gap = Align(block + 8, 64) + 40;
    changed[gap] = 1;
    XSFLIB_CHECK(unread(changed) ==
                 "an archive laid out another way: the tools lay its bytes out otherwise from byte " +
                     std::to_string(gap));
    changed = good;
    changed.push_back(0);
    XSFLIB_CHECK(!unread(changed).empty());
    changed.resize(good.size() - 4);
    XSFLIB_CHECK(unread(changed) == "an archive laid out another way: a file past the archive's end");

    const sar::Tables tables = sar::TablesOf(good, Flavor::kNx);
    const sar::StringBlock strings = sar::StringBlock::Read(*tables.strings, Flavor::kNx);
    InfoBlock info = InfoBlock::Read(tables.info, Flavor::kNx);
    info.sounds[1][20] |= 0x10;
    XSFLIB_CHECK(unread(sar::WithTables(good, Flavor::kNx, strings, info)) ==
                 "an archive laid out another way: an entry with options that the model doesn't hold");
    info = InfoBlock::Read(tables.info, Flavor::kNx);
    info.sounds[1][10] = 1;
    XSFLIB_CHECK(unread(sar::WithTables(good, Flavor::kNx, strings, info)) ==
                 "an archive laid out another way: the entry of sound 1 holds fields that the model doesn't hold");

    sar_files::ArchiveSpec spec;
    spec.sounds = {sar_files::SoundSpec{.name = "SEQ_A"}};
    spec.names = false;
    XSFLIB_CHECK(unread(sar_tools::Archive(Flavor::kNx, spec)) ==
                 "an archive laid out another way: an archive without names");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("Archive model",
                                    []
                                    {
                                        for (const Flavor flavor : kFlavors)
                                        {
                                            TestEmpty(flavor);
                                            TestLayout(flavor);
                                            TestEntries(flavor);
                                            TestRead(flavor);
                                            TestStreamVersions(flavor);
                                        }
                                        TestEmpty(Flavor::kCtr);
                                        TestLayout(Flavor::kCtr);
                                        TestCtrVersions();
                                        TestToolsArchive(Flavor::kNx);
                                        TestToolsArchive(Flavor::kCafe);
                                        TestCtrToolsArchive();
                                        TestRefusals();
                                    });
}
