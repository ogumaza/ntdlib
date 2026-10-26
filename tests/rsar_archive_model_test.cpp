// SPDX-License-Identifier: MIT

// Tests of the model of the Wii's sound archives (ntdlib/rsar/archive_model.h): an archive built from a model made from
// nothing, read back by the model's reader and by ntdlib's reader of the tables, the layout's choices, and the models
// that the builder refuses.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"

namespace
{

using ntdlib::rsar::ArchiveModel;
using ntdlib::rsar::BuildArchive;
using ntdlib::rsar::FileModel;
using ntdlib::rsar::ReadArchiveModel;
using ntdlib::rsar::SoundArchive;
using ntdlib::rsar::SoundModel;
using ntdlib::rsar::SoundType;
using Bytes = std::vector<uint8_t>;

// Bytes of `size` that count up from `first`.
Bytes Counting(std::size_t size, uint8_t first)
{
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8_t>(first + i);
    }

    return bytes;
}

// A model with a sound of each kind, two banks, two players and a group. File 0 is a sequence file, files 1 and 2
// are banks with wave data, file 3 is a wave sound file, and file 4 lies beside the archive. The group holds the
// sequence file and the first bank. The last group holds the others.
ArchiveModel Model()
{
    ntdlib::rsar::SequenceFile sequence;
    sequence.data = {0xC1, 0x64, 0x3C, 0x64, 0x30, 0x80, 0x30, 0xFF};
    sequence.labels = {{"SEQ_SONG", 0}, {"SEQ_SONG_WAIT", 5}};

    ArchiveModel model;
    model.version = 0x0104;
    model.player_settings.sequence_sounds = 16;
    model.players = {{"PLAYER_BGM", 1, 0}, {"PLAYER_SE", 4, 0x8000}};
    model.banks = {{"BANK_A", 1}, {"BANK_B", 2}};
    model.groups = {{"GROUP_MAIN", {0, 1}}};
    model.files = {
        {.bytes = sequence.Write()},
        {.bytes = Counting(64, 1), .wave_data = Counting(96, 2)},
        {.bytes = Counting(32, 3), .wave_data = Counting(32, 4)},
        {.bytes = Counting(32, 5), .wave_data = Counting(64, 6)},
        {.path = "stream/BGM.brstm", .path_size = 0x1234},
    };

    SoundModel song;
    song.name = "SEQ_SONG";
    song.volume = 100;
    song.player_priority = 70;
    song.user_params = {7, 0};
    song.pan_mode = 1;
    song.pan_curve = 2;
    song.settings_3d = {.flags = 0xF, .decay_curve = 2, .decay_ratio = 64, .doppler_factor = 16};
    song.bank = 1;
    song.allocate_track_flags = 3;
    song.channel_priority = 80;
    song.release_priority_fix = true;
    SoundModel wait = song;
    wait.name = "SEQ_SONG_WAIT";
    wait.start_offset = 5;
    wait.bank = 0;
    wait.player_id = 1;
    SoundModel wave;
    wave.name = "WSD_HIT";
    wave.type = SoundType::kWaveSound;
    wave.file_id = 3;
    wave.player_id = 1;
    wave.wave_sound_index = 2;
    wave.remote_filter = 5;
    SoundModel stream;
    stream.name = "STRM_BGM";
    stream.type = SoundType::kStream;
    stream.file_id = 4;
    stream.allocate_channel_count = 4;
    stream.stream_track_flags = 3;
    model.sounds = {song, wait, wave, stream};

    return model;
}

// Where SYMB's offset `index` leads: 0 for the string table, then the trees of the sounds, the players, the groups and
// the banks.
std::size_t SymbolsPart(const Bytes& archive, std::size_t index)
{
    const std::size_t body = xsflib::LoadBe32(&archive[0x10]) + std::size_t{8};

    return body + xsflib::LoadBe32(&archive[body + 4 * index]);
}

