// SPDX-License-Identifier: MIT

// Tests of the Wii's model of NW4R SND (rvl/snd), on the AX model with no DSP: the parameter blocks that a note's
// channel sets up, the ramps from frame to frame, the release and the end of a channel, the voices that NW4R SND drops
// by priority when all are in use, a short sequence played from an archive, a sequence in an arrangement, sequences
// that start together, what the builds of October 2006, 2008 and 2009 do differently, the build of AXFX that a build
// links, the loops that a length analysis counts, snapshots of the model, and the end of a wave without the DSP.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/snd/arrangement.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/effects.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/sequence.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/testing/rsar_files.h"

using ntdlib::testing::rsar_files::Bytes;
using xsflib::testing::Check;

namespace
{

using namespace ntdlib::rvl;

// Builds of NW4R SND: those of Wii Sports, Mario Kart Wii, Wii Music and Animal Crossing, the builds of November 20
// 2008 and September 24 2009, and the one that comes with AX of October 2006.
constexpr snd::SndBuild kBuild2007{20070608};
constexpr snd::SndBuild kBuildMarch2008{20080308};
constexpr snd::SndBuild kBuildJune2008{20080620};
constexpr snd::SndBuild kBuildSeptember2008{20080919};
constexpr snd::SndBuild kBuildNovember2008{20081120};
constexpr snd::SndBuild kBuildSeptember2009{20090924};
constexpr snd::SndBuild kBuildOctober2006{0, true};

// An archive whose sequence sound has the tracks in the mask `tracks` and plays track 0 from the start of `commands`.
// The archive's waves loop unless `loop` is false.
ntdlib::rsar::SoundArchive MakeArchive(const Bytes& commands, uint32_t tracks, bool loop = true)
{
    return ntdlib::rsar::SoundArchive::Load(
        ntdlib::testing::rsar_files::MakeArchive(ntdlib::testing::rsar_files::MakeSequence(commands), 0, tracks, loop),
        snd::kRsarOptions);
}

// AX, initialised as the game does before NW4R SND starts, with a compressor table of zeros. The build of NW4R SND that
// comes with AX of October 2006 runs on that AX.
ax::System& Initialised(ax::System& ax, snd::SndBuild build)
{
    ax.Init(build.October2006() ? ax::Build::kOctober2006 : ax::Build::kDecember2006);

    return ax;
}

// The AX model and the engine on it. The engine follows `build` of NW4R SND. A frame runs as in the game: AX's
// interrupt, then the sound frame.
struct Model
{
    explicit Model(const Bytes& commands = {0xff}, snd::SndBuild build = {}, uint32_t tracks = 1)
        : archive(MakeArchive(commands, tracks)), engine{memory, Initialised(ax, build), archive, build}
    {
    }

    void RunFrame()
    {
        ax.StartFrame();
        ax.FinishFrame();
        engine.FrameProcess();
    }

    // The PB of the first voice that AX hands out: the last of its voices.
    ax::Pb FirstPb() const
    {
        return ax.ReadPb(ax.VoiceCount() - 1);
    }

    // Runs `frames` frames, then returns the first voice's PB as the microcode reads it in the next.
    ax::Pb PbAfter(int frames)
    {
        for (int i = 0; i < frames; i++)
        {
            RunFrame();
        }

        ax.StartFrame();
        const ax::Pb next = FirstPb();
        ax.FinishFrame();
        engine.FrameProcess();

        return next;
    }

    // Runs frames until the sound ends, at most `frames` of them. Returns the number of frames run.
    int RunUntilEnd(int frames)
    {
        int ran = 0;
        while (engine.IsBusy() && ran < frames)
        {
            RunFrame();
            ran++;
        }

        return ran;
    }

    ntdlib::dsp::WiiMemory memory;
    ax::System ax{memory};
    ntdlib::rsar::SoundArchive archive;
    snd::Engine engine;
};

// A second of a 32 kHz PCM16 mono wave that doesn't loop.
snd::WaveData MakeWave(ntdlib::dsp::WiiMemory& memory)
{
    const std::vector<uint8_t> kSamples(64000, 0);
    snd::WaveData wave;
    wave.format = ntdlib::rsar::WaveEncoding::kPcm16;
    wave.sample_rate = 32000;
    wave.loop_end = 32000;
    wave.channels[0].data = memory.Store(kSamples.data(), kSamples.size());

    return wave;
}

void TestNote()
{
    Model model;
    const snd::WaveData wave = MakeWave(model.memory);

    // A note a fifth up, centred, at full volume. A bank's NoteOn starts notes this way during a sound frame.
    snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, 64, -1, -1);
    Check(channel != nullptr, "a channel for a note");
    if (channel == nullptr)
    {
        return;
    }

    channel->key = 67;
    channel->original_key = 60;
    channel->Start(model.engine, wave, -1);
    model.engine.Channels().UpdateAllChannel(model.engine);
    model.engine.Voices().UpdateAllVoices();

    // AX's first voice is its last. It gets the note's parameters with the next frame.
    const ax::Pb pb = model.PbAfter(0);

    const auto kCenter = static_cast<uint16_t>(32768.0f * ntdlib::nwsnd::tables::kStereoPan[0][128]);
    const uint32_t ratio = ax::Join(pb.src.ratio_hi, pb.src.ratio_lo);
    const uint32_t kExpectedRatio = static_cast<uint32_t>(65536.0f * ntdlib::nwsnd::CalcPitchRatio(7 * 256));
    Check(pb.state == 1, "the note runs");
    Check(pb.mix.l == kCenter && pb.mix.r == kCenter && pb.mix.delta_l == 0 &&
              pb.MixerCtrl() == (ax::mixer::kL | ax::mixer::kR),
          "the note's mix");
    Check(pb.ve.current_volume == 32767 && pb.ve.current_delta == 0, "the note's volume");
    Check(ratio == kExpectedRatio && pb.src_select == 0 && pb.coef_select == 2, "the note's pitch and converter");
    Check(pb.addr.format == 10 &&
              ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo) == ntdlib::dsp::kMem2Base / 2 &&
              ax::Join(pb.addr.end_address_hi, pb.addr.end_address_lo) == ntdlib::dsp::kMem2Base / 2 + 31999,
          "the note's samples");
    Check(ax::Join(pb.addr.loop_address_hi, pb.addr.loop_address_lo) == snd::kZeroBuffer / 2 && pb.addr.loop_flag == 0,
          "a note that doesn't loop goes on to the zero buffer");

    // At the default release rate, 127, the envelope falls to its floor at once. The channel stops in its next update.
    channel->Release(model.engine);
    model.RunFrame();
    model.RunFrame();

    Check(model.engine.Channels().ActiveCount() == 0 && model.engine.Voices().PrioVoices().empty(),
          "a released note stops, and its channel and voice are free");

    model.ax.StartFrame();
    Check(model.FirstPb().state == 0, "the AX voice stopped");
}

