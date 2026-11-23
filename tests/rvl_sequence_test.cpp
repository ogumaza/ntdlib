// SPDX-License-Identifier: MIT

// Tests of the Wii's model of NW4R SND (rvl/snd), on the AX model with no DSP: the parameter blocks that a note's
// channel sets up, the ramps from frame to frame, the release and the end of a channel, the voices that NW4R SND drops
// by priority when all are in use, in the Wii's pools and in smaller ones, the voices that a game's build drops when
// the AxVoices run out, a short sequence played from an archive, a sequence in an arrangement, sequences that start
// together, the loops that a length analysis counts, snapshots of the model, and the end of a wave without the DSP.
// rvl_sequence_builds_test checks what the builds of October 2006, 2008 and 2009 do differently, and the build of AXFX
// that a build links.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dsp/memory.h"
#include "ntdlib/nwsnd/revolution_policy.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/sequence.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/testing/rsar_files.h"
#include "rvl_sequence_test_util.h"

using rvl_sequence_tests::Bytes;
using rvl_sequence_tests::Check;
using rvl_sequence_tests::Initialised;
using rvl_sequence_tests::kBuild2007;
using rvl_sequence_tests::kBuildJune2008;
using rvl_sequence_tests::kBuildMarch2008;
using rvl_sequence_tests::kBuildNovember2008;
using rvl_sequence_tests::kBuildOctober2006;
using rvl_sequence_tests::kBuildSeptember2008;
using rvl_sequence_tests::kBuildSeptember2009;
using rvl_sequence_tests::MakeArchive;
using rvl_sequence_tests::MakeWave;
using rvl_sequence_tests::Model;
using rvl_sequence_tests::Started;
using rvl_sequence_tests::WaveVoice;

namespace
{

using namespace ntdlib::rvl;
using seqlib::testing::Throws;

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

// Pools smaller than the Wii's, as a game's start-up may set them up.
constexpr snd::EngineOptions kSmallPools{.ax_voices = 64, .voices = 64, .channels = 65};

// The notes that a 65th note of priority 100 drops, after 64 notes take voices on an engine with `pools`. The first 10
// notes have priority 32, and the others 64. Each note is given by the order in which it started.
std::vector<std::size_t> DroppedBy65thNote(const snd::EngineOptions& pools)
{
    Model model({0xff}, {}, 1, pools);
    const snd::WaveData wave = MakeWave(model.memory);
    std::vector<snd::ChannelId> places;
    for (int i = 0; i < 64; i++)
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
    const snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, 100, -1, -1);
    Check(channel != nullptr, "the 65th note gets a channel");

    const std::list<snd::ChannelId>& in_use = model.engine.Channels().InUse();
    std::vector<std::size_t> dropped;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        if (std::find(in_use.begin(), in_use.end(), places[i]) == in_use.end())
        {
            dropped.push_back(i);
        }
    }

    return dropped;
}

void TestPoolSizes()
{
    // With the Wii's pools, a 65th note takes a free voice. With pools of 64 AxVoices, 64 voices and 65 channels, it
    // takes the voice of the oldest note of the lowest priority.
    Check(DroppedBy65thNote({}).empty(), "the Wii's pools drop no note for a 65th");
    Check(DroppedBy65thNote(kSmallPools) == std::vector<std::size_t>{0},
          "pools of 64 voices drop the oldest lowest-priority note for a 65th");

    // An empty pool is refused.
    const bool refused = Throws<std::invalid_argument>([&] { Model empty({0xff}, {}, 1, {.voices = 0}); });

    Check(refused, "an empty pool of voices is refused");
}

// What a note of `priority` does after 32 notes on a wave of two channels take the 64 AxVoices of the small pools, with
// EngineOptions::drop_voice_for_ax_voice set to `drop`. The 32 notes have priority 64, and their voices leave 32 voices
// free.
struct AxVoiceDrop
{
    bool started = false;     // whether the note got a channel
    bool first_stops = false; // whether the first of the 32 notes stopped
};

AxVoiceDrop NoteAfterAxVoicesRunOut(bool drop, uint32_t priority)
{
    snd::EngineOptions pools = kSmallPools;
    pools.drop_voice_for_ax_voice = drop;
    Model model({0xff}, {}, 1, pools);
    snd::WaveData wave = MakeWave(model.memory);
    wave.channel_count = 2;
    wave.channels[1] = wave.channels[0];
    std::vector<snd::ChannelId> places;
    for (int i = 0; i < 32; i++)
    {
        snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 2, 64, -1, -1);
        Check(channel != nullptr, "a channel for a note on two channels while AxVoices are free");
        if (channel != nullptr)
        {
            channel->Start(model.engine, wave, -1);
            places.push_back(channel->place);
        }
    }

    model.RunFrame();
    const snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 2, priority, -1, -1);
    const std::list<snd::ChannelId>& in_use = model.engine.Channels().InUse();
    const bool first_stops = !places.empty() && std::find(in_use.begin(), in_use.end(), places[0]) == in_use.end();

    return {channel != nullptr, first_stops};
}

