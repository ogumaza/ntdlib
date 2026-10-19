// SPDX-License-Identifier: MIT

// Tests of the archive play test's main (ntdlib/testing/archive_play.h) on a console made up here: the command lines
// that it rejects, its checks that the archive has the sound, that the sound starts and that it sounds, the WAV file,
// and the channel log, with each channel's track, its voice's volume and pitch, and its outputs. The files go in a
// temporary folder.

#include "ntdlib/testing/archive_play.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "seqlib/testing/check.h"
#include "temp_folder.h"
#include "xsflib/files.h"
#include "xsflib/testing/channel_log.h"
#include "xsflib/testing/check.h"
#include "xsflib/wav.h"

using ntdlib::nwsnd::ChannelId;
using ntdlib::nwsnd::kNoChannel;
using ntdlib::nwsnd::kNoVoice;
using ntdlib::nwsnd::VoiceId;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

namespace
{

// The made-up console. Its archive is a text file with a sound's label on each line. A sound called "Silent" never
// sounds, and one called "Broken" doesn't start. The others play from the first frame. Each frame's sample is the
// frame's number times the gain, on the left, and its negative on the right. A second has three frames.
struct TestEnvelope
{
    uint32_t GetStatus() const
    {
        return status;
    }

    float Level() const
    {
        return level;
    }

    uint32_t status = 0;
    float level = 0.0f;
};

struct TestChannel
{
    uint8_t key = 0;
    uint8_t original_key = 0;
    float velocity_volume = 0.0f;
    TestEnvelope envelope;
    VoiceId voice = kNoVoice;
    float init_pan = 0.0f;
    ChannelId next_in_track = kNoChannel;

    // The console's outputs.
    float pan = 0.0f;
    float span = 0.0f;
    float send = 0.0f;
};

struct TestChannels
{
    const std::list<ChannelId>& InUse() const
    {
        return in_use;
    }

    const TestChannel& At(ChannelId place) const
    {
        return pool.at(static_cast<size_t>(place));
    }

    std::array<TestChannel, 3> pool;
    std::list<ChannelId> in_use;
};

struct TestVoice
{
    float Volume() const
    {
        return volume;
    }

    float Pitch() const
    {
        return pitch;
    }

    float volume = 0.0f;
    float pitch = 0.0f;
};

struct TestVoices
{
    const TestVoice& At(VoiceId place) const
    {
        return pool.at(static_cast<size_t>(place));
    }

    std::array<TestVoice, 2> pool{{{0.125f, 2.0f}, {0.75f, 1.5f}}};
};

struct TestTrack
{
    ChannelId ChannelList() const
    {
        return newest;
    }

    ChannelId newest = kNoChannel;
};

// A player with tracks 3 and 5 open.
struct TestPlayer
{
    const TestTrack* Track(int index) const
    {
        return index == 3 || index == 5 ? &tracks.at(static_cast<size_t>(index)) : nullptr;
    }

    std::array<TestTrack, 16> tracks;
};

struct TestRandom
{
    uint32_t State() const
    {
        return state;
    }

    uint32_t state = 0;
};

struct TestArchive
{
    std::optional<uint32_t> FindSound(const std::string& label) const
    {
        for (size_t i = 0; i < sounds.size(); ++i)
        {
            if (sounds[i] == label)
            {
                return static_cast<uint32_t>(i);
            }
        }

        return std::nullopt;
    }

    std::vector<std::string> sounds;
};

struct TestOptions
{
    int voice_count = 96;
    int gain = 1;
};

// The options of the last model made.
TestOptions last_options;

class TestEngine
{
public:
    TestEngine(const TestArchive& archive, const TestOptions& options) : archive_(archive), gain_(options.gain)
    {
    }

    TestPlayer* StartSequence(uint32_t index)
    {
        silent_ = archive_.sounds.at(index) == "Silent";
        return archive_.sounds.at(index) == "Broken" ? nullptr : &player_;
    }

