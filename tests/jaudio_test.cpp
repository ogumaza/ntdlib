// SPDX-License-Identifier: MIT

// Tests of the JAudio2 readers on files built here (ntdlib/testing/jaudio_files.h): the wave systems, the banks of both
// versions, the sound table with its names, the sequence collection, RARC archives, and the readers' messages, by the
// defaults and by strict rules.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/testing/jaudio_files.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace jaudio = ntdlib::jaudio;
namespace files = ntdlib::testing::jaudio_files;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

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

// WaveGroups on wave systems made here: for each system, the group that holds the most of the missing waves first.
void TestWaveGroups()
{
    jaudio::WaveSystem three;
    three.number = 3;
    three.groups = {{"a.aw", {1, 2, 3}}, {"b.aw", {3, 4}}, {"c.aw", {4, 5}}};
    jaudio::WaveSystem seven;
    seven.number = 7;
    seven.groups = {{"d.aw", {8}}, {"e.aw", {1}}};

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
        {"BGM", {{"FIELD", {{0x62, 70, 0x123, "BGM_FIELD"}, {0x11, 50, 7, "BGM_OLD"}}}}},
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
    const uint32_t table_at = 4 + 12 + 12 + 16 + 12 + 16 + 4;
    const auto names_at = static_cast<uint32_t>(table_at + blocks.table.size());
    const auto sequence_at = static_cast<uint32_t>(names_at + blocks.names.size());
    const uint32_t more_at = sequence_at + 4;
    const auto ws_at = static_cast<uint32_t>(more_at + more.Size());
    files::Bytes a;
    a.Tag("AA_<");
    a.Tag("bst ").U32(table_at).U32(names_at);
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

// A bank whose size ends inside its LIST chunk. Two of Twilight Princess's banks are like that. The game reads each
// chunk whole.
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

    // The same archive compressed with Yaz0, every byte as a literal: groups of eight chunks after a flag byte of 0xff.
    files::Bytes yaz0;
    yaz0.Tag("Yaz0").U32(static_cast<uint32_t>(rarc.size())).U32(0).U32(0);
    for (std::size_t i = 0; i < rarc.size(); i++)
    {
        if (i % 8 == 0)
        {
            yaz0.U8(0xff);
        }
        yaz0.U8(rarc[i]);
    }

    Check(jaudio::IsYaz0(yaz0.Data()) && !jaudio::IsYaz0(rarc), "IsYaz0 checks the magic");
    const std::vector<uint8_t> decoded = jaudio::DecodeYaz0(yaz0.Data());
    Check(decoded == rarc && jaudio::ReadRarc(decoded).size() == 2, "a Yaz0 archive decodes to the archive");
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
    files::Bytes yaz0;
    yaz0.Tag("Yaz0").U32(static_cast<uint32_t>(rarc.size())).U32(0).U32(0);
    for (std::size_t i = 0; i < rarc.size(); i++)
    {
        if (i % 8 == 0)
        {
            yaz0.U8(0xff);
        }
        yaz0.U8(rarc[i]);
    }

    Check(jaudio::RepackSequenceTable(yaz0.Data(), table) == out, "a Yaz0 archive is decoded first");

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

// Yaz0's repeats: a short one, one whose length takes a third byte, and the damaged cases.
void TestYaz0()
{
    const auto yaz0 = [](uint32_t size, const std::vector<uint8_t>& body)
    {
        files::Bytes b;
        b.Tag("Yaz0").U32(size).U32(0).U32(0);
        for (const uint8_t byte : body)
        {
            b.U8(byte);
        }

        return b.Data();
    };
    const auto text = [](const std::vector<uint8_t>& bytes)
    {
        return std::string(bytes.begin(), bytes.end());
    };

    // Four literals, a repeat of 12 bytes from 4 back, then three literals.
    Check(text(jaudio::DecodeYaz0(yaz0(19, {0xf7, 'A', 'B', 'C', 'D', 0xa0, 0x03, 'X', 'Y', 'Z'}))) ==
              "ABCDABCDABCDABCDXYZ",
          "a short repeat");
    // A literal, then a repeat of 0x12 + 0x10 bytes from 1 back.
    Check(text(jaudio::DecodeYaz0(yaz0(35, {0x80, 'Q', 0x00, 0x00, 0x10}))) == std::string(35, 'Q'),
          "a repeat whose length takes a third byte");

    Check(
        ErrorOf([&] { jaudio::DecodeYaz0(yaz0(4, {0x00, 0x10, 0x00})); }) == "Yaz0 data repeats bytes before its start",
        "a repeat before the start");
    Check(!ErrorOf([&] { jaudio::DecodeYaz0(yaz0(19, {0xf7, 'A', 'B'})); }).empty(), "truncated Yaz0 data");
    Check(ErrorOf([&] { jaudio::DecodeYaz0(yaz0(0x4000001, {})); }) == "Yaz0 data decodes to more than 64 MiB",
          "a decoded size over 64 MiB");
    Check(ErrorOf([&] { jaudio::DecodeYaz0(std::vector<uint8_t>{'R', 'A', 'R', 'C'}); }) == "not Yaz0 data",
          "data that isn't Yaz0");
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
    backwards[kBsc + 8] = 0;
    backwards[kBsc + 9] = 0;
    backwards[kBsc + 10] = 0;
    backwards[kBsc + 11] = 0;
    const std::size_t sc_start = static_cast<std::size_t>(no_sc[kBsc + 4]) << 24 | no_sc[kBsc + 5] << 16 |
                                 no_sc[kBsc + 6] << 8 | no_sc[kBsc + 7];
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

uint32_t Load32(const std::vector<uint8_t>& data, std::size_t at)
{
    return uint32_t{data[at]} << 24 | uint32_t{data[at + 1]} << 16 | uint32_t{data[at + 2]} << 8 | data[at + 3];
}

void Store32(std::vector<uint8_t>& data, std::size_t at, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        data[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(value >> (24 - 8 * i));
    }
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
    Store32(holes.table, 0x14, 0);
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
    const uint32_t table_at = Load32(baa, kTableCommand + 4);
    std::vector<uint8_t> cut = baa;
    Store32(cut, kTableCommand + 8, table_at + 4);
    Check(jaudio::ReadSounds(cut, strict).size() == 3, "the strict rules read past the table block's end");
    Check(ErrorOf([&] { jaudio::ReadSounds(cut); }) == "read out of bounds at offset 12",
          "by default, the table is read inside its block");
    std::vector<uint8_t> backwards = baa;
    Store32(backwards, kTableCommand + 8, 0);
    Check(jaudio::ReadSounds(backwards, strict).size() == 3, "the strict rules don't check a block's end");
    Check(ErrorOf([&] { jaudio::ReadSounds(backwards); }) == "archive has a chunk that ends before it starts",
          "by default, a block that ends before it starts is an error");

    // A name whose offset is the end of the data. The first sound's name is the first group's first slot.
    std::vector<uint8_t> at_end = baa;
    const uint32_t names_at = Load32(baa, kNamesCommand + 4);
    const uint32_t section = Load32(baa, names_at + 0x14);
    const uint32_t group = Load32(baa, names_at + section + 8);
    Store32(at_end, names_at + group + 8, static_cast<uint32_t>(baa.size() - names_at));
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
    const uint32_t ws_at = Load32(baa, 4 + 8);
    std::vector<uint8_t> bad_wsys = baa;
    bad_wsys[ws_at] = 'X';
    std::vector<uint8_t> bad_winf = baa;
    bad_winf[ws_at + 0x20] = 'X';
    Check(names(bad_wsys) == "a wave system in the BAA archive is damaged", "a wave system without WSYS");
    Check(names(bad_winf) == "a wave system in the BAA archive is damaged", "a wave system without WINF");
    Check(ErrorOf([&] { jaudio::WaveArchiveNames(bad_winf); }) == "wave system is damaged",
          "the default message for a damaged wave system");
}

} // namespace

int main()
{
    TestWaveSystem();
    TestBanks();
    TestBankChunkPastItsSize();
    TestWaveGroups();
    TestSoundTable();
    TestLaterLayout();
    TestFirstLayouts();
    TestRarc();
    TestRepackSequenceTable();
    TestReadSequenceTable();
    TestYaz0();
    TestMessages();
    TestLightReaders();
    TestStrictRules();
    TestStrictMessages();

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("jaudio tests passed\n");

    return 0;
}
