// SPDX-License-Identifier: MIT

// Tests of rd2::Output, the model of Nintendo R&D2's mixer, on small synthetic cartridges. No game ROM is needed. Each
// expected value follows a rule of the mixer in ntdlib/rd2/output.h. The sequencer gives each voice's level and pan for
// the frame. These tests work out the mixer's bytes from them, the sample's points and the rule.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/output.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/testing/rd2_cart.h"

using xsflib::testing::Check;

namespace
{

using namespace ntdlib::testing::rd2_cart;
using ntdlib::agb::Rom;
using ntdlib::rd2::DriverInfo;
using ntdlib::rd2::kFrameSamples;
using ntdlib::rd2::Output;
using ntdlib::rd2::VoiceMix;

constexpr uint32_t kFirstSample = kSamples + 8; // the first sample's header
constexpr uint32_t kFirstPoints = kFirstSample + 16;
constexpr uint32_t kFirstLength = 64;
constexpr int kEchoFrames = 18; // the frames of the driver's echo line

bool Older(Revision revision)
{
    return revision == Revision::kSuperMarioAdvance2;
}

// Fills the looped sample with one point.
void FillLooped(Cart& cart, int point)
{
    for (uint32_t i = 0; i < kFirstLength; i++)
    {
        cart.Put8(kFirstPoints + i, uint8_t(int8_t(point)));
    }
}

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    Check(ntdlib::rd2::DetectDriver(rom, ntdlib::rd2::DriverOverrides(), info, error), "the driver is found");

    return info;
}

// A voice's levels for its points on the left and the right, with its pan a byte and its level 16 bits: 127 less the
// pan or the pan, times the level, in 16 bits, then shifted.
int LeftOf(const VoiceMix& mix, Revision revision)
{
    const uint32_t product = uint32_t(127 - int(mix.pan)) * (mix.volume & 0xFFFF);
    return int((product & 0xFFFF) >> (Older(revision) ? 7 : 8));
}

int RightOf(const VoiceMix& mix, Revision revision)
{
    const uint32_t product = uint32_t(mix.pan) * (mix.volume & 0xFFFF);
    return int((product & 0xFFFF) >> (Older(revision) ? 7 : 8));
}

// A voice's part of the mix for a point: the older revision shifts each voice's part right by 8 as it adds it.
int PartOf(int level, int point, Revision revision)
{
    const int part = level * point;
    return Older(revision) ? part >> 8 : part;
}

// The byte that a sum of the mix gives: the newer revisions divide by 128 towards zero. Then the sum saturates.
int ByteOf(int16_t sum, Revision revision)
{
    int x = sum;
    if (!Older(revision))
    {
        x = x / 128;
    }

    return x > 127 ? 127 : x < -128 ? -128 : x;
}

void CheckBytes(const Output& output, int left, int right, const char* what)
{
    bool same = true;
    for (size_t i = 0; i < kFrameSamples; i++)
    {
        same = same && output.FifoA()[i] == left && output.FifoB()[i] == right;
    }

    Check(same, what);
}

void TestRegisters()
{
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Output output(rom, info);
    const std::vector<uint8_t> registers = output.SoundRegisters();
    Check(registers.size() == ntdlib::agb::kSoundRegisterBytes, "the registers from SOUND1CNT_L up to the FIFOs");
    const auto at = [&registers](uint32_t address)
    {
        return registers[address - ntdlib::agb::kRegSound1CntL];
    };

    // FIFO A on the left and FIFO B on the right, both at full volume on timer 0, and the PSG at half volume.
    Check(at(0x04000082) == 0x0D && at(0x04000083) == 0x12, "SOUNDCNT_H");
    Check(at(0x04000080) == 0x77 && at(0x04000081) == 0xFF, "SOUNDCNT_L");
    Check(at(0x04000084) == 0x80, "the sound on");
    Check(at(0x04000089) == 0x42, "8 bits at 65,536 Hz");
}