    // Frame 1 has no channels. In frame 2, channel 2 sounds on voice 1, and no track's list holds it. Channel 0 sounds
    // without a voice as track 3's newest channel. From frame 3, channel 1 sounds on voice 0 too. It comes after the
    // other two among the channels in use, and before channel 0 in track 3's list.
    void RunFrame()
    {
        ++frame_;
        random_.state = static_cast<uint32_t>(frame_) * 0x01010101;
        const auto sample = static_cast<int16_t>(silent_ ? 0 : frame_ * gain_);
        output_ = {sample, static_cast<int16_t>(-sample)};
        if (frame_ == 2)
        {
            channels_.pool[2] = {.key = 60,
                                 .original_key = 64,
                                 .velocity_volume = 0.5f,
                                 .envelope = {3, -12.5f},
                                 .voice = 1,
                                 .init_pan = 0.25f,
                                 .next_in_track = kNoChannel,
                                 .pan = -0.5f,
                                 .span = 1.0f,
                                 .send = 0.5f};
            channels_.pool[0] = {.key = 48,
                                 .original_key = 60,
                                 .velocity_volume = 1.0f,
                                 .envelope = {0, 0.0f},
                                 .voice = kNoVoice,
                                 .init_pan = -0.25f,
                                 .next_in_track = kNoChannel,
                                 .pan = 0.5f,
                                 .span = 0.0f,
                                 .send = 0.0f};
            channels_.in_use = {2, 0};
            player_.tracks[3].newest = 0;
        }
        else if (frame_ == 3)
        {
            channels_.pool[1] = {.key = 72,
                                 .original_key = 72,
                                 .velocity_volume = 0.25f,
                                 .envelope = {4, -3.0f},
                                 .voice = 0,
                                 .init_pan = 0.0f,
                                 .next_in_track = 0,
                                 .pan = 0.0f,
                                 .span = 0.5f,
                                 .send = 1.0f};
            channels_.in_use.push_back(1);
            player_.tracks[3].newest = 1;
        }
    }

    TestRandom& Rng()
    {
        return random_;
    }

    TestChannels& Channels()
    {
        return channels_;
    }

    TestVoices& Voices()
    {
        return voices_;
    }

    std::span<const int16_t> Output() const
    {
        return output_;
    }

private:
    const TestArchive& archive_;
    int gain_;
    bool silent_ = false;
    int frame_ = 0;
    std::array<int16_t, 2> output_{};
    TestRandom random_;
    TestChannels channels_;
    TestVoices voices_;
    TestPlayer player_;
};

struct TestModel
{
    TestModel(const TestArchive& archive, const TestOptions& options)
        : engine(std::make_unique<TestEngine>(archive, options))
    {
        last_options = options;
    }

    std::unique_ptr<TestEngine> engine;
};

struct TestConsole
{
    using Archive = TestArchive;
    using Options = TestOptions;
    using Model = TestModel;

    static constexpr bool kSends = true;
    static constexpr const char* kUsage = " [--gain <factor>]";

    // An empty file isn't an archive.
    static Archive Load(std::vector<uint8_t> bytes)
    {
        if (bytes.empty())
        {
            throw std::runtime_error("not an archive");
        }

        Archive archive;
        std::istringstream lines(std::string(bytes.begin(), bytes.end()));
        for (std::string line; std::getline(lines, line);)
        {
            archive.sounds.push_back(line);
        }

        return archive;
    }

    static int Frames(int seconds)
    {
        return seconds * 3;
    }

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.engine->Output();
    }

    static void Outputs(const TestChannel& channel, xsflib::testing::ChannelState& state)
    {
        state.pan = channel.pan;
        state.span = channel.span;
        state.main_send = channel.send;
        state.fx_send = {channel.send / 2, 0.0f, channel.send / 4};
    }

    // --gain takes a whole number from 1.
    static bool Option(const std::string& name, const char* value, Options& options)
    {
        if (name != "--gain")
        {
            return false;
        }

        options.gain = std::atoi(value);
        return options.gain > 0;
    }
};

// What a run of ArchivePlay returned, and what its checks printed.
struct Run
{
    int status = 0;
    std::string printed;
};

// Runs ArchivePlay on the made-up console with `arguments` after the program's name. The run's checks are counted apart
// from the checks of this test.
Run Play(std::vector<std::string> arguments)
{
    arguments.insert(arguments.begin(), "archive_play_test");
    std::vector<char*> argv;
    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const int checks = seqlib::testing::checks;
    const int failures = seqlib::testing::failures;
    std::ostream* const failure_stream = seqlib::testing::failure_stream;
    std::ostream* const success_stream = seqlib::testing::success_stream;
    std::ostringstream printed;
    seqlib::testing::checks = 0;
    seqlib::testing::failures = 0;
    seqlib::testing::failure_stream = &printed;
    seqlib::testing::success_stream = &printed;
    last_options = {0, 0};

    Run run;
    run.status = ntdlib::testing::ArchivePlay<TestConsole>(static_cast<int>(arguments.size()), argv.data());
    run.printed = printed.str();

    seqlib::testing::checks = checks;
    seqlib::testing::failures = failures;
    seqlib::testing::failure_stream = failure_stream;
    seqlib::testing::success_stream = success_stream;

    return run;
}

std::vector<uint8_t> Bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

std::vector<uint8_t> FileBytes(const std::filesystem::path& path)
{
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(path, bytes);

    return bytes;
}

