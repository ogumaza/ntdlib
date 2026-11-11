// SPDX-License-Identifier: MIT

// Tests of the JAudio2 readers on files built here (ntdlib/testing/jaudio_files.h): the wave systems, the banks of both
// versions, the sound table with its names, the sequence collection, RARC archives, and the readers' messages, by the
// defaults and by strict rules. The readers read the little-endian files of the Switch's releases as they read the
// big-endian ones, and the banks and wave systems with 64-bit offsets too. Pikmin's bank file holds banks and wave
// systems in either layout.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "jaudio_describe.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/bx.h"
#include "ntdlib/jaudio/jmap.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/testing/jaudio_files.h"

namespace jaudio = ntdlib::jaudio;
namespace files = ntdlib::testing::jaudio_files;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

using ntdlib::testing::jaudio_describe::Describe;

void TestWaveSystem()
{
    const jaudio::SoundArchive archive =
        jaudio::SoundArchive::Parse(files::StartUpArchive(files::BankV0File(0, 100), {}));
    const jaudio::WaveSystem* ws = archive.FindWaveSystem(0);

    Check(ws != nullptr && ws->waves.size() == 2, "the wave system has two waves");
    const jaudio::WaveInfo* first = ws ? &ws->waves.at(0) : nullptr;
    const jaudio::WaveInfo* second = ws ? &ws->waves.at(1) : nullptr;
    Check(
        first && first->format == jaudio::WaveFormat::kPcm16 && first->base_key == 60 && first->sample_rate == 32000.0f,
        "a wave's format, key and rate");
    Check(first && first->loop && first->loop_start == 32 && first->loop_end == 64 && first->sample_count == 64,
          "wave 0 loops from sample 32");
    Check(second && !second->loop && second->offset == 128 && second->size == 128 && second->archive == "test.aw",
          "wave 1's place in its AW file");
    Check(archive.FindWaveSystem(1) == nullptr, "no other wave system");
    Check(ws && ws->groups.size() == 1 && ws->groups[0].archive == "test.aw" &&
              ws->groups[0].waves == std::vector<uint16_t>{0, 1},
          "the wave system's group and its waves");

    // ReadWaveSystems reads the same systems without the rest of the archive.
    const std::vector<jaudio::WaveSystem> systems =
        jaudio::ReadWaveSystems(files::StartUpArchive(files::BankV0File(0, 100), {}));
    Check(systems.size() == 1 && systems[0].number == 0 && systems[0].waves.size() == 2 &&
              systems[0].groups.size() == 1 && systems[0].groups[0].waves.size() == 2,
          "ReadWaveSystems reads the archive's wave system");
}

// A group of the AW file `archive` that holds the waves `waves`.
jaudio::WaveGroup Group(const std::string& archive, const std::vector<uint16_t>& waves)
{
    jaudio::WaveGroup group;
    group.archive = archive;
    group.waves = waves;

    return group;
}

// WaveGroups on wave systems made here: for each system, the group that holds the most of the missing waves first.
void TestWaveGroups()
{
    jaudio::WaveSystem three;
    three.number = 3;
    three.groups = {Group("a.aw", {1, 2, 3}), Group("b.aw", {3, 4}), Group("c.aw", {4, 5})};
    jaudio::WaveSystem seven;
    seven.number = 7;
    seven.groups = {Group("d.aw", {8}), Group("e.aw", {1})};

    // In system 3, a and c hold two of the waves each, and a comes first. Then c holds the rest. Wave 9 is in no group,
    // and system 9 doesn't exist.
    const std::vector<uint32_t> groups =
        jaudio::WaveGroups({three, seven}, {{3, 1}, {3, 2}, {3, 4}, {3, 5}, {3, 9}, {7, 1}, {9, 1}});
    Check(groups == std::vector<uint32_t>{3 << 16 | 0, 3 << 16 | 2, 7 << 16 | 1}, "the groups that hold the waves");
    Check(jaudio::WaveGroups({three, seven}, {}).empty(), "no waves give no groups");

    // Whether the chosen groups hold a wave, and their AW files.
    Check(jaudio::GroupsHold({three, seven}, groups, {3, 5}) && jaudio::GroupsHold({three, seven}, groups, {7, 1}),
          "the chosen groups hold the waves");
    Check(!jaudio::GroupsHold({three, seven}, groups, {3, 9}) && !jaudio::GroupsHold({three, seven}, groups, {7, 8}),
          "a wave outside the chosen groups");
    Check(jaudio::GroupArchives({three, seven}, groups) == std::vector<std::string>{"a.aw", "c.aw", "e.aw"},
          "the chosen groups' AW files");

    // The waves that the chosen groups lack. Wave 3:9 is in no group, and system 9 doesn't exist. Neither counts.
    const std::vector<jaudio::WaveUse> kPlayed = {{3, 1}, {3, 4}, {3, 5}, {3, 9}, {7, 1}, {9, 1}};
    const std::vector<uint32_t> kFirst = {3 << 16 | 0};
    Check(jaudio::MissingWaves({three, seven}, kFirst, kPlayed) == 3, "the waves that group a lacks");
    Check(jaudio::MissingWaves({three, seven}, groups, kPlayed) == 0, "the chosen groups lack no wave");
}

// The wave archives in a model's DSP memory: each from a 32-byte boundary, in the order of their names. An empty or
// missing archive gets no address.
void TestWaveArchiveLayout()
{
    const jaudio::WaveArchiveLayout layout =
        jaudio::LayOutWaveArchives({{"b.aw", 40}, {"a.aw", 64}, {"c.aw", 0}, {"d.aw", 1}}, 0x8000);
    Check(layout.addresses == std::map<std::string, uint32_t>{{"a.aw", 0x8000}, {"b.aw", 0x8040}, {"d.aw", 0x8080}},
          "each archive's address");
    Check(layout.end == 0x80a0, "the memory's size");
    Check(jaudio::LayOutWaveArchives({}, 0x8000).end == 0x8000, "no archive");
}

// A JMap table (BCSV) in a RARC archive, as Super Mario Galaxy 2's BgmParam.arc lays it out: the rows' names and
// volumes in the table's order, with a row's name twice. The volume field's mask and shift apply. The table's name
// matches whatever its case, and the archive may be compressed. The Switch's release stores the archive and the table
// little-endian ("CRAR"). A missing table, a missing field or a field of another type can't be read.
void TestJMapTable()
{
    // The hashes of "Volume" and "SoundIdName" in Galaxy 2's table.
    Check(jaudio::JMapHash("Volume") == 0x990fd33a && jaudio::JMapHash("SoundIdName") == 0xc42d0c15,
          "the hashes of the fields' names");

    const auto table = [](uint32_t volume_mask, uint8_t volume_type, bool big_endian = true)
    {
        std::vector<uint8_t> bcsv;
        const auto u32 = [&](uint32_t v)
        {
            for (int i = 0; i < 4; i++)
            {
                bcsv.push_back(static_cast<uint8_t>(v >> (big_endian ? 24 - 8 * i : 8 * i)));
            }
        };
        // A field's offset in a row (16 bits), its shift and its type.
        const auto place = [&](uint16_t offset, uint8_t shift, uint8_t type)
        {
            bcsv.push_back(static_cast<uint8_t>(big_endian ? offset >> 8 : offset));
            bcsv.push_back(static_cast<uint8_t>(big_endian ? offset : offset >> 8));
            bcsv.push_back(shift);
            bcsv.push_back(type);
        };
        u32(3);
        u32(2);
        u32(40);
        u32(8);
        u32(0x990fd33a);
        u32(volume_mask);
        place(0, 4, volume_type);
        u32(0xc42d0c15);
        u32(0xffffffff);
        place(4, 0, 6);
        u32(0x690);
        u32(0);
        u32(0x17f0);
        u32(6);
        u32(0x50);
        u32(0);
        for (const char c : std::string_view("BGM_A\0BGM_B\0", 12))
        {
            bcsv.push_back(static_cast<uint8_t>(c));
        }
        return files::RarcFile({{0, "BgmParam.bcsv", bcsv}}, big_endian);
    };

    const auto read = [](const std::vector<uint8_t>& arc, std::string_view file)
    {
        std::vector<std::pair<std::string, uint32_t>> rows;
        for (const jaudio::JMapNamedValue& row : jaudio::ReadJMapNamedValues(arc, file, "SoundIdName", "Volume"))
        {
            rows.emplace_back(row.name, row.value);
        }
        return rows;
    };

    const std::vector<std::pair<std::string, uint32_t>> expected = {{"BGM_A", 0x69}, {"BGM_B", 0x17f}, {"BGM_A", 5}};
    Check(read(table(0xffffffff, 0), "BgmParam.bcsv") == expected, "the rows in the table's order");
    Check(read(table(0xffffffff, 0), "bgmparam.BCSV") == expected, "the table's name in other capitals");
    Check(read(files::LiteralYaz0(table(0xffffffff, 0)), "BgmParam.bcsv") == expected, "a compressed archive");
    Check(read(table(0xffffffff, 0, false), "BgmParam.bcsv") == expected, "a little-endian archive");
    Check(read(files::LiteralYaz0(table(0x000000f0, 0, false)), "BgmParam.bcsv") ==
              std::vector<std::pair<std::string, uint32_t>>{{"BGM_A", 9}, {"BGM_B", 15}, {"BGM_A", 5}},
          "a compressed little-endian archive, with the mask before the shift");
    Check(read(table(0x000000f0, 0), "BgmParam.bcsv") ==
              std::vector<std::pair<std::string, uint32_t>>{{"BGM_A", 9}, {"BGM_B", 15}, {"BGM_A", 5}},
          "the mask before the shift");
    Check(!ErrorOf([&] { read(table(0xffffffff, 0), "Other.bcsv"); }).empty(), "a missing table");
    Check(!ErrorOf([&] { jaudio::ReadJMapNamedValues(table(0xffffffff, 0), "BgmParam.bcsv", "SoundIdName", "Pan"); })
               .empty(),
          "a missing field");
    Check(!ErrorOf([&] { read(table(0xffffffff, 6), "BgmParam.bcsv"); }).empty(), "a volume that isn't an integer");
}

