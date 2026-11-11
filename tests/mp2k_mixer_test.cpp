// SPDX-License-Identifier: MIT

// Unit tests for MP2K's mixer and its stores to the PSG's registers. They build small synthetic cartridges in memory.
// No game ROM is needed.

#include "mp2k_test_util.h"

namespace ntdlib::sequence_export::mp2k
{
namespace
{

void TestCompressedSample()
{
    // One block: the first point is 10, the second byte's low half (2) adds 4, the third byte's halves (1 and 8) add 1
    // and -64, and the rest add 0. The second byte's high half isn't used.
    Cart cart;
    std::vector<uint8_t> block(33, 0);
    block[0] = 10;
    block[1] = 0xF2;
    block[2] = 0x18;
    const uint32_t wave = cart.CompressedWave(block, 64);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, wave)}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("compressed");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The sample holds the master volume of 12 too, at 13/16, so each of its points is 208 times the decoded one.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    const uint32_t start = ntdlib::testing::agb_files::Le32(sf.Record("shdr", 46, 0) + 20);
    const auto point = [&](uint32_t i)
    {
        return int16_t(Le16(&pcm[2 * (start + i)])) / (256 * 13 / 16);
    };

    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(point(0) == 10 && point(1) == 14 && point(2) == 15 && point(3) == -49 && point(63) == -49);
}

// Returns the volume at which the mixer plays a channel on a side: the channel's volume there times its envelope's
// level, over 256. The level is the envelope times the master volume plus 1, over 16. Each is rounded down.
int MixVolume(const DriverInfo& info, int volume, int envelope)
{
    return (volume * ((envelope * (info.master_volume + 1)) >> 4)) >> 8;
}

// Returns the reverb's point for `sum`, the sum of the points that the reverb adds up: the sum times `reverb`, over 2
// to the `shift` and rounded down. A result whose bit 7 is set goes up by 1.
int8_t ReverbPoint(int32_t sum, int reverb, int shift)
{
    int32_t value = (sum * reverb) >> shift;
    if (value & 0x80)
    {
        value++;
    }

    return int8_t(value);
}

void TestMixFixedPitch()
{
    // Four tracks play a sample of 224 points at a fixed pitch: one frame of the mixer's output at 13379 Hz. Three
    // play at the centre, and one on the left. Each of the frame's points is the sum of each channel's point times its
    // volume over 256, rounded down. The sum wraps round at 8 bits. The channels stop at the sample's end.
    Cart cart;
    const std::vector<int8_t> pcm = Jumpy(224);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    const auto track = [](int pan)
    {
        return Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, pan).Note(24, 60, 127).Wait(24).Fine();
    };
    cart.Song({track(0x40), track(0x40), track(0x40), track(0x10)}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 4);
    const int f = FirstSound(m);
    SEQLIB_REQUIRE(f >= 0 && f + 1 < int(m.right.size()));

    bool wrapped = false;
    bool sides_differ = false;
    for (size_t i = 0; i < pcm.size(); i++)
    {
        uint8_t right = 0;
        uint8_t left = 0;
        int sum = 0;
        for (int c = 0; c < 4; c++)
        {
            const Channel& channel = seq.GetChannel(c);
            SEQLIB_CHECK(channel.status == 0 && channel.envelope == 255);
            const int volume = MixVolume(info, channel.right, channel.envelope);
            right = uint8_t(right + Added(pcm[i], volume));
            left = uint8_t(left + Added(pcm[i], MixVolume(info, channel.left, channel.envelope)));
            sum += int8_t(Added(pcm[i], volume));
        }

        SEQLIB_CHECK_EQ(int(m.right[size_t(f)][i]), int(int8_t(right)));
        SEQLIB_CHECK_EQ(int(m.left[size_t(f)][i]), int(int8_t(left)));
        wrapped = wrapped || sum < -128 || sum > 127;
        sides_differ = sides_differ || right != left;
    }

    SEQLIB_CHECK(wrapped && sides_differ);
    SEQLIB_CHECK(
        FirstSound(MixedFrames{{m.right.begin() + f + 1, m.right.end()}, {m.left.begin() + f + 1, m.left.end()}}) < 0);
}