void TestFromNothing()
{
    const ArchiveModel model = Model();
    const Bytes archive = BuildArchive(model);
    XSFLIB_CHECK(ReadArchiveModel(archive) == model);
    XSFLIB_CHECK(xsflib::LoadBe32(&archive[8]) == archive.size());
    XSFLIB_CHECK(xsflib::LoadBe32(&archive[0x18]) % 32 == 0 && xsflib::LoadBe32(&archive[0x20]) % 32 == 0);

    // The string table holds the players', the groups', the banks' and then the sounds' names. The last group has none.
    const std::size_t table = SymbolsPart(archive, 0);
    XSFLIB_CHECK(xsflib::LoadBe32(&archive[table]) == 2 + 1 + 2 + 4);

    ntdlib::rsar::ReadOptions options;
    options.playback_fields = true;
    options.printable_names = false;
    const SoundArchive read = SoundArchive::Load(archive, options);
    XSFLIB_CHECK(read.Version() == 0x0104 && read.Sounds().size() == 4 && read.Banks().size() == 2);
    XSFLIB_CHECK(read.Sounds()[0].name == "SEQ_SONG" && read.Sounds()[0].volume == 100 && read.Sounds()[0].bank == 1);
    XSFLIB_CHECK(read.Sounds()[0].pan_mode == 1 && read.Sounds()[0].pan_curve == 2);
    XSFLIB_CHECK(read.Sounds()[0].channel_priority == 80 && read.Sounds()[0].release_priority_fix);
    XSFLIB_CHECK(read.Sounds()[1].start_offset == 5 && read.Sounds()[1].player_id == 1);
    XSFLIB_CHECK(read.Sounds()[2].type == SoundType::kWaveSound && read.Sounds()[2].sub_index == 2);
    XSFLIB_CHECK(read.Sounds()[3].type == SoundType::kStream && read.Sounds()[3].file_id == 4);
    XSFLIB_CHECK(read.Players().size() == 2 && read.Players()[1].sound_count == 4 &&
                 read.Players()[1].heap_size == 0x8000);
    XSFLIB_CHECK(read.Banks()[1].name == "BANK_B" && read.Banks()[1].file_id == 2);

    // The group holds the sequence file and the first bank. The last group holds the second bank and the wave sound
    // file. Each file's bytes and wave data come back.
    XSFLIB_CHECK(read.Groups().size() == 2 && read.Groups()[0].items.size() == 2 && read.Groups()[1].items.size() == 2);
    XSFLIB_CHECK(read.Groups()[1].items[0].file_id == 2 && read.Groups()[1].items[1].file_id == 3);
    for (uint32_t id = 0; id < 4; ++id)
    {
        const std::span<const uint8_t> bytes = read.FileData(id);
        const std::span<const uint8_t> wave_data = read.WaveData(id);
        XSFLIB_CHECK(Bytes(bytes.begin(), bytes.end()) == model.files[id].bytes);
        XSFLIB_CHECK(Bytes(wave_data.begin(), wave_data.end()) == model.files[id].wave_data);
    }
    XSFLIB_CHECK(read.FileData(4).empty());
}

void TestChoices()
{
    // The path's length is a multiple of 4. Most tools end it with a zero, then pad it. The others end it without one.
    ArchiveModel model = Model();
    const Bytes terminated = BuildArchive(model);
    model.terminated_paths = false;
    const Bytes unterminated = BuildArchive(model);
    XSFLIB_CHECK(unterminated != terminated);
    XSFLIB_CHECK(ReadArchiveModel(unterminated) == model);

    // The names end 2 bytes past a multiple of 4. One version of the tools starts the trees on the next one.
    model = Model();
    const Bytes packed = BuildArchive(model);
    model.aligned_trees = true;
    const Bytes aligned = BuildArchive(model);
    XSFLIB_CHECK(SymbolsPart(aligned, 1) % 4 == 0 && SymbolsPart(packed, 1) % 4 != 0);
    XSFLIB_CHECK(ReadArchiveModel(aligned) == model);
    model.aligned_trees = false;
    XSFLIB_CHECK(ReadArchiveModel(packed) == model);

    // An archive with a player alone: the trees without names have their roots at 0xFFFFFFFF.
    ArchiveModel empty;
    empty.version = 0x0101;
    empty.players = {{"PLAYER_DEFAULT", 1, 0}};
    const Bytes alone = BuildArchive(empty);
    XSFLIB_CHECK(ReadArchiveModel(alone) == empty);
    XSFLIB_CHECK(xsflib::LoadBe32(&alone[SymbolsPart(alone, 1)]) == 0xFFFFFFFF);
    XSFLIB_CHECK(xsflib::LoadBe32(&alone[SymbolsPart(alone, 2)]) == 0);
    XSFLIB_CHECK(xsflib::LoadBe32(&alone[SymbolsPart(alone, 4)]) == 0xFFFFFFFF);
}

void TestRefusals()
{
    auto refused = [](const ArchiveModel& model)
    {
        return !xsflib::testing::ErrorOf<std::invalid_argument>([&] { BuildArchive(model); }).empty();
    };

    ArchiveModel model = Model();
    model.sounds[1].name = "SEQ_SONG";
    XSFLIB_CHECK(refused(model));
    model = Model();
    model.banks[0].name = "SEQ_SONG"; // a sound and a bank go in other trees
    XSFLIB_CHECK(ReadArchiveModel(BuildArchive(model)) == model);
    model = Model();
    model.version = 0x0100;
    XSFLIB_CHECK(refused(model));
    model = Model();
    model.files[1].bytes.resize(40);
    XSFLIB_CHECK(refused(model));
    model = Model();
    model.groups[0].file_ids.push_back(4);
    XSFLIB_CHECK(refused(model));
    model = Model();
    model.sounds[0].bank = 2;
    XSFLIB_CHECK(refused(model));
    model = Model();
    model.sounds[0].type = SoundType::kUnknown;
    XSFLIB_CHECK(refused(model));

    // A byte that the tools leave zero: the archive is laid out another way.
    Bytes archive = BuildArchive(Model());
    const std::size_t info = xsflib::LoadBe32(&archive[0x18]) + std::size_t{8};
    const std::size_t sounds = info + xsflib::LoadBe32(&archive[info + 4]);
    const std::size_t sound = info + xsflib::LoadBe32(&archive[sounds + 8]);
    archive[sound + 0x2B] = 1;
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<xsflib::FormatError>([&] { ReadArchiveModel(archive); }).empty());
    archive = BuildArchive(Model());
    archive.resize(archive.size() - 32);
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<xsflib::FormatError>([&] { ReadArchiveModel(archive); }).empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("RSAR archive model",
                                    []
                                    {
                                        TestFromNothing();
                                        TestChoices();
                                        TestRefusals();
                                    });
}