// The later layout of a sound's entry, and a start-up archive with a "bfca" command.
void TestLaterLayout()
{
    const jaudio::ItemLayout earlier = jaudio::LayoutOf(0x11);
    const jaudio::ItemLayout later = jaudio::LayoutOf(0x62);
    Check(earlier.volume == 2 && earlier.file == 10u && !jaudio::LayoutOf(0x10).file, "the earlier layout");
    Check(later.volume == 1 && later.file == 2u && jaudio::LayoutOf(0x41).volume == 1 && !jaudio::LayoutOf(0x41).file,
          "the later layout");
    Check(jaudio::LayoutOf(0x72).stream && !jaudio::LayoutOf(0x72).file && !later.stream && !earlier.stream &&
              !jaudio::LayoutOf(0x31).stream,
          "types 0x70 to 0x7f of the later layout play streams");

    const files::SoundTableBlocks blocks = files::SoundTable({
        {"SE", {{"SYSTEM", {{0x41, 90, 0, "SE_A"}}}}},
        {"BGM", {{"FIELD", {{0x62, 70, 0x123, "BGM_FIELD", 0x8001}, {0x11, 50, 7, "BGM_OLD", 0x8001}}}}},
    });
    // The command list: "AA_<", bfca with its argument, bst and bstn with two each, then ">_AA".
    const auto kTableAt = static_cast<uint32_t>(4 + 8 + 12 + 12 + 4);
    const auto names_at = static_cast<uint32_t>(kTableAt + blocks.table.size());
    files::Bytes a;
    a.Tag("AA_<");
    a.Tag("bfca").U32(0);
    a.Tag("bst ").U32(kTableAt).U32(names_at);
    a.Tag("bstn").U32(names_at).U32(static_cast<uint32_t>(names_at + blocks.names.size()));
    a.Tag(">_AA").Append(blocks.table).Append(blocks.names);

    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadSounds(a.Data());
    Check(sounds.size() == 3, "the archive with a bfca command has three sounds");
    if (sounds.size() != 3)
    {
        return;
    }

    Check(sounds[0].volume == 90 && sounds[0].source == jaudio::SoundEntry::Source::kCollection,
          "a later sound that plays no file");
    Check(sounds[1].volume == 70 && sounds[1].source == jaudio::SoundEntry::Source::kArchive &&
              sounds[1].resource == 0x123,
          "a later sound that plays a file of the sequence archive");
    Check(sounds[2].volume == 50 && sounds[2].source == jaudio::SoundEntry::Source::kArchive && sounds[2].resource == 7,
          "an earlier sound in the same table");
    Check(sounds[1].switches == 0x8001 && sounds[2].switches == 0 && sounds[0].switches == 0,
          "a later sound of types 0x60 to 0x6f gives its switches; other sounds give none");
}

// The first layouts of a sound's entry (Mario Kart: Double Dash!!), and a start-up archive with "bms " and "baac"
// commands.
void TestFirstLayouts()
{
    const jaudio::ItemLayout effect = jaudio::FirstLayoutOf(0x10);
    const jaudio::ItemLayout music = jaudio::FirstLayoutOf(0x20);
    const jaudio::ItemLayout stream = jaudio::FirstLayoutOf(0x30);
    Check(effect.volume == 8 && music.volume == 8 && stream.volume == 8 && !effect.file && !music.file && !stream.file,
          "the first layouts hold the volume at +8 and no file");
    Check(music.block && !effect.block && !stream.block && stream.stream && !effect.stream && !music.stream,
          "type 0x20 plays a sequence block, and type 0x30 a stream");
    Check(!jaudio::LayoutOf(0x20).block && !jaudio::LayoutOf(0x30).stream, "the other layouts play no block");

    const files::SoundTableBlocks blocks = files::SoundTable(
        {
            {"TYPE_SE", {{"SE_SYSTEM", {{0x10, 90, 0, "SE_A"}, {0x10, 80, 0, "SE_B"}}}}},
            {"TYPE_BGM", {{"MAIN_BGM", {{0x20, 70, 0, "BGM_TITLE"}}}}},
            {"TYPE_STREAM", {{"MAIN_BGM", {{0x30, 60, 0, "STRM_GOAL"}}}}},
        },
        true);
    const std::vector<uint8_t> ws = files::WaveSystemFile(0, 2);

    // The block of "baac": a count, an offset, and a start-up archive with no commands.
    files::Bytes more;
    more.U32(1).U32(8).Tag("AA_<").Tag(">_AA");

    // The command list: "AA_<", bst and bstn with two arguments each, bms with three, baac with two, ws with three,
    // then ">_AA". The table, the names, the sequence block, baac's block and the wave system follow it.
    const uint32_t kTableAt = 4 + 12 + 12 + 16 + 12 + 16 + 4;
    const auto names_at = static_cast<uint32_t>(kTableAt + blocks.table.size());
    const auto sequence_at = static_cast<uint32_t>(names_at + blocks.names.size());
    const uint32_t more_at = sequence_at + 4;
    const auto ws_at = static_cast<uint32_t>(more_at + more.Size());
    files::Bytes a;
    a.Tag("AA_<");
    a.Tag("bst ").U32(kTableAt).U32(names_at);
    a.Tag("bstn").U32(names_at).U32(sequence_at);
    a.Tag("bms ").U32(0x01000000).U32(sequence_at).U32(more_at);
    a.Tag("baac").U32(more_at).U32(ws_at);
    a.Tag("ws  ").U32(0).U32(ws_at).U32(0xffffffff);
    a.Tag(">_AA").Append(blocks.table).Append(blocks.names).U32(0xc1000000).Append(more.Data()).Append(ws);

    jaudio::BaaOptions first;
    first.first_layouts = true;
    const std::vector<jaudio::SoundEntry> sounds = jaudio::ReadSounds(a.Data(), first);
    Check(sounds.size() == 4, "the archive with the first layouts has four sounds");
    if (sounds.size() != 4)
    {
        return;
    }

    Check(sounds[0].volume == 90 && sounds[0].source == jaudio::SoundEntry::Source::kCollection &&
              sounds[0].resource == 0 && sounds[1].volume == 80 && sounds[1].resource == 1,
          "an effect plays the collection's sequence at its place in its group");
    Check(sounds[2].volume == 70 && sounds[2].source == jaudio::SoundEntry::Source::kBlock &&
              sounds[2].resource == 0x01000000 && !sounds[2].stream,
          "music plays the sequence block with its ID");
    Check(sounds[3].volume == 60 && sounds[3].stream && !sounds[0].stream, "a stream");

    // By default, the entries are read by LayoutOf. The bytes at +2 are 0.
    const std::vector<jaudio::SoundEntry> by_default = jaudio::ReadSounds(a.Data());
    Check(by_default.size() == 4 && by_default[0].volume == 0 &&
              by_default[2].source == jaudio::SoundEntry::Source::kCollection && !by_default[3].stream,
          "the first layouts are off by default");

    // Every reader walks past the "baac" command.
    Check(jaudio::WaveArchiveNames(a.Data()) == std::vector<std::string>{"test.aw"} &&
              jaudio::ReadWaveSystems(a.Data()).size() == 1,
          "the wave system after a baac command");
    Check(ErrorOf([&] { jaudio::SoundArchive::Parse(a.Data()); }).empty(), "Parse walks past a baac command");
}

// Mario Kart: Double Dash!!'s start-up archive: Parse reads the sound table by the first layouts, the sequence blocks
// ("bms ") and the wave systems and banks of the "baac" block's archives.
void TestSequenceBlocksAndBaac()
{
    const files::SoundTableBlocks blocks = files::SoundTable(
        {
            {"TYPE_SE", {{"SE_SYSTEM", {{0x10, 90, 0, "SE_A"}}}}},
            {"TYPE_BGM", {{"MAIN_BGM", {{0x20, 102, 0, "BGM_TITLE"}}}}},
        },
        true);

    // An archive of the "baac" block: a wave system and a bank, both after its command list. Each archive numbers its
    // wave system 3. The first archive's bank is bank 6, the second's bank 2.
    const auto more = [](uint32_t bank)
    {
        const std::vector<uint8_t> ws = files::WaveSystemFile(bank, 2, "voice" + std::to_string(bank) + ".aw");
        const std::vector<uint8_t> ibnk = files::BankV0File(bank, 10);
        constexpr uint32_t kWsAt = 4 + 16 + 12 + 4;
        const auto bank_at = static_cast<uint32_t>(kWsAt + ws.size());
        files::Bytes a;
        a.Tag("AA_<");
        a.Tag("ws  ").U32(3).U32(kWsAt).U32(0xffffffff);
        a.Tag("bnk ").U32(3).U32(bank_at);
        a.Tag(">_AA").Append(ws).Append(ibnk);
        return a.Data();
    };
    const std::vector<uint8_t> first = more(6), second = more(2);
    files::Bytes baac;
    baac.U32(2).U32(12).U32(static_cast<uint32_t>(12 + first.size())).Append(first).Append(second);

    // The command list: "AA_<", bst and bstn, two sequence blocks, baac, then ">_AA". The table, the names, the blocks
    // and baac's block follow it.
    const uint32_t kTableAt = 4 + 12 + 12 + 16 + 16 + 12 + 4;
    const auto names_at = static_cast<uint32_t>(kTableAt + blocks.table.size());
    const auto sequence_at = static_cast<uint32_t>(names_at + blocks.names.size());
    const uint32_t second_sequence_at = sequence_at + 4;
    const uint32_t baac_at = second_sequence_at + 8;
    files::Bytes a;
    a.Tag("AA_<");
    a.Tag("bst ").U32(kTableAt).U32(names_at);
    a.Tag("bstn").U32(names_at).U32(sequence_at);
    a.Tag("bms ").U32(0x01000000).U32(sequence_at).U32(second_sequence_at);
    a.Tag("bms ").U32(0x01000001).U32(second_sequence_at).U32(baac_at);
    a.Tag("baac").U32(baac_at).U32(static_cast<uint32_t>(baac_at + baac.Size()));
    a.Tag(">_AA").Append(blocks.table).Append(blocks.names).U32(0xc1000000).U32(0x80000000).U32(0xc1000000);
    a.Append(baac.Data());

    jaudio::BaaOptions options;
    options.first_layouts = true;
    const jaudio::SoundArchive archive = jaudio::SoundArchive::Parse(a.Data(), options);
    Check(archive.Sounds().size() == 2 && archive.Sounds()[1].volume == 102 &&
              archive.Sounds()[1].source == jaudio::SoundEntry::Source::kBlock &&
              archive.Sounds()[1].resource == 0x01000000,
          "Parse reads the sound table by the first layouts");
    const jaudio::SoundArchive by_default = jaudio::SoundArchive::Parse(a.Data());
    Check(by_default.Sounds().size() == 2 && by_default.Sounds()[1].source == jaudio::SoundEntry::Source::kCollection,
          "Parse reads the earlier layouts by default");

    const std::optional<std::span<const uint8_t>> title = archive.SequenceBlock(0x01000000);
    const std::optional<std::span<const uint8_t>> next = archive.SequenceBlock(0x01000001);
    Check(title && title->size() == 4 && (*title)[0] == 0xc1 && next && next->size() == 8 && (*next)[0] == 0x80,
          "each sequence block runs from its start to its end");
    Check(!archive.SequenceBlock(0x01000002), "no block for another ID");

    // The wave system of the k-th archive is numbered (k + 1) << 16 | 3, and the archive's bank plays it.
    const jaudio::Bank* six = archive.FindBank(6);
    const jaudio::Bank* two = archive.FindBank(2);
    Check(six != nullptr && six->wave_system == (1u << 16 | 3) && two != nullptr && two->wave_system == (2u << 16 | 3),
          "each archive's bank plays the archive's wave system");
    const jaudio::WaveSystem* six_ws = archive.FindWaveSystem(1u << 16 | 3);
    const jaudio::WaveSystem* two_ws = archive.FindWaveSystem(2u << 16 | 3);
    Check(six_ws != nullptr && two_ws != nullptr && archive.FindWaveSystem(3) == nullptr &&
              six_ws->groups.size() == 1 && six_ws->groups[0].archive == "voice6.aw" && two_ws->groups.size() == 1 &&
              two_ws->groups[0].archive == "voice2.aw",
          "each archive's wave system names its wave archive");
    Check(archive.Warnings().empty(), "the archive reads without warnings");

    // The second archive's offset, 4, lies before the first's. The first archive then ends before it starts.
    std::vector<uint8_t> data = a.Data();
    for (std::size_t i = 0; i < 4; i++)
    {
        data[baac_at + 8 + i] = i == 3 ? 4 : 0;
    }
    Check(!ErrorOf([&] { jaudio::SoundArchive::Parse(data, options); }).empty(),
          "Parse rejects an archive of the baac block that ends before it starts");
}

