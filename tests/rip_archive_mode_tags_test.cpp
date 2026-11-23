// SPDX-License-Identifier: MIT

// Tests the tags of the minis that the archive mode of rip tools writes (rip/archive_mode.h), with a made-up rip
// format, on archives built with sar_files.h in a temporary folder: a sound's volume, the length tags, the tempo and
// pitch tags, and the variants of a sound that waits for the game's variables.

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rip/archive_mode.h"
#include "ntdlib/sar/formats.h"
#include "rip_archive_mode_test_util.h"
#include "temp_folder.h"

namespace
{

using namespace rip_archive_mode_tests;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

// A tool's sound_volume gives a sound's mini its volume tag, and the length analysis the same factor. A sound at 1 has
// no tag. The shortest decimal reads back as the same float.
void TestSoundVolume()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;

    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    tool.sound_volume = [](const TestRip&, const rip::Job& job)
    {
        return job.label == "SEQ_A" ? 0.71f : 1.0f;
    };
    std::map<std::string, float> analyzed;
    tool.analyze_lengths =
        [&analyzed](std::vector<rip::Job>& jobs, sar::SoundArchive, const TestFormat::ArchiveDescriptor&, unsigned)
    {
        for (rip::Job& job : jobs)
        {
            job.timing = EndedAt50Ms();
            analyzed[job.label] = job.volume;
        }
    };
    RipLoose(tool, input, "out", "", messages, true);

    Check(analyzed == std::map<std::string, float>{{"SEQ_A", 0.71f}, {"SEQ_B", 1.0f}},
          "the length analysis gets each sound's volume");
    const fs::path mini_a = folder.Path() / "out" / "0000 SEQ_A.minitst";
    const fs::path mini_b = folder.Path() / "out" / "0001 SEQ_B.minitst";
    Check(MiniTag(mini_a, "volume") == "0.71", "a mini holds its sound's volume");
    Check(MiniTag(mini_a, "_lib") == "loose.tstlib", "the mini still has its library");
    Check(MiniTag(mini_b, "_lib") == "loose.tstlib", "the other mini is there");
    Check(MiniTag(mini_b, "volume").empty(), "a sound at 1 has no volume tag");

    xsflib::psf::Tags tags;
    rip::SetRatioTag("volume", 2.37f, tags);
    Check(tags["volume"] == "2.37", "2.37 keeps its decimal: " + tags["volume"]);
}

// A mini's length tags count from the first sample that isn't 0. SEQ_A loops: 8 s from its first sample at 1 s, with
// passes of 3 s from 3 s. SEQ_B ends at 2.5 s, 2 s after its first sample. A timing without a first sample stops the
// rip with an error, and no mini follows.
void TestLengthTags()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;
    std::string refused;

    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    tool.analyze_lengths =
        [](std::vector<rip::Job>& jobs, sar::SoundArchive, const TestFormat::ArchiveDescriptor&, unsigned)
    {
        for (rip::Job& job : jobs)
        {
            job.timing = job.label == "SEQ_A" ? xsflib::rip::PassesAt(900, 300, 10.0, 7000.0) : rip::EndAt(250, 10.0);
            job.timing->first_sample_ms = job.label == "SEQ_A" ? 1000 : 500;
            job.timing->first_sound_ms = job.timing->first_sample_ms;
        }
    };
    rip::ArchiveRipTool<TestFormat, TestRip> without_first = TestTool();
    without_first.analyze_lengths =
        [](std::vector<rip::Job>& jobs, sar::SoundArchive, const TestFormat::ArchiveDescriptor&, unsigned)
    {
        for (rip::Job& job : jobs)
        {
            job.timing = rip::EndAt(250, 10.0);
        }
    };
    RipLoose(tool, input, "out", "", messages, true);
    RipLoose(without_first, input, "refused", "", refused, true);

    const fs::path mini_a = folder.Path() / "out" / "0000 SEQ_A.minitst";
    const fs::path mini_b = folder.Path() / "out" / "0001 SEQ_B.minitst";
    Check(MiniTag(mini_a, "length") == "0:08.000" && MiniTag(mini_a, "fade") == "7" &&
              MiniTag(mini_a, "loop_start") == "0:02.000" && MiniTag(mini_a, "loop_length") == "0:03.000",
          "a loop's tags count from its first sample: " + MiniTag(mini_a, "length") + ", " +
              MiniTag(mini_a, "loop_start") + ", " + MiniTag(mini_a, "loop_length"));
    Check(MiniTag(mini_b, "length") == "0:02.000" && MiniTag(mini_b, "fade") == "1" &&
              MiniTag(mini_b, "loop_start").empty(),
          "an ending's length runs from its first sample to its last sound: " + MiniTag(mini_b, "length"));
    Check(refused.find("error: the length analysis gave 0000 SEQ_A.minitst no first sample") != std::string::npos &&
              !fs::exists(folder.Path() / "refused" / "0000 SEQ_A.minitst"),
          "a timing without a first sample stops the rip: " + refused);
}