// Checks that a channel's output for `frames` frames from frame `first` interpolates between the points that `point`
// gives. The channel's position starts at point `start`. It moves by the channel's step for each point of the output:
// forwards for a `direction` of 1, and backwards for -1. Each point of the output is the point at the position, plus
// the difference to the next point times the fraction between them, over 2^23 and rounded down. The product wraps round
// at 32 bits.
template <typename Point>
void CheckInterpolation(const MixedFrames& m, const DriverInfo& info, const Channel& c, int first, int frames,
                        int32_t start, int direction, Point point)
{
    const uint32_t step = c.frequency * info.step_scale;
    SEQLIB_CHECK((step & 0x7FFFFF) != 0);
    SEQLIB_REQUIRE(first >= 0 && first + frames <= int(m.right.size()));

    const int right = MixVolume(info, c.right, c.envelope);
    const int left = MixVolume(info, c.left, c.envelope);
    int32_t position = start;
    uint32_t fraction = 0;
    int differences = 0;
    for (int f = first; f < first + frames; f++)
    {
        for (int i = 0; i < info.samples_per_frame; i++)
        {
            const int32_t s0 = point(position);
            const int32_t s1 = point(position + direction);
            const int32_t value = s0 + (int32_t(fraction * uint32_t(s1 - s0)) >> 23);
            differences += m.right[size_t(f)][size_t(i)] != int8_t(Added(value, right));
            differences += m.left[size_t(f)][size_t(i)] != int8_t(Added(value, left));

            fraction += step;
            position += direction * int32_t(fraction >> 23);
            fraction &= 0x7FFFFF;
        }
    }

    SEQLIB_CHECK_EQ(differences, 0);
}

void TestMixInterpolation()
{
    // A saw wave plays a fifth above its rate. The saw's points fall by 252 where it wraps round.
    Cart cart;
    const std::vector<int8_t> pcm = Saw(2000);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 4);
    const Channel& c = seq.GetChannel(0);
    SEQLIB_CHECK(c.status != 0 && c.envelope == 255);
    CheckInterpolation(m, info, c, FirstSound(m), 3, 0, 1, [&](int32_t at) { return int32_t(pcm.at(size_t(at))); });
}

void TestMixSpecialSamples()
{
    // A compressed sample of 8 blocks, and a saw wave played backwards. Both play a fifth above their rates.
    Cart cart;
    std::vector<uint8_t> blocks(8 * 33);
    uint32_t seed = 1;
    for (uint8_t& byte : blocks)
    {
        seed = seed * 1103515245u + 12345u;
        byte = uint8_t(seed >> 16);
    }
    const std::vector<int8_t> points = Decompress(blocks);
    const std::vector<int8_t> pcm = Saw(1200);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, cart.CompressedWave(blocks, 512))},
                                             {1, Cart::Voice(kVoiceReverse, cart.Wave(pcm))}});
    for (int voice = 0; voice < 2; voice++)
    {
        cart.Song({Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(24).Fine()}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.special_samples && points.size() == 512);

    // The compressed sample's position counts points from its start. The reversed sample plays from its last point
    // backwards.
    Sequencer compressed(rom, info, 0);
    const MixedFrames cm = Mix(compressed, 2);
    CheckInterpolation(cm, info, compressed.GetChannel(0), FirstSound(cm), 1, 0, 1,
                       [&](int32_t at) { return int32_t(points.at(size_t(at))); });

    Sequencer reversed(rom, info, 1);
    const MixedFrames rm = Mix(reversed, 3);
    CheckInterpolation(rm, info, reversed.GetChannel(0), FirstSound(rm), 2, int32_t(pcm.size()) - 1, -1,
                       [&](int32_t at) { return int32_t(pcm.at(size_t(at))); });
}