void TestRelease()
{
    Model model;
    const snd::WaveData wave = MakeWave(model.memory);
    snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, 64, -1, -1);
    channel->envelope.SetRelease(100);
    channel->Start(model.engine, wave, -1);
    model.RunFrame();
    model.RunFrame();

    // A slower release ramps the envelope down over each frame. The volume falls from where the last ramp ended.
    channel->Release(model.engine);
    const ax::Pb first = model.PbAfter(1);
    model.ax.StartFrame();
    const ax::Pb second = model.FirstPb();

    const int16_t delta = static_cast<int16_t>(first.ve.current_delta);
    Check(delta < 0, "the release ramps down");
    Check(second.ve.current_volume == static_cast<uint16_t>(first.ve.current_volume + delta * 96),
          "the next frame starts where the ramp ended");
    Check(model.engine.Voices().At(model.engine.Voices().PrioVoices().front()).Priority() == 1,
          "a released voice drops to priority 1");
}

// An AX voice of priority 31 with MakeWave's wave, acquired after the first frame of `model`.
snd::AxVoice& WaveVoice(Model& model)
{
    model.RunFrame();
    const snd::WaveData wave = MakeWave(model.memory);
    snd::AxVoice* voice = model.engine.AxVoices().AcquireAxVoice(31, nullptr);
    voice->Setup(wave.channels[0].data, ax::SampleFormat::kPcm16, 32000);
    voice->SetAddr(false, wave.channels[0].data, 0, wave.loop_end);

    return *voice;
}

// The volume at which an AX voice starts a frame after its envelope lands at 74 and then aims for 0, under `build`. A
// step towards 0 would take the volume below 0 in a frame. The step is therefore 0. A 2007 build then leaves the volume
// where it was. A 2008 build lands it on the target (AxVoiceParamBlock::Sync). A released note's last frame does the
// same.
uint16_t VolumeAfterLastStep(snd::SndBuild build)
{
    Model model({0xff}, build);
    snd::AxVoice& voice = WaveVoice(model);

    voice.SetVe(74.5f / 32767.0f, 74.5f / 32767.0f);
    voice.Sync();
    voice.SetVe(0.0f, 0.0f);
    voice.Sync();
    model.ax.StartFrame();

    return model.FirstPb().ve.current_volume;
}

void TestLastStep()
{
    const uint16_t older = VolumeAfterLastStep(kBuild2007);
    const uint16_t newer = VolumeAfterLastStep(kBuildMarch2008);

    Check(older == 74, "a 2007 build leaves the volume where it was (" + std::to_string(older) + ")");
    Check(newer == 0, "a 2008 build lands the volume on its target (" + std::to_string(newer) + ")");
}

void TestDrop()
{
    Model model;
    const snd::WaveData wave = MakeWave(model.memory);

    // 96 notes take every voice. The 97th, of higher priority, takes the oldest voice of the lowest priority. Its
    // channel stops. The pool hands the new note another channel.
    std::vector<snd::ChannelId> places;
    for (int i = 0; i < 96; i++)
    {
        snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, i < 10 ? 32 : 64, -1, -1);
        Check(channel != nullptr, "a channel while voices are free");
        if (channel != nullptr)
        {
            channel->Start(model.engine, wave, -1);
            places.push_back(channel->place);
        }
    }

    model.RunFrame();
    const snd::Channel* lower = model.engine.Channels().AllocChannel(model.engine, 1, 16, -1, -1);
    const snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, 100, -1, -1);

    const std::list<snd::ChannelId>& in_use = model.engine.Channels().InUse();
    std::vector<std::size_t> dropped;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        if (std::find(in_use.begin(), in_use.end(), places[i]) == in_use.end())
        {
            dropped.push_back(i);
        }
    }

    Check(lower == nullptr, "no voice for a lower priority");
    Check(channel != nullptr && dropped.size() == 1 && dropped[0] == 0, "the oldest lowest-priority note dropped");
}

// The volume at which a note of `velocity` starts on instrument 0. The region's volume is 100 and the sound's volume is
// 90.
uint16_t ExpectedVolume(int velocity)
{
    const float scale = static_cast<float>(velocity) / 127.0f;
    return static_cast<uint16_t>(static_cast<int>(32767.0f * (scale * scale * (100.0f / 127.0f) * (90.0f / 127.0f))));
}

void TestSequence()
{
    // Release 127, a note of 48 ticks (key 60, velocity 100), and the end. At the default tempo of 120 and timebase of
    // 48, 48 ticks last 500 ms, or about 167 frames.
    Model model({0xd3, 0x7f, 0x3c, 0x64, 0x30, 0xff});
    snd::SeqPlayer* player = model.engine.StartSequence(0);
    Check(player != nullptr, "the sequence starts");
    if (player == nullptr)
    {
        return;
    }

    const ax::Pb pb = model.PbAfter(1);

    const uint16_t kExpectedVe = ExpectedVolume(100);
    Check(pb.state == 1 && pb.addr.format == 0 && pb.addr.loop_flag == 1, "the note plays its looping ADPCM wave");
    Check(pb.ve.current_volume == kExpectedVe, "the note's volume: velocity squared, the region's and the sound's (" +
                                                   std::to_string(pb.ve.current_volume) + ", expected " +
                                                   std::to_string(kExpectedVe) + ")");

    // The sound ends when its note has been released and stopped, and its track has reached the end.
    const int frames = 2 + model.RunUntilEnd(398);

    Check(!model.engine.IsBusy() && frames >= 167 && frames <= 170,
          "the sequence ends after its note (" + std::to_string(frames) + " frames)");
}