void TestBanks()
{
    const jaudio::Bank v0 = jaudio::Bank::Parse(files::BankV0File(7, 100), 3);
    const jaudio::Bank v1 = jaudio::Bank::Parse(files::BankV1File(129, 0, 600), 0);

    Check(v0.number == 7 && v0.version == 0 && v0.wave_system == 3, "a version 0 bank's number and wave system");
    Check(v0.programs.contains(0) && v0.programs.contains(0xe4) && v0.programs.size() == 2,
          "drum sets are programs from 0xe4");
    const auto* inst = v0.programs.contains(0) ? std::get_if<jaudio::Instrument>(&v0.programs.at(0)) : nullptr;
    Check(inst && inst->keys.size() == 1 && inst->keys[0].velocities.size() == 1 &&
              inst->keys[0].velocities[0].wave_id == 0 && inst->oscillators.size() == 1,
          "the instrument plays wave 0 with its oscillator");
    Check(
        v0.oscillators.size() == 1 && v0.oscillators[0].release.size() == 2 && v0.oscillators[0].release[0].time == 100,
        "the oscillator's release table");
    const auto* drums = v0.programs.contains(0xe4) ? std::get_if<jaudio::DrumSet>(&v0.programs.at(0xe4)) : nullptr;
    const auto* drum = drums && drums->keys[36] ? &*drums->keys[36] : nullptr;
    Check(drum && drum->pitch == 2.0f && drum->velocities.size() == 1 && drum->velocities[0].wave_id == 1 &&
              drum->velocities[0].volume == 0.5f,
          "a drum plays its region");
    Check(drum && std::abs(drum->pan - 1.0f) < 1e-6f && drum->release == 200, "PER2 gives a drum's pan and release");
    Check(drums && !drums->keys[37], "a drum set plays only its keys");

    Check(v1.number == 129 && v1.version == 1 && v1.oscillators.size() == 1, "a version 1 bank reads its oscillators");
    const auto* inst1 = v1.programs.contains(0) ? std::get_if<jaudio::Instrument>(&v1.programs.at(0)) : nullptr;
    Check(inst1 && inst1->keys.size() == 1 && inst1->keys[0].high_key == 64 && inst1->keys[0].velocities.size() == 2,
          "the version 1 instrument's key region");
    Check(inst1 && inst1->keys[0].velocities[0].high_velocity == 63 && inst1->keys[0].velocities[1].wave_id == 1,
          "velocity regions select the wave");
    Check(inst1 && inst1->effects.size() == 1 && inst1->effects[0].kind == jaudio::InstEffect::Kind::kRandom &&
              inst1->effects[0].target == 1,
          "a random pitch effect");
}

// A bank whose size ends inside its LIST chunk. The game reads each chunk whole.
void TestBankChunkPastItsSize()
{
    std::vector<uint8_t> bank = files::BankV1File(129, 0, 600);
    xsflib::StoreBe32(bank.data() + 4, static_cast<uint32_t>(bank.size() - 4));

    jaudio::Bank parsed;
    const std::string error = ErrorOf([&] { parsed = jaudio::Bank::Parse(bank, 0); });
    const jaudio::SoundArchive archive = jaudio::SoundArchive::Parse(files::StartUpArchive(bank, {}));

    Check(error.empty() && parsed.programs.contains(0), "a bank reads its LIST chunk past its size: " + error);
    const jaudio::Bank* in_archive = archive.FindBank(129);
    const std::string warnings = archive.Warnings().empty() ? "" : archive.Warnings()[0];
    Check(warnings.empty() && in_archive != nullptr && in_archive->programs.contains(0),
          "so does a bank in a start-up archive: " + warnings);
}

void TestSoundTable()
{
    const files::SoundTableBlocks table = files::SoundTable({
        {"SE", {{"SYSTEM", {{0x10, 100, 0, "SE_A"}, {0x12, 127, 0, "SE_B"}}}}},
        {"BGM", {{"FIELD", {{0x11, 90, 7, "BGM_FIELD"}}}}},
    });
    const std::vector<uint8_t> baa = files::StartUpArchive(files::BankV0File(0, 100), {{0xff}, {0xff}}, table);
    const jaudio::SoundArchive archive = jaudio::SoundArchive::Parse(baa);
    const std::vector<jaudio::SoundEntry>& sounds = archive.Sounds();

    Check(sounds.size() == 3, "the table's three sounds");
    if (sounds.size() != 3)
    {
        return;
    }

    Check(sounds[0].name == "SE_A" && sounds[0].type == 0x10 && sounds[0].volume == 100 && sounds[0].Id() == 0,
          "the first sound");
    Check(
        sounds[1].index == 1 && sounds[1].source == jaudio::SoundEntry::Source::kCollection && sounds[1].resource == 1,
        "a sound of the collection plays the sequence at its index");
    Check(sounds[2].section == 1 && sounds[2].group == 0 && sounds[2].Id() == 0x01000000 && sounds[2].type == 0x11,
          "the second section's sound");
    Check(sounds[2].source == jaudio::SoundEntry::Source::kArchive && sounds[2].resource == 7,
          "a sound of type 0x11 plays a file of the sequence archive");
    Check(sounds[2].section_name.empty() && sounds[2].group_name.empty(), "the section and group names stay empty");

    // A sound's offset is where its entry starts in the archive: the volume at +2, and an archive sound's file in the
    // low half of the word at +8.
    const auto byte = [&baa](uint32_t offset)
    {
        return offset < baa.size() ? int{baa[offset]} : -1;
    };
    Check(byte(sounds[0].offset + 2) == 100 && byte(sounds[1].offset + 2) == 127 && byte(sounds[2].offset + 2) == 90 &&
              byte(sounds[2].offset + 10) == 0 && byte(sounds[2].offset + 11) == 7,
          "each sound's offset is where its entry starts in the archive");
    Check(archive.CollectionSequence(0, 1) && !archive.CollectionSequence(0, 2) && !archive.CollectionSequence(1, 0),
          "the collection's sequences");
}

void TestRarc()
{
    const std::vector<uint8_t> rarc = files::RarcFile({{3, "a.bms", {1, 2, 3}}, {9, "long_name.bms", {4, 5, 6, 7, 8}}});

    const std::vector<jaudio::ArchiveFile> read = jaudio::ReadRarc(rarc);

    Check(read.size() == 2, "the folder is left out");
    Check(read.size() == 2 && read[0].id == 3 && read[0].name == "a.bms" && read[0].data.size() == 3 &&
              read[0].data[2] == 3,
          "the first file");
    Check(read.size() == 2 && read[1].id == 9 && read[1].name == "long_name.bms" && read[1].data.size() == 5 &&
              read[1].data[0] == 4,
          "the second file");

    // The same archive compressed with Yaz0.
    const std::vector<uint8_t> yaz0 = files::LiteralYaz0(rarc);
    Check(jaudio::IsYaz0(yaz0) && !jaudio::IsYaz0(rarc), "IsYaz0 checks the magic");
    const std::vector<uint8_t> decoded = jaudio::DecodeYaz0(yaz0);
    Check(decoded == rarc && jaudio::ReadRarc(decoded).size() == 2, "a Yaz0 archive decodes to the archive");

    // DecodeIfYaz0 gives the archive itself, or decodes the compressed archive into its vector.
    std::vector<uint8_t> into;
    const std::span<const uint8_t> plain = jaudio::DecodeIfYaz0(rarc, into);
    Check(plain.data() == rarc.data() && plain.size() == rarc.size() && into.empty(),
          "DecodeIfYaz0 gives data that isn't Yaz0 as it is");
    const std::span<const uint8_t> unpacked = jaudio::DecodeIfYaz0(yaz0, into);
    Check(into == rarc && unpacked.data() == into.data() && unpacked.size() == into.size(),
          "DecodeIfYaz0 decodes Yaz0 data into its vector");
}

// A sequence table: the count, zeros up to 0x20, then each sequence's offset and size, then the sequences.
std::vector<uint8_t> SequenceTable(const std::vector<std::string>& sequences)
{
    std::vector<uint8_t> table(0x20 + sequences.size() * 8);
    xsflib::StoreBe32(table.data(), static_cast<uint32_t>(sequences.size()));
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        xsflib::StoreBe32(table.data() + 0x20 + i * 8, static_cast<uint32_t>(table.size()));
        xsflib::StoreBe32(table.data() + 0x24 + i * 8, static_cast<uint32_t>(sequences[i].size()));
        table.insert(table.end(), sequences[i].begin(), sequences[i].end());
    }

    return table;
}

// The text that `data` holds.
std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

// RepackSequenceTable: each file of the archive takes the table's sequence whose index is the file's id.
void TestRepackSequenceTable()
{
    // Each file keeps its id, its name and its place.
    const std::string kField = "the field sequence from the table, longer than 32 bytes";
    const std::vector<uint8_t> rarc =
        files::RarcFile({{2, "se.bms", {'o', 'l', 'd'}}, {0, "field.bms", {'o', 'l', 'd'}}, {1, "b.bms", {'b'}}});
    const std::vector<uint8_t> table = SequenceTable({kField, "boss", "se"});
    const std::vector<uint8_t> out = jaudio::RepackSequenceTable(rarc, table);
    const std::vector<jaudio::ArchiveFile> read = jaudio::ReadRarc(out);
    Check(read.size() == 3 && read[0].id == 2 && read[0].name == "se.bms" && Text(read[0].data) == "se" &&
              read[1].id == 0 && read[1].name == "field.bms" && Text(read[1].data) == kField && read[2].id == 1 &&
              read[2].name == "b.bms" && Text(read[2].data) == "boss",
          "each file takes the sequence at its id");

    // Each file's data starts on a 32-byte boundary. The header gives the archive's size, and the data's size twice.
    const std::size_t data = 0x20 + xsflib::LoadBe32(out.data() + 0xc);
    bool aligned = true;
    for (const jaudio::ArchiveFile& file : read)
    {
        aligned = aligned && (static_cast<std::size_t>(file.data.data() - out.data()) - data) % 0x20 == 0;
    }

    Check(aligned, "each file's data is aligned");
    Check(xsflib::LoadBe32(out.data() + 4) == out.size() && xsflib::LoadBe32(out.data() + 0x10) == out.size() - data &&
              xsflib::LoadBe32(out.data() + 0x14) == out.size() - data,
          "the header's sizes");

    // A Yaz0 archive, every byte as a literal, gives the same result.
    Check(jaudio::RepackSequenceTable(files::LiteralYaz0(rarc), table) == out, "a Yaz0 archive is decoded first");

    // A file whose id is past the table's end, and a table cut short, are rejected.
    Check(ErrorOf([&] { jaudio::RepackSequenceTable(rarc, SequenceTable({kField, "boss"})); }) ==
              "the sequence table has no sequence for file 2 of the sequence archive",
          "a file past the table's end");
    Check(!ErrorOf([&] { jaudio::RepackSequenceTable(rarc, std::span(table).first(table.size() - 1)); }).empty(),
          "a table cut short");
}