// Checks the reverb in each frame after `first`. Nothing plays in those frames. A frame's part of the buffer holds the
// reverb of two frames' output: the frame a buffer's length of frames before it, and the frame after that. The first
// wrote the same part. The stereo mixer adds both sides of both frames, and gives each side the same reverb. The mono
// mixer adds its one side.
void CheckReverb(const MixedFrames& m, const DriverInfo& info, int first, int reverb)
{
    const int period = int(ntdlib::mp2k::kPcmBufferSize) / info.samples_per_frame;
    const auto at = [&](const std::vector<std::vector<int8_t>>& side, int f, int i)
    {
        return f < 0 ? 0 : int32_t(side[size_t(f)][size_t(i)]);
    };

    int differences = 0;
    for (int f = first + 1; f < int(m.right.size()); f++)
    {
        for (int i = 0; i < info.samples_per_frame; i++)
        {
            const int8_t expected =
                info.mono ? ReverbPoint(at(m.right, f - period, i) + at(m.right, f - period + 1, i), reverb, 8)
                          : ReverbPoint(at(m.right, f - period, i) + at(m.left, f - period, i) +
                                            at(m.right, f - period + 1, i) + at(m.left, f - period + 1, i),
                                        reverb, 9);
            differences += m.right[size_t(f)][size_t(i)] != expected;
            differences += m.left[size_t(f)][size_t(i)] != expected;
        }
    }

    SEQLIB_CHECK_EQ(differences, 0);
    SEQLIB_CHECK(m.right.size() > size_t(first + 3 * period));
    SEQLIB_CHECK(FirstSound(MixedFrames{{m.right.begin() + first + 2 * period, m.right.end()},
                                        {m.left.begin() + first + 2 * period, m.left.end()}}) >= 0);
}

void TestMixReverb()
{
    // A sample of 224 points plays for one frame at a fixed pitch, on the left. The song sets a reverb of 100.
    Cart cart;
    const std::vector<int8_t> pcm = Jumpy(224);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x10).Note(24, 60, 127).Wait(48).Fine()},
              voices, 0, 0, 0x80 | 100);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 40);
    const int f = FirstSound(m);
    SEQLIB_REQUIRE(f >= 0);

    const Channel& c = seq.GetChannel(0);
    const int right = MixVolume(info, c.right, c.envelope);
    const int left = MixVolume(info, c.left, c.envelope);
    SEQLIB_CHECK(c.status == 0 && right != left);
    for (size_t i = 0; i < pcm.size(); i++)
    {
        SEQLIB_CHECK_EQ(int(m.right[size_t(f)][i]), int(int8_t(Added(pcm[i], right))));
        SEQLIB_CHECK_EQ(int(m.left[size_t(f)][i]), int(int8_t(Added(pcm[i], left))));
    }
    CheckReverb(m, info, f, 100);
}

void TestMixMono()
{
    // The mono mixer plays a channel at the average of its volumes. Its reverb is the sum of its one side's points
    // times the reverb, over 256.
    Cart cart;
    const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
    for (uint32_t i = 0; i < 12; i++)
    {
        cart.Put8(kMonoVolume + i, kMono[i]);
    }
    const std::vector<int8_t> pcm = Jumpy(224);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x10).Note(24, 60, 127).Wait(48).Fine()},
              voices, 0, 0, 0x80 | 100);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.mono);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 40);
    const int f = FirstSound(m);
    SEQLIB_REQUIRE(f >= 0);

    const Channel& c = seq.GetChannel(0);
    const int volume = ((c.right + c.left) * ((c.envelope * (info.master_volume + 1)) >> 4)) >> 9;
    SEQLIB_CHECK(c.status == 0 && c.right != c.left);
    for (size_t i = 0; i < pcm.size(); i++)
    {
        SEQLIB_CHECK_EQ(int(m.right[size_t(f)][i]), int(int8_t(Added(pcm[i], volume))));
    }
    SEQLIB_CHECK(m.left == m.right);
    CheckReverb(m, info, f, 100);
}

// A store to a sound register: the address's low byte, the size in bytes and the value.
using Store = std::tuple<uint32_t, int, uint32_t>;

// A frame's stores to the sound registers, and PSG channel `n`'s state after it.
struct StoreFrame
{
    std::vector<Store> stores;
    Channel channel;
};

// Runs `frames` frames, and returns those that store to the sound registers.
std::vector<StoreFrame> StoreFrames(Sequencer& seq, int frames, int n)
{
    std::vector<StoreFrame> out;
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        StoreFrame frame;
        for (const ntdlib::agb::RegisterWrite& w : seq.SoundWrites())
        {
            frame.stores.emplace_back(w.address & 0xFF, w.size, w.value);
        }
        frame.channel = seq.GetChannel(kFirstPsgChannel + n - 1);
        if (!frame.stores.empty())
        {
            out.push_back(frame);
        }
    }

    return out;
}