// A sequence in an arrangement: the tracks left out play at a volume of 0, the tempo ratio
// changes how fast the ticks go, and the variables are set before the first tick.
void TestArrangement()
{
    // Track 0 opens track 1 at 11 and plays key 60 at velocity 100. Track 1 plays key 64 at velocity 50. Each note
    // lasts 48 ticks and has the fastest release.
    const Bytes kTwoTracks = {0x88, 0x01, 0x00, 0x00, 0x0b, 0xd3, 0x7f, 0x3c, 0x64,
                              0x30, 0xff, 0xd3, 0x7f, 0x40, 0x32, 0x30, 0xff};

    // The volumes of the voices that play after the first frame, from the lowest.
    const auto volumes = [&kTwoTracks](uint16_t tracks)
    {
        Model model(kTwoTracks, kBuild2007, 3);
        model.engine.StartSequence(0, {.tracks = tracks});
        model.RunFrame();
        model.ax.StartFrame();
        std::vector<uint16_t> found;
        for (uint32_t i = 0; i < model.ax.VoiceCount(); i++)
        {
            const ax::Pb pb = model.ax.ReadPb(i);
            if (pb.state == 1)
            {
                found.push_back(pb.ve.current_volume);
            }
        }

        model.ax.FinishFrame();
        model.engine.FrameProcess();
        std::sort(found.begin(), found.end());

        return found;
    };

    const uint16_t kLoud = ExpectedVolume(100);
    const uint16_t kQuiet = ExpectedVolume(50);
    Check(volumes(0xffff) == std::vector<uint16_t>{kQuiet, kLoud}, "both tracks play");
    Check(volumes(0x0001) == std::vector<uint16_t>{0, kLoud}, "track 1 plays at a volume of 0");
    Check(volumes(0x0002) == std::vector<uint16_t>{0, kQuiet}, "track 0 plays at a volume of 0");

    // A note of 48 ticks and the end. At the default tempo it lasts about 167 frames.
    const auto frames = [](float ratio)
    {
        Model model({0xd3, 0x7f, 0x3c, 0x64, 0x30, 0xff}, kBuild2007);
        model.engine.StartSequence(0, {.tempo_ratio = ratio});

        return model.RunUntilEnd(1000);
    };

    const int normal = frames(1.0f);
    const int fast = frames(2.0f);
    const int slow = frames(0.5f);
    Check(fast >= 84 && fast <= 86 && slow >= 334 && slow <= 336 && normal >= 167 && normal <= 170,
          "the tempo ratio scales the ticks (" + std::to_string(slow) + ", " + std::to_string(normal) + " and " +
              std::to_string(fast) + " frames)");

    // Track 0 compares variable 0 with 1, and jumps to 16 if they're equal. There it plays key 64. Otherwise it plays
    // key 60.
    const Bytes kBranch = {0xf0, 0x90, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x10, 0xd3,
                           0x7f, 0x3c, 0x64, 0x30, 0xff, 0xd3, 0x7f, 0x40, 0x64, 0x30, 0xff};

    const auto ratio = [](const xsflib::psf::Variables& variables, Model& model)
    {
        model.engine.StartSequence(0, {.variables = variables});
        const ax::Pb pb = model.PbAfter(1);
        return ax::Join(pb.src.ratio_hi, pb.src.ratio_lo) / 65536.0;
    };

    Model unset(kBranch, kBuild2007);
    Model set(kBranch, kBuild2007);
    const double low = ratio({}, unset);
    const double high = ratio({{0, 1}, {16, 3}, {31, -7}}, set);
    Check(std::abs(high / low - std::pow(2.0, 4.0 / 12.0)) < 0.001, "the first tick reads the player's variable");
    Check(set.engine.GlobalVariables()[0] == 3 && set.engine.GlobalVariables()[15] == -7,
          "the global variables are set");
}

// Sequences that start together: each player's notes get the sends that the
// game sets on its sound, and a channel keeps them after its track has let it go.
void TestSoundsTogether()
{
    // The aux A sends of the voices that play after the first frame.
    const auto sends_after_start = [](int players, const std::vector<snd::SoundSend>& sends)
    {
        Model model({0xd3, 0x7f, 0x3c, 0x64, 0x30, 0xff}, kBuild2007);
        model.engine.SetEffects({}, sends, 0);
        for (int i = 0; i < players; i++)
        {
            model.engine.StartSequence(0);
        }

        model.RunFrame();
        model.ax.StartFrame();
        std::vector<uint16_t> found;
        for (uint32_t i = 0; i < model.ax.VoiceCount(); i++)
        {
            const ax::Pb pb = model.ax.ReadPb(i);
            if (pb.state == 1)
            {
                found.push_back(pb.mix.aux_a_l);
            }
        }

        model.ax.FinishFrame();
        model.engine.FrameProcess();

        return found;
    };

    const std::vector<snd::SoundSend> kSendA = {{.sound = 0, .bus = 0, .send = 0.5f}};
    const std::vector<uint16_t> alone = sends_after_start(1, kSendA);
    const std::vector<uint16_t> together = sends_after_start(2, kSendA);
    Check(alone.size() == 1 && alone[0] > 0, "the note of a sound with a send plays into aux A");
    Check(together.size() == 2 && together[0] == alone[0] && together[1] == alone[0],
          "each of two sequences that start together gets its sound's send");
    Check(sends_after_start(2, {}) == std::vector<uint16_t>{0, 0}, "a sound without sends plays into no aux bus");

    // A note of a tick with the slowest release. The track ends after it and lets the channel go while the note
    // releases.
    Model model({0xd3, 0x00, 0x3c, 0x64, 0x01, 0xff}, kBuild2007);
    model.engine.SetEffects({}, kSendA, 0);
    snd::SeqPlayer* player = model.engine.StartSequence(0);
    const uint16_t first = model.PbAfter(1).mix.aux_a_l;
    const uint16_t later = model.PbAfter(30).mix.aux_a_l;
    Check(player != nullptr && !player->IsActive() && model.engine.IsBusy() && first == alone[0] && later == first,
          "a channel that its track has let go keeps its sound's send (" + std::to_string(first) + ", then " +
              std::to_string(later) + ")");
    Check(model.engine.SoundSends(0)[0] == 0.5f && model.engine.SoundSends(1)[0] == 0.0f &&
              model.engine.SoundSends(-1)[0] == 0.0f,
          "a player that the engine doesn't have gets no sends");
}

// The voices' PBs after the first frame of a sequence of `commands` played under `build`, and the model to play on.
struct Started
{
    Started(const Bytes& commands, snd::SndBuild build) : model(commands, build)
    {
        player = model.engine.StartSequence(0);
        pb = model.PbAfter(1);
    }

    Model model;
    snd::SeqPlayer* player = nullptr;
    ax::Pb pb;
};