// ReadSequenceTable: each sequence of the table, by its index, without a name. An empty entry is left out.
void TestReadSequenceTable()
{
    const std::vector<uint8_t> table = SequenceTable({"field", "", "boss"});

    const std::vector<jaudio::ArchiveFile> files = jaudio::ReadSequenceTable(table);

    Check(files.size() == 2 && files[0].id == 0 && files[0].name.empty() && Text(files[0].data) == "field" &&
              files[1].id == 2 && files[1].name.empty() && Text(files[1].data) == "boss",
          "the table's sequences by their indices");
    Check(files.size() == 2 && files[0].data.data() == table.data() + 0x38, "the data lies in the table");

    // A table too short for its entries, and an entry past the table's end, are rejected.
    Check(ErrorOf([&] { jaudio::ReadSequenceTable(std::span(table).first(0x30)); }) == "the sequence table is damaged",
          "a table cut short in its entries");
    Check(!ErrorOf([&] { jaudio::ReadSequenceTable(std::span(table).first(table.size() - 1)); }).empty(),
          "an entry past the table's end");
}

void TestMessages()
{
    const std::vector<uint8_t> good = files::StartUpArchive(files::BankV0File(0, 100), {});
    auto parse = [](std::vector<uint8_t> baa)
    {
        return ErrorOf([&]() { jaudio::SoundArchive::Parse(std::move(baa)); });
    };

    std::vector<uint8_t> unknown = good;
    unknown[4] = 'x';
    std::vector<uint8_t> backwards = good;
    std::vector<uint8_t> no_sc = good;
    // The bsc command: its tag at 4 + 16 + 12, its start and end after it.
    const std::size_t kBsc = 4 + 16 + 12;
    xsflib::StoreBe32(backwards.data() + kBsc + 8, 0);
    const std::size_t sc_start = xsflib::LoadBe32(no_sc.data() + kBsc + 4);
    no_sc[sc_start] = 'X';

    XSFLIB_CHECK(parse({'A', 'A'}) == "not a JAudio2 start-up archive (BAA)");
    XSFLIB_CHECK(parse(unknown) == "archive has a chunk of unknown kind");
    XSFLIB_CHECK(parse(backwards) == "archive has a chunk that ends before it starts");
    XSFLIB_CHECK(parse(no_sc) == "archive's sequence collection has no SC header");
    XSFLIB_CHECK(ErrorOf([]() { jaudio::ReadRarc(std::vector<uint8_t>(0x40)); }) == "not an RARC archive");
    XSFLIB_CHECK(ErrorOf([]() { jaudio::ReadWaveSystem(std::vector<uint8_t>(0x40), 0); }) ==
                 "wave system has no WSYS header");
    XSFLIB_CHECK(ErrorOf([]() { jaudio::Bank::Parse(std::vector<uint8_t>(0x40), 0); }) == "bank has no IBNK header");

    std::vector<uint8_t> not_a_bank(0x40);
    not_a_bank[7] = 0x40; // its size
    const std::vector<uint8_t> bad_bank = files::StartUpArchive(not_a_bank, {});
    const jaudio::SoundArchive with_bad_bank = jaudio::SoundArchive::Parse(bad_bank);
    Check(with_bad_bank.Warnings().size() == 1 && with_bad_bank.Warnings()[0].ends_with(": bank has no IBNK header") &&
              with_bad_bank.Warnings()[0].starts_with("bank at offset "),
          "a bank that can't be read becomes a warning");
}

// Strict rules and messages for the start-up archive.
jaudio::BaaOptions Strict()
{
    jaudio::BaaOptions o;
    o.not_an_archive = "it isn't a BAA archive";
    o.unknown_command = "the BAA archive has a command that isn't known";
    o.past_end = "the BAA archive is damaged";
    o.table_damaged = "the BAA archive's sound table is damaged";
    o.no_table = "the BAA archive has no sound table";
    o.no_sounds = "the BAA archive's sound table lists no sounds";
    o.wave_system_damaged = "a wave system in the BAA archive is damaged";
    o.min_size = 0;
    o.tables_reach_end = true;
    o.check_magic = true;
    o.skip_empty = true;
    o.cap_counts = true;
    o.check_name_counts = true;
    o.names_inside = true;
    o.sources = false;
    o.section_names = true;

    return o;
}

// The command list of TableArchive: two wave systems, the sound table, and its names when it has them.
constexpr std::size_t kTableCommand = 4 + 2 * 16;
constexpr std::size_t kNamesCommand = kTableCommand + 12;

// A start-up archive whose command list names the wave system of WaveSystemFile twice, then `blocks`' sound table and,
// unless `names` is false, its name table. The blocks follow the list in that order, and the archive ends with them.
std::vector<uint8_t> TableArchive(const files::SoundTableBlocks& blocks, bool names = true)
{
    const std::vector<uint8_t> ws = files::WaveSystemFile(0, 2);
    const auto ws_at = static_cast<uint32_t>(kNamesCommand + (names ? 12 : 0) + 4);
    const auto table_at = static_cast<uint32_t>(ws_at + ws.size());
    const auto names_at = static_cast<uint32_t>(table_at + blocks.table.size());
    files::Bytes a;
    a.Tag("AA_<");
    a.Tag("ws  ").U32(0).U32(ws_at).U32(0);
    a.Tag("ws  ").U32(1).U32(ws_at).U32(0);
    a.Tag("bst ").U32(table_at).U32(names_at);
    if (names)
    {
        a.Tag("bstn").U32(names_at).U32(static_cast<uint32_t>(names_at + blocks.names.size()));
    }
    a.Tag(">_AA").Append(ws).Append(blocks.table);
    if (names)
    {
        a.Append(blocks.names);
    }

    return a.Data();
}

const files::SoundTableBlocks& ThreeSounds()
{
    static const files::SoundTableBlocks kBlocks = files::SoundTable({
        {"SE", {{"SYSTEM", {{0x10, 100, 0, "SE_A"}, {0x12, 127, 0, "SE_B"}}}}},
        {"BGM", {{"FIELD", {{0x11, 90, 7, "BGM_FIELD"}}}}},
    });

    return kBlocks;
}

// ReadSounds and WaveArchiveNames by the defaults, as Parse reads, and by the strict rules.
void TestLightReaders()
{
    const std::vector<uint8_t> baa = TableArchive(ThreeSounds());

    const std::vector<jaudio::SoundEntry> strict = jaudio::ReadSounds(baa, Strict());
    Check(strict.size() == 3, "the strict rules read the three sounds");
    if (strict.size() == 3)
    {
        Check(strict[0].name == "SE_A" && strict[0].section_name == "SE" && strict[0].group_name == "SYSTEM" &&
                  strict[0].volume == 100 && strict[0].Id() == 0,
              "the strict rules give the section's and group's names");
        Check(strict[2].Id() == 0x01000000 && strict[2].type == 0x11 && strict[2].section_name == "BGM" &&
                  strict[2].group_name == "FIELD" && strict[2].resource == 0,
              "the strict rules give no sources");
    }

    const std::vector<jaudio::SoundEntry> by_default = jaudio::ReadSounds(baa);
    Check(by_default.size() == 3 && by_default[2].resource == 7 && by_default[2].section_name.empty() &&
              by_default[1].name == "SE_B",
          "the defaults read the table as Parse does");
    const jaudio::SoundArchive archive = jaudio::SoundArchive::Parse(baa);
    const std::vector<jaudio::SoundEntry>& parsed = archive.Sounds();
    Check(strict.size() == 3 && by_default.size() == 3 && parsed.size() == 3 && parsed[2].offset > 0 &&
              strict[2].offset == parsed[2].offset && by_default[2].offset == parsed[2].offset,
          "the strict rules and the defaults give the entries' offsets as Parse does");
    Check(jaudio::ReadSounds(files::StartUpArchive(files::BankV0File(0, 100), {})).empty(),
          "by default, an archive without a sound table has no sounds");

    const std::vector<std::string> kOne = {"test.aw"};
    Check(jaudio::WaveArchiveNames(baa, Strict()) == kOne, "two wave systems name one archive, given once");
    Check(jaudio::WaveArchiveNames(baa) == kOne, "the archive names by default");
}

