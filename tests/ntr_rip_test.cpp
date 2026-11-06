// SPDX-License-Identifier: MIT

// Tests of the archive mode's parts of a rip tool for the DS's sound archives (ntdlib/ntr/rip.h), on an archive that
// sdat::BuildArchive lays out as the SDK's tools do. The expected labels, files and numbers are worked out by hand from
// the archive's model.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/rip.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace ntr = ntdlib::ntr;
namespace rip = ntdlib::ntr::rip;
namespace sdat = ntdlib::sdat;
using files::Bytes;
using ntr::SequenceRef;
using xsflib::testing::ErrorOf;

constexpr uint16_t kNone = 0xFFFF;
constexpr uint32_t kNoSequence = 0xFFFFFFFF;

// A wave of a square that loops.
files::Wave SquareWave()
{
    return {.loop = 1,
            .loop_length = 4,
            .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
}

// The archive's files:
//   0: BGM_FIELD's sequence, also played by SE_JUMP, NO_BANK, EMPTY_BANK, LOST_WAVES and JINGLE
//   1: sequence 1's sequence
//   2: SE_SYSTEM, a sequence archive of two sequences and an entry without one
//   3: BANK_A, and BANK_C's file too
//   4: WA_A. It loads its waves one at a time (bit 24 of its file's ID)
//   5: BANK_B
//   6: WA_B
//   7: empty: BANK_EMPTY's and WA_EMPTY's file
//   8: sequence archive 1's file. It isn't a sequence archive
sdat::ArchiveModel Model()
{
    sdat::ArchiveModel model;
    sdat::Symbols symbols;
    symbols.names[0] = {"BGM_FIELD",  std::nullopt, "SE_JUMP", std::nullopt, "NO_BANK",
                        "EMPTY_BANK", "LOST_WAVES", "NO_FILE", "JINGLE"};
    symbols.names[1] = {"SE_SYSTEM", std::nullopt};
    symbols.sequence_names = {std::vector<std::optional<std::string>>{"SE_OK", std::nullopt, std::nullopt},
                              std::vector<std::optional<std::string>>{}};
    symbols.names[2] = {"BANK_A", "BANK_B", "BANK_EMPTY", "BANK_C"};
    symbols.names[3] = {"WA_A", "WA_B", "WA_EMPTY"};
    symbols.names[4] = {"PLAYER_SE", "PLAYER_MUSIC2", "PLAYER_SE_FANFARE", "PLAYER_BGMX"};
    model.symbols = symbols;

    const auto sequence = [](uint32_t file_id, uint16_t bank, uint8_t player)
    {
        return sdat::SequenceInfo{file_id, bank, 127, 64, 64, player, 0, ""};
    };
    model.sequences = {sequence(0, 0, 0), sequence(1, 1, 1), sequence(0, 0, 2),  std::nullopt,     sequence(0, 5, 0),
                       sequence(0, 2, 0), sequence(0, 3, 0), sequence(20, 0, 0), sequence(0, 0, 3)};
    model.sequence_archives = {sdat::SequenceArchiveInfo{2, "", {}}, sdat::SequenceArchiveInfo{8, "", {}}};
    model.banks = {sdat::BankInfo{3, {0, kNone, kNone, kNone}, ""}, sdat::BankInfo{5, {1, kNone, kNone, kNone}, ""},
                   sdat::BankInfo{7, {kNone, kNone, kNone, kNone}, ""}, sdat::BankInfo{3, {7, 2, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{0x01000004, ""}, sdat::WaveArchiveInfo{6, ""},
                           sdat::WaveArchiveInfo{0x01000007, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}, sdat::PlayerInfo{1, 0, 0, 0, ""},
                     sdat::PlayerInfo{1, 0, 0, 0, ""}, sdat::PlayerInfo{1, 0, 0, 0, ""}};

    const Bytes bank = files::Bank({{.type = 1, .region = {.wave = 0}}});
    const Bytes wave_archive = files::WaveArchive({SquareWave()});
    model.files = {
        files::Sequence({0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF}),
        files::Sequence({0x43, 0x60, 0x18, 0x80, 0x18, 0x40, 0x7F, 0x18, 0xFF}),
        files::SequenceArchive(
            {{.offset = 0, .bank = 1, .player = 1}, {.offset = 4, .bank = 0, .player = 2}, {.offset = kNoSequence}},
            {0x48, 0x7F, 0x30, 0xFF, 0x30, 0x50, 0x18, 0xFF}),
        bank,
        wave_archive,
        bank,
        wave_archive,
        {},
        {1, 2, 3, 4},
    };

    return model;
}

struct TestArchive
{
    Bytes bytes = sdat::BuildArchive(Model());
    sdat::Archive archive = sdat::Archive::Parse(bytes);
};

std::vector<std::string> Labels(const std::vector<rip::Job>& jobs)
{
    std::vector<std::string> labels;
    for (const rip::Job& job : jobs)
    {
        labels.push_back(job.label);
    }
    return labels;
}

// The labels: the SYMB block's names, else SEQ_ and the number, or the sequence archive's label and the number.
void TestLabels()
{
    const TestArchive test;
    XSFLIB_CHECK(rip::Label(test.archive, {0}) == "BGM_FIELD");
    XSFLIB_CHECK(rip::Label(test.archive, {1}) == "SEQ_1");
    XSFLIB_CHECK(rip::Label(test.archive, {0, 0}) == "SE_OK");
    XSFLIB_CHECK(rip::Label(test.archive, {0, 1}) == "SE_SYSTEM_1");
    XSFLIB_CHECK(rip::Label(test.archive, {1, 0}) == "SEQARC_1_0");
    XSFLIB_CHECK(rip::Label(test.archive, {3}) == "SEQ_3");
    XSFLIB_CHECK(rip::Label(test.archive, {7, 2}) == "SEQARC_7_2");

    // Without a SYMB block, every label is a number.
    sdat::ArchiveModel model = Model();
    model.symbols.reset();
    const Bytes bytes = sdat::BuildArchive(model);
    const sdat::Archive unnamed = sdat::Archive::Parse(bytes);
    XSFLIB_CHECK(rip::Label(unnamed, {0}) == "SEQ_0");
    XSFLIB_CHECK(rip::Label(unnamed, {0, 1}) == "SEQARC_0_1");
}

// Music: a label with BGM, or a player whose words name music without SE.
void TestMusic()
{
    const TestArchive test;
    XSFLIB_CHECK(rip::IsMusic(test.archive, {0}));     // BGM_FIELD on PLAYER_SE
    XSFLIB_CHECK(rip::IsMusic(test.archive, {1}));     // on PLAYER_MUSIC2
    XSFLIB_CHECK(!rip::IsMusic(test.archive, {2}));    // on PLAYER_SE_FANFARE
    XSFLIB_CHECK(!rip::IsMusic(test.archive, {8}));    // on PLAYER_BGMX
    XSFLIB_CHECK(rip::IsMusic(test.archive, {0, 0}));  // SE_OK on PLAYER_MUSIC2
    XSFLIB_CHECK(!rip::IsMusic(test.archive, {0, 1})); // on PLAYER_SE_FANFARE
    XSFLIB_CHECK(!rip::IsMusic(test.archive, {3}));    // no record
    XSFLIB_CHECK(!rip::IsMusic(test.archive, {1, 0})); // no sequence archive

    // The words SONG and FANFARE name music too, with or without digits at their ends. SE among the words names
    // effects.
    for (const char* name : {"PLAYER_SONG3", "PLAYER_FANFARE_2", "PLAYER_BGM_SE"})
    {
        sdat::ArchiveModel model = Model();
        model.symbols->names[4][1] = name;
        const Bytes bytes = sdat::BuildArchive(model);
        const sdat::Archive archive = sdat::Archive::Parse(bytes);
        XSFLIB_CHECK(rip::IsMusic(archive, {1}) == (std::string(name) != "PLAYER_BGM_SE"));
    }
}

// The archive's sequences with records, then the sequence archives' sequences. Sequence 3 has no record, the third
// entry of SE_SYSTEM has no sequence, and sequence archive 1 can't be read.
void TestSelection()
{
    const TestArchive test;
    const std::vector<rip::Job> all = rip::SelectSequences(test.archive, std::regex(""), false);
    XSFLIB_CHECK(Labels(all) == (std::vector<std::string>{"BGM_FIELD", "SEQ_1", "SE_JUMP", "NO_BANK", "EMPTY_BANK",
                                                          "LOST_WAVES", "NO_FILE", "JINGLE", "SE_OK", "SE_SYSTEM_1"}));
    XSFLIB_CHECK(all.size() == 10 && all[1].sequence == SequenceRef{1} && all[9].sequence == (SequenceRef{0, 1}));
    XSFLIB_CHECK(std::ranges::none_of(all, [](const rip::Job& job) { return job.timing.has_value(); }));

    XSFLIB_CHECK(Labels(rip::SelectSequences(test.archive, std::regex(""), true)) ==
                 (std::vector<std::string>{"BGM_FIELD", "SEQ_1", "SE_OK"}));
    XSFLIB_CHECK(Labels(rip::SelectSequences(test.archive, std::regex("^SE_"), false)) ==
                 (std::vector<std::string>{"SE_JUMP", "SE_OK", "SE_SYSTEM_1"}));
    XSFLIB_CHECK(rip::SelectSequences(test.archive, std::regex("^SE_"), true).size() == 1);
}

void TestMissingData()
{
    const TestArchive test;
    XSFLIB_CHECK(rip::MissingData(test.archive, {0}).empty());
    XSFLIB_CHECK(rip::MissingData(test.archive, {1}).empty());
    XSFLIB_CHECK(rip::MissingData(test.archive, {0, 0}).empty());
    XSFLIB_CHECK(rip::MissingData(test.archive, {0, 1}).empty());
    XSFLIB_CHECK(rip::MissingData(test.archive, {3}) == std::vector<std::string>{"the sequence's record"});
    XSFLIB_CHECK(rip::MissingData(test.archive, {0, 2}) == std::vector<std::string>{"the sequence archive's sequence"});
    XSFLIB_CHECK(rip::MissingData(test.archive, {1, 0}) == std::vector<std::string>{"the sequence archive's sequence"});
    XSFLIB_CHECK(rip::MissingData(test.archive, {4}) ==
                 std::vector<std::string>{"bank 5, which the archive doesn't list"});
    XSFLIB_CHECK(rip::MissingData(test.archive, {5}) == std::vector<std::string>{"bank BANK_EMPTY"});
    XSFLIB_CHECK(rip::MissingData(test.archive, {6}) ==
                 (std::vector<std::string>{"wave archive 7, which the archive doesn't list", "wave archive WA_EMPTY"}));
    XSFLIB_CHECK(rip::MissingData(test.archive, {7}) == std::vector<std::string>{"the sequence's file"});

    // A sequence archive's file that is empty.
    sdat::ArchiveModel model = Model();
    model.files[2].clear();
    const Bytes bytes = sdat::BuildArchive(model);
    const sdat::Archive archive = sdat::Archive::Parse(bytes);
    XSFLIB_CHECK(rip::MissingData(archive, {0, 0}) == std::vector<std::string>{"the sequence archive's sequence"});

    // Without names, a bank and a wave archive go by their numbers.
    model = Model();
    model.symbols.reset();
    const Bytes unnamed_bytes = sdat::BuildArchive(model);
    const sdat::Archive unnamed = sdat::Archive::Parse(unnamed_bytes);
    XSFLIB_CHECK(rip::MissingData(unnamed, {5}) == std::vector<std::string>{"bank #2"});
    XSFLIB_CHECK(rip::MissingData(unnamed, {6}) ==
                 (std::vector<std::string>{"wave archive 7, which the archive doesn't list", "wave archive #2"}));
}

std::vector<ntr::SoundOutput> Play(const sdat::Archive& archive, const SequenceRef& sequence)
{
    constexpr int kFrames = 300;
    ntr::ArchivePlayer player(archive);
    player.Start(sequence);
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < kFrames; frame++)
    {
        player.RunFrame(out);
    }
    return out;
}

bool Sounds(const std::vector<ntr::SoundOutput>& out)
{
    return std::ranges::any_of(out, [&](const ntr::SoundOutput& s) { return !(s == out.front()); });
}

// A library keeps every record and the files that its sequences play, and plays them as the whole archive does.
void TestKeepSequenceData()
{
    const TestArchive test;
    const std::vector<SequenceRef> field = {{0}};
    const std::optional<Bytes> library = rip::KeepSequenceData(test.archive, field);
    XSFLIB_CHECK(library.has_value());
    if (!library)
    {
        return;
    }

    const sdat::Archive kept = sdat::Archive::Parse(*library);
    XSFLIB_CHECK(kept.sequences.size() == 9 && kept.sequence_archives.size() == 2 && kept.banks.size() == 4);
    XSFLIB_CHECK(kept.files.size() == 9 && kept.symbols == test.archive.symbols);
    for (uint32_t id = 0; id < 9; id++)
    {
        const bool keeps = id == 0 || id == 3 || id == 4;
        XSFLIB_CHECK(keeps ? std::ranges::equal(kept.File(id), test.archive.File(id)) : kept.File(id).empty());
    }
    XSFLIB_CHECK(library->size() < test.bytes.size());
    const std::vector<ntr::SoundOutput> whole = Play(test.archive, {0});
    XSFLIB_CHECK(Sounds(whole) && Play(kept, {0}) == whole);

    // Two sequences: a sequence archive's sequence and sequence 1 take files 1, 2, 5 and 6.
    const std::vector<SequenceRef> two = {{0, 0}, {1}};
    const std::optional<Bytes> both = rip::KeepSequenceData(test.archive, two);
    XSFLIB_CHECK(both.has_value());
    if (both)
    {
        const sdat::Archive read = sdat::Archive::Parse(*both);
        for (uint32_t id = 0; id < 9; id++)
        {
            const bool keeps = id == 1 || id == 2 || id == 5 || id == 6;
            XSFLIB_CHECK(keeps ? std::ranges::equal(read.File(id), test.archive.File(id)) : read.File(id).empty());
        }
        for (const SequenceRef& sequence : two)
        {
            const std::vector<ntr::SoundOutput> out = Play(test.archive, sequence);
            XSFLIB_CHECK(Sounds(out) && Play(read, sequence) == out);
        }
    }

    // A sequence that lacks data, and an archive laid out otherwise: here, a file moved off its multiple of 32.
    const std::vector<SequenceRef> lacking = {{0}, {5}};
    XSFLIB_CHECK(!rip::KeepSequenceData(test.archive, lacking));
    Bytes moved = test.bytes;
    const uint32_t fat = xsflib::LoadLe32(&moved[0x20]);
    moved[fat + 12] = static_cast<uint8_t>(moved[fat + 12] + 4);
    const sdat::Archive other = sdat::Archive::Parse(moved);
    XSFLIB_CHECK(!rip::KeepSequenceData(other, field));
}

void TestSoundIds()
{
    XSFLIB_CHECK(rip::SoundId({5}) == 5 && rip::SoundId({0x7FFFFFFF}) == 0x7FFFFFFF);
    XSFLIB_CHECK(rip::SoundId({3, 7}) == 0x80030007 && rip::SoundId({0x7FFF, 0xFFFF}) == 0xFFFFFFFF);
    XSFLIB_CHECK(rip::SequenceOf(5) == SequenceRef{5} && rip::SequenceOf(0x80030007) == (SequenceRef{3, 7}));
    XSFLIB_CHECK(rip::SequenceOf(0xFFFFFFFF) == (SequenceRef{0x7FFF, 0xFFFF}));
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([] { rip::SoundId({0x80000000}); }) ==
                 "no sound ID for sequence 2147483648");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([] { rip::SoundId({0x8000, 0}); }) ==
                 "no sound ID for sequence 0 of sequence archive 32768");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([] { rip::SoundId({0, 0x10000}); }) ==
                 "no sound ID for sequence 65536 of sequence archive 0");
}

