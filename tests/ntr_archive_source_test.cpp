// SPDX-License-Identifier: MIT

// Tests of the DS's steps of archive mode (ntdlib/ntr/archive_source.h) in xsflib's archive mode, on a set of a test
// program and an archive that testing/sdat_files.h builds. The source must play what ntr::ArchivePlayer plays.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/archive_source.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
using files::Bytes;

constexpr const char* kArchivePath = "sound_data.sdat";

// A test program's set: its files by path, and the sequence that it plays.
struct TestSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto file = files.find(path);
        return file != files.end() ? &file->second : nullptr;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    std::optional<ntr::SequenceRef> sequence;
    ntr::Variables variables;
    ntr::snd::Build build;
};

struct TestProgram
{
    using Set = TestSet;

    static std::optional<std::string> Select(TestSet& set, ntr::ArchiveSelection& selection)
    {
        if (!set.sequence)
        {
            return "no sequence selected";
        }

        selection.archive_path = kArchivePath;
        selection.sequence = *set.sequence;
        selection.settings.variables = set.variables;
        selection.build = set.build;
        return std::nullopt;
    }
};

using Source = xsflib::player::ArchiveSource<ntr::ArchiveTraits<TestProgram>>;

// The sequence (file 0) plays key 60 for 48 ticks, waits 48 more and ends: 192 frames. It plays bank 0 (file 1),
// whose one instrument plays the only wave of wave archive 0 (file 2).
const Bytes kNote = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

Bytes ArchiveFile(const Bytes& bytecode = kNote)
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 0, "SEQ_A")};
    spec.items[2] = {files::BankItem(1, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK")};
    spec.items[3] = {files::FileItem(2, "WAVE")};
    spec.items[4] = {files::PlayerItem(1, 0, 0, "PLAYER_0")};
    const files::Wave wave{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    spec.files = {files::Sequence(bytecode), files::Bank({{.type = 1, .region = {.wave = 0}}}),
                  files::WaveArchive({wave})};
    return files::SoundArchive(spec);
}

TestSet MakeSet()
{
    TestSet set;
    set.files[kArchivePath] = ArchiveFile();
    set.sequence = ntr::SequenceRef{0};
    set.variables.global[2] = 7;
    return set;
}

void TestPcm16()
{
    std::vector<int16_t> out;
    ntr::AppendPcm16({{0x200, 0}, {0x3FF, 0x201}}, out);
    XSFLIB_CHECK(out == (std::vector<int16_t>{0, -32768, 32704, 64}));
}

void TestPlaysAsThePlayer()
{
    TestSet set = MakeSet();
    const Bytes file = set.files[kArchivePath];
    Source source;
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Busy());
    XSFLIB_CHECK(!source.Start(set, out));
    XSFLIB_CHECK(out.empty());

    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0}, {.variables = set.variables});
    std::vector<int16_t> expected;
    int frames = 0;
    while (source.Busy() && frames < 1000)
    {
        XSFLIB_CHECK(source.RunFrame(out));
        std::vector<ntr::SoundOutput> samples;
        player.RunFrame(samples);
        ntr::AppendPcm16(samples, expected);
        frames++;
    }
    XSFLIB_CHECK(frames >= 192 && frames < 200);
    XSFLIB_CHECK(!player.Busy());
    XSFLIB_CHECK(out == expected);

    const std::span<const uint8_t> memory = source.GetModel()->MainMemory();
    XSFLIB_CHECK(memory[ntr::ArchivePlayer::kSharedWork + 0x264 - ntr::ArchivePlayer::kMainMemory] == 7);
}

// The set's archive moves into the source. The copy of the set keeps the caller's.
std::vector<int16_t> Play(TestSet set, const ntr::snd::Build& build, int frames)
{
    set.build = build;
    Source source;
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Start(set, out));
    for (int frame = 0; frame < frames; frame++)
    {
        source.RunFrame(out);
    }
    return out;
}

// The set's build of the driver reaches the model. A track volume of 200 reads past the end of the driver's table of
// levels: into the levels of i/127 in a build that holds them, and into the next table in a build without them.
void TestBuild()
{
    TestSet set = MakeSet();
    set.files[kArchivePath] = ArchiveFile({0xC1, 0xC8, 0x3C, 0x7F, 0x30, 0xFF});
    const Bytes file = set.files[kArchivePath];
    const std::vector<int16_t> without_table = Play(set, {}, 50);
    const std::vector<int16_t> with_table = Play(set, {.decibel_table = true}, 50);
    XSFLIB_CHECK(with_table != without_table);

    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive, {.build = {.decibel_table = true}});
    player.Start({0}, {.variables = set.variables});
    std::vector<int16_t> expected;
    for (int frame = 0; frame < 50; frame++)
    {
        std::vector<ntr::SoundOutput> samples;
        player.RunFrame(samples);
        ntr::AppendPcm16(samples, expected);
    }
    XSFLIB_CHECK(with_table == expected);
}

void TestSnapshots()
{
    TestSet set = MakeSet();
    Source source;
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Start(set, out));
    for (int frame = 0; frame < 10; frame++)
    {
        source.RunFrame(out);
    }

    const std::shared_ptr<ntr::ArchivePlayer::Snapshot> first = source.Save(nullptr);
    source.RunFrame(out);
    const std::shared_ptr<ntr::ArchivePlayer::Snapshot> second = source.Save(first.get());
    std::vector<int16_t> played;
    for (int frame = 0; frame < 100; frame++)
    {
        source.RunFrame(played);
    }

    source.Restore(*second);
    std::vector<int16_t> again;
    for (int frame = 0; frame < 100; frame++)
    {
        source.RunFrame(again);
    }
    XSFLIB_CHECK(again == played);
}

void TestErrors()
{
    std::vector<int16_t> out;
    Source source;
    TestSet unselected = MakeSet();
    unselected.sequence.reset();
    XSFLIB_CHECK(source.Start(unselected, out) == "no sequence selected");

    TestSet missing = MakeSet();
    missing.files.clear();
    XSFLIB_CHECK(source.Start(missing, out) == "the set has no file sound_data.sdat");

    TestSet unknown = MakeSet();
    unknown.sequence = ntr::SequenceRef{3};
    XSFLIB_CHECK(source.Start(unknown, out) == "the archive has no sequence 3");
}

} // namespace

int main()
{
    TestPcm16();
    TestPlaysAsThePlayer();
    TestBuild();
    TestSnapshots();
    TestErrors();

    return xsflib::testing::Summary("DS archive source");
}