// Each strict rule for the sound table, against the defaults where they differ.
void TestStrictRules()
{
    const jaudio::BaaOptions strict = Strict();

    // A section at offset 0 is skipped, and the others keep their numbers. The root's slots start at 0x14.
    files::SoundTableBlocks holes = ThreeSounds();
    xsflib::StoreBe32(holes.table.data() + 0x14, 0);
    const std::vector<jaudio::SoundEntry> skipped = jaudio::ReadSounds(TableArchive(holes), strict);
    Check(skipped.size() == 1 && skipped[0].Id() == 0x01000000 && skipped[0].name == "BGM_FIELD",
          "a section at offset 0 is skipped");

    // A name only where the name table's counts reach: a name table with one sound fewer.
    files::SoundTableBlocks fewer = ThreeSounds();
    fewer.names = files::SoundTable({
                                        {"SE", {{"SYSTEM", {{0x10, 100, 0, "SE_A"}}}}},
                                        {"BGM", {{"FIELD", {{0x11, 90, 7, "BGM_FIELD"}}}}},
                                    })
                      .names;
    const std::vector<jaudio::SoundEntry> counted = jaudio::ReadSounds(TableArchive(fewer), strict);
    Check(counted.size() == 3 && counted[0].name == "SE_A" && counted[1].name.empty() && counted[2].name == "BGM_FIELD",
          "a sound past the name table's count has no name");

    // At most 0x100 sections: a count of 0x101 whose last slot lies at the end of the data.
    files::Bytes t;
    t.Tag("BST ").U32(0).U32(0).U32(0x30);    // the root at 0x30
    t.U32(1).U32(0x18);                       // 0x10: a section of one group, at 0x18
    t.U32(1).U32(0).U32(0x10u << 24 | 0x24);  // 0x18: a group of one sound, its entry at 0x24
    t.U8(0).U8(0).U8(64).U8(0).U32(0).U32(0); // 0x24: the entry, with volume 64
    t.U32(0x101).U32(0x10);                   // 0x30: the root
    for (int s = 1; s < 0x100; s++)
    {
        t.U32(0);
    }

    const std::vector<uint8_t> many = TableArchive({t.Data(), {}}, false);
    const std::vector<jaudio::SoundEntry> capped = jaudio::ReadSounds(many, strict);
    Check(capped.size() == 1 && capped[0].volume == 64 && capped[0].name.empty(), "at most 0x100 sections");
    jaudio::BaaOptions uncapped = strict;
    uncapped.cap_counts = false;
    Check(ErrorOf([&] { jaudio::ReadSounds(many, uncapped); }) == "the BAA archive is damaged",
          "without the cap, the 0x101st slot lies past the end");

    // The tables reach to the end of the archive: a table block of 4 bytes, and one that ends before it starts.
    const std::vector<uint8_t> baa = TableArchive(ThreeSounds());
    const uint32_t table_at = xsflib::LoadBe32(baa.data() + kTableCommand + 4);
    std::vector<uint8_t> cut = baa;
    xsflib::StoreBe32(cut.data() + kTableCommand + 8, table_at + 4);
    Check(jaudio::ReadSounds(cut, strict).size() == 3, "the strict rules read past the table block's end");
    Check(ErrorOf([&] { jaudio::ReadSounds(cut); }) == "read out of bounds at offset 12",
          "by default, the table is read inside its block");
    std::vector<uint8_t> backwards = baa;
    xsflib::StoreBe32(backwards.data() + kTableCommand + 8, 0);
    Check(jaudio::ReadSounds(backwards, strict).size() == 3, "the strict rules don't check a block's end");
    Check(ErrorOf([&] { jaudio::ReadSounds(backwards); }) == "archive has a chunk that ends before it starts",
          "by default, a block that ends before it starts is an error");

    // A name whose offset is the end of the data. The first sound's name is the first group's first slot.
    std::vector<uint8_t> at_end = baa;
    const uint32_t names_at = xsflib::LoadBe32(baa.data() + kNamesCommand + 4);
    const uint32_t section = xsflib::LoadBe32(baa.data() + names_at + 0x14);
    const uint32_t group = xsflib::LoadBe32(baa.data() + names_at + section + 8);
    xsflib::StoreBe32(at_end.data() + names_at + group + 8, static_cast<uint32_t>(baa.size() - names_at));
    Check(ErrorOf([&] { jaudio::ReadSounds(at_end, strict); }) == "the BAA archive is damaged",
          "the strict rules reject a name at the end of the data");
    const std::vector<jaudio::SoundEntry> empty_name = jaudio::ReadSounds(at_end);
    Check(empty_name.size() == 3 && empty_name[0].name.empty(), "by default, a name at the end of the data is empty");
}

// The strict rules' messages for the start-up archive.
void TestStrictMessages()
{
    const jaudio::BaaOptions strict = Strict();
    const auto sounds = [&](const std::vector<uint8_t>& baa)
    {
        return ErrorOf([&] { jaudio::ReadSounds(baa, strict); });
    };
    const auto names = [&](const std::vector<uint8_t>& baa)
    {
        return ErrorOf([&] { jaudio::WaveArchiveNames(baa, strict); });
    };

    Check(sounds({'A', 'A'}) == "the BAA archive is damaged", "data shorter than a word");
    Check(sounds({'A', 'A', '_', '<'}) == "the BAA archive is damaged", "a command list cut short");
    Check(sounds(std::vector<uint8_t>(8)) == "it isn't a BAA archive", "data without AA_<");

    const std::vector<uint8_t> baa = TableArchive(ThreeSounds());
    std::vector<uint8_t> unknown = baa;
    unknown[4] = 'x';
    Check(sounds(unknown) == "the BAA archive has a command that isn't known", "a command that isn't known");
    Check(names(unknown) == "the BAA archive has a command that isn't known", "WaveArchiveNames walks the same list");
    Check(sounds(files::StartUpArchive(files::BankV0File(0, 100), {})) == "the BAA archive has no sound table",
          "an archive without a sound table");

    files::SoundTableBlocks bad_table = ThreeSounds();
    bad_table.table[0] = 'X';
    Check(sounds(TableArchive(bad_table)) == "the BAA archive's sound table is damaged", "a table without BST");
    files::SoundTableBlocks bad_names = ThreeSounds();
    bad_names.names[0] = 'X';
    Check(sounds(TableArchive(bad_names)) == "the BAA archive's sound table is damaged", "a name table without BSTN");
    Check(sounds(TableArchive(files::SoundTable({}))) == "the BAA archive's sound table lists no sounds",
          "a table without sounds");

    // The wave system at the start of the blocks gives its WINF block at +0x20.
    const uint32_t ws_at = xsflib::LoadBe32(baa.data() + 4 + 8);
    std::vector<uint8_t> bad_wsys = baa;
    bad_wsys[ws_at] = 'X';
    std::vector<uint8_t> bad_winf = baa;
    bad_winf[ws_at + 0x20] = 'X';
    Check(names(bad_wsys) == "a wave system in the BAA archive is damaged", "a wave system without WSYS");
    Check(names(bad_winf) == "a wave system in the BAA archive is damaged", "a wave system without WINF");
    Check(ErrorOf([&] { jaudio::WaveArchiveNames(bad_winf); }) == "wave system is damaged",
          "the default message for a damaged wave system");
}

// The text of every field that the readers give for a sound, a wave system or a bank, to compare what two files give.
std::string Describe(const jaudio::SoundEntry& s)
{
    return "sound " + std::to_string(s.Id()) + " " + s.name + " " + std::to_string(s.type) + " " +
           std::to_string(s.volume) + " " + std::to_string(s.stream) + " " + s.section_name + " " + s.group_name + " " +
           std::to_string(static_cast<int>(s.source)) + " " + std::to_string(s.resource) + " " +
           std::to_string(std::bit_cast<uint32_t>(s.pitch)) + " " + std::to_string(s.offset) + "\n";
}

std::string Describe(const std::vector<jaudio::OscPoint>& points)
{
    std::string text;
    for (const jaudio::OscPoint& p : points)
    {
        text += " " + std::to_string(p.mode) + "," + std::to_string(p.time) + "," + std::to_string(p.value);
    }

    return text + "\n";
}

std::string Describe(const std::vector<jaudio::InstEffect>& effects)
{
    std::string text;
    for (const jaudio::InstEffect& e : effects)
    {
        text += " effect " + std::to_string(static_cast<int>(e.kind)) + " " + std::to_string(e.target) + " " +
                std::to_string(std::bit_cast<uint32_t>(e.base)) + " " +
                std::to_string(std::bit_cast<uint32_t>(e.width)) + " " + std::to_string(e.trigger) + " " +
                std::to_string(e.center) + " " + std::to_string(std::bit_cast<uint32_t>(e.low)) + " " +
                std::to_string(std::bit_cast<uint32_t>(e.high)) + "\n";
    }

    return text;
}

std::string Describe(const std::vector<jaudio::VelocityRegion>& velocities)
{
    std::string text;
    for (const jaudio::VelocityRegion& v : velocities)
    {
        text += "  velocity " + std::to_string(v.high_velocity) + " " + std::to_string(v.wave_id) + " " +
                std::to_string(std::bit_cast<uint32_t>(v.volume)) + " " +
                std::to_string(std::bit_cast<uint32_t>(v.pitch)) + "\n";
    }

    return text;
}

std::string Describe(const jaudio::Bank& bank)
{
    std::string text = "bank " + std::to_string(bank.number) + " " + std::to_string(bank.wave_system) + " " +
                       std::to_string(bank.version) + "\n";
    for (const jaudio::Oscillator& o : bank.oscillators)
    {
        text += " oscillator " + std::to_string(o.target) + " " + std::to_string(std::bit_cast<uint32_t>(o.rate)) +
                " " + std::to_string(o.release_is_attack) + " " + std::to_string(std::bit_cast<uint32_t>(o.scale)) +
                " " + std::to_string(std::bit_cast<uint32_t>(o.vertex)) + Describe(o.attack) + Describe(o.release);
    }

    for (const auto& [number, program] : bank.programs)
    {
        if (const auto* inst = std::get_if<jaudio::Instrument>(&program))
        {
            text += " instrument " + std::to_string(number) + " " +
                    std::to_string(std::bit_cast<uint32_t>(inst->volume)) + " " +
                    std::to_string(std::bit_cast<uint32_t>(inst->pitch)) + "\n";
            for (const std::size_t o : inst->oscillators)
            {
                text += " uses " + std::to_string(o) + "\n";
            }
            text += Describe(inst->effects);
            for (const jaudio::KeyRegion& key : inst->keys)
            {
                text += " key " + std::to_string(key.high_key) + "\n" + Describe(key.velocities);
            }
            continue;
        }

        text += " drums " + std::to_string(number) + "\n";
        const jaudio::DrumSet& drums = std::get<jaudio::DrumSet>(program);
        for (std::size_t key = 0; key < drums.keys.size(); key++)
        {
            if (const std::optional<jaudio::Drum>& d = drums.keys[key])
            {
                text += " drum " + std::to_string(key) + " " + std::to_string(std::bit_cast<uint32_t>(d->volume)) +
                        " " + std::to_string(std::bit_cast<uint32_t>(d->pitch)) + " " +
                        std::to_string(std::bit_cast<uint32_t>(d->pan)) + " " + std::to_string(d->release) + "\n" +
                        Describe(d->effects) + Describe(d->velocities);
            }
        }
    }

    return text;
}

// Everything that Parse gives for a start-up archive: its sounds, its wave systems and banks of numbers below 0x100,
// its collection's sequences and its warnings.
std::string Describe(const jaudio::SoundArchive& archive)
{
    std::string text;
    for (const jaudio::SoundEntry& sound : archive.Sounds())
    {
        text += Describe(sound);
    }

    for (uint32_t number = 0; number < 0x100; number++)
    {
        if (const jaudio::WaveSystem* ws = archive.FindWaveSystem(number))
        {
            text += Describe(*ws);
        }
        if (const jaudio::Bank* bank = archive.FindBank(number))
        {
            text += Describe(*bank);
        }
    }

    for (uint32_t category = 0; category < 0x100; category++)
    {
        for (uint32_t index = 0; archive.CollectionSequence(category, index); index++)
        {
            const std::span<const uint8_t> collection = archive.Collection();
            const uint32_t at = *archive.CollectionSequence(category, index);
            text += "sequence " + std::to_string(category) + " " + std::to_string(index) + " " +
                    std::to_string(at < collection.size() ? collection[at] : 0) + "\n";
        }
    }

    for (const std::string& warning : archive.Warnings())
    {
        text += "warning " + warning + "\n";
    }

    return text;
}

