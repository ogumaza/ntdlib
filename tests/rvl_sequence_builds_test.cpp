// SPDX-License-Identifier: MIT

// Tests of the Wii's model of NW4R SND (rvl/snd) under its builds: the low-pass table and the gains above full level
// of the builds of November 2008 and September 2009, what the build of October 2006 does differently in its ticks,
// pans, releases, ramps and mixes, and the build of AXFX that a build links.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/rvl/snd/effects.h"
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
        TestLpfTable();
        TestGainsAboveFull();
        TestTicksOctober2006();
        TestPanOctober2006();
        TestReleaseStopOctober2006();
        TestRampOctober2006();
        TestMixOctober2006();
        TestAxfxBuild();
    };
    return xsflib::testing::RunTest("Wii NW4R SND builds", tests);
}