void TestPsgSquareStores()
{
    // A note on square channel 1 starts with the sweep in NR10 (0x60), the duty and the length in NR11 (0x62), the
    // frequency in NR13 and NR14 (0x64, 0x65), both outputs in NR51 (0x81, bits 0 and 4), then the envelope in NR12
    // (0x63): a level of 0 that the hardware raises a step every 1/64 s, with bit 3 for upwards. NR14's bit 7 restarts
    // the channel, and the driver restarts it a second time while NR10's bit 3 is clear. At the attack's end, NR12
    // takes the sustain's level with a step time of 0. A bend writes the new frequency alone. The release of 0 and the
    // echo of 0 turn the channel off: NR12 = 8, then NR14 = 0x80.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {1, 0, 15, 0}, 0x15)}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 127)
                   .Wait(20)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 60, 1);
    const uint32_t f = KeyToPsgFrequency(1, 60, 0);
    const uint32_t bent = KeyToPsgFrequency(1, 60, 128);
    SEQLIB_REQUIRE_EQ(frames.size(), 4);

    const std::vector<Store> start = {
        {0x60, 1, 0x15}, {0x62, 1, 0x80}, {0x64, 1, f & 0xFF},        {0x65, 1, f >> 8},
        {0x81, 1, 0x11}, {0x63, 1, 0x09}, {0x65, 1, 0x80 | (f >> 8)}, {0x65, 1, 0x80 | (f >> 8)}};
    SEQLIB_CHECK(frames[0].stores == start);
    SEQLIB_CHECK_EQ(frames[0].channel.envelope, 0);

    const int level = frames[1].channel.envelope;
    const std::vector<Store> sustain = {{0x81, 1, 0x11},
                                        {0x63, 1, uint32_t(0x08 + (level << 4))},
                                        {0x65, 1, 0x80 | (f >> 8)},
                                        {0x65, 1, 0x80 | (f >> 8)}};
    SEQLIB_CHECK_EQ(frames[1].channel.status, ntdlib::mp2k::kPhaseSustain);
    SEQLIB_CHECK(level > 0 && level == frames[1].channel.goal);
    SEQLIB_CHECK(frames[1].stores == sustain);

    SEQLIB_CHECK(frames[2].stores == std::vector<Store>({{0x64, 1, bent & 0xFF}, {0x65, 1, bent >> 8}}));
    SEQLIB_CHECK(frames[3].stores == std::vector<Store>({{0x63, 1, 0x08}, {0x65, 1, 0x80}}));
    SEQLIB_CHECK_EQ(frames[3].channel.status, 0);
}

void TestPsgWaveStores()
{
    // A note on the wave channel loads its pattern into the wave RAM (0x90 to 0x9F) in four words, while NR30 (0x70)
    // has the channel play the other bank. NR30 then turns the channel off, NR31 (0x72) takes the length, and NR33 and
    // NR34 (0x74, 0x75) the frequency with the length's enable in bit 6 and the restart in bit 7. NR32 (0x73) takes the
    // volume code of the envelope's level: mute, 25%, 50%, 75% or full. NR30's bit 7 turns the channel on, and NR34
    // restarts it. A second note with the same pattern leaves the wave RAM alone. A note with another pattern loads it.
    // A note's end turns the channel off in NR30.
    static const uint8_t kVolumes[16] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40,
                                         0x40, 0x40, 0x80, 0x80, 0x80, 0x80, 0x20, 0x20};
    Cart cart;
    std::vector<uint8_t> first(16);
    std::vector<uint8_t> second(16);
    for (size_t i = 0; i < 16; i++)
    {
        first[i] = uint8_t(0x10 * i + 1);
        second[i] = uint8_t(0xF0 - 0x10 * i);
    }
    const uint32_t a = cart.Place(first);
    const uint32_t b = cart.Place(second);
    const uint32_t voices = cart.VoiceGroup(
        {{0, Cart::Voice(3, a, 60, {0, 0, 15, 0}, 0, 5)}, {1, Cart::Voice(3, b, 60, {0, 0, 15, 0}, 0, 5)}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(4, 60, 127)
                   .Wait(8)
                   .Note(4, 60, 127)
                   .Wait(8)
                   .Cmd(kCmdVoice, 1)
                   .Note(4, 60, 127)
                   .Wait(8)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 40, 3);
    const uint32_t f = KeyToPsgFrequency(3, 60, 0);
    SEQLIB_REQUIRE_EQ(frames.size(), 6);

    const auto start = [&](const std::vector<uint8_t>* pattern, int level)
    {
        std::vector<Store> stores;
        if (pattern)
        {
            stores.emplace_back(0x70, 1, 0x40);
            for (uint32_t i = 0; i < 4; i++)
            {
                stores.emplace_back(0x90 + 4 * i, 4, Le32(pattern->data() + 4 * i));
            }
        }
        const std::vector<Store> rest = {{0x70, 1, 0x00},     {0x72, 1, 5},
                                         {0x74, 1, f & 0xFF}, {0x75, 1, 0xC0 | (f >> 8)},
                                         {0x81, 1, 0x44},     {0x73, 1, kVolumes[level]},
                                         {0x70, 1, 0x80},     {0x75, 1, 0xC0 | (f >> 8)}};
        stores.insert(stores.end(), rest.begin(), rest.end());
        return stores;
    };
    const std::vector<Store> off = {{0x70, 1, 0x00}};

    const int level = frames[0].channel.envelope;
    SEQLIB_CHECK(level > 1 && level < 16);
    SEQLIB_CHECK(frames[0].stores == start(&first, level));
    SEQLIB_CHECK(frames[1].stores == off);
    SEQLIB_CHECK(frames[2].stores == start(nullptr, level));
    SEQLIB_CHECK(frames[3].stores == off);
    SEQLIB_CHECK(frames[4].stores == start(&second, level));
    SEQLIB_CHECK(frames[5].stores == off);
}