// The sounds of a table with both layouts of entries, and the names of its sections and groups.
files::SoundTableBlocks BothLayouts(bool big_endian)
{
    return files::SoundTable(
        {
            {"SE", {{"SYSTEM", {{0x10, 100, 0, "SE_A"}, {0x41, 90, 0, "SE_B"}}}}},
            {"BGM", {{"FIELD", {{0x11, 80, 7, "BGM_OLD"}, {0x62, 70, 0x123, "BGM_NEW"}}}}},
        },
        false, big_endian);
}

// A little-endian start-up archive gives what the big-endian one gives, by every reader.
void TestLittleEndianArchive()
{
    const std::vector<std::vector<uint8_t>> kSequences = {{0xff}, {0xfe, 0xff}};
    const std::vector<uint8_t> be =
        files::StartUpArchive(files::BankV1File(129, 0, 600), kSequences, BothLayouts(true));
    const std::vector<uint8_t> le =
        files::StartUpArchive(files::BankV1File(129, 0, 600, false), kSequences, BothLayouts(false), false);

    const std::string from_be = Describe(jaudio::SoundArchive::Parse(be));
    const std::string from_le = Describe(jaudio::SoundArchive::Parse(le));

    Check(le != be && le.size() == be.size() && le[0] == '<' && le[3] == 'A', "the archive is little-endian");
    Check(from_be.find("sound 16777217 BGM_NEW") != std::string::npos && from_be.find("key 64") != std::string::npos,
          "the big-endian archive's sounds and bank: " + from_be);
    Check(from_le == from_be, "the little-endian archive gives the same sounds, wave system, bank and sequences");

    const auto sounds = [](const std::vector<uint8_t>& baa, const jaudio::BaaOptions& options)
    {
        std::string text;
        for (const jaudio::SoundEntry& sound : jaudio::ReadSounds(baa, options))
        {
            text += Describe(sound);
        }

        return text;
    };
    jaudio::BaaOptions strict;
    strict.check_magic = true;
    strict.skip_empty = true;
    strict.cap_counts = true;
    strict.check_name_counts = true;
    strict.names_inside = true;
    strict.section_names = true;
    Check(sounds(le, {}) == sounds(be, {}) && sounds(le, strict) == sounds(be, strict) &&
              sounds(be, strict).find("SYSTEM") != std::string::npos,
          "ReadSounds reads both archives alike, by the defaults and by strict rules");
    Check(jaudio::WaveArchiveNames(le) == std::vector<std::string>{"test.aw"} &&
              jaudio::WaveArchiveNames(le, strict) == jaudio::WaveArchiveNames(be, strict),
          "WaveArchiveNames reads the little-endian wave system");
    const std::vector<jaudio::WaveSystem> systems = jaudio::ReadWaveSystems(le);
    Check(systems.size() == 1 && Describe(systems[0]) == Describe(jaudio::ReadWaveSystems(be).at(0)),
          "ReadWaveSystems reads the little-endian wave system");
}

// A little-endian RARC archive, plain and compressed with Yaz0, gives the files of the big-endian one.
void TestLittleEndianRarc()
{
    const std::vector<files::RarcFileSpec> kFiles = {{3, "a.bms", {1, 2, 3}}, {9, "long_name.bms", {4, 5, 6, 7, 8}}};
    const std::vector<uint8_t> be = files::RarcFile(kFiles);
    const std::vector<uint8_t> le = files::RarcFile(kFiles, false);

    const auto text = [](std::span<const uint8_t> rarc)
    {
        std::string out;
        for (const jaudio::ArchiveFile& file : jaudio::ReadRarc(rarc))
        {
            out += std::to_string(file.id) + " " + file.name + " " + Text(file.data) + "\n";
        }

        return out;
    };

    Check(le != be && le[0] == 'C' && le[3] == 'R', "the archive is little-endian");
    Check(text(le) == text(be) && jaudio::ReadRarc(le).size() == 2, "the little-endian archive gives the same files");
    Check(text(jaudio::DecodeYaz0(files::LiteralYaz0(le))) == text(be), "so does the archive compressed with Yaz0");

    // The Switch's release of Pikmin 2 has wide entries of 24 bytes.
    const std::vector<uint8_t> wide = files::RarcFile(kFiles, false, jaudio::kWideRarcEntrySize);
    std::string from_wide;
    for (const jaudio::ArchiveFile& file : jaudio::ReadRarc(wide, jaudio::kWideRarcEntrySize))
    {
        from_wide += std::to_string(file.id) + " " + file.name + " " + Text(file.data) + "\n";
    }
    Check(wide != le && from_wide == text(be), "the archive with wide entries gives the same files");
}

// A little-endian bank of either version gives what the big-endian one gives, with its tags in either order.
void TestLittleEndianBanks()
{
    const std::vector<uint8_t> be = files::BankV1File(129, 0, 600);
    const std::vector<uint8_t> le = files::BankV1File(129, 0, 600, false);
    const std::string from_be = Describe(jaudio::Bank::Parse(be, 4));

    Check(le != be && Describe(jaudio::Bank::Parse(le, 4)) == from_be, "the little-endian bank of version 1");

    // The bank with its oscillator's tag in the big-endian order.
    std::vector<uint8_t> kept = le;
    const std::size_t osci = std::string(kept.begin(), kept.end()).find("icsO");
    Check(osci != std::string::npos, "the little-endian bank has its oscillator");
    if (osci != std::string::npos)
    {
        kept[osci] = 'O';
        kept[osci + 1] = 's';
        kept[osci + 2] = 'c';
        kept[osci + 3] = 'i';
        Check(Describe(jaudio::Bank::Parse(kept, 4)) == from_be, "a little-endian bank's tag can keep its order");
    }

    // A version 0 bank. The little-endian one holds its drum table at 0x544. The Switch's release of Pikmin 2 does too.
    const std::vector<uint8_t> v0_be = files::BankV0File(7, 100);
    const std::vector<uint8_t> v0_le = files::BankV0File(7, 100, false);
    const std::string from_v0 = Describe(jaudio::Bank::Parse(v0_be, 0));
    Check(from_v0.find("drums 228") != std::string::npos && from_v0.find("drum 36") != std::string::npos,
          "the big-endian bank of version 0 has its drum set: " + from_v0);
    Check(v0_le.size() == v0_be.size() + 0x190 && xsflib::LoadLe32(v0_le.data() + 0x544) != 0,
          "the little-endian bank's drum table is at 0x544");
    Check(Describe(jaudio::Bank::Parse(v0_le, 0)) == from_v0, "the little-endian bank of version 0");
}

// A little-endian wave system gives what the big-endian one gives.
void TestLittleEndianWaveSystem()
{
    const std::vector<uint8_t> be = files::WaveSystemFile(5, 3, "other.aw");
    const std::vector<uint8_t> le = files::WaveSystemFile(5, 3, "other.aw", false);

    const std::string from_be = Describe(jaudio::ReadWaveSystem(be, 5));

    Check(le != be && le[0] == 'S' && le[3] == 'W', "the wave system is little-endian");
    Check(from_be.find("group other.aw 0 1 2") != std::string::npos, "the big-endian wave system: " + from_be);
    Check(Describe(jaudio::ReadWaveSystem(le, 5)) == from_be, "the little-endian wave system");

    // The Switch's release of Pikmin 2 has wide records of the archives.
    const std::vector<uint8_t> wide = files::WaveSystemFile(5, 3, "other.aw", false, jaudio::kWideWaveCountAt);
    Check(wide.size() == le.size() + 0x1c &&
              Describe(jaudio::ReadWaveSystem(wide, 5, jaudio::kWideWaveCountAt)) == from_be,
          "the little-endian wave system with wide records");
}

// A version 0 bank with 64-bit offsets gives what the big-endian bank gives. The Switch's release of Pikmin keeps its
// banks that way: the tag and the size in the big-endian order, the BANK chunk's tag reversed, the other fields
// little-endian, and the drum table at 0x744.
void TestWideOffsetBank()
{
    const std::vector<uint8_t> be = files::BankV0File(7, 100);
    const std::vector<uint8_t> wide = files::BankV0File(7, 100, false, 8);

    const std::string from_be = Describe(jaudio::Bank::Parse(be, 3));
    const std::string from_wide = Describe(jaudio::Bank::Parse(wide, 3));

    Check(std::string(wide.begin(), wide.begin() + 4) == "IBNK" && xsflib::LoadBe32(wide.data() + 4) == be.size() &&
              xsflib::LoadLe32(wide.data() + 8) == 7 &&
              std::string(wide.begin() + 0x20, wide.begin() + 0x24) == "KNAB" &&
              xsflib::LoadLe32(wide.data() + 0x744) != 0 && wide.size() > be.size(),
          "the bank has the layout of the Switch's release of Pikmin");
    Check(from_be.find("drum 36") != std::string::npos && from_wide == from_be,
          "the bank with 64-bit offsets gives what the big-endian bank gives: " + from_wide);

    // An offset whose high half isn't 0: the instrument's.
    std::vector<uint8_t> far = wide;
    far[0x24 + 4] = 1;
    Check(ErrorOf([&] { jaudio::Bank::Parse(far, 3); }) == "a 64-bit offset is out of range",
          "a 64-bit offset past 4 GiB");
}

// An instrument's flags are the low byte of the word at +4 of its record, in each layout of a version 0 bank. A version
// 1 bank leaves them 0.
void TestInstrumentFlags()
{
    const auto flags = [](const std::vector<uint8_t>& file)
    {
        const jaudio::Bank bank = jaudio::Bank::Parse(file, 0);
        const auto* inst = bank.programs.contains(0) ? std::get_if<jaudio::Instrument>(&bank.programs.at(0)) : nullptr;
        return inst ? int{inst->flags} : -1;
    };

    Check(flags(files::BankV0File(7, 100)) == 0, "an instrument without flags");
    Check(flags(files::BankV0File(7, 100, true, 4, 0, 0x2c)) == 0x2c, "the big-endian bank's flags");
    Check(flags(files::BankV0File(7, 100, false, 4, 0, 0x2c)) == 0x2c, "the little-endian bank's flags");
    Check(flags(files::BankV0File(7, 100, false, 8, 0, 0x2c)) == 0x2c, "the flags of a bank with 64-bit offsets");
    Check(flags(files::BankV0File(7, 100, true, 4, 0, 0x1a5)) == 0xa5, "the flags are the word's low byte");
    Check(flags(files::BankV1File(129, 0, 600)) == 0, "a version 1 bank's instrument has no flags");

    // The flags change none of the other fields.
    Check(Describe(jaudio::Bank::Parse(files::BankV0File(7, 100, false, 8, 0, 0x2c), 0)) ==
              Describe(jaudio::Bank::Parse(files::BankV0File(7, 100), 0)),
          "the bank with flags gives the other fields of the bank without them");
}

