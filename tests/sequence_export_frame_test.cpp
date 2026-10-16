// SPDX-License-Identifier: MIT

// Tests of the command-line frame: the text and file helpers, the options, and the batch with a converter made up here.
// The batch's files go in a temporary folder. Also the help of a converter of sound archives.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/testing/sar_files.h"
#include "temp_folder.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::sequence_export;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace fs = std::filesystem;

namespace
{

// A Conversion as the frame reads it.
struct FakePerformance
{
    bool loop = false;
    int loops = 0;
    bool holds = false;
    bool truncated = false;
    double seconds = 0;
    std::vector<int> presets;
    std::map<std::string, uint32_t> approximations;
};

struct FakeSfzFile
{
    std::string name;
    std::string text;
};

struct FakeSfzSample
{
    std::string name;
    std::vector<uint8_t> wav;
};

struct FakeSfz
{
    std::vector<FakeSfzFile> files;
    std::vector<FakeSfzSample> samples;
};

struct FakeConversion
{
    FakePerformance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    FakeSfz sfz;
    std::size_t samples = 0;
    int channels = 0;
    std::vector<std::string> warnings;
};

// An input of the made-up converter: a text file with a sequence name on each line.
struct FakeSource
{
    std::vector<std::string> names;
};

std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string Text(const std::optional<std::vector<uint8_t>>& bytes)
{
    return bytes ? std::string(bytes->begin(), bytes->end()) : "<none>";
}

// The made-up converter. Its sequences convert to files that hold their names. A sequence named "bad" fails.
// `prepared_loops` and `before_inputs` record what the hooks received.
struct FakeConverter
{
    FakeConverter()
    {
        converter.program = "fake";
        converter.version_line = "fake 1.2.3";
        converter.input_word = "input";
        converter.folder_message = " is a folder";
        converter.samples_folder = "samples";
        converter.usage = []()
        {
            std::puts("usage: fake [options] <input> [...]");
        };
        converter.option = VarOption<PerformOptions>;
        converter.before_inputs = [this]()
        {
            before_inputs++;
        };
        converter.open = [](const fs::path& input, const std::string&) -> std::optional<FakeSource>
        {
            const std::optional<std::vector<uint8_t>> bytes = ReadFile(input);
            if (!bytes)
            {
                return std::nullopt;
            }

            FakeSource source;
            std::string line;
            for (const uint8_t b : *bytes)
            {
                if (b == '\n')
                {
                    source.names.push_back(line);
                    line.clear();
                }
                else
                {
                    line.push_back(static_cast<char>(b));
                }
            }

            return source;
        };
        converter.sequences = [](const FakeSource& source)
        {
            std::vector<Sequence> sequences;
            for (std::size_t i = 0; i < source.names.size(); i++)
            {
                sequences.push_back({i, source.names[i]});
            }

            return sequences;
        };
        converter.prepare =
            [](const fs::path&, const FakeSource&, const Options<PerformOptions>&, PerformOptions& perform)
        {
            perform.loops += 10;
        };
        converter.convert = [this](FakeSource& source, std::size_t index, const PerformOptions& perform)
        {
            const std::string& name = source.names[index];
            if (name == "bad")
            {
                throw std::runtime_error("can't convert it");
            }

            prepared_loops = perform.loops;
            FakeConversion c;
            c.midi = Bytes("MIDI " + name);
            c.sf2 = Bytes("SF2 " + name);
            c.sfz.files.push_back({"Track 0.sfz", "<region> sample=../samples/shared.wav"});
            c.sfz.samples.push_back({"shared.wav", Bytes("WAV")});

            return c;
        };
    }