void TestSweepPitch()
{
    // A sweep of -64 over the note: two semitones down at the start in a 2007 build, one in a 2008 build.
    const Bytes kCommands = {0xe3, 0xff, 0xc0, 0x3c, 0x64, 0x30, 0xff};

    const Started older(kCommands, kBuild2007);
    const Started newer(kCommands, kBuildMarch2008);

    const double older_ratio = ax::Join(older.pb.src.ratio_hi, older.pb.src.ratio_lo) / 65536.0;
    const double newer_ratio = ax::Join(newer.pb.src.ratio_hi, newer.pb.src.ratio_lo) / 65536.0;
    Check(std::abs(older_ratio - std::pow(2.0, -2.0 / 12.0)) < 0.005,
          "a 2007 build sweeps from two semitones down (" + std::to_string(older_ratio) + ")");
    Check(std::abs(newer_ratio - std::pow(2.0, -1.0 / 12.0)) < 0.005,
          "a 2008 build sweeps from one semitone down (" + std::to_string(newer_ratio) + ")");
}

void TestTempo()
{
    // The fastest release, a tempo from variable 1 (which is -1) and a note of 48 ticks. A 2007 build races through the
    // sequence. A 2008 build stops the sequence and holds the note.
    const Bytes kCommands = {0xd3, 0x7f, 0xa1, 0xe1, 0x01, 0x3c, 0x64, 0x30, 0xff};
    Started older(kCommands, kBuild2007);
    Started newer(kCommands, kBuildMarch2008);

    const int older_frames = older.model.RunUntilEnd(300);
    const int newer_frames = newer.model.RunUntilEnd(300);

    Check(older_frames < 10, "a 2007 build plays a tempo of -1 at full speed (" + std::to_string(older_frames) + ")");
    Check(newer_frames == 300 && newer.model.engine.Channels().ActiveCount() == 1,
          "a 2008 build stops at a tempo of 0");
    Check(newer.player->IsStuck(newer.model.engine) && newer.player->IsSettled(newer.model.engine),
          "a sequence at a tempo of 0 waits for ever");
}

void TestVelocityRange()
{
    // From June 2008, a velocity range of 50 turns a velocity of 100 into 39. Earlier builds ignore it.
    const Bytes kCommands = {0xb3, 0x32, 0x3c, 0x64, 0x30, 0xff};

    const Started march(kCommands, kBuildMarch2008);
    const Started june(kCommands, kBuildJune2008);

    Check(march.pb.ve.current_volume == ExpectedVolume(100), "a build of March 2008 ignores the velocity range");
    Check(june.pb.ve.current_volume == ExpectedVolume(39),
          "a build of June 2008 scales the velocity (" + std::to_string(june.pb.ve.current_volume) + ")");
}

void TestEnvReset()
{
    // The slowest attack, then the bank's envelope again from June 2008. The bank's attack is instant.
    const Bytes kCommands = {0xd0, 0x00, 0xfb, 0x3c, 0x7f, 0x30, 0xff};

    const Started march(kCommands, kBuildMarch2008);
    const Started june(kCommands, kBuildJune2008);

    Check(march.pb.ve.current_volume < ExpectedVolume(127) / 2, "a build of March 2008 keeps the track's attack");
    Check(june.pb.ve.current_volume == ExpectedVolume(127), "a build of June 2008 takes the bank's attack again");
}

// Whether `pb`'s biquad filter is on with the coefficients in `row` of a table.
bool BiquadIs(const ax::Pb& pb, const int16_t* row)
{
    return pb.biquad.on == ax::kBiquadOn && pb.biquad.b0 == static_cast<uint16_t>(row[0]) &&
           pb.biquad.b1 == static_cast<uint16_t>(row[1]) && pb.biquad.b2 == static_cast<uint16_t>(row[2]) &&
           pb.biquad.a1 == static_cast<uint16_t>(row[3]) && pb.biquad.a2 == static_cast<uint16_t>(row[4]);
}

void TestBiquad()
{
    // The high-pass filter (type 2) at 90 over 127 under a note of 96 ticks that doesn't wait, then the value 0 at tick
    // 24 and no filter at tick 48. At 90, the filter takes row 96 × 90 / 127 = 68 of its table.
    const Bytes kCommands = {0xc7, 0x00, 0xb4, 0x02, 0xb5, 0x5a, 0x3c, 0x64, 0x60, 0x80,
                             0x18, 0xb5, 0x00, 0x80, 0x18, 0xb4, 0x00, 0x80, 0x18, 0xff};

    const Started march(kCommands, kBuildMarch2008);
    Started june(kCommands, kBuildJune2008);
    const ax::Pb at_0 = june.model.PbAfter(140);
    const ax::Pb off = june.model.PbAfter(90);

    Check(march.pb.biquad.on == 0, "a build of March 2008 ignores the biquad filter");
    Check(BiquadIs(june.pb, ntdlib::nwsnd::tables::kBiquadHpf[68]), "a build of June 2008 filters the note");
    Check(BiquadIs(at_0, ntdlib::nwsnd::tables::kBiquadHpf[0]), "the filter's value changes under the note");
    Check(off.biquad.on == 0 && off.state == 1, "type 0 turns the filter off under the note");

    // The band-passes take their rows at value × (2 - value). A type without a preset has no filter.
    const std::optional<std::array<uint16_t, 5>> band_pass = snd::BiquadFilterCoefs(3, 0.5f);

    Check(band_pass && (*band_pass)[0] == static_cast<uint16_t>(ntdlib::nwsnd::tables::kBiquadBpf512[90][0]) &&
              (*band_pass)[4] == static_cast<uint16_t>(ntdlib::nwsnd::tables::kBiquadBpf512[90][4]),
          "a band-pass at 0.5 takes row 121 × 0.75");
    Check(!snd::BiquadFilterCoefs(0, 0.5f) && !snd::BiquadFilterCoefs(6, 0.5f) && !snd::BiquadFilterCoefs(64, 0.5f),
          "types 0, 6 and 64 have no coefficients");
}

void TestIgnoreNoteOff()
{
    // The fastest release, a note of 4 ticks on instrument 6 (whose region ignores note-offs) and a long wait. A 2008
    // build keeps the note when its length runs out. A 2007 build releases it.
    const Bytes kCommands = {0xd3, 0x7f, 0x81, 0x06, 0x3c, 0x64, 0x04, 0x80, 0x60, 0xff};
    Started older(kCommands, kBuild2007);
    Started newer(kCommands, kBuildMarch2008);

    for (int i = 0; i < 40; i++)
    {
        older.model.RunFrame();
        newer.model.RunFrame();
    }

    Check(older.model.engine.Channels().ActiveCount() == 0, "a 2007 build releases the note at its end");
    Check(newer.model.engine.Channels().ActiveCount() == 1, "a 2008 build lets the note ignore its note-off");
}