// A wave system with 64-bit offsets gives what the big-endian system gives. The Switch's release of Pikmin keeps its
// wave systems that way, with wide records of the archives.
void TestWideOffsetWaveSystem()
{
    const std::vector<uint8_t> be = files::WaveSystemFile(5, 3, "other.aw");
    const std::vector<uint8_t> wide = files::WaveSystemFile(5, 3, "other.aw", false, jaudio::kWideWaveCountAt, 8);

    const std::string from_be = Describe(jaudio::ReadWaveSystem(be, 5));
    const std::string from_wide = Describe(jaudio::ReadWaveSystem(wide, 5, jaudio::kWideWaveCountAt));

    const uint32_t winf = xsflib::LoadLe32(wide.data() + 0x10);
    Check(std::string(wide.begin(), wide.begin() + 4) == "WSYS" && xsflib::LoadBe32(wide.data() + 4) == be.size() &&
              xsflib::LoadLe32(wide.data() + 0x14) == 0 && winf + 4 <= wide.size() &&
              std::string(wide.begin() + winf, wide.begin() + winf + 4) == "FNIW",
          "the wave system has the layout of the Switch's release of Pikmin");
    Check(from_be.find("group other.aw 0 1 2") != std::string::npos && from_wide == from_be,
          "the wave system with 64-bit offsets gives what the big-endian system gives: " + from_wide);
}

// The wave system of WaveSystemFile with `scene`, of three waves, in a layout: 0 big-endian, 1 the little-endian one of
// the Switch's release of Pikmin 2, 2 the one with 64-bit offsets of the Switch's release of Pikmin.
std::vector<uint8_t> SceneFile(const files::SceneSpec& scene, int layout)
{
    const std::size_t wave_count_at = layout == 0 ? jaudio::kWaveCountAt : jaudio::kWideWaveCountAt;

    return files::WaveSystemFile(5, 3, "other.aw", layout == 0, wave_count_at, layout == 2 ? 8 : 4, scene);
}

jaudio::WaveSystem ReadSceneFile(const files::SceneSpec& scene, int layout)
{
    return jaudio::ReadWaveSystem(SceneFile(scene, layout), 5,
                                  layout == 0 ? jaudio::kWaveCountAt : jaudio::kWideWaveCountAt);
}

// A scene with an external mode, three dependencies, and words in C-EX and C-ST.
files::SceneSpec FullScene()
{
    files::SceneSpec scene;
    scene.external_mode = 3;
    scene.dependencies = {7, 2, 3};
    scene.cex = std::vector<uint32_t>{0x00010005, 0x00020001};
    scene.cst = std::vector<uint32_t>{9};

    return scene;
}

// A wave system's group holds its scene with the values that the system stores, and every wave of its archive. A list
// whose offset is null has no value. Without C-DF, the archive's waves have no ids. Each layout gives the same.
void TestWaveScenes()
{
    files::SceneSpec null_lists;
    null_lists.cex = std::nullopt;
    null_lists.cst = std::nullopt;
    files::SceneSpec null_cdf;
    null_cdf.null_cdf = true;

    const jaudio::WaveSystem plain = ReadSceneFile({}, 0);
    const jaudio::WaveSystem full = ReadSceneFile(FullScene(), 0);
    const jaudio::WaveSystem without_lists = ReadSceneFile(null_lists, 0);
    const jaudio::WaveSystem without_cdf = ReadSceneFile(null_cdf, 0);

    Check(plain.groups.size() == 1 && full.groups.size() == 1 && without_lists.groups.size() == 1 &&
              without_cdf.groups.size() == 1,
          "each system has one group");
    if (plain.groups.size() != 1 || full.groups.size() != 1 || without_lists.groups.size() != 1 ||
        without_cdf.groups.size() != 1)
    {
        return;
    }

    const std::vector<uint32_t> kWords = {0x50000, 0x50001, 0x50002};
    const jaudio::WaveGroup& group = plain.groups[0];
    Check(group.scene.external_mode == 0 && group.scene.dependencies.empty() && group.scene.cdf == kWords &&
              group.scene.cex == std::vector<uint32_t>() && group.scene.cst == std::vector<uint32_t>(),
          "a scene with no dependencies and empty lists C-EX and C-ST: " + Describe(group.scene));
    Check(group.archive_waves.size() == 3 && group.archive_waves[0].loop && !group.archive_waves[2].loop &&
              group.archive_waves[2].offset == 256 && group.archive_waves[2].archive == "other.aw" &&
              Describe(group.archive_waves[1]) == Describe(plain.waves.at(1)),
          "the group holds every wave of its archive");

    const jaudio::WaveScene& scene = full.groups[0].scene;
    Check(scene.external_mode == 3 && scene.dependencies == std::vector<uint32_t>{7, 2, 3} && scene.cdf == kWords &&
              scene.cex == std::vector<uint32_t>{0x00010005, 0x00020001} && scene.cst == std::vector<uint32_t>{9},
          "the scene's external mode, dependencies and lists: " + Describe(scene));
    Check(full.groups[0].waves == std::vector<uint16_t>{0, 1, 2} && full.waves.size() == 3,
          "C-DF gives the archive's waves their ids");
    Check(without_lists.groups[0].scene.cdf == kWords && !without_lists.groups[0].scene.cex &&
              !without_lists.groups[0].scene.cst,
          "lists with null offsets have no value");
    Check(!without_cdf.groups[0].scene.cdf && without_cdf.groups[0].scene.cex == std::vector<uint32_t>() &&
              without_cdf.groups[0].waves.empty() && without_cdf.waves.empty() &&
              without_cdf.groups[0].archive_waves.size() == 3,
          "without C-DF, the archive's waves have no ids");

    for (const files::SceneSpec& spec : {files::SceneSpec(), FullScene(), null_lists, null_cdf})
    {
        const std::string from_be = Describe(ReadSceneFile(spec, 0));
        Check(Describe(ReadSceneFile(spec, 1)) == from_be && Describe(ReadSceneFile(spec, 2)) == from_be,
              "each layout gives what the big-endian system gives: " + from_be);
    }
}

// A list has no value if its offset doesn't point at the list's tag. The Switch's release of Pikmin 2 has such
// offsets. Here C-EX's tag is overwritten, and C-ST's offset points at the WINF chunk.
void TestSceneListsElsewhere()
{
    std::vector<uint8_t> file = SceneFile(FullScene(), 0);
    const std::vector<uint8_t> kScene = {'S', 'C', 'N', 'E'};
    const std::vector<uint8_t> kCex = {'C', '-', 'E', 'X'};
    const auto scene = std::search(file.begin(), file.end(), kScene.begin(), kScene.end());
    const auto cex = std::search(file.begin(), file.end(), kCex.begin(), kCex.end());
    Check(scene != file.end() && cex != file.end(), "the system has its scene and C-EX");
    if (scene == file.end() || cex == file.end())
    {
        return;
    }

    std::fill_n(cex, 4, 0);
    xsflib::StoreBe32(&*scene + 0x14, xsflib::LoadBe32(file.data() + 0x10));

    const jaudio::WaveSystem ws = jaudio::ReadWaveSystem(file, 5);

    Check(ws.groups.size() == 1 && ws.groups[0].scene.cdf == std::vector<uint32_t>{0x50000, 0x50001, 0x50002} &&
              !ws.groups[0].scene.cex && !ws.groups[0].scene.cst &&
              ws.groups[0].waves == std::vector<uint16_t>{0, 1, 2},
          "lists whose offsets don't point at their tags have no value");
}

// A scene with 64-bit offsets has the layout of the Switch's release of Pikmin: the tag reversed, the mode, the number
// of dependencies, the offsets of the lists in 8 bytes each, then the dependencies.
void TestWideScene()
{
    const std::vector<uint8_t> file = SceneFile(FullScene(), 2);
    const std::vector<uint8_t> kTag = {'E', 'N', 'C', 'S'};

    const auto tag = std::search(file.begin(), file.end(), kTag.begin(), kTag.end());
    const auto at = static_cast<std::size_t>(tag - file.begin());
    const auto word = [&file, at](std::size_t offset)
    {
        return xsflib::LoadLe32(file.data() + at + offset);
    };

    Check(tag != file.end() && at + 0x30 <= file.size() && word(4) == 3 && word(8) == 3 && word(0x10) == 0 &&
              word(0x18) == 0 && word(0x20) == 0 && word(0x24) == 7 && word(0x28) == 2 && word(0x2c) == 3,
          "the scene has the layout of the Switch's release of Pikmin");
}

// A bank file of four wave systems and four banks. Place 1 of the systems' table is empty. System 3 gives the number
// 0xffff, as two of Pikmin's do. Bank 1 plays the waves of system 0. Bank 3's waves name the system at the bank's
// place. A little-endian file has the layout of the Switch's release of Pikmin.
std::vector<uint8_t> BankFile(bool big_endian)
{
    const std::size_t width = big_endian ? 4 : 8;
    const std::size_t wave_count_at = big_endian ? jaudio::kWaveCountAt : jaudio::kWideWaveCountAt;
    const auto system = [&](uint32_t number, int waves, const std::string& archive)
    {
        return files::WaveSystemFile(number, waves, archive, big_endian, wave_count_at, width);
    };
    const auto bank = [&](uint32_t number, uint16_t wave_system)
    {
        return files::BankV0File(number, 100, big_endian, width, wave_system);
    };

    return files::BxFile({system(0, 2, "a.aw"), {}, system(2, 3, "c.aw"), system(0xffff, 2, "d.aw")},
                         {bank(0, 0), bank(1, 0), bank(2, 2), bank(3, 0xffff)}, big_endian);
}

// Everything that a bank file's readers give, and their warnings.
std::string DescribeBankFile(std::span<const uint8_t> bx)
{
    std::string text;
    for (const jaudio::WaveSystem& ws : jaudio::ReadBxWaveSystems(bx))
    {
        text += Describe(ws);
    }

    std::vector<std::string> warnings;
    for (const jaudio::Bank& bank : jaudio::ReadBxBanks(bx, warnings))
    {
        text += Describe(bank);
    }
    for (const std::string& warning : warnings)
    {
        text += "warning " + warning + "\n";
    }

    return text;
}

// A bank file gives its wave systems by their places, and each bank with the place of the system that its waves name.
// The little-endian file gives what the big-endian one gives.
void TestBankFile()
{
    const std::vector<uint8_t> be = BankFile(true);
    const std::vector<uint8_t> le = BankFile(false);

    const std::vector<jaudio::WaveSystem> systems = jaudio::ReadBxWaveSystems(be);
    std::vector<std::string> warnings;
    const std::vector<jaudio::Bank> banks = jaudio::ReadBxBanks(be, warnings);
    const std::string from_le = DescribeBankFile(le);

    Check(systems.size() == 3 &&
              Describe(systems[0]) == Describe(jaudio::ReadWaveSystem(files::WaveSystemFile(0, 2, "a.aw"), 0)) &&
              Describe(systems[1]) == Describe(jaudio::ReadWaveSystem(files::WaveSystemFile(2, 3, "c.aw"), 2)) &&
              Describe(systems[2]) == Describe(jaudio::ReadWaveSystem(files::WaveSystemFile(0xffff, 2, "d.aw"), 3)),
          "the wave systems take their places in the table");
    const std::vector<uint32_t> kPlays = {0, 0, 2, 3};
    bool banks_match = banks.size() == kPlays.size() && warnings.empty();
    for (std::size_t i = 0; banks_match && i < banks.size(); i++)
    {
        const auto number = static_cast<uint32_t>(i);
        const std::vector<uint8_t> alone = files::BankV0File(number, 100);
        banks_match = Describe(banks[i]) == Describe(jaudio::Bank::Parse(alone, kPlays[i]));
    }
    Check(banks_match, "each bank plays the system that its waves name");
    Check(from_le == DescribeBankFile(be), "the little-endian file gives what the big-endian file gives: " + from_le);
}