void CheckRefusals(const TempFolder& folder)
{
    const std::string archive = folder.Write("archive.txt", Bytes("Sound\n")).string();
    const std::vector<std::vector<std::string>> command_lines = {
        {},
        {archive, "Sound"},
        {archive, "Sound", "1", "--wav"},
        {archive, "Sound", "1", "--loud", "1"},
        {archive, "Sound", "1", "--gain", "0"},
        {archive, "Sound", "1", "--voices", "0"},
    };
    for (size_t i = 0; i < command_lines.size(); ++i)
    {
        const Run run = Play(command_lines[i]);
        Check(run.status == 2 && run.printed.empty() && last_options.voice_count == 0,
              "wrong command line " + std::to_string(i) + " gives the usage and plays nothing");
    }
}

// Whether `printed` is the text of a failed check whose message is `message`. Newer builds of seqlib start the text
// with the check's place, its file and line. Older builds start it with "CHECK failed". The test takes either form.
bool FailedCheck(const std::string& printed, const std::string& message)
{
    const std::string text = "CHECK failed: " + message;

    return printed == text || printed.ends_with(": " + text);
}

void CheckFailures(const TempFolder& folder)
{
    const std::string archive = folder.Write("archive.txt", Bytes("Sound\nSilent\nBroken\n")).string();

    Run run = Play({(folder.Path() / "missing.txt").string(), "Sound", "1"});
    Check(run.status == 1 && run.printed.empty(), "a file that can't be read fails");
    run = Play({folder.Write("empty.txt", {}).string(), "Sound", "1"});
    Check(run.status == 1 && run.printed.empty(), "an archive that can't be loaded fails");

    run = Play({archive, "Missing", "1"});
    Check(run.status == 1 && FailedCheck(run.printed, "the archive has Missing\n"),
          "a sound that the archive lacks fails");
    run = Play({archive, "Broken", "1"});
    Check(run.status == 1 && FailedCheck(run.printed, "Broken starts\n"), "a sound that doesn't start fails");

    const std::filesystem::path wav = folder.Path() / "silent.wav";
    run = Play({archive, "Silent", "1", "--wav", wav.string()});
    Check(run.status == 1 && FailedCheck(run.printed, "Silent sounds in its first 1 s\n1 check(s) failed\n"),
          "a sound that never sounds fails");
    Check(FileBytes(wav) == xsflib::WavFile(std::vector<int16_t>(6, 0), 2, 48000),
          "the WAV file is written when the sound never sounds");
    Check(last_options.voice_count == 96 && last_options.gain == 1, "the options keep their defaults");
}

void CheckPlay(const TempFolder& folder)
{
    const std::string archive = folder.Write("archive.txt", Bytes("Silent\nSound\n")).string();
    const std::filesystem::path wav = folder.Path() / "sound.wav";
    const std::filesystem::path log = folder.Path() / "sound.tsv";
    const Run run = Play(
        {archive, "Sound", "1", "--voices", "24", "--gain", "2", "--wav", wav.string(), "--channels", log.string()});
    Check(run.status == 0 && run.printed == "archive play tests passed\n", "a sound that sounds passes");
    Check(last_options.voice_count == 24 && last_options.gain == 2,
          "--voices and the console's options reach the model");
    Check(FileBytes(wav) == xsflib::WavFile(std::vector<int16_t>{2, -2, 4, -4, 6, -6}, 2, 48000),
          "the WAV file holds every frame's output");

    const std::string expected =
        "frame\trandom\tnote\tchannel\ttrack\tkey\toriginal_key\tvelocity\tenvelope\tlevel\tvolume\tpitch\tpan\tspan\t"
        "note_pan\tmain_send\tfx_send_a\tfx_send_b\tfx_send_c\n"
        "1\t01010101\t0\t00000000\t-1\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\n"
        "2\t02020202\t1\t00000002\t-1\t60\t64\t0.5\t3\t-12.5\t0.75\t1.5\t-0.5\t1\t0.25\t0.5\t0.25\t0\t0.125\n"
        "2\t02020202\t2\t00000000\t3\t48\t60\t1\t0\t0\t0\t0\t0.5\t0\t-0.25\t0\t0\t0\t0\n"
        "3\t03030303\t1\t00000002\t-1\t60\t64\t0.5\t3\t-12.5\t0.75\t1.5\t-0.5\t1\t0.25\t0.5\t0.25\t0\t0.125\n"
        "3\t03030303\t2\t00000000\t3\t48\t60\t1\t0\t0\t0\t0\t0.5\t0\t-0.25\t0\t0\t0\t0\n"
        "3\t03030303\t3\t00000001\t3\t72\t72\t0.25\t4\t-3\t0.125\t2\t0\t0.5\t0\t1\t0.5\t0\t0.25\n";
    Check(FileBytes(log) == Bytes(expected), "the log gives each channel's track, voice and outputs");
}

} // namespace

int main()
{
    const TempFolder folder("archive_play_test");
    CheckRefusals(folder);
    CheckFailures(folder);
    CheckPlay(folder);

    return xsflib::testing::Summary("archive play");
}