void TestKeyGroup()
{
    // The fastest release, no note wait, and two long notes at once on instrument 7, in key group 1. In a 2008 build,
    // the second note releases the first.
    const Bytes kCommands = {0xd3, 0x7f, 0xc7, 0x00, 0x81, 0x07, 0x3c, 0x64, 0x60, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff};
    Started older(kCommands, kBuild2007);
    Started newer(kCommands, kBuildMarch2008);

    older.model.RunFrame();
    newer.model.RunFrame();

    Check(older.model.engine.Channels().ActiveCount() == 2, "a 2007 build plays both notes");
    Check(newer.model.engine.Channels().ActiveCount() == 1, "a 2008 build releases the key group's first note");
}

// The first PB of a note under `build`. `setup` sets the note's channel before the note starts. `adjust` changes the
// note's voice after the channel has set it up.
template <typename Setup, typename Adjust>
ax::Pb FirstPbOfNote(snd::SndBuild build, Setup setup, Adjust adjust)
{
    Model model({0xff}, build);
    const snd::WaveData wave = MakeWave(model.memory);
    snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, 64, -1, -1);
    Check(channel != nullptr, "a channel for a note");
    if (channel == nullptr)
    {
        return {};
    }

    setup(*channel);
    channel->Start(model.engine, wave, -1);
    model.engine.Channels().UpdateAllChannel(model.engine);
    Check(channel->voice != snd::kNoVoice, "a voice for the note");
    if (channel->voice == snd::kNoVoice)
    {
        return {};
    }

    adjust(model.engine.Voices().At(channel->voice));
    model.engine.Voices().UpdateAllVoices();
    model.ax.StartFrame();

    return model.FirstPb();
}

// The first PB of a note under `build`, with the voice as the channel sets it up.
template <typename Setup>
ax::Pb FirstPbOfNote(snd::SndBuild build, Setup setup)
{
    return FirstPbOfNote(build, setup, [](snd::Voice&) {});
}

void TestLpfTable()
{
    // Builds from November 2008 on take the low-pass filter's cutoff from a table of 80 to 16000 Hz. Older builds
    // compute 32000 * 2^(10 (scale - 1)). Both kinds hold the scale to 0 and 1.
    struct Cutoff
    {
        float scale;
        uint16_t table;
        uint16_t power;
    };

    constexpr std::array<Cutoff, 7> kCutoffs = {{
        {-0.25f, 80, 31},               // held to 0
        {0.25f, 160, 176},              // entry 3
        {0x1.79cd6ep-2f, 320, 403},     // entry 6, just below the edge of entry 7
        {0.5f, 800, 1000},              // entry 10
        {0x1.cccccap-1f, 12800, 15999}, // the float below 0.9: entry 22, the last before 16000 Hz
        {0.9f, 16000, 15999},           // 16000 Hz: the filter is off
        {1.5f, 16000, 32000},           // held to 1
    }};

    for (const Cutoff& cutoff : kCutoffs)
    {
        const uint16_t table = snd::CalcLpfFreq(cutoff.scale, kBuildNovember2008);
        const uint16_t power = snd::CalcLpfFreq(cutoff.scale, kBuildSeptember2008);
        Check(table == cutoff.table && power == cutoff.power, "the cutoffs at " + std::to_string(cutoff.scale) + ": " +
                                                                  std::to_string(table) + " and " +
                                                                  std::to_string(power) + " Hz");
    }

    // The game divides by 1/30 to count the table's steps. A multiplication by 30 would take entry 7 at 0x1.79cd6ep-2f
    // in kCutoffs. The 3DS's multiplication by 29.999998 would take entry 5 at 0x1.57ab4cp-2f.
    Check(snd::CalcLpfFreq(0x1.57ab4cp-2f, kBuildNovember2008) == 320, "the table's steps divide by 1/30");

    // A note under the track's low-pass value -0.5: the scale 0.5.
    const auto filtered = [](snd::Channel& channel)
    {
        channel.lpf = -0.5f;
    };
    const ax::Pb older = FirstPbOfNote(kBuildSeptember2008, filtered);
    const ax::Pb newer = FirstPbOfNote(kBuildNovember2008, filtered);
    const ax::LpfCoefs at_1000 = ax::CalcLpfCoefs(1000);
    const ax::LpfCoefs at_800 = ax::CalcLpfCoefs(800);

    Check(older.lpf.on == ax::kLpfOn && older.lpf.a0 == at_1000.a0 && older.lpf.b0 == at_1000.b0,
          "a build of September 2008 filters the note at 1000 Hz");
    Check(newer.lpf.on == ax::kLpfOn && newer.lpf.a0 == at_800.a0 && newer.lpf.b0 == at_800.b0,
          "a build of November 2008 filters the note at 800 Hz");
}

// The envelope as the microcode reads it in the first two frames of an AX voice whose gain stays at `gain`, under
// `build`.
std::array<ax::PbVe, 2> EnvelopeAtGain(float gain, snd::SndBuild build)
{
    Model model({0xff}, build);
    snd::AxVoice& voice = WaveVoice(model);

    std::array<ax::PbVe, 2> envelope;
    for (ax::PbVe& frame : envelope)
    {
        voice.SetVe(gain, gain);
        voice.Sync();
        model.ax.StartFrame();
        frame = model.FirstPb().ve;
        model.ax.FinishFrame();
    }

    return envelope;
}