void TestLevels(Revision revision, int point, uint8_t pan)
{
    Cart cart(true, revision);
    FillLooped(cart, point);
    cart.Sequences({{TrackData().Bytes({0xC3, pan, 0xC2, kHeld}).Note(60, 100, 127).Wait(100).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    output.Frame();
    output.Frame();

    const std::vector<VoiceMix>& mixes = output.Song()->Mixes();
    Check(mixes.size() == 1 && mixes[0].pan == pan, "one voice at the track's pan");
    const int left = PartOf(LeftOf(mixes[0], revision), point, revision);
    const int right = PartOf(RightOf(mixes[0], revision), point, revision);
    Check(left != 0 || right != 0, "the voice sounds");
    CheckBytes(output, ByteOf(int16_t(left), revision), ByteOf(int16_t(right), revision),
               "each side's bytes follow from the voice's level, the pan and the point");
    Check(!output.MixerSilent(), "the voice plays");
}

void TestPoints()
{
    // At note 60 the step is a point a point: the mix reads the sample's points in order, and from its loop's end
    // back to its loop's start, 16.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kHeld}).Note(60, 100, 127).Wait(100).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    output.Frame();

    const VoiceMix mix = output.Song()->Mixes().at(0);
    Check(mix.step == 0x100, "a point for each of the mixer's points");
    const int left = LeftOf(mix, Revision::kLinkToThePast);
    bool same = true;
    for (uint32_t i = 0; i < kFrameSamples; i++)
    {
        const uint32_t at = i < kFirstLength ? i : 16 + (i - 16) % (kFirstLength - 16);
        const int point = int(at) * 4 - 128;
        same = same && output.FifoA()[i] == ByteOf(int16_t(left * point), Revision::kLinkToThePast);
    }
    Check(same, "the bytes follow the sample's points through its loop");
}

void TestSums(Revision revision)
{
    // Every voice hard to the left with the same point: the sum wraps in 16 bits, and the byte saturates after that.
    for (const int point : {127, -128, 40})
    {
        Cart cart(true, revision);
        FillLooped(cart, point);
        std::vector<std::vector<uint8_t>> tracks;
        for (int t = 0; t < ntdlib::rd2::kSampleVoices; t++)
        {
            tracks.push_back(TrackData().Bytes({0xC3, 0, 0xE0, 0xFF, 0xC2, kHeld}).Note(60, 100, 127).Wait(100).End());
        }

        cart.Sequences({tracks});
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        Output output(rom, info);
        output.Play(0);
        output.Frame();
        output.Frame();

        int16_t left = 0;
        int16_t right = 0;
        for (const VoiceMix& mix : output.Song()->Mixes())
        {
            left = int16_t(left + PartOf(LeftOf(mix, revision), point, revision));
            right = int16_t(right + PartOf(RightOf(mix, revision), point, revision));
        }

        Check(output.Song()->Mixes().size() == size_t(ntdlib::rd2::kSampleVoices), "every sample voice plays");
        CheckBytes(output, ByteOf(left, revision), ByteOf(right, revision), "the sum of the voices' parts in 16 bits");
    }
}

// The echo's rule for one voice of a constant point, side by side with the model. Each frame:
//
// - While the echo runs, the echo's buffer starts from what the frame of 18 frames before kept.
// - The voice adds its part to the buffer while the echo runs and it has a send, and to the mix otherwise.
// - The level moves by 1 towards the goal.
// - In the newer revisions, the echo's frames run while the level is 15 or less, and for 17 frames after it leaves.
// - An echo's frame adds the buffer to the mix, and keeps the buffer shifted right by the level.
//
// The older revision rounds the shift to nearest, with its echo always running. The newer ones shift towards zero.
// The echo plays a held voice with `first` as the goal, and from frame 54 on with `then`.
void TestEcho(Revision revision, uint8_t first, int point, uint8_t then)
{
    Cart cart(true, revision);
    FillLooped(cart, point);
    cart.Sequences({{TrackData().Bytes({0xE3, 0x7F, 0xC3, 0x20, 0xC2, kHeld}).Note(60, 1000, 127).Wait(1000).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    output.SetEchoGoal(first);

    uint8_t goal = first;
    const bool older = Older(revision);
    int level = older ? 31 : 16;
    int counter = 0;
    std::array<std::array<int16_t, kEchoFrames>, 2> history = {};
    std::array<int16_t, 2> echo = {};
    bool same = true;
    bool sent = false;
    for (int f = 0; f < 4 * kEchoFrames + 30; f++)
    {
        if (f == 3 * kEchoFrames)
        {
            goal = then;
            output.SetEchoGoal(then);
        }

        output.Frame();
        const bool runs = older || level <= 15;
        std::array<int16_t, 2> mix = {};
        for (size_t side = 0; side < 2; side++)
        {
            if (runs)
            {
                echo[side] = history[side][size_t(f % kEchoFrames)];
            }
        }

        for (const VoiceMix& voice : output.Song()->Mixes())
        {
            const std::array<int, 2> parts = {PartOf(LeftOf(voice, revision), point, revision),
                                              PartOf(RightOf(voice, revision), point, revision)};
            const bool wet = runs && voice.echo != 0;
            sent = sent || wet;
            for (size_t side = 0; side < 2; side++)
            {
                int16_t& target = wet ? echo[side] : mix[side];
                target = int16_t(target + parts[side]);
            }
        }

        level += goal < level ? -1 : goal > level ? 1 : 0;
        counter = level <= 15 ? kEchoFrames : counter > 0 ? counter - 1 : 0;
        if (older || counter != 0)
        {
            for (size_t side = 0; side < 2; side++)
            {
                mix[side] = int16_t(mix[side] + echo[side]);
                int32_t kept = echo[side];
                if (older)
                {
                    const uint32_t round = level == 0 ? 0 : 1u << (level - 1);
                    kept = int32_t(uint32_t(kept) + round) >> level;
                }
                else
                {
                    kept = (kept < 0 ? kept + int32_t((1u << level) - 1) : kept) >> level;
                }

                echo[side] = int16_t(kept);
                history[side][size_t(f % kEchoFrames)] = echo[side];
            }
        }

        same = same && std::all_of(output.FifoA().begin(), output.FifoA().end(),
                                   [&](int8_t b) { return b == ByteOf(mix[0], revision); });
        same = same && std::all_of(output.FifoB().begin(), output.FifoB().end(),
                                   [&](int8_t b) { return b == ByteOf(mix[1], revision); });
    }

    Check(sent == (older || first <= 15 || then <= 15), "the voice sends to the echo while the echo runs");
    Check(same, "the echo's rule gives the bytes of each frame");
    Check(output.EchoLevel() == goal, "the level moved to the goal");
}

void TestEchoOffWithoutGoal()
{
    // The driver's init leaves the echo off: a voice with a send plays dry.
    Cart cart;
    FillLooped(cart, 64);
    cart.Sequences({{TrackData().Bytes({0xE3, 0x7F, 0xC2, kHeld}).Note(60, 100, 127).Wait(100).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    Check(output.EchoLevel() == 16, "the echo's level is off");
    for (int f = 0; f < kEchoFrames + 2; f++)
    {
        output.Frame();
    }

    const VoiceMix& mix = output.Song()->Mixes().at(0);
    const int left = LeftOf(mix, Revision::kLinkToThePast);
    CheckBytes(output, ByteOf(int16_t(left * 64), Revision::kLinkToThePast),
               ByteOf(int16_t(RightOf(mix, Revision::kLinkToThePast) * 64), Revision::kLinkToThePast),
               "the voice plays dry");
}

void TestMutes()
{
    // Two tracks: track 0 hard left and track 1 hard right. A mute takes its voice out of the mix, and the sequencer's
    // voices stay.
    Cart cart;
    FillLooped(cart, 64);
    const std::vector<uint8_t> left = TrackData().Bytes({0xC3, 0, 0xC2, kHeld}).Note(60, 100, 127).Wait(100).End();
    const std::vector<uint8_t> right = TrackData().Bytes({0xC3, 127, 0xC2, kHeld}).Note(60, 100, 127).Wait(100).End();
    cart.Sequences({{left, right}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output whole(rom, info);
    Output muted(rom, info);
    whole.Play(0);
    muted.Play(0);
    muted.SetMutedTracks(1);
    for (int f = 0; f < 3; f++)
    {
        whole.Frame();
        muted.Frame();
    }

    Check(whole.FifoA()[50] != 0 && whole.FifoB()[50] != 0, "both voices sound");
    Check(muted.FifoA()[50] == 0, "the muted track's voice leaves the left");
    Check(muted.FifoB()[50] == whole.FifoB()[50], "the other track's voice stays");
    Check(muted.Song()->Mixes().size() == whole.Song()->Mixes().size(), "the sequencer plays both voices");
}

void TestMutedEchoRings()
{
    // A muted voice leaves the echo's input, and what it has put in the echo's history rings on. At a level of 0, the
    // history keeps all of it: each frame then plays what the frame of 18 frames before played.
    Cart cart;
    FillLooped(cart, 2);
    cart.Sequences({{TrackData().Bytes({0xE3, 0x7F, 0xC2, kHeld}).Note(60, 1000, 127).Wait(1000).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    output.SetEchoGoal(0);

    std::vector<int> played;
    const auto frame = [&]
    {
        output.Frame();
        played.push_back(output.FifoA()[10]);
    };

    for (int f = 0; f < 16 + 2 * kEchoFrames; f++)
    {
        frame();
    }

    Check(output.EchoLevel() == 0, "the level has reached 0");
    Check(played.back() != played[played.size() - 1 - kEchoFrames], "the voice still adds to the echo");
    Check(played.back() != 0 && played.back() < 127, "the sum neither lies at 0 nor saturates");
    output.SetMutedTracks(1);
    for (int f = 0; f < kEchoFrames; f++)
    {
        frame();
        // The frame's slot of the history keeps the sum of the frame's last pass.
        Check(played.back() == played[played.size() - 1 - kEchoFrames], "the echo plays on without its voice");
    }
}

void TestPsgChannels()
{
    Cart cart;
    cart.Sequences({{TrackData().End(), TrackData().Bytes({0xC2, kSquare}).Note(60, 20, 127).Wait(20).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    Check(output.PsgChannelTrack(0, 1) == -1, "no note yet");
    output.Frame();
    Check(output.PsgChannelTrack(0, 1) == 1, "square 2 belongs to the track of its last note");
    Check(output.PsgChannelTrack(0, 0) == -1, "square 1 has played no note");
    Check(output.PsgChannelTrack(1, 1) == -1 && output.PsgUnits() == 1, "the console's unit alone");
    Check(!output.SoundWrites().empty(), "the note stores to the PSG's registers");
    Check(!output.SoundWrites().empty() && output.SoundWrites()[0].address >= ntdlib::agb::kRegSound1CntL,
          "the stores reach the sound registers");
}

void TestMixerSilent()
{
    // The mixer falls silent once no sample voice plays, a frame's mix holds nothing and the echo's history holds
    // nothing. Note 60 plays the plain sample's 4,000 points at 4 for each of the mixer's points: 6 frames.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kPlain}).Note(60, 100, 127).Wait(100).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    output.Play(0);
    std::vector<bool> silent;
    bool sounded = false;
    for (int f = 0; f < 8; f++)
    {
        output.Frame();
        silent.push_back(output.MixerSilent());
        sounded = sounded || output.FifoA()[100] != 0;
    }

    Check(sounded, "the note sounded");
    Check(!silent[0] && !silent[4], "the mixer plays while the sample lasts");
    Check(!silent[5], "the frame of the sample's end holds the sample's last points");
    Check(silent[6] && silent[7], "the mixer falls silent after the sample's end");
    Check(!output.Song()->Ended(), "the song goes on, with its track waiting");

    // A voice with an echo's send keeps the mixer from falling silent after its sample's end, while the history holds
    // anything.
    Cart wet_cart;
    wet_cart.Sequences({{TrackData().Bytes({0xE3, 0x7F, 0xC2, kPlain}).Note(60, 100, 127).Wait(100).End()}});
    const Rom wet = wet_cart.ToRom();
    const DriverInfo wet_info = Detect(wet);
    Output echoed(wet, wet_info);
    echoed.Play(0);
    echoed.SetEchoGoal(0);
    for (int f = 0; f < 40; f++)
    {
        echoed.Frame();
    }
    Check(!echoed.MixerSilent(), "the history holds the voice's points");

    // A voice that plays points of 0 can still sound later on. The mixer isn't silent while it plays.
    Cart zeros_cart;
    FillLooped(zeros_cart, 0);
    zeros_cart.Sequences({{TrackData().Bytes({0xC2, kHeld}).Note(60, 100, 127).Wait(100).End()}});
    const Rom zeros = zeros_cart.ToRom();
    const DriverInfo zeros_info = Detect(zeros);
    Output quiet(zeros, zeros_info);
    quiet.Play(0);
    quiet.Frame();
    quiet.Frame();
    Check(std::all_of(quiet.FifoA().begin(), quiet.FifoA().end(), [](int8_t b) { return b == 0; }), "the bytes are 0");
    Check(!quiet.MixerSilent(), "the voice plays");
}

void TestEnhancedStartsBeforeTheSong()
{
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Output output(rom, info);
    bool refused = false;
    try
    {
        output.StartEnhancedMixing();
    }
    catch (const std::logic_error&)
    {
        refused = true;
    }
    Check(refused, "the enhanced mode comes after the request");

    output.Play(0);
    output.Frame();
    refused = false;
    try
    {
        output.StartEnhancedMixing();
    }
    catch (const std::logic_error&)
    {
        refused = true;
    }
    Check(refused, "the enhanced mode comes before the song's first frame");
}

} // namespace

int main()
{
    TestRegisters();
    for (const Revision revision :
         {Revision::kLinkToThePast, Revision::kSuperMarioAdvance2, Revision::kSuperMarioAdvance3})
    {
        TestLevels(revision, 64, 0x40);
        TestLevels(revision, -33, 0x40);
        TestLevels(revision, 127, 0x10);
        TestLevels(revision, -128, 0x70);
        TestSums(revision);
        TestEcho(revision, 1, 64, 1);
        TestEcho(revision, 3, -50, 3);
        TestEcho(revision, 15, 100, 15);
        TestEcho(revision, 31, 64, 31);

        // The level leaves the echo's range with a history that isn't empty, and later comes back to it.
        TestEcho(revision, 3, 64, 31);
        TestEcho(revision, 1, -50, 20);
        TestEcho(revision, 31, 64, 3);
    }

    TestPoints();
    TestEchoOffWithoutGoal();
    TestMutes();
    TestMutedEchoRings();
    TestPsgChannels();
    TestMixerSilent();
    TestEnhancedStartsBeforeTheSong();
    return xsflib::testing::Summary("rd2_output");
}