void TestPsgNoiseStores()
{
    // A note on the noise channel puts the length in NR41 (0x78) and its 7-bit type in NR43's bit 3 (0x7C). The pitch
    // then goes in NR43's other bits, and NR44 (0x7D) takes the length's enable. NR51's bits 3 and 7 give both outputs.
    // NR42 (0x79) takes the sustain's level, with bit 3 set: the attack and the decay of 0 skip to the sustain. NR44's
    // bit 7 restarts the channel once.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(4, 1, 60, {0, 0, 15, 0}, 0, 0x20)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(8).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 20, 4);
    const uint32_t f = KeyToPsgFrequency(4, 60, 0);
    SEQLIB_REQUIRE_EQ(frames.size(), 2);

    const int level = frames[0].channel.envelope;
    const std::vector<Store> start = {{0x78, 1, 0x20}, {0x7C, 1, 0x08}, {0x7C, 1, (0x08 | f) & 0xFF},
                                      {0x7D, 1, 0x40}, {0x81, 1, 0x88}, {0x79, 1, uint32_t(0x08 + (level << 4))},
                                      {0x7D, 1, 0xC0}};
    SEQLIB_CHECK(level > 0);
    SEQLIB_CHECK(frames[0].stores == start);
    SEQLIB_CHECK(frames[1].stores == std::vector<Store>({{0x79, 1, 0x08}, {0x7D, 1, 0x80}}));
}