void TestGainsAboveFull()
{
    // A gain of 1.5 is (int)(32767 * 1.5) = 49150 in AX's envelope. Builds from September 2009 on keep the envelope
    // within 0 to 0xffff and hold it at 49150. Older builds keep it within 0 to 32767. Their step therefore takes it
    // towards 32767: (32767 - 49150) / 96 = -170 a sample. Only builds from September 2009 on hand AX such a gain.
    const std::array<ax::PbVe, 2> newer = EnvelopeAtGain(1.5f, kBuildSeptember2009);
    const std::array<ax::PbVe, 2> older = EnvelopeAtGain(1.5f, kBuildNovember2008);

    Check(newer[0].current_volume == 49150 && newer[0].current_delta == 0 && newer[1].current_volume == 49150 &&
              newer[1].current_delta == 0,
          "a build of September 2009 holds the envelope at 49150");
    Check(older[0].current_volume == 49150 && static_cast<int16_t>(older[0].current_delta) == -170,
          "a build of November 2008 steps the envelope towards 32767");

    // A gain of 2.5 is (int)(32767 * 2.5) = 81917, held to 0xffff.
    Check(EnvelopeAtGain(2.5f, kBuildSeptember2009)[0].current_volume == 0xffff, "the envelope stops at 0xffff");

    // A note at the track volume 1.5. Older builds hold the voice's volume to full level.
    const auto loud = [](snd::Channel& channel)
    {
        channel.track_volume = 1.5f;
    };

    Check(FirstPbOfNote(kBuildSeptember2009, loud).ve.current_volume == 49150,
          "a build of September 2009 plays the note at 1.5 times full level");
    Check(FirstPbOfNote(kBuildNovember2008, loud).ve.current_volume == 32767,
          "a build of November 2008 plays the note at full level");

    // A note at the main out volume 1.5. The mix multiplies the main send by it. Older builds hold the volume to full
    // level.
    const auto louder_out = [](snd::Channel& channel)
    {
        channel.outputs.volume = 1.5f;
    };
    const float center = ntdlib::nwsnd::tables::kStereoPan[0][128];
    Check(FirstPbOfNote(kBuildSeptember2009, louder_out).mix.l == static_cast<uint16_t>(32768.0f * (1.5f * center)),
          "a build of September 2009 mixes the note at 1.5 times full level");
    Check(FirstPbOfNote(kBuildNovember2008, louder_out).mix.l == static_cast<uint16_t>(32768.0f * center),
          "a build of November 2008 mixes the note at full level");

    // A volume LFO at 84 cycles a second goes 0.252 of the way round in the 3 ms frame. That's about its peak. The
    // envelope's target is then 6 dB above full level. Builds from September 2009 on ramp the envelope up. Older builds
    // hold it at full level.
    const auto tremolo = [](snd::Channel& channel)
    {
        channel.lfo_types[0] = 1;
        channel.lfos[0].param.depth = 1.0f;
        channel.lfos[0].param.speed = 84.0f;
    };
    const ax::PbVe rising = FirstPbOfNote(kBuildSeptember2009, tremolo).ve;
    const ax::PbVe held = FirstPbOfNote(kBuildNovember2008, tremolo).ve;

    Check(rising.current_volume == 32767 && static_cast<int16_t>(rising.current_delta) > 0,
          "a build of September 2009 ramps the envelope above full level");
    Check(held.current_volume == 32767 && held.current_delta == 0,
          "a build of November 2008 holds the envelope at full level");

    // A voice whose envelope ramps from 1.5 times full level to full level in its first frame. Builds from September
    // 2009 on start AX's envelope at 49150 and step it by (32767 - 49150) / 96 = -170 a sample. Older builds hold the
    // start to full level. A note can't start above full level. A volume LFO starts at 0. The test therefore sets the
    // voice's envelope.
    const auto from_above_full = [](snd::Voice& voice)
    {
        voice.SetVeVolume(1.0f, 1.5f);
    };
    const auto plain = [](snd::Channel&) {};
    const ax::PbVe falling = FirstPbOfNote(kBuildSeptember2009, plain, from_above_full).ve;
    const ax::PbVe level = FirstPbOfNote(kBuildNovember2008, plain, from_above_full).ve;
    Check(falling.current_volume == 49150 && static_cast<int16_t>(falling.current_delta) == -170,
          "a build of September 2009 starts the envelope at 49150");
    Check(level.current_volume == 32767 && level.current_delta == 0,
          "a build of November 2008 starts the envelope at full level");
}

// The number of times a sequence of `commands`, with tracks 0 and 1, reports a loop in 400 frames (1.2 s). Returns -1
// if the sequence doesn't start or has ended by then. Every sequence that these tests count runs for ever.
int LoopsOf(const Bytes& commands)
{
    Model model(commands, {}, 3);
    const bool started = model.engine.StartSequence(0) != nullptr;
    int loops = 0;
    model.engine.SetLoopHook([&loops](int, uint32_t) { loops++; });

    for (int i = 0; i < 400; i++)
    {
        model.RunFrame();
    }

    return started && model.engine.IsBusy() ? loops : -1;
}

void TestLoops()
{
    // A note of 12 ticks (125 ms) repeated forever on track 0: by an endless loop, by a jump back, and by a jump back
    // inside a subroutine. A subroutine that draws a random value until it differs from the last jumps back that way. A
    // wait of 12 ticks repeated by a jump back changes nothing in the sound.
    const int endless = LoopsOf({0xd4, 0x00, 0x3c, 0x64, 0x0c, 0xfc, 0xff});
    const int jump = LoopsOf({0x3c, 0x64, 0x0c, 0x89, 0x00, 0x00, 0x00});
    const int in_call = LoopsOf({0x8a, 0x00, 0x00, 0x05, 0xff, 0x3c, 0x64, 0x0c, 0x89, 0x00, 0x00, 0x05});
    const int idle = LoopsOf({0x80, 0x0c, 0x89, 0x00, 0x00, 0x00});

    // Track 0 opens track 1 and ends. Track 1 repeats a note of 12 ticks, or a wait of 12 ticks, forever.
    const int noted = LoopsOf({0x88, 0x01, 0x00, 0x00, 0x06, 0xff, 0xd4, 0x00, 0x3c, 0x64, 0x0c, 0xfc});
    const int silent = LoopsOf({0x88, 0x01, 0x00, 0x00, 0x06, 0xff, 0xd4, 0x00, 0x80, 0x0c, 0xfc});

    // Track 0 opens track 1, then waits a tick and jumps back forever. Track 1 repeats a note of 12 ticks forever.
    const int polled =
        LoopsOf({0x88, 0x01, 0x00, 0x00, 0x0b, 0x80, 0x01, 0x89, 0x00, 0x00, 0x05, 0xd4, 0x00, 0x3c, 0x64, 0x0c, 0xfc});

    Check(endless >= 8 && jump >= 8, "an endless loop and a jump back on track 0 are loops (" +
                                         std::to_string(endless) + ", " + std::to_string(jump) + ")");
    Check(in_call == 0, "a jump back inside a subroutine isn't a loop");
    Check(idle == 0, "a loop of waits on track 0 isn't the sequence's");
    Check(noted >= 8,
          "once track 0 has ended, track 1's loops of notes are the sequence's (" + std::to_string(noted) + ")");
    Check(silent == 0, "a loop of waits on track 1 isn't the sequence's");
    Check(polled >= 8 && polled <= 10,
          "while track 0 waits a tick at a time, track 1's loops are the sequence's (" + std::to_string(polled) + ")");
}