    Converter<PerformOptions, FakeSource, FakeConversion> converter;
    int before_inputs = 0;
    int prepared_loops = 0;
};

void TestText()
{
    const std::string kName = "caf\xc3\xa9/\xc3\xbc.mid";

    const fs::path path = PathFromUtf8(kName);

    Check(Utf8(path) == kName, "a UTF-8 path survives the round trip");
    XSFLIB_CHECK(AsciiLower("AbC \xc3\x89") == "abc \xc3\x89");
    XSFLIB_CHECK(Duration(65.3) == "1:05.3");
    XSFLIB_CHECK(Duration(59.96) == "1:00.0");
    XSFLIB_CHECK(Duration(0) == "0:00.0");
    XSFLIB_CHECK(ParseInteger("42") == 42);
    XSFLIB_CHECK(ParseInteger("-7") == -7);
    XSFLIB_CHECK(ParseInteger("0x1F") == 31);
    XSFLIB_CHECK(ParseInteger("-0x10") == -16);
    XSFLIB_CHECK(!ParseInteger(""));
    XSFLIB_CHECK(!ParseInteger("x"));
    XSFLIB_CHECK(!ParseInteger("4294967296"));
}

void TestFiles()
{
    const TempFolder folder("sequence_export_frame_test");
    const fs::path file = folder.Path() / "a.bin";
    const fs::path nowhere = folder.Path() / "missing" / "b.bin";

    WriteFile(file, std::string("hello"));
    const std::string error = ErrorOf([&]() { WriteFile(nowhere, std::string("x")); });

    Check(Text(ReadFile(file)) == "hello", "a file reads back what was written");
    Check(!ReadFile(nowhere), "a missing file reads as nothing");
    Check(error == "can't write " + Utf8(nowhere), "a failed write names the file: " + error);
}

void TestGroupFilesInSarcs()
{
    // An archive with two groups kept outside it. Group A holds its bank A, file 1, and group B its bank B, file 2.
    namespace sar_files = ntdlib::testing::sar_files;
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ", .file = 0, .banks = {0, 1}}};
    spec.banks = {{"BANK_A", 1}, {"BANK_B", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 0xFFFFFFFF}, {"GROUP_B", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(true, {0xff}, {}), "", false}, {{}, "", true}, {{}, "", true}};
    const TempFolder folder("sequence_export_frame_test");
    const fs::path input = folder.Write("Sound.bfsar", sar_files::Archive(true, spec));
    const auto load = [&input]
    {
        return ntdlib::sar::SoundArchive::Load(*ReadFile(input), CafeSettings().archive);
    };
    const auto bank = [](const std::string& text)
    {
        return Bytes("FBNK " + text);
    };
    const auto text = [](std::span<const uint8_t> data)
    {
        return std::string(data.begin(), data.end());
    };

    // A SARC with another archive's group. Its file 1 is a wave archive. No group is found.
    folder.Write("A.sarc", sar_files::Sarc(
                               true, {sar_files::Group(true, {{1, Bytes("FWAR")}, {2, bank("of another archive")}})}));
    ntdlib::sar::SoundArchive alone = load();
    Check(AddGroupFiles(alone, input, "Sound.bfsar") == 0 && alone.FileData(1).empty() && alone.FileData(2).empty(),
          "another archive's group in a SARC is left out");

    // Group A's file beside the archive, and a SARC with copies of both groups. Group B comes from the SARC. The SARC's
    // copy of group A changes nothing.
    folder.Write("GROUP_A.bfgrp", sar_files::Group(true, {{1, bank("A by name")}}));
    folder.Write("B.sarc", sar_files::Sarc(true, {sar_files::Group(true, {{1, bank("A in a SARC")}}),
                                                  sar_files::Group(true, {{2, bank("B in a SARC")}})}));
    ntdlib::sar::SoundArchive both = load();
    Check(AddGroupFiles(both, input, "Sound.bfsar") == 3 && text(both.FileData(1)) == "FBNK A by name" &&
              text(both.FileData(2)) == "FBNK B in a SARC",
          "a group in a SARC after the groups found by name");

    // With every group found by name, the SARCs aren't read.
    folder.Write("GROUP_B.bfgrp", sar_files::Group(true, {{2, bank("B by name")}}));
    ntdlib::sar::SoundArchive named = load();
    Check(AddGroupFiles(named, input, "Sound.bfsar") == 2 && text(named.FileData(2)) == "FBNK B by name",
          "no SARC read once every group is found");
}

void TestOptions()
{
    const std::vector<std::string> kArgs = {"fake",   "--output", "out",   "--only", "BGM",    "--loops", "0",
                                            "--seed", "0x10",     "--var", "3=-5",   "--list", "-q",      "a.bin"};
    const OptionHandler<PerformOptions> var = VarOption<PerformOptions>;

    const Options<PerformOptions> o = ParseOptions<PerformOptions>(kArgs, var);
    const Options<PerformOptions> help = ParseOptions<PerformOptions>({"fake", "-h", "--bad"}, var);

    XSFLIB_CHECK(o.output == "out" && o.only == "BGM" && o.list && o.quiet && !o.help);
    XSFLIB_CHECK(o.perform.loops == 1 && o.perform.seed == 16);
    XSFLIB_CHECK(o.perform.variables.size() == 1 && o.perform.variables.at(3) == -5);
    XSFLIB_CHECK(o.archives == std::vector<std::string>{"a.bin"});
    XSFLIB_CHECK(help.help);

    auto error = [&](std::vector<std::string> args)
    {
        args.insert(args.begin(), "fake");
        return ErrorOf([&]() { ParseOptions<PerformOptions>(args, var); });
    };
    XSFLIB_CHECK(error({"--bad"}) == "unknown option --bad");
    XSFLIB_CHECK(error({"--loops"}) == "--loops requires a value");
    XSFLIB_CHECK(error({"--loops", "two"}) == "--loops requires a number, not two");
    XSFLIB_CHECK(error({"--seed", "0x100000000"}) == "--seed 0x100000000 is out of range");
    XSFLIB_CHECK(error({"--seed", "-5"}) == "--seed requires a number, not -5");
    XSFLIB_CHECK(error({"--seed", "99999999999x"}) == "--seed requires a number, not 99999999999x");
    XSFLIB_CHECK(error({"--loops", "4294967296"}) == "--loops 4294967296 is out of range");
    XSFLIB_CHECK(error({"--var", "3"}) == "--var requires N=V, not 3");
    XSFLIB_CHECK(error({"--var", "32=1"}) == "--var 32=1 is out of range");
    XSFLIB_CHECK(error({"--var", "1=32768"}) == "--var 1=32768 is out of range");
    XSFLIB_CHECK(ErrorOf([]() { ParseOptions<PerformOptions>({"fake", "--var", "1=2"}, {}); }) ==
                 "unknown option --var");
}

void TestVoicesOption()
{
    // --voices takes a count of voices from 1 to 1024, or off as 0. Without it, voice_limit is 0: every note is kept.
    const OptionHandler<PerformOptions> voices = VoicesOption<PerformOptions>;
    auto parse = [&](const std::string& value)
    {
        return ParseOptions<PerformOptions>({"fake", "--voices", value}, voices).perform.voice_limit;
    };
    auto error = [&](const std::string& value)
    {
        return ErrorOf([&]() { parse(value); });
    };

    XSFLIB_CHECK(parse("78") == 78 && parse("0x60") == 96 && parse("1024") == 1024);
    XSFLIB_CHECK(parse("off") == 0);
    XSFLIB_CHECK(ParseOptions<PerformOptions>({"fake"}, voices).perform.voice_limit == 0);
    XSFLIB_CHECK(error("0") == "--voices 0 is out of range");
    XSFLIB_CHECK(error("1025") == "--voices 1025 is out of range");
    XSFLIB_CHECK(error("all") == "--voices requires a number or off, not all");
}

void TestArchiveOptions()
{
    // --voices is an option only where the console has a voice count. The program's options still follow.
    ArchiveConverter<ntdlib::sar::SoundArchive> c;
    const OptionHandler<PerformOptions> options = ArchiveOptions(c);
    const std::vector<std::string> kArgs = {"fake", "--voices", "78", "--var", "1=2"};

    const std::string unknown = ErrorOf([&]() { ParseOptions<PerformOptions>(kArgs, options); });
    c.settings.voice_limit = 96;
    const Options<PerformOptions> o = ParseOptions<PerformOptions>(kArgs, options);

    XSFLIB_CHECK(unknown == "unknown option --voices");
    XSFLIB_CHECK(o.perform.voice_limit == 78 && o.perform.variables.at(1) == 2);
}

void TestSwitchHasNoVoices()
{
    // The Switch's model doesn't drop the notes that nn::atk drops. A program for the Switch offers no --voices.
    ArchiveConverter<ntdlib::sar::SoundArchive> c;
    c.settings = SwitchSettings();

    const std::string text = UsageText(c);

    XSFLIB_CHECK(c.settings.voice_limit == 0);
    XSFLIB_CHECK(text.find("--voices") == std::string::npos);
}

void TestBatch()
{
    const TempFolder folder("sequence_export_frame_test");
    const fs::path input = folder.Path() / "game.txt";
    WriteFile(input, std::string("Song\nsong\nsamples\nbad\n"));
    FakeConverter fake;
    const fs::path out = folder.Path() / "game_fake";

    const int rc = Run({"fake", "--quiet", "--loops", "2", Utf8(input)}, fake.converter);

    Check(rc == 1, "a sequence that fails makes the exit status 1");
    Check(Text(ReadFile(out / "Song.mid")) == "MIDI Song", "a sequence's MIDI file goes in <input>_<program>");
    Check(Text(ReadFile(out / "song_1.mid")) == "MIDI song", "a name that differs only in case gets its index");
    Check(Text(ReadFile(out / "samples_2.sf2")) == "SF2 samples", "the samples folder's name is taken");
    Check(Text(ReadFile(out / "sfz" / "Song" / "Track 0.sfz")).starts_with("<region>"), "the SFZ files go in sfz");
    Check(Text(ReadFile(out / "sfz" / "samples" / "shared.wav")) == "WAV", "the shared samples go in sfz/samples");
    Check(!fs::exists(out / "bad.mid"), "a sequence that fails writes nothing");
    Check(fake.before_inputs == 1 && fake.prepared_loops == 12, "the hooks run, and prepare sets the options");
}

void TestBatchInputs()
{
    const TempFolder folder("sequence_export_frame_test");
    const fs::path a = folder.Path() / "x.a";
    const fs::path b = folder.Path() / "x.b";
    WriteFile(a, std::string("One\n"));
    WriteFile(b, std::string("Two\n"));
    FakeConverter fake;

    const int collision = Run({"fake", "--quiet", Utf8(a), Utf8(b)}, fake.converter);
    const int missing = Run({"fake", "--quiet", Utf8(folder.Path() / "none")}, fake.converter);
    const int is_folder = Run({"fake", "--quiet", Utf8(folder.Path())}, fake.converter);
    const int listed = Run({"fake", "--list", "--only", "^T", Utf8(b)}, fake.converter);
    const int none = Run({"fake", "--quiet", "--only", "^Z", Utf8(b)}, fake.converter);
    const std::string several = ErrorOf([&]() { Run({"fake", "-o", "out", Utf8(a), Utf8(b)}, fake.converter); });

    Check(collision == 1, "an input whose output folder another input took fails");
    Check(Text(ReadFile(folder.Path() / "x_fake" / "One.mid")) == "MIDI One", "the first input still converts");
    Check(!fs::exists(folder.Path() / "x_fake" / "Two.mid"), "the second input writes nothing");
    Check(missing == 1 && is_folder == 1 && listed == 0 && none == 1, "missing, folder, --list and --only inputs");
    Check(several == "--output requires a single input. Otherwise, output goes in a folder beside each input.",
          "--output with several inputs: " + several);
    Check(Run({"fake", "--version"}, fake.converter) == 0 && Run({"fake"}, fake.converter) == 2, "--version and usage");
    Check(fake.before_inputs == 5, "the hook runs for each run with inputs, after the options are checked");
}

void TestOtherInputs()
{
    // An input that another of the program's converters reads doesn't reach the converter. Its status counts.
    const TempFolder folder("sequence_export_frame_test");
    const fs::path a = folder.Path() / "x.a";
    const fs::path b = folder.Path() / "y.other";
    WriteFile(a, std::string("One\n"));
    WriteFile(b, std::string("Two\n"));
    FakeConverter fake;
    std::vector<std::string> others;
    const OtherInput<PerformOptions> other = [&others](const std::string& path, const Options<PerformOptions>& options,
                                                       const std::regex&, bool several,
                                                       std::vector<std::pair<fs::path, std::string>>&)
    {
        if (!path.ends_with(".other"))
        {
            return std::optional<int>();
        }

        others.push_back(Utf8(PathFromUtf8(path).filename()) + (options.quiet && several ? " quiet, several" : ""));
        return std::optional<int>(1);
    };

    const int rc = Run({"fake", "--quiet", Utf8(a), Utf8(b)}, fake.converter, other);

    Check(rc == 1 && others == std::vector<std::string>{"y.other quiet, several"}, "the other converter's input");
    Check(Text(ReadFile(folder.Path() / "x_fake" / "One.mid")) == "MIDI One", "the converter's input");
    Check(!fs::exists(folder.Path() / "y_fake"), "nothing of the other input from the converter");
}

void TestRunMain()
{
    char program[] = "fake";
    char* argv[] = {program, nullptr};

    const int thrown =
        RunMain(1, argv, [](const std::vector<std::string>&) -> int { throw std::runtime_error("broken"); });
    const int returned = RunMain(
        1, argv, [](const std::vector<std::string>& args) { return args == std::vector<std::string>{"fake"} ? 3 : 4; });

    Check(thrown == 1, "an exception gives exit status 1");
    Check(returned == 3, "the arguments reach the run, and its status comes back");
}

// The help that UsageText puts together, with a program's lines and wording.
void TestUsage()
{
    ArchiveConverter<ntdlib::sar::SoundArchive> c;
    c.program = "xyz2midi";
    c.description = "xyz2midi: convert sound archive sequences (.bxsar) to MIDI.\n";
    c.extension = ".bxsar";
    const std::string text = UsageText(c);

    Check(text.starts_with("xyz2midi: convert sound archive sequences (.bxsar) to MIDI.\n"
                           "\n"
                           "usage: xyz2midi [options] <archive.bxsar> [<archive.bxsar> ...]\n"),
          "the description, then the usage line: " + text);
    Check(text.find("<archive name>_xyz2midi\nbeside the archive. You can also drop archives onto xyz2midi.exe on "
                    "Windows\nor Xyz2midi.app on macOS.\n") != std::string::npos,
          "the output folder, the program and the app take the name");
    Check(text.find("  --seed N       initial game PRNG state (default 0x12345678)\n") != std::string::npos,
          "the --seed line's default wording");
    Check(text.find("repeat for more variables\n  --list") != std::string::npos, "no extra options");
    Check(text.ends_with("  --version      show the version\n"), "the last line");

    c.seed_option = "initial game random state (default 0x12345678)";
    c.extra_options = "  --xyz FILE     read FILE\n";
    const std::string worded = UsageText(c);

    Check(worded.find("  --seed N       initial game random state (default 0x12345678)\n") != std::string::npos,
          "the program's wording of --seed");
    Check(worded.find("repeat for more variables\n  --xyz FILE     read FILE\n  --list") != std::string::npos,
          "the extra options after --var");

    c.settings.voice_limit = 24;
    const std::string voices = UsageText(c);

    Check(voices.find("repeat for more variables\n"
                      "  --voices N     drop the notes that the game drops for lack of a voice,\n"
                      "                 with N voices (the game has 24 unless it sets another\n"
                      "                 count). The result only approximates the game.\n"
                      "  --voices off   keep every note (default)\n"
                      "  --xyz FILE     read FILE\n") != std::string::npos,
          "the --voices lines with the console's count, before the extra options");
}

} // namespace

int main()
{
    TestText();
    TestFiles();
    TestGroupFilesInSarcs();
    TestOptions();
    TestVoicesOption();
    TestArchiveOptions();
    TestSwitchHasNoVoices();
    TestBatch();
    TestBatchInputs();
    TestOtherInputs();
    TestRunMain();
    TestUsage();

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("frame tests passed\n");

    return 0;
}