// A state's tempo ratio and pitch go into the tags that the tool names, as the shortest decimals that read back as the
// same floats. A job at 1 gets neither tag, and so does every job of a tool that names no tags. The length analysis
// gets each state's values.
void TestTempoAndPitchTags()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;

    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    tool.sound_states = [](const TestRip&, const std::vector<rip::Job>& selected)
    {
        std::vector<rip::Job> jobs;
        for (const rip::Job& job : selected)
        {
            jobs.push_back(job);
            if (job.label == "SEQ_A")
            {
                rip::Job faster = job;
                faster.label = "SEQ_A 140 BPM";
                faster.tempo_ratio = 140.0f / 120.0f;
                faster.pitch = 1.122462f;
                jobs.push_back(faster);
                faster.label = "SEQ_A 200 BPM";
                faster.tempo_ratio = 200.0f / 120.0f;
                faster.pitch = 1.0f;
                jobs.push_back(faster);
            }
        }

        return jobs;
    };
    std::map<std::string, std::pair<float, float>> analyzed;
    tool.analyze_lengths =
        [&analyzed](std::vector<rip::Job>& jobs, sar::SoundArchive, const TestFormat::ArchiveDescriptor&, unsigned)
    {
        for (rip::Job& job : jobs)
        {
            job.timing = EndedAt50Ms();
            analyzed[job.label] = {job.tempo_ratio, job.pitch};
        }
    };
    rip::ArchiveRipTool<TestFormat, TestRip> without_tags = tool;
    tool.tempo_tag = "tst_tempo";
    tool.pitch_tag = "tst_pitch";
    RipLoose(tool, input, "tags", "", messages, true);
    RipLoose(without_tags, input, "none", "", messages, true);

    const fs::path tags = folder.Path() / "tags";
    Check(analyzed.at("SEQ_A 140 BPM") == std::pair(140.0f / 120.0f, 1.122462f) &&
              analyzed.at("SEQ_A") == std::pair(1.0f, 1.0f),
          "the length analysis gets each state's tempo ratio and pitch");
    Check(MiniTag(tags / "0000 SEQ_A_140_BPM.minitst", "tst_tempo") == "1.1666666" &&
              MiniTag(tags / "0000 SEQ_A_140_BPM.minitst", "tst_pitch") == "1.122462",
          "a state's mini holds its tempo ratio and its pitch");
    Check(MiniTag(tags / "0000 SEQ_A_200_BPM.minitst", "tst_tempo") == "1.6666666" &&
              MiniTag(tags / "0000 SEQ_A_200_BPM.minitst", "tst_pitch").empty() &&
              MiniTag(tags / "0000 SEQ_A_200_BPM.minitst", "_lib") == "loose.tstlib",
          "a pitch of 1 gets no tag");
    Check(MiniTag(tags / "0000 SEQ_A.minitst", "tst_tempo").empty() &&
              MiniTag(tags / "0000 SEQ_A.minitst", "tst_pitch").empty() &&
              MiniTag(tags / "0001 SEQ_B.minitst", "tst_tempo").empty() &&
              MiniTag(tags / "0000 SEQ_A.minitst", "_lib") == "loose.tstlib",
          "a job at 1 gets neither tag");
    const fs::path none = folder.Path() / "none" / "0000 SEQ_A_140_BPM.minitst";
    Check(MiniTag(none, "_lib") == "loose.tstlib" && MiniTag(none, "tst_tempo").empty() &&
              MiniTag(none, "tst_pitch").empty(),
          "a tool that names no tags writes none");
}