struct TestDescriptor
{
    std::vector<uint8_t> Serialize() const
    {
        return {archive_path.begin(), archive_path.end()};
    }

    std::string archive_path;
};

struct TestFormat
{
    using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;
    using ArchiveDescriptor = TestDescriptor;
    static constexpr uint8_t kPsfVersion = 0x7F;
};

// A library holds the archive at its path, with the descriptor in the reserved area. A mini names its sequence's
// sound ID and its library.
void TestLibraryAndMini()
{
    const TestArchive test;
    const Bytes library =
        rip::BuildArchiveLibrary<TestFormat>("sound/sound_data.sdat", test.bytes, {}, {{"title", "a game"}});
    xsflib::psf::PsfFile psf;
    xsflib::psf::ProgramChunks chunks;
    XSFLIB_CHECK(!xsflib::psf::ParsePsf(library, psf) && !xsflib::psf::ParseProgram(psf.program, chunks));
    XSFLIB_CHECK(psf.version == 0x7F && psf.tags.at("title") == "a game");
    XSFLIB_CHECK(std::string(psf.reserved.begin(), psf.reserved.end()) == "sound/sound_data.sdat");
    const std::vector<uint8_t>* file = xsflib::psf::FindFile(chunks.files, "sound/sound_data.sdat");
    XSFLIB_CHECK(file && *file == test.bytes && !chunks.sound);

    const Bytes mini = rip::BuildMini<TestFormat>({0, 1}, "game.lib", {{"title", "SE_SYSTEM_1"}});
    xsflib::psf::PsfFile mini_psf;
    xsflib::psf::ProgramChunks mini_chunks;
    XSFLIB_CHECK(!xsflib::psf::ParsePsf(mini, mini_psf) && !xsflib::psf::ParseProgram(mini_psf.program, mini_chunks));
    XSFLIB_CHECK(mini_psf.version == 0x7F && mini_psf.reserved.empty() && mini_chunks.files.empty());
    XSFLIB_CHECK(mini_chunks.sound == 0x80000001u);
    XSFLIB_CHECK(mini_psf.tags.at("_lib") == "game.lib" && mini_psf.tags.at("title") == "SE_SYSTEM_1");
}

} // namespace

int main()
{
    TestLabels();
    TestMusic();
    TestSelection();
    TestMissingData();
    TestKeepSequenceData();
    TestSoundIds();
    TestLibraryAndMini();

    return xsflib::testing::Summary("DS rip parts");
}