void TestHeldNotes()
{
    // Track 0 waits out a note of 10,000 ticks or of 9,000 ticks. The note of 10,000 ticks is held until the game stops
    // the sound.
    Started held({0x3c, 0x64, 0xce, 0x10, 0xff}, kBuild2007);
    Started long_note({0x3c, 0x64, 0xc6, 0x28, 0xff}, kBuild2007);

    held.model.RunUntilEnd(10);
    long_note.model.RunUntilEnd(10);

    // Track 0 plays a note of 10,000 ticks, or of 9,000, without waiting for it. The note decays at once to a full
    // sustain. The track then calls a subroutine. The subroutine bends the note up for 6 ticks and back for 6 ticks,
    // then jumps back to its start.
    const int holding = LoopsOf({0xd1, 0x7f, 0xd2, 0x7f, 0xc7, 0x00, 0x3c, 0x64, 0xce, 0x10, 0x8a, 0x00, 0x00, 0x0f,
                                 0xff, 0xc4, 0x10, 0x80, 0x06, 0xc4, 0x00, 0x80, 0x06, 0x89, 0x00, 0x00, 0x0f});
    const int not_held = LoopsOf({0xd1, 0x7f, 0xd2, 0x7f, 0xc7, 0x00, 0x3c, 0x64, 0xc6, 0x28, 0x8a, 0x00, 0x00, 0x0f,
                                  0xff, 0xc4, 0x10, 0x80, 0x06, 0xc4, 0x00, 0x80, 0x06, 0x89, 0x00, 0x00, 0x0f});

    Check(held.player->IsStuck(held.model.engine) && held.player->IsSettled(held.model.engine),
          "a track that waits out a held note won't read on before the game stops the sound");
    Check(!long_note.player->IsStuck(long_note.model.engine), "a note of 9,000 ticks isn't a held one");
    Check(holding >= 8, "a subroutine that loops for ever while the track holds a note is the sequence's loop (" +
                            std::to_string(holding) + ")");
    Check(not_held == 0, "one that loops while the track holds a note of 9,000 ticks isn't");
}

// Runs `frames` frames. Returns the PBs that each frame hands the microcode, one frame after another.
std::vector<uint8_t> PbsOfFrames(Model& model, int frames)
{
    constexpr uint32_t kSize = ax::kVoiceCount * 0x140;
    std::vector<uint8_t> pbs;
    for (int i = 0; i < frames; i++)
    {
        model.RunFrame();
        const uint8_t* bytes = model.memory.At(ax::layout::kPbs, kSize);
        pbs.insert(pbs.end(), bytes, bytes + kSize);
    }

    return pbs;
}

void TestSnapshot()
{
    // Notes of different lengths and a release of 64. At the snapshot, one note is releasing, one is sounding and one
    // is still to come.
    Model model({0xd3, 0x40, 0x3c, 0x64, 0x30, 0x40, 0x64, 0x18, 0x43, 0x50, 0x60, 0xff});
    Check(model.engine.StartSequence(0) != nullptr, "the sequence for the snapshot starts");
    PbsOfFrames(model, 200);

    // Everything the frames change: the memory with its PBs, AX, and the engine with its players.
    const ntdlib::dsp::WiiMemory::Snapshot memory = model.memory.Save();
    const ax::System ax = model.ax;
    const snd::Engine::Snapshot engine = model.engine.Save();
    const std::vector<uint8_t> first = PbsOfFrames(model, 300);
    const bool busy = model.engine.IsBusy();

    model.memory.Restore(memory);
    model.ax = ax;
    model.engine.Restore(engine);
    const std::vector<uint8_t> second = PbsOfFrames(model, 300);

    Check(first == second && model.engine.IsBusy() == busy, "the frames after a restored snapshot are the same");

    // A sequence started after the snapshot can't be undone.
    model.engine.StartSequence(0);
    bool refused = false;
    try
    {
        model.engine.Restore(engine);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }

    Check(refused, "a snapshot from before another sequence started is refused");
}

void TestWaveEndWithoutDsp()
{
    // A note of 48 ticks (167 frames) on a wave that doesn't loop, played on the model without the DSP. Its channel
    // stops once the wave has played to its end, a frame or two in. The microcode would stop it at the same point.
    const ntdlib::rsar::SoundArchive archive = MakeArchive({0x3c, 0x64, 0x30, 0xff}, 1, false);
    auto model = std::make_unique<snd::ArchiveModel>();
    model->StartWithoutDsp(archive);

    Check(model->engine->StartSequence(0) != nullptr, "the sequence without the DSP starts");
    model->RunFrameWithoutDsp();
    Check(model->engine->Channels().ActiveCount() == 1, "the note starts");

    int frames = 1;
    while (model->engine->Channels().ActiveCount() > 0 && frames < 200)
    {
        model->RunFrameWithoutDsp();
        frames++;
    }

    Check(frames <= 3, "the note's channel stops when its wave ends (after " + std::to_string(frames) + " frames)");
}

// The sound frame, counting from 1, in which the first note of `commands` starts under `build`, or 0 if none starts
// within `frames`.
int NoteFrame(const Bytes& commands, snd::SndBuild build, int frames)
{
    Model model(commands, build);
    model.engine.StartSequence(0);
    for (int frame = 1; frame <= frames; frame++)
    {
        model.RunFrame();
        if (model.engine.Channels().ActiveCount() > 0)
        {
            return frame;
        }
    }

    return 0;
}

void TestTicksOctober2006()
{
    // A wait of a tick, then a note. At a tempo of 120 a tick lasts 3.47 frames. A 2007 build plays the second tick in
    // the frame it falls in, the fourth. The build of October 2006 counts 416ths of a tick, and takes the whole ticks
    // out before it adds the frame's 120. It plays the second tick in the fifth frame.
    const Bytes kOneTick = {0x80, 0x01, 0x3c, 0x64, 0x30, 0xff};

    Check(NoteFrame(kOneTick, kBuild2007, 10) == 4, "a 2007 build plays the second tick in the fourth frame");
    Check(NoteFrame(kOneTick, kBuildOctober2006, 10) == 5, "the build of October 2006 plays it in the fifth");

    // After a wait of 1,250 ticks, the note starts in frame 1 + ceil(1250 * 416 / 120). A 2007 build takes 48 ticks a
    // beat, and reaches the note about six frames later.
    const Bytes kLongWait = {0x80, 0x89, 0x62, 0x3c, 0x64, 0x30, 0xff};

    const int older = NoteFrame(kLongWait, kBuild2007, 5000);
    const int october = NoteFrame(kLongWait, kBuildOctober2006, 5000);

    Check(october == 4335, "the build of October 2006's ticks (note in frame " + std::to_string(october) + ")");
    Check(older >= 4339 && older <= 4343, "a 2007 build's ticks (note in frame " + std::to_string(older) + ")");
}