// A sound with variants is ripped once for each of them, and not without them. The length analysis gets each
// variant's variables, and a variant that plays nothing is skipped. A variant's mini has the sound's number and its
// label with the variables after it, in a file name's safe characters (xsflib::rip::FileNames). Its tags hold the label
// and the variables. The library gets each sound once. With sound_tag_holds_sound_label, the tag tst_sound holds the
// sound's label alone.
void TestVariants()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;

    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    tool.variables_tag = "tst_var";
    tool.variants = [](const TestRip&, const rip::Job& job)
    {
        return job.label == "SEQ_B" ? std::vector<xsflib::psf::Variables>{{{1, 4}}, {{1, 5}}, {{1, 4}, {17, -2}}}
                                    : std::vector<xsflib::psf::Variables>();
    };

    std::vector<std::pair<std::string, xsflib::psf::Variables>> analyzed;
    tool.analyze_lengths =
        [&analyzed](std::vector<rip::Job>& jobs, sar::SoundArchive, const TestFormat::ArchiveDescriptor&, unsigned)
    {
        for (rip::Job& job : jobs)
        {
            analyzed.emplace_back(job.label, job.variables);
            job.timing = EndedAt50Ms();
            job.timing->silent = job.variables == xsflib::psf::Variables{{1, 5}};
        }
    };

    std::vector<std::vector<uint32_t>> received;
    const auto default_library = tool.build_library;
    tool.build_library = [&received, default_library](const TestRip& to_rip, std::span<const uint32_t> sounds,
                                                      const xsflib::psf::Tags& tags)
    {
        received.emplace_back(sounds.begin(), sounds.end());
        return default_library(to_rip, sounds, tags);
    };

    RipLoose(tool, input, "out", "", messages, true);

    const fs::path out = folder.Path() / "out";
    const fs::path both = out / "0001 SEQ_B_1_4__17_-2.minitst";
    using Analyzed = std::vector<std::pair<std::string, xsflib::psf::Variables>>;
    Check(analyzed == Analyzed{{"SEQ_A", {}},
                               {"SEQ_B 1=4", {{1, 4}}},
                               {"SEQ_B 1=5", {{1, 5}}},
                               {"SEQ_B 1=4, 17=-2", {{1, 4}, {17, -2}}}},
          "the analysis gets each variant with its variables");
    Check(fs::exists(out / "0000 SEQ_A.minitst") && fs::exists(out / "0001 SEQ_B_1_4.minitst") && fs::exists(both) &&
              !fs::exists(out / "0001 SEQ_B_1_5.minitst") && !fs::exists(out / "0001 SEQ_B.minitst"),
          "a mini for SEQ_A and for each variant of SEQ_B that plays");
    Check(MiniTag(both, "tst_var") == "1=4, 17=-2" && MiniTag(both, "title") == "SEQ_B 1=4, 17=-2" &&
              MiniTag(both, "tst_sound") == "SEQ_B 1=4, 17=-2" &&
              MiniTag(out / "0000 SEQ_A.minitst", "tst_var").empty(),
          "a variant's mini holds its variables and its label");
    Check(messages.find("skipped SEQ_B 1=5: it makes no sound") != std::string::npos,
          "the variant that plays nothing is skipped: " + messages);
    Check(received == std::vector<std::vector<uint32_t>>{{0, 1}}, "build_library gets each sound once");

    tool.sound_tag_holds_sound_label = true;
    RipLoose(tool, input, "sound_labels", "", messages, true);
    const fs::path sound_label = folder.Path() / "sound_labels" / "0001 SEQ_B_1_4__17_-2.minitst";
    Check(MiniTag(sound_label, "tst_sound") == "SEQ_B" && MiniTag(sound_label, "title") == "SEQ_B 1=4, 17=-2" &&
              MiniTag(folder.Path() / "sound_labels" / "0000 SEQ_A.minitst", "tst_sound") == "SEQ_A",
          "with sound_tag_holds_sound_label, tst_sound holds the sound's label alone");
    tool.sound_tag_holds_sound_label = false;

    // A format may write its variables in its form, in the label and in the tag.
    tool.format_variables = [](const xsflib::psf::Variables& variables)
    {
        return "n" + std::to_string(variables.size());
    };
    analyzed.clear();
    RipLoose(tool, input, "formatted", "", messages, true);
    const fs::path formatted = folder.Path() / "formatted" / "0001 SEQ_B_n2.minitst";
    Check(fs::exists(formatted) && MiniTag(formatted, "tst_var") == "n2" && MiniTag(formatted, "title") == "SEQ_B n2",
          "a variant's label and tag take the format's form");
}

} // namespace

int main()
{
    TestSoundVolume();
    TestLengthTags();
    TestTempoAndPitchTags();
    TestVariants();

    return xsflib::testing::Summary("rip_archive_mode_tags");
}