void TestPsgWaveVolumes()
{
    // The wave channel's NR32 (0x73) takes the volume code of the envelope's level: levels 0 and 1 mute it, 2 to 5 play
    // at 25%, 6 to 9 at 50%, 10 to 13 at 75% and 14 and 15 at the full volume. Notes at falling velocities start at
    // falling levels.
    static const uint8_t kVolumes[16] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40,
                                         0x40, 0x40, 0x80, 0x80, 0x80, 0x80, 0x20, 0x20};
    Cart cart;
    const uint32_t pattern = cart.Place(std::vector<uint8_t>(16, 0x5A));
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(3, pattern, 60, {0, 0, 15, 0})}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127);
    for (int velocity = 127; velocity > 0; velocity -= 8)
    {
        track.Note(2, 60, uint8_t(velocity)).Wait(4);
    }
    cart.Song({track.Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::set<int> levels;
    int starts = 0;
    for (const StoreFrame& frame : StoreFrames(seq, 80, 3))
    {
        for (const Store& s : frame.stores)
        {
            if (std::get<0>(s) == 0x73)
            {
                const int level = frame.channel.envelope;
                SEQLIB_REQUIRE(level < 16);
                SEQLIB_CHECK_EQ(std::get<2>(s), kVolumes[level]);
                levels.insert(level);
                starts++;
            }
        }
    }
    SEQLIB_CHECK_EQ(starts, 16);
    SEQLIB_CHECK(levels.size() >= 14);
}

void TestEnvelopeCounter()
{
    // A square's decay of 1 count a step falls a level a frame after the note's first frame, until the channel stops at
    // level 0. The frame in which the envelopes' frame counter reaches 0 takes another count. With the counter set to
    // c, that's each frame n, counting from 1, with n mod 15 = c.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 1, 0, 0})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(96, 60, 127).Wait(96).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    for (int counter : {0, 1, 5, 14})
    {
        Sequencer seq(rom, info, 0);
        seq.SetEnvelopeCounter(counter);
        int level = 15;
        bool matches = true;
        for (int n = 1; n <= 24; n++)
        {
            seq.Step();
            level -= (n > 1 ? 1 : 0) + (n % 15 == counter ? 1 : 0);
            const Channel& c = seq.GetChannel(kFirstPsgChannel);
            matches &= (c.status ? int(c.envelope) : 0) == std::max(level, 0);
        }
        SEQLIB_CHECK(matches);
    }

    Sequencer seq(rom, info, 0);
    for (int counter : {-1, 15})
    {
        bool threw = false;
        try
        {
            seq.SetEnvelopeCounter(counter);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        SEQLIB_CHECK(threw);
    }
}

void TestPsgEchoStores()
{
    // After a release of 0, a track's echo holds the note at the echo's share of its level: 128 of 256, rounded up.
    // NR12 (0x63) takes that level with bit 3 set and a step time of 0, and NR14 (0x65) restarts the channel. A sweep
    // of 0 leaves NR10's bit 3 set, so the channel restarts once. The echo lasts 3 frames, then NR12 = 8 and NR14 =
    // 0x80 turn the channel off.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Raw({kCmdXcmd, kXcmdEchoVol, 128, kCmdXcmd, kXcmdEchoLen, 3})
                   .Note(4, 60, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<int> frame_numbers;
    std::vector<StoreFrame> frames;
    for (int f = 0; f < 30; f++)
    {
        for (const StoreFrame& frame : StoreFrames(seq, 1, 1))
        {
            frames.push_back(frame);
            frame_numbers.push_back(f);
        }
    }
    SEQLIB_REQUIRE_EQ(frames.size(), 3);

    const uint32_t f = KeyToPsgFrequency(1, 60, 0);
    const int goal = frames[0].channel.goal;
    const int echo = (goal * 128 + 255) >> 8;
    SEQLIB_CHECK(echo > 0);
    SEQLIB_CHECK_EQ(frames[1].channel.envelope, echo);
    SEQLIB_CHECK(
        frames[1].stores ==
        std::vector<Store>({{0x81, 1, 0x11}, {0x63, 1, uint32_t(0x08 + (echo << 4))}, {0x65, 1, 0x80 | (f >> 8)}}));
    SEQLIB_CHECK(frames[2].stores == std::vector<Store>({{0x63, 1, 0x08}, {0x65, 1, 0x80}}));
    SEQLIB_CHECK_EQ(frame_numbers[2] - frame_numbers[1], 3);
}

void TestPsgStartingRegisters()
{
    // The driver sets a channel's bits of NR51 and keeps the others as it reads them. A game that left channel 2's
    // outputs on keeps them through channel 1's note. The registers come as 64 bytes from 0x04000060.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(8).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<uint8_t> registers(ntdlib::agb::kSoundRegisterBytes, 0);
    registers[0x21] = 0x22;
    seq.SetSoundRegisters(registers);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 20, 1);
    SEQLIB_REQUIRE(!frames.empty());
    bool outputs = false;
    for (const Store& s : frames[0].stores)
    {
        outputs = outputs || (std::get<0>(s) == 0x81 && std::get<2>(s) == 0x33);
    }
    SEQLIB_CHECK(outputs);

    bool threw = false;
    try
    {
        seq.SetSoundRegisters(std::vector<uint8_t>(16, 0));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    SEQLIB_CHECK(threw);
}

int RunTests()
{
    TestCompressedSample();
    TestMixFixedPitch();
    TestMixInterpolation();
    TestMixSpecialSamples();
    TestMixReverb();
    TestMixMono();
    TestPsgSquareStores();
    TestPsgWaveStores();
    TestPsgNoiseStores();
    TestPsgWaveVolumes();
    TestEnvelopeCounter();
    TestPsgEchoStores();
    TestPsgStartingRegisters();

    return seqlib::testing::Summary("mp2k_mixer");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