void TestPanOctober2006()
{
    // A pan of 1. The build of October 2006 raises it to 2, a step in from the left. The right side then gets a little.
    // A 2007 build plays it at the far left.
    const Bytes kCommands = {0xc0, 0x01, 0x3c, 0x64, 0x30, 0xff};

    const Started older(kCommands, kBuild2007);
    const Started october(kCommands, kBuildOctober2006);

    Check(older.pb.mix.l != 0 && older.pb.mix.r == 0, "a 2007 build plays a pan of 1 at the far left");
    Check(october.pb.mix.r != 0 && october.pb.mix.l > october.pb.mix.r,
          "the build of October 2006 plays a pan of 1 a step in");
}

// The frames from the start of `commands` under `build` until its notes have all stopped, at most `frames`.
int FramesUntilSilent(const Bytes& commands, snd::SndBuild build, int frames)
{
    Started started(commands, build);
    int ran = 1;
    while (started.model.engine.Channels().ActiveCount() > 0 && ran < frames)
    {
        started.model.RunFrame();
        ran++;
    }

    return ran;
}

void TestReleaseStopOctober2006()
{
    // A track at volume 0 plays a note of 4 ticks with a slow release, and waits on without it. The build of October
    // 2006 stops a released note once its volume times its envelope is 0, here at once. A 2007 build waits for the
    // envelope of a note that a track owns.
    const Bytes kCommands = {0xc7, 0x00, 0xc1, 0x00, 0xd3, 0x64, 0x3c, 0x64, 0x04, 0x80, 0x88, 0x00, 0xff};

    const int older = FramesUntilSilent(kCommands, kBuild2007, 2000);
    const int october = FramesUntilSilent(kCommands, kBuildOctober2006, 2000);

    Check(october < 30,
          "the build of October 2006 stops a silent note on release (" + std::to_string(october) + " frames)");
    Check(older > 600, "a 2007 build plays out the release (" + std::to_string(older) + " frames)");
}

// The first voice's envelope as the microcode reads it in each of `frames` frames of `commands` under `build`.
std::vector<ax::PbVe> RampOf(const Bytes& commands, snd::SndBuild build, int frames)
{
    Started started(commands, build);
    std::vector<ax::PbVe> ramp = {started.pb.ve};
    for (int i = 1; i < frames; i++)
    {
        ramp.push_back(started.model.PbAfter(0).ve);
    }

    return ramp;
}

// Whether each frame after a ramp starts where the ramp ended.
bool StartsWhereRampsEnd(const std::vector<ax::PbVe>& ramp)
{
    for (std::size_t i = 1; i < ramp.size(); i++)
    {
        const auto delta = static_cast<int16_t>(ramp[i - 1].current_delta);
        const auto end = static_cast<uint16_t>(ramp[i - 1].current_volume + delta * ax::kSamplesPerFrame);
        if (delta != 0 && ramp[i].current_volume != end)
        {
            return false;
        }
    }

    return true;
}

void TestRampOctober2006()
{
    // A note's decay to the sustain level 64. A 2007 build starts each frame's ramp where the last one ended. The build
    // of October 2006 starts it at the volume that the last one aimed for. Both settle at the sustain level.
    const Bytes kCommands = {0xd1, 0x78, 0xd2, 0x40, 0x3c, 0x7f, 0x60, 0xff};

    const std::vector<ax::PbVe> older = RampOf(kCommands, kBuild2007, 40);
    const std::vector<ax::PbVe> october = RampOf(kCommands, kBuildOctober2006, 40);

    Check(StartsWhereRampsEnd(older), "a 2007 build starts each ramp where the last one ended");
    Check(!StartsWhereRampsEnd(october), "the build of October 2006 starts each ramp at the last one's target");
    Check(older.back().current_delta == 0 && october.back().current_delta == 0, "both settle at the sustain level");

    // Nothing ends the build of October 2006's ramps after AX's frame. The volume stays where the decay took it.
    Check(older.back().current_volume < older.front().current_volume &&
              october.back().current_volume < october.front().current_volume,
          "both settle below the note's first volume");
}

// Whether the first voice's mix ever ramps within 80 frames of `commands` under `build`.
bool MixRamps(const Bytes& commands, snd::SndBuild build)
{
    Started started(commands, build);
    for (int i = 0; i < 80; i++)
    {
        if (started.model.PbAfter(0).MixerCtrl() & ax::mixer::kDelta)
        {
            return true;
        }
    }

    return false;
}

void TestMixOctober2006()
{
    // A pan to the far left while the note plays. A 2007 build ramps the mix there over a frame. The build of October
    // 2006 sets the new mix at once.
    const Bytes kCommands = {0xc7, 0x00, 0x3c, 0x64, 0x30, 0x80, 0x0c, 0xc0, 0x00, 0x80, 0x0c, 0xff};

    Check(MixRamps(kCommands, kBuild2007), "a 2007 build ramps the mix");
    Check(!MixRamps(kCommands, kBuildOctober2006), "the build of October 2006 sets the mix at once");
}

void TestAxfxBuild()
{
    // AXFX comes with AX. On a later AX, a build without a banner links AXFX of December 2006.
    Check(snd::AxfxBuild(kBuildOctober2006, ax::Build::kOctober2006) == axfx::Build::kOctober2006,
          "AX of October 2006 comes with AXFX of October 2006");
    Check(snd::AxfxBuild(kBuildOctober2006, ax::Build::kDecember2006) == axfx::Build::kDecember2006,
          "a build without a banner on a later AX links AXFX of December 2006");
    Check(snd::AxfxBuild(kBuild2007, ax::Build::kDecember2006) == axfx::Build::kDecember2006,
          "a 2007 build links AXFX of December 2006");
    Check(snd::AxfxBuild(kBuildMarch2008, ax::Build::kDecember2006) == axfx::Build::kSeptember2008,
          "a build from March 2008 on links AXFX of September 2008");
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestNote();
        TestRelease();
        TestLastStep();
        TestDrop();
        TestSequence();
        TestArrangement();
        TestSoundsTogether();
        TestSweepPitch();
        TestTempo();
        TestVelocityRange();
        TestEnvReset();
        TestBiquad();
        TestIgnoreNoteOff();
        TestKeyGroup();
        TestLpfTable();
        TestGainsAboveFull();
        TestLoops();
        TestHeldNotes();
        TestSnapshot();
        TestWaveEndWithoutDsp();
        TestTicksOctober2006();
        TestPanOctober2006();
        TestReleaseStopOctober2006();
        TestRampOctober2006();
        TestMixOctober2006();
        TestAxfxBuild();
    };
    return xsflib::testing::RunTest("Wii NW4R SND", tests);
}