void TestDropForAxVoice()
{
    // With every AxVoice in use and voices free, the drop step lets a note of the same or a higher priority take the
    // AxVoices of the oldest note of the lowest priority. A note of a lower priority doesn't start. Without the step,
    // no note starts.
    const AxVoiceDrop higher = NoteAfterAxVoicesRunOut(true, 100);
    const AxVoiceDrop same = NoteAfterAxVoicesRunOut(true, 64);
    const AxVoiceDrop lower = NoteAfterAxVoicesRunOut(true, 32);
    const AxVoiceDrop without = NoteAfterAxVoicesRunOut(false, 100);
    Check(higher.started && higher.first_stops, "the step drops the oldest note for a note of higher priority");
    Check(same.started && same.first_stops, "the step drops the oldest note for a note of the same priority");
    Check(!lower.started && !lower.first_stops, "the step drops nothing for a note of lower priority");
    Check(!without.started && !without.first_stops, "without the step, no note starts when the AxVoices run out");
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

// Whether two PBs give their voices the same mix, bit for bit.
bool SameMix(const ax::Pb& a, const ax::Pb& b)
{
    static_assert(std::has_unique_object_representations_v<ax::PbMix>);
    return std::memcmp(&a.mix, &b.mix, sizeof(ax::PbMix)) == 0;
}

// The settings of a sound player: a sequence that starts takes the player's low-pass and adds the player's sends to the
// sends that the game sets on its sound (BasicSound::UpdateParam).
void TestSoundPlayer()
{
    const Bytes kNote = {0xd3, 0x7f, 0x3c, 0x64, 0x30, 0xff};

    // The first PB of the note, and the low-pass scale of its voice, with `settings` and the game's `sends`.
    struct Note
    {
        ax::Pb pb;
        float lpf_freq = -1.0f;
    };
    const auto note = [&kNote](const snd::SoundPlayerSettings& settings, const std::vector<snd::SoundSend>& sends)
    {
        Note found;
        Model model(kNote, kBuildSeptember2009);
        model.engine.SetEffects({}, sends, 0);
        model.engine.SetSoundPlayerSettings(settings);
        model.engine.SetVoiceParamHook(
            [&found](snd::ChannelId, const ntdlib::nwsnd::RevolutionPolicy::ChannelVoiceParam& param)
            { found.lpf_freq = param.lpf_freq; });
        model.engine.StartSequence(0);
        found.pb = model.PbAfter(1);
        return found;
    };

    const Note plain = note({}, {});
    const Note filtered = note({.lpf = -0.55f}, {});
    Check(plain.lpf_freq == 1.0f && plain.pb.lpf.on != ax::kLpfOn, "by default, the scale is 1 and the low-pass off");
    Check(filtered.lpf_freq == 0.45f && filtered.pb.lpf.on == ax::kLpfOn,
          "a low-pass of -0.55 gives a scale of 0.45 and turns AX's low-pass on");

    const std::vector<snd::SoundSend> kQuarter = {{.sound = 0, .bus = 1, .send = 0.25f}};
    const std::vector<snd::SoundSend> kHalf = {{.sound = 0, .bus = 1, .send = 0.5f}};
    const snd::SoundPlayerSettings kPlayerQuarter = {.fx_sends = {0.0f, 0.25f, 0.0f}};
    const Note by_player = note(kPlayerQuarter, {});
    const Note by_sound = note({}, kQuarter);
    Check(by_player.pb.mix.aux_b_l > 0 && SameMix(by_player.pb, by_sound.pb),
          "a player's send of 0.25 mixes as the game's send of 0.25 on the sound");
    Check(SameMix(note(kPlayerQuarter, kQuarter).pb, note({}, kHalf).pb),
          "the player's send adds to the sound's: 0.25 and 0.25 mix as 0.5");
}

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
    const bool refused = Throws<std::runtime_error>([&] { model.engine.Restore(engine); });

    Check(refused, "a snapshot from before another sequence started is refused");
}

void TestSnapshotOfSmallPools()
{
    // A snapshot of pools smaller than the Wii's, taken with every voice in use, comes back whole. The notes after the
    // snapshot drop voices.
    Model model({0xff}, {}, 1, kSmallPools);
    const snd::WaveData wave = MakeWave(model.memory);
    const auto start_notes = [&model, &wave](int count, uint32_t priority)
    {
        for (int i = 0; i < count; i++)
        {
            snd::Channel* channel = model.engine.Channels().AllocChannel(model.engine, 1, priority, -1, -1);
            if (channel != nullptr)
            {
                channel->Start(model.engine, wave, -1);
            }
        }
    };

    start_notes(64, 32);
    PbsOfFrames(model, 2);
    const ntdlib::dsp::WiiMemory::Snapshot memory = model.memory.Save();
    const ax::System ax = model.ax;
    const snd::Engine::Snapshot engine = model.engine.Save();
    start_notes(8, 64);
    const std::vector<uint8_t> first = PbsOfFrames(model, 20);
    const std::list<snd::ChannelId> first_in_use = model.engine.Channels().InUse();

    model.memory.Restore(memory);
    model.ax = ax;
    model.engine.Restore(engine);
    start_notes(8, 64);
    const std::vector<uint8_t> second = PbsOfFrames(model, 20);

    Check(model.engine.Voices().PrioVoices().size() == 64, "the small pools stay full");
    Check(first == second && model.engine.Channels().InUse() == first_in_use,
          "the frames after a restored snapshot of small pools are the same");
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

} // namespace

int main()
{
    const auto tests = []
    {
        TestNote();
        TestRelease();
        TestLastStep();
        TestDrop();
        TestPoolSizes();
        TestDropForAxVoice();
        TestSequence();
        TestArrangement();
        TestSoundsTogether();
        TestSoundPlayer();
        TestSweepPitch();
        TestTempo();
        TestVelocityRange();
        TestEnvReset();
        TestBiquad();
        TestIgnoreNoteOff();
        TestKeyGroup();
        TestLoops();
        TestHeldNotes();
        TestSnapshot();
        TestSnapshotOfSmallPools();
        TestWaveEndWithoutDsp();
    };
    return xsflib::testing::RunTest("Wii NW4R SND", tests);
}