// A bank whose waves name a system that the file lacks, or more than one system, is left out with a warning. The first
// system that gives a number takes it. The numbers above 0xff name no system.
void TestBankFileRefusals()
{
    const auto system = [](uint32_t number)
    {
        return files::WaveSystemFile(number, 2);
    };
    const auto bank = [](uint32_t number, uint16_t wave_system)
    {
        return files::BankV0File(number, 100, true, 4, wave_system);
    };

    // Bank 4's drum plays wave 1 of system 0. Its instrument plays from system 2.
    std::vector<uint8_t> two_systems = bank(4, 2);
    const std::vector<uint8_t> kDrumRegion = {0x7f, 0, 0, 0, 0, 2, 0, 1, 0x3f, 0, 0, 0};
    const auto drum = std::search(two_systems.begin(), two_systems.end(), kDrumRegion.begin(), kDrumRegion.end());
    Check(drum != two_systems.end(), "the bank has its drum's velocity region");
    if (drum != two_systems.end())
    {
        drum[5] = 0;
    }

    const std::vector<uint8_t> bx =
        files::BxFile({system(0), {}, system(2), system(0x100), system(2)},
                      {bank(0, 5), bank(1, 0xffff), bank(2, 2), bank(3, 0x100), two_systems});

    std::vector<std::string> warnings;
    const std::vector<jaudio::Bank> banks = jaudio::ReadBxBanks(bx, warnings);

    Check(banks.size() == 1 && banks[0].number == 2 && banks[0].wave_system == 2,
          "only bank 2 is read, and it plays the first system that gives the number 2");
    const std::string kLacks = ": bank plays a wave system that the file lacks";
    Check(warnings.size() == 4 && warnings[0].starts_with("bank at offset ") && warnings[0].ends_with(kLacks) &&
              warnings[1].ends_with(kLacks) && warnings[2].ends_with(kLacks) &&
              warnings[3].ends_with(": bank plays the waves of more than one wave system"),
          "a warning for each bank that is left out");

    // A header whose table of wave systems lies past the end of the file.
    std::vector<uint8_t> cut(16);
    cut[3] = 0x10;
    cut[7] = 1;
    Check(ErrorOf([&] { jaudio::ReadBxWaveSystems(cut); }) == "the bank file is damaged" &&
              ErrorOf([&] { jaudio::ReadBxBanks(cut, warnings); }) == "the bank file is damaged",
          "a file cut short");
}

// Three sequences, and where they lie in a file of sequences of 0x100 bytes.
const std::vector<files::AafSequenceSpec> kSequences = {
    {"se.bms", 0, 0x40}, {"field.com", 0x40, 0x80}, {"VeryLongNameOfAPiece.com", 0xc0, 0x40}};

// Each sequence that an index gives: its id, its name, and its data's offset in `file` and size.
std::string DescribeSequences(const std::vector<jaudio::ArchiveFile>& sequences, std::span<const uint8_t> file)
{
    std::string text;
    for (const jaudio::ArchiveFile& sequence : sequences)
    {
        text += std::to_string(sequence.id) + " " + sequence.name + " " +
                std::to_string(sequence.data.data() - file.data()) + " " + std::to_string(sequence.data.size()) + "\n";
    }

    return text;
}

// An index gives each sequence's number, its name and its data in the file of sequences. The little-endian index of
// the Switch's layout gives the same.
void TestSequenceIndex()
{
    const std::vector<uint8_t> file(0x100);
    const std::vector<uint8_t> be = files::BarcFile(kSequences);
    const std::vector<uint8_t> le = files::BarcFile(kSequences, "test.arc", false, 8);

    const std::string kSequenceList = "0 se.bms 0 64\n1 field.com 64 128\n2 VeryLongNameO 192 64\n";
    Check(DescribeSequences(jaudio::ReadBarcSequenceFiles(be, file), file) == kSequenceList, "a big-endian index");
    Check(DescribeSequences(jaudio::ReadBarcSequenceFiles(le, file, false), file) == kSequenceList,
          "a little-endian index");

    const auto refusal = [&file](std::span<const uint8_t> index, std::size_t file_size)
    {
        return ErrorOf([&] { jaudio::ReadBarcSequenceFiles(index, std::span(file).first(file_size)); });
    };

    std::vector<uint8_t> not_index = be;
    not_index[0] = 'X';
    std::vector<uint8_t> too_many = be;
    too_many[13] = 1;
    Check(refusal(be, 0xff) == "the sequence index names a sequence outside the sequence archive",
          "a sequence that ends past the file");
    Check(refusal(not_index, 0x100) == "the sequence index is damaged", "an index without its tag");
    Check(refusal(too_many, 0x100) == "the sequence index is damaged", "an index of more than 0x10000 sequences");
    Check(refusal(std::span(be).first(be.size() - 1), 0x100) == "the sequence index is damaged", "an index cut short");
}

// Sets the sequence whose data entry `number` of a sequence index shares: the 16 bits after the entry's name.
void SetShare(std::vector<uint8_t>& index, std::size_t entries_at, bool big_endian, uint16_t number, uint16_t shares)
{
    const std::size_t at = entries_at + std::size_t{number} * 0x20 + 14;
    index[at] = static_cast<uint8_t>(big_endian ? shares >> 8 : shares);
    index[at + 1] = static_cast<uint8_t>(big_endian ? shares : shares >> 8);
}

// An entry can share the data of another sequence, which can share the data of another. The game reads the data of the
// chain's last sequence, the one that shares no other sequence's data (0x27a8a0 in the main of the Switch's release of
// Pikmin). Each sequence's data_id gives that sequence. The sequence keeps the data that its entry gives. A chain that
// loops, or that names a sequence past the index's end, is refused.
void TestSharedSequences()
{
    const std::vector<uint8_t> file(0x100);
    const std::vector<files::AafSequenceSpec> sequences = {
        {"a.bms", 0, 0x40}, {"b.bms", 0x40, 0}, {"c.bms", 0x40, 0}, {"d.bms", 0x40, 0x40}};
    const auto data_ids = [&file](std::span<const uint8_t> index, bool big_endian)
    {
        std::string text;
        for (const jaudio::ArchiveFile& sequence : jaudio::ReadBarcSequenceFiles(index, file, big_endian))
        {
            text += std::to_string(sequence.id) + ":" +
                    (sequence.data_id ? std::to_string(*sequence.data_id) : std::string("none")) + " " +
                    std::to_string(sequence.data.size()) + " ";
        }

        return text;
    };

    // Sequence 2 shares sequence 1's data, and sequence 1 shares sequence 0's. Both read sequence 0's data. In the
    // Switch's little-endian layout too, whose entries start at 0x24.
    for (const bool big_endian : {true, false})
    {
        std::vector<uint8_t> index = files::BarcFile(sequences, "test.arc", big_endian, big_endian ? 4 : 8);
        const std::size_t entries_at = big_endian ? 0x20 : 0x24;
        Check(data_ids(index, big_endian) == "0:0 64 1:1 0 2:2 0 3:3 64 ", "sequences that share no data");
        SetShare(index, entries_at, big_endian, 1, 0);
        SetShare(index, entries_at, big_endian, 2, 1);
        Check(data_ids(index, big_endian) == "0:0 64 1:0 0 2:0 0 3:3 64 ",
              std::string("a chain of shared data in a ") + (big_endian ? "big" : "little") + "-endian index");
    }

    const auto refusal = [&file](std::span<const uint8_t> index)
    {
        return ErrorOf([&] { jaudio::ReadBarcSequenceFiles(index, file); });
    };

    std::vector<uint8_t> loop = files::BarcFile(sequences);
    SetShare(loop, 0x20, true, 1, 2);
    SetShare(loop, 0x20, true, 2, 1);
    std::vector<uint8_t> self = files::BarcFile(sequences);
    SetShare(self, 0x20, true, 3, 3);
    std::vector<uint8_t> past = files::BarcFile(sequences);
    SetShare(past, 0x20, true, 1, 0);
    SetShare(past, 0x20, true, 3, 4);
    Check(refusal(loop) == "the sequence index shares data in a loop", "a loop of shared data");
    Check(refusal(self) == "the sequence index shares data in a loop", "a sequence that shares its data");
    Check(refusal(past) == "the sequence index shares the data of a sequence past its end",
          "a share of a sequence past the index's end");
}

// FindBarc finds the first index on a 4-byte boundary whose count is at most 0x10000 and whose entries fit.
void TestFindBarc()
{
    const std::vector<uint8_t> index = files::BarcFile(kSequences);
    std::vector<uint8_t> data(0x40);
    std::copy_n("BARC", 4, data.begin() + 2);
    std::copy_n("BARC", 4, data.begin() + 8);
    data[8 + 14] = 1;
    data.insert(data.end(), index.begin(), index.end());

    Check(jaudio::FindBarc(data) == 0x40, "the index after a tag off the boundary and a tag whose entries don't fit");
    Check(!jaudio::FindBarc(std::span(data).first(data.size() - 1)), "an index whose entries run past the data");
    Check(!jaudio::FindBarc(std::vector<uint8_t>(0x100)) && !jaudio::FindBarc(std::span(index).first(4)),
          "data without an index");

    const std::vector<uint8_t> le = files::BarcFile(kSequences, "test.arc", false, 8);
    std::vector<uint8_t> le_data(0x20);
    le_data.insert(le_data.end(), le.begin(), le.end());
    Check(jaudio::FindBarc(le_data, false) == 0x20, "a little-endian index");
}

} // namespace

int main()
{
    TestWaveSystem();
    TestBanks();
    TestBankChunkPastItsSize();
    TestWaveGroups();
    TestWaveArchiveLayout();
    TestJMapTable();
    TestSoundTable();
    TestLaterLayout();
    TestFirstLayouts();
    TestSequenceBlocksAndBaac();
    TestRarc();
    TestRepackSequenceTable();
    TestReadSequenceTable();
    TestMessages();
    TestLightReaders();
    TestStrictRules();
    TestStrictMessages();
    TestLittleEndianArchive();
    TestLittleEndianRarc();
    TestLittleEndianBanks();
    TestLittleEndianWaveSystem();
    TestWideOffsetBank();
    TestInstrumentFlags();
    TestWideOffsetWaveSystem();
    TestWaveScenes();
    TestSceneListsElsewhere();
    TestWideScene();
    TestBankFile();
    TestBankFileRefusals();
    TestSequenceIndex();
    TestSharedSequences();
    TestFindBarc();

    return xsflib::testing::Summary("jaudio");
}
