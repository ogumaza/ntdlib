// SPDX-License-Identifier: MIT

// Tests of the model of Konami's output stage and mixer (ntdlib/konami/output.h), and of the reference that runs a
// game's driver (ntdlib/konami/game_driver.h). They build small synthetic cartridges in memory. No game ROM is needed.
// Each expected value follows a rule of the driver's code, and the comments give the rule. The sequencer's records
// count as given: the tests take a record's values from Records() where the stage stores them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "konami_output_image.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/game_driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"

namespace
{

using namespace konami_testing;

using ntdlib::agb::kRomBase;
using ntdlib::agb::RegisterWrite;
using ntdlib::agb::Rom;
using ntdlib::konami::DriverInfo;
using ntdlib::konami::GameDriver;
using ntdlib::konami::GameRoutines;
using ntdlib::konami::kVoiceCoded;
using ntdlib::konami::kVoiceLooped;
using ntdlib::konami::kVoiceOn;
using ntdlib::konami::Output;
using ntdlib::konami::Revision;
using ntdlib::konami::SongEntrySize;
using ntdlib::konami::TrackCount;

bool SameWrites(const std::vector<RegisterWrite>& writes, std::initializer_list<RegisterWrite> expected)
{
    bool same = writes.size() == expected.size();
    if (!same)
    {
        std::printf("%zu stores, expected %zu\n", writes.size(), expected.size());
        for (const RegisterWrite& w : writes)
        {
            std::printf("  %08X/%d = %X\n", w.address, w.size, w.value);
        }
    }

    size_t i = 0;
    for (const RegisterWrite& e : expected)
    {
        if (i >= writes.size() || writes[i].address != e.address || writes[i].size != e.size ||
            writes[i].value != e.value)
        {
            if (i < writes.size())
            {
                std::printf("write %zu: %08X/%d = %X, expected %08X/%d = %X\n", i, writes[i].address, writes[i].size,
                            writes[i].value, e.address, e.size, e.value);
            }
            same = false;
        }
        i++;
    }

    return same;
}

bool SameBytes(const std::vector<int8_t>& bytes, size_t from, std::initializer_list<int> expected)
{
    size_t i = from;
    for (int e : expected)
    {
        if (i >= bytes.size() || bytes[i] != e)
        {
            std::printf("byte %zu: %d, expected %d\n", i, i < bytes.size() ? int(bytes[i]) : -999, e);
            return false;
        }
        i++;
    }

    return true;
}

bool AllZero(const std::vector<int8_t>& bytes, size_t from, size_t to)
{
    for (size_t i = from; i < to && i < bytes.size(); i++)
    {
        if (bytes[i] != 0)
        {
            std::printf("byte %zu: %d, expected 0\n", i, int(bytes[i]));
            return false;
        }
    }

    return to <= bytes.size();
}

// The Ultimate Masters revision. A voice's levels come from the volume's row of 64 at the record's two pan bytes. The
// 24-byte build's mixer shifts a product down by 7. The 28-byte build's adds 128 to a negative product first.
void TestUltimateLevels(uint32_t record_word, std::initializer_list<int> right, std::initializer_list<int> left)
{
    Image m;
    m.Mixer(record_word);
    // Track 4: pan 0x10, wait 0, note (vol 16, sample 0), wait 16, end. A pan up to 0x3F gives the right side's byte,
    // and the left side's stays at 0x3F.
    m.OneTrack(4, {0x00, 0xF0, 0x10, 0x00, 0xA8, 0x10, 0x00, 0x10, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x10, 48);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 96);
    m.Sample(0, 0x1000, 4, -1, {-128, -1, 1, 127});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kUltimateMasters));
    output.Play(0);
    XSFLIB_CHECK(output.Frame());
    const auto& v = output.Voices()[0];
    XSFLIB_CHECK(v.flags == kVoiceOn && v.volume == 16 && v.right == 48 && v.left == 96);
    XSFLIB_CHECK(v.position == kHeaders + 12 && v.count == 4);

    // A sample that ends takes the voice's flags and volume to 0, and the block's other points stay silent.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(a.size() == 14 * 16 && b.size() == 14 * 16);
    XSFLIB_CHECK(SameBytes(a, 0, right));
    XSFLIB_CHECK(SameBytes(b, 0, left));
    XSFLIB_CHECK(AllZero(a, 4, a.size()) && AllZero(b, 4, b.size()));
    XSFLIB_CHECK(output.Voices()[0].flags == 0 && output.Voices()[0].volume == 0);
    XSFLIB_CHECK(output.Rate() == 16.0 * 16777216.0 / (16 * 1254));
}

void TestUltimateStep()
{
    // A step of 0x1800 moves 1.5 points for each output. A loop goes back to the start plus the header's loop, for the
    // length less the loop. A bend picks the pitch table's next entry: the step is the entry times the header's, over
    // 4096.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128); // a level of 128 gives each sample as it stands
    m.Sample(0, 0x1800, 8, 4, {1, 2, 3, 4, 5, 6, 7, 8});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kUltimateMasters));
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.Voices()[0].flags == (kVoiceOn | kVoiceLooped));
    XSFLIB_CHECK(output.Voices()[0].step == 0x1800);

    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);

    // 1, 2, 4, 5, 7, 8, where the count passes 0. The loop then starts at byte 4 with the fraction as it stands: 5, 6,
    // 8. That pass ends half a point past the last, and the next plays 5, 7, 8.
    XSFLIB_CHECK(SameBytes(a, 0, {1, 2, 4, 5, 7, 8, 5, 6, 8, 5, 7, 8}));
    XSFLIB_CHECK(SameBytes(b, 0, {1, 2, 4, 5, 7, 8, 5, 6, 8, 5, 7, 8}));

    Image bent;
    bent.Mixer(kRecords24);
    bent.OneTrack(4, {0x00, 0xF2, 0x41, 0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    bent.Sample(0, 0x1000, 8, -1, {1, 2, 3, 4, 5, 6, 7, 8});
    const Rom bent_rom = bent.ToRom();
    Output bent_output(bent_rom, InfoOf(Revision::kUltimateMasters));
    bent_output.Play(0);
    bent_output.Frame();
    XSFLIB_CHECK(bent_output.Records()[4].pitch == 1);
    XSFLIB_CHECK(bent_output.Voices()[0].step == 4103);
}

void TestDoubleLoop()
{
    // With equal levels at a step of 0x800, the fast loop gives two outputs for each sample byte.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
    m.Sample(0, 0x800, 4, -1, {10, -20, 30, -40});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kUltimateMasters));
    output.Play(0);
    output.Frame();
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(SameBytes(a, 0, {10, 10, -20, -20, 30, 30, -40, -40, 0}));
    XSFLIB_CHECK(SameBytes(b, 0, {10, 10, -20, -20, 30, 30, -40, -40, 0}));
    XSFLIB_CHECK(output.Voices()[0].flags == 0);
}

void TestPanRetrigger()
{
    // Track 4: note (vol 16, sample 0), wait 1, pan 0x10, wait 64, end. Ultimate Masters' stage takes a pan's levels
    // into a playing voice. Prince of Tennis 2004's keeps the levels until a new note or volume.
    for (const auto& [revision, right] :
         {std::pair{Revision::kUltimateMasters, 48}, std::pair{Revision::kPrinceOfTennis, 96}})
    {
        Image m;
        m.Mixer(kRecords24);
        m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x01, 0xF0, 0x10, 0x40, 0xFD}, revision);
        m.Put8(kVolumes + 16 * 64 + 0x10, 48);
        m.Put8(kVolumes + 16 * 64 + 0x3F, 96);
        m.Sample(0, 0x1000, 0x100, -1, {});
        const Rom rom = m.ToRom();

        Output output(rom, InfoOf(revision));
        output.Play(0);
        output.Frame();
        XSFLIB_CHECK(output.Voices()[0].right == 96 && output.Voices()[0].left == 96);
        output.Frame();
        XSFLIB_CHECK(output.Voices()[0].right == right && output.Voices()[0].left == 96);
    }
}

void TestPackedAndAdpcm()
{
    // A build of 28-byte records holds 4-bit ADPCM. A nibble of 7 adds the step's 15/8, and moves 8 entries up the
    // step table: 13, 43, 106, 242, 536, 1167, 2524, 5434, 11672. A sample is the value's high byte, and the first
    // output comes before the first nibble: 10 outputs take 9 nibbles.
    {
        Image m;
        m.Mixer(kRecords28);
        m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
        m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
        m.Sample(0, 0x1000, 0x80000000u | 10, -1, {0x77, 0x77, 0x77, 0x77, 0x77});
        const Rom rom = m.ToRom();

        Output output(rom, InfoOf(Revision::kUltimateMasters));
        output.Play(0);
        output.Frame();
        XSFLIB_CHECK(output.Voices()[0].flags == (kVoiceOn | kVoiceCoded));
        std::vector<int8_t> a;
        std::vector<int8_t> b;
        output.Mix(a, b);
        XSFLIB_CHECK(SameBytes(a, 0, {0, 0, 0, 0, 0, 2, 4, 9, 21, 45, 0}));
        XSFLIB_CHECK(output.Voices()[0].adpcm_value == 11672);
    }

    // The 24-byte build's coded samples start with a table of 32 pairs. A code from -96 up to -65 gives a pair of the
    // table: its high byte, then its low byte. A code from -112 up to -97 gives that many zeros, less -112. A code
    // below -112 gives the next byte, that many times, less -128. Any other code is a sample.
    {
        Image m;
        m.Mixer(kRecords24);
        m.OneTrack(4, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD}, Revision::kUltimateMasters);
        m.Put8(kVolumes + 16 * 64 + 0x3F, 128);
        std::vector<int8_t> bytes(64);
        bytes[2] = 17; // pair 1's low byte
        bytes[3] = 34; // pair 1's high byte
        for (int code : {10, -95, -126, 40, -109, 50})
        {
            bytes.push_back(int8_t(code));
        }
        m.Sample(0, 0x1000, 0x80000000u | 9, -1, bytes);
        const Rom rom = m.ToRom();

        Output output(rom, InfoOf(Revision::kUltimateMasters));
        output.Play(0);
        output.Frame();
        std::vector<int8_t> a;
        std::vector<int8_t> b;
        output.Mix(a, b);
        XSFLIB_CHECK(SameBytes(a, 0, {10, 34, 17, 40, 40, 0, 0, 0, 50, 0}));
    }
}

void TestEcho(uint32_t record_word, int sample, int dry, int wet)
{
    // Track 4: feedback 0x40 on bus 0, delay 1 (2 blocks), voice 0 on bus 0, note (vol 16, sample 0), wait 127, end.
    Image m;
    m.Mixer(record_word);
    m.OneTrack(4, {0x00, 0xF7, 0x40, 0x00, 0xF8, 0x01, 0x00, 0xF9, 0x01, 0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD},
               Revision::kUltimateMasters);
    m.Put8(kVolumes + 16 * 64 + 0x3F, 64);
    m.Sample(0, 0x1000, 16, 0, std::vector<int8_t>(16, int8_t(sample)));
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kUltimateMasters));
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.Echo(0).feedback_goal == 0x40 && output.Echo(0).delay_blocks == 2);
    XSFLIB_CHECK(output.Echo(0).routed == (1u << 22 | 0x40000000));

    // The frame that runs the commands mixes dry. The next frame's update moves the feedback 8 towards its goal, and
    // the voice's bits in the mask send it to the bus.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(SameBytes(a, 0, {dry, dry}));
    output.Frame();
    XSFLIB_CHECK(output.Echo(0).feedback == 8);
    XSFLIB_CHECK(output.EchoMask() == (1u << 22 | 0x40000000));

    // Each block reads the line 2 blocks behind the place that it writes: the first two read silence. The block after
    // them adds what the first block stored, times the feedback, over 256.
    output.Mix(a, b);
    for (size_t i = 0; i < 32; i++)
    {
        XSFLIB_CHECK(a[i] == dry && b[i] == dry);
    }
    for (size_t i = 32; i < 64; i++)
    {
        XSFLIB_CHECK(a[i] == dry + wet && b[i] == dry + wet);
    }
}

void TestStop()
{
    // Track 4: wait 2, stop the song. Track 5: note (vol 16, sample 0), wait 127. The frame after the stop stops every
    // voice. Then the model stays idle.
    Image m;
    m.Mixer(kRecords24);
    m.Song(0, kBase, 0x100, {{4, 0x00}, {5, 0x10}}, Revision::kUltimateMasters);
    m.PutBytes(kBase, {0x02, 0xFF, 0x00});
    m.PutBytes(kBase + 0x10, {0x00, 0xA8, 0x10, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Sample(0, 0x1000, 0x100, -1, {});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kUltimateMasters));
    output.Play(0);
    int frames = 0;
    while (!output.Song()->Stopped() && frames < 16)
    {
        XSFLIB_CHECK(output.Frame());
        frames++;
    }

    XSFLIB_CHECK(frames == 3);
    XSFLIB_CHECK(output.Voices()[1].flags == kVoiceOn);
    XSFLIB_CHECK(output.Frame());
    XSFLIB_CHECK(output.Voices()[1].flags == 0 && output.Voices()[1].volume == 0);
    XSFLIB_CHECK(!output.Frame());
    XSFLIB_CHECK(output.SoundWrites().empty());
}

void TestUltimateStores()
{
    // The first frame stops the wave channel, loads a silent wave and flips the wave RAM's bank. The init leaves 0xBB
    // in NR51: every channel on both sides, but the wave channel. A pan up to 0x3F gives the left side's byte 0x3F and
    // the right side's the pan, and the channel leaves the right side: 0xBA. The square's stores follow, then those of
    // the other channels' first rests, at volume 0, with square 2's duty byte of 0x80.
    Image m;
    m.Mixer(kRecords24);
    // Track 0: pan 0x10, wait 0, PSG note (vol 3, note 24), wait 2, rest, wait 2, loop the song.
    m.OneTrack(0, {0x00, 0xF0, 0x10, 0x00, 0xD3, 0x18, 0x02, 0xE0, 0x02, 0xFF, 0x01}, Revision::kUltimateMasters);
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kUltimateMasters));
    output.Play(0);
    output.Frame();
    const auto& first = output.Records()[0];
    XSFLIB_CHECK(first.vol == 3 && first.trig != 0 && first.pitch == 24 * 32);
    XSFLIB_CHECK(SameWrites(output.SoundWrites(), {{kSound3CntH, 2, 0},
                                                   {kWaveRam, 4, 0},
                                                   {kWaveRam + 4, 4, 0},
                                                   {kWaveRam + 8, 4, 0},
                                                   {kWaveRam + 12, 4, 0},
                                                   {kSound3CntL, 2, 0x80},
                                                   {kNr51, 1, 0xBA},
                                                   {kSound1CntH, 2, uint32_t(3 << 12 | first.b2)},
                                                   {kSound1CntX, 2, 0x8000u | (24 * 32)},
                                                   {kSound2CntL, 2, 0x80},
                                                   {kSound2CntH, 2, 0x8000},
                                                   {kSound3CntH, 2, 0},
                                                   {kSound3CntX, 2, 0},
                                                   {kSound4CntL, 2, 0},
                                                   {kSound4CntH, 2, 0x1234}}));

    // Each loop of the song stops the wave channel again, and the wave RAM's other bank takes the silent wave.
    for (int f = 0; f < 16 && !output.Song()->LoopedLastFrame(); f++)
    {
        output.Frame();
    }

    XSFLIB_CHECK(output.Song()->LoopedLastFrame());
    const std::vector<RegisterWrite>& writes = output.SoundWrites();
    XSFLIB_CHECK(writes.size() >= 6);
    XSFLIB_CHECK(writes[0].address == kSound3CntH && writes[0].value == 0);
    XSFLIB_CHECK(writes[5].address == kSound3CntL && writes[5].value == 0xC0);

    // The wave channel joins NR51 at its first note, on both sides: 0xFF.
    Image wave;
    wave.Mixer(kRecords24);
    wave.OneTrack(2, {0x00, 0xD3, 0x18, 0x02, 0xFD}, Revision::kUltimateMasters);
    const Rom wave_rom = wave.ToRom();
    Output wave_output(wave_rom, InfoOf(Revision::kUltimateMasters));
    wave_output.Play(0);
    wave_output.Frame();
    XSFLIB_CHECK(wave_output.SoundWrites().size() > 6);
    XSFLIB_CHECK(wave_output.SoundWrites()[6].address == kNr51 && wave_output.SoundWrites()[6].value == 0xFF);
}

// The WCT 2004 layout. A record holds one pan byte. Its low nibble gives the right side's entry of the volume's row of
// 16, and its high nibble the left side's. The 28-byte build's mixer adds 1 to a negative value after the shift by 4
// in both of its loops. The 24-byte build's loop for equal levels doesn't.
void TestWctLevels(uint32_t record_word, std::initializer_list<int> equal, std::initializer_list<int> right,
                   std::initializer_list<int> left)
{
    Image m;
    m.Mixer(record_word);
    // Track 4: note (vol 8, sample 0), wait 1, pan 0x8F, wait 64, end.
    m.OneTrack(4, {0x00, 0xA8, 0x00, 0x01, 0xF0, 0x8F, 0x40, 0xFD}, Revision::kWct2004);
    for (uint32_t volume = 0; volume < 256; volume++)
    {
        m.Put8(kVolumes + volume * 16 + 15, 8);
        m.Put8(kVolumes + volume * 16 + 8, 4);
    }
    m.Sample(0, 0x1000, 8, 0, {-32, -1, 1, 127, -32, -1, 1, 127});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kWct2004));
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.Voices()[0].right == 8 && output.Voices()[0].left == 8);
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(SameBytes(a, 0, equal));
    XSFLIB_CHECK(SameBytes(b, 0, equal));

    // A playing voice takes its levels in every frame.
    output.Frame();
    XSFLIB_CHECK(output.Voices()[0].right == 8 && output.Voices()[0].left == 4);
    output.Mix(a, b);
    XSFLIB_CHECK(SameBytes(a, 0, right));
    XSFLIB_CHECK(SameBytes(b, 0, left));
}

void TestWctPsg()
{
    // A pan off centre on a square's track takes two channels: the channel plays the right side's level, and the next
    // channel the left side's. t = 3, 2 and 1 give 0x77. Square 1 then gives 0xFF, less 0x33, plus 0x21: 0xED. The wave
    // channel's bits stay out until its first note: 0xA9. The other channels' first rests follow.
    Image m;
    m.Mixer(kRecords24);
    m.OneTrack(0, {0x00, 0xF0, 0x8F, 0x00, 0xD8, 0x30, 0x10, 0xFD}, Revision::kWct2004);
    for (uint32_t volume = 0; volume < 256; volume++)
    {
        m.Put8(kVolumes + volume * 16 + 15, 5);
        m.Put8(kVolumes + volume * 16 + 8, 3);
    }
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kWct2004));
    output.Play(0);
    output.Frame();
    const auto& r = output.Records()[0];
    const uint32_t frequency = 0x8000u | uint32_t(r.pitch);
    XSFLIB_CHECK(SameWrites(output.SoundWrites(), {{kSound3CntH, 2, 0},
                                                   {kWaveRam, 4, 0},
                                                   {kWaveRam + 4, 4, 0},
                                                   {kWaveRam + 8, 4, 0},
                                                   {kWaveRam + 12, 4, 0},
                                                   {kSound3CntL, 2, 0x80},
                                                   {kNr51, 1, 0xA9},
                                                   {kSound1CntH, 2, uint32_t(5 << 12 | r.b2)},
                                                   {kSound1CntX, 2, frequency},
                                                   {kSound2CntL, 2, uint32_t(3 << 12 | r.b2)},
                                                   {kSound2CntH, 2, frequency},
                                                   {kSound3CntH, 2, 0},
                                                   {kSound3CntX, 2, 0},
                                                   {kSound4CntL, 2, 0},
                                                   {kSound4CntH, 2, 0x1234}}));
}

void TestLevelTable()
{
    // Rave Master's mixer reads a table of 256 bytes for each level, at the sample's byte taken as unsigned. The
    // 24-byte build holds pairs in its coded samples. The 20-byte build holds none, and a code from -96 up is a sample.
    for (const uint32_t record_word : {kRecords24, kRecords20})
    {
        Image m;
        m.Mixer(record_word);
        m.LevelTable();
        m.OneTrack(4, {0x00, 0xA8, 0x00, 0x7F, 0xFD}, Revision::kRaveMaster);
        for (uint32_t volume = 0; volume < 256; volume++)
        {
            m.Put8(kVolumes + volume * 16 + 15, 2);
        }
        for (uint32_t i = 0; i < 256; i++)
        {
            m.Put8(kLevelTable + 2 * 256 + i, uint8_t(i / 8));
        }

        std::vector<int8_t> bytes(64);
        bytes[0] = -1; // pair 0's low byte: at -1, the table's last entry
        bytes[1] = 8;  // pair 0's high byte
        if (record_word == kRecords24)
        {
            bytes.push_back(-96);
            m.Sample(0, 0x1000, 0x80000000u | 2, -1, bytes);
        }
        else
        {
            m.Sample(0, 0x1000, 0x80000000u | 1, -1, {-90});
        }

        const Rom rom = m.ToRom();
        Output output(rom, InfoOf(Revision::kRaveMaster));
        output.Play(0);
        output.Frame();
        XSFLIB_CHECK(output.Voices().size() == 8);
        XSFLIB_CHECK(output.Voices()[0].flags == (kVoiceOn | kVoiceCoded));
        std::vector<int8_t> a;
        std::vector<int8_t> b;
        output.Mix(a, b);
        if (record_word == kRecords24)
        {
            XSFLIB_CHECK(SameBytes(a, 0, {8 / 8, 255 / 8, 0}));
        }
        else
        {
            // -90 is the byte 166.
            XSFLIB_CHECK(SameBytes(a, 0, {166 / 8, 0}));
        }
    }
}

// The ring mixers. A voice sounds on one FIFO: voices 0 to 2 on FIFO A, and voices 3 to 5 on FIFO B. Its level is its
// volume plus 1, and its value the product over 16.
void TestRingLevels()
{
    Image m;
    m.Mixer(0);
    // Song 0: voice 0 and voice 3, each with a note (vol 12). Sample 0 ends after 4 points. Sample 1 plays each byte
    // twice, at a step of 0x800, and ends after 3 bytes. Song 1: voice 0 at volume 0. A voice at volume 0 doesn't run.
    m.Song(0, kBase, 0x100, {{4, 0x00}, {7, 0x10}}, Revision::kEternalDuelist);
    m.PutBytes(kBase + 0x00, {0x00, 0xAC, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x10, {0x00, 0xAC, 0x01, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x100, kWaitingTrack);
    m.Song(1, kBase + 0x180, 0x100, {{4, 0x00}}, Revision::kEternalDuelist);
    m.PutBytes(kBase + 0x180, {0x00, 0xA0, 0x00, 0x7F, 0xFD});
    m.PutBytes(kBase + 0x280, kWaitingTrack);
    m.Sample(0, 0x1000, 4, -1, {-128, -1, 1, 127});
    m.Sample(1, 0x800, 3, -1, {16, -16, 32});
    for (uint32_t i = 0; i < 16; i++)
    {
        m.Put8(kWaves + i, uint8_t(0x10 + i));
    }
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kEternalDuelist));
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.Voices().size() == 6);
    XSFLIB_CHECK(output.Voices()[0].flags == kVoiceOn && output.Voices()[0].volume == 12);

    // A song's start stores NR51 and SOUNDCNT_H, and loads wave 0. A loop does the same. A ring's output stage then
    // stores NR51 from the four PSG records. Each holds its channel's bits for both sides. The channels' first rests
    // follow, at volume 0, with the squares' duty bytes of 0x80.
    XSFLIB_CHECK(SameWrites(output.SoundWrites(), {{kNr51, 1, 0xFF},
                                                   {kSoundCntH, 2, 0x330E},
                                                   {kSound3CntH, 2, 0},
                                                   {kWaveRam, 4, 0x13121110},
                                                   {kWaveRam + 4, 4, 0x17161514},
                                                   {kWaveRam + 8, 4, 0x1B1A1918},
                                                   {kWaveRam + 12, 4, 0x1F1E1D1C},
                                                   {kSound3CntL, 2, 0x80},
                                                   {kNr51, 1, 0xFF},
                                                   {kSound1CntH, 2, 0x80},
                                                   {kSound1CntX, 2, 0x8000},
                                                   {kSound2CntL, 2, 0x80},
                                                   {kSound2CntH, 2, 0x8000},
                                                   {kSound3CntH, 2, 0},
                                                   {kSound3CntX, 2, 0},
                                                   {kSound4CntL, 2, 0},
                                                   {kSound4CntH, 2, 0x1234}}));

    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(a.size() == 224 && b.size() == 224);
    XSFLIB_CHECK(SameBytes(a, 0, {-104, -1, 0, 103, 0}));
    XSFLIB_CHECK(AllZero(a, 4, a.size()));
    XSFLIB_CHECK(SameBytes(b, 0, {13, 13, -13, -13, 26, 26, 0}));
    XSFLIB_CHECK(AllZero(b, 6, b.size()));
    XSFLIB_CHECK(output.Voices()[0].flags == 0 && output.Voices()[3].flags == 0);

    Output silent(rom, InfoOf(Revision::kEternalDuelist));
    silent.Play(1);
    silent.Frame();
    silent.Mix(a, b);
    XSFLIB_CHECK(silent.Voices()[0].flags == kVoiceOn && silent.Voices()[0].volume == 0);
    XSFLIB_CHECK(silent.Voices()[0].position == kHeaders + 12);
    XSFLIB_CHECK(AllZero(a, 0, a.size()));
}

void TestRingWrap()
{
    // The ring holds 704 points. 14 blocks a frame reach 672 after three frames, and the fourth frame's 224 points fall
    // in two parts: 32 up to the ring's end, then 192. The mixer checks a voice's flags once, for both parts. A voice
    // that ends in the first part therefore runs again in the second: it plays its last point once more, and ends
    // again.
    constexpr int kLength = 672 + 10;
    Image m;
    m.Mixer(0);
    m.OneTrack(4, {0x00, 0xAF, 0x00, 0x7F, 0xFD}, Revision::kEternalDuelist);
    std::vector<int8_t> bytes;
    for (int i = 0; i < kLength; i++)
    {
        bytes.push_back(int8_t(16 * (1 + i % 7)));
    }
    m.Sample(0, 0x1000, uint32_t(kLength), -1, bytes);
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kEternalDuelist));
    output.Play(0);
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    for (int f = 0; f < 3; f++)
    {
        output.Frame();
        output.Mix(a, b);
        XSFLIB_CHECK(a.size() == 224);
        for (int i = 0; i < 224; i++)
        {
            // A volume of 15 gives a level of 16, and the product over 16 gives each sample as it stands.
            XSFLIB_CHECK(a[size_t(i)] == bytes[size_t(f * 224 + i)]);
        }
    }

    output.Frame();
    output.Mix(a, b);
    XSFLIB_CHECK(a.size() == 224);
    for (int i = 0; i < 10; i++)
    {
        XSFLIB_CHECK(a[size_t(i)] == bytes[size_t(672 + i)]);
    }
    XSFLIB_CHECK(AllZero(a, 10, 32));
    XSFLIB_CHECK(a[32] == bytes[size_t(kLength - 1)]);
    XSFLIB_CHECK(AllZero(a, 33, a.size()));
    XSFLIB_CHECK(output.Voices()[0].flags == 0);
}

void TestRingLoop()
{
    // A looped voice goes back to the header's data plus the loop, for the length less the loop. A ring voice's step of
    // 0x800 gives two outputs for each byte on the way.
    Image m;
    m.Mixer(0);
    m.OneTrack(4, {0x00, 0xAF, 0x00, 0x7F, 0xFD}, Revision::kEternalDuelist);
    m.Sample(0, 0x1000, 6, 4, {1, 2, 3, 4, 5, 6});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kEternalDuelist));
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.Voices()[0].flags == (kVoiceOn | kVoiceLooped));
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(SameBytes(a, 0, {1, 2, 3, 4, 5, 6, 5, 6, 5, 6}));
}

void TestRingNr51Twice()
{
    // Some builds' output stage opens with a store of NR51 and the setting's test, before the store that every build
    // makes. The opening: movs r2, #0xE0 / lsls r2, r2, #1 / add r2, r8 / ldrb r0, [r2] / strb r0, [r3].
    Image m;
    m.Mixer(0);
    m.OneTrack(4, {0x00, 0xFD}, Revision::kCaptainTsubasa);
    m.PutHalfwords(kRomBase + 0x4A00, {0x22E0, 0x0052, 0x4442, 0x7810, 0x7018});
    const Rom rom = m.ToRom();

    Output output(rom, InfoOf(Revision::kCaptainTsubasa));
    output.Play(0);
    output.Frame();
    int stores = 0;
    for (const RegisterWrite& w : output.SoundWrites())
    {
        stores += w.address == kNr51 ? 1 : 0;
    }

    // The song's start stores NR51 once, and the output stage twice.
    XSFLIB_CHECK(stores == 3);
}

// The revision without a mixer. A FIFO and a timer serve each two voices. A voice's bytes are the product of each
// sample and its volume plus 1, over 32.
void TestFifoVoice()
{
    Image m;
    // Voice 0: note (vol 12, map entry 1), wait 60, end. Map entry 1 is sample 5, of two blocks. Pitch 0's period is
    // 0x100 cycles.
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    m.Put16(kPeriods, 0x100);
    std::vector<int8_t> bytes = Block({-128, -1, 1, 127});
    const std::vector<int8_t> second = Block({64, -64});
    bytes.insert(bytes.end(), second.begin(), second.end());
    FifoSample(m, 5, 2, 0x4000, bytes);
    for (uint32_t i = 0; i < 16; i += 2)
    {
        m.Put16(kWaves + i, uint16_t(0x1100 + i));
    }
    const Rom rom = m.ToRom();

    Output output(rom, FifoInfo());
    XSFLIB_CHECK(output.Rate() == 0.0);
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.Voices().size() == 4);
    XSFLIB_CHECK(output.Voices()[0].flags == kVoiceOn);
    XSFLIB_CHECK(output.FifoCycles(0) == 0x100);
    // FIFO B follows timer 0 until its first note chooses timer 1. Its DMA stays off, and it takes nothing.
    XSFLIB_CHECK(output.FifoCycles(1) == 0x100);

    // The song's start clears square 1's sweep and loads wave 0 with the bank's stores. The stage stores the squares'
    // first rests, NR50 and the wave and noise channels' first rests. The voice's start then stores SOUNDCNT_H's high
    // byte with FIFO A's reset bit.
    const int nr50 = output.Song()->PsgVolume();
    XSFLIB_CHECK(SameWrites(
        output.SoundWrites(),
        {{kSound1CntL, 2, 0},        {kSound3CntL, 2, 0x40},     {kWaveRam, 2, 0x1100},     {kWaveRam + 2, 2, 0x1102},
         {kWaveRam + 4, 2, 0x1104},  {kWaveRam + 6, 2, 0x1106},  {kWaveRam + 8, 2, 0x1108}, {kWaveRam + 10, 2, 0x110A},
         {kWaveRam + 12, 2, 0x110C}, {kWaveRam + 14, 2, 0x110E}, {kSound3CntL, 2, 0x80},    {kSound3CntX, 2, 0x8000},
         {kSound1CntH, 2, 0x80},     {kSound1CntX, 2, 0x8000},   {kSound2CntL, 2, 0x80},    {kSound2CntH, 2, 0x8000},
         {kNr50, 1, uint32_t(nr50)}, {kSound3CntH, 2, 0},        {kSound3CntX, 2, 0},       {kSound4CntL, 2, 0},
         {kSound4CntH, 2, 0x8000},   {kSoundCntHHigh, 1, 0x3B}}));

    // The first DMA takes the buffer as the start left it: silence. Its interrupt fills the next block, and so on. The
    // interrupt after the sample's last block stops the timer.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(a.size() == 48 && b.empty());
    XSFLIB_CHECK(AllZero(a, 0, 16));
    XSFLIB_CHECK(SameBytes(a, 16, {-52, -1, 0, 51, -52}));
    XSFLIB_CHECK(SameBytes(a, 32, {26, -26, 26, -26}));
    XSFLIB_CHECK(output.FifoCycles(0) == 0);
    XSFLIB_CHECK(output.Voices()[0].flags == 0);
}

void TestFifoTwoVoices()
{
    // Voice 0: note (vol 12, map entry 1) and a note on voice 1 (vol 6, map entry 2), wait 60, end. A start of the
    // first voice stops a second voice that plays at a rate of the period table. Two voices at their samples' rates
    // both sound, at the second's rate.
    const auto play = [](uint16_t period, std::vector<int8_t>& a)
    {
        Image m;
        m.OneTrack(4, {0x00, 0xBC, 0x01, 0x00, 0xA6, 0x02, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
        m.PutBytes(kMap, {0x00, 0x05, 0x07});
        m.Put16(kPeriods, period);
        FifoSample(m, 5, 1, 0x300, Block({64}));
        FifoSample(m, 7, 1, 0x200, Block({-64}));
        const Rom rom = m.ToRom();

        Output output(rom, FifoInfo());
        output.Play(0);
        output.Frame();
        XSFLIB_CHECK(output.FifoCycles(0) == (period ? uint32_t(period) : 0x200u));
        XSFLIB_CHECK(output.Voices()[0].flags != 0);
        XSFLIB_CHECK((output.Voices()[1].flags != 0) == (period == 0));
        std::vector<int8_t> b;
        output.Mix(a, b);
    };

    std::vector<int8_t> a;
    play(0x100, a);
    XSFLIB_CHECK(a.size() == 32);
    XSFLIB_CHECK(AllZero(a, 0, 16));
    XSFLIB_CHECK(SameBytes(a, 16, {26, 26})); // 64 * 13 / 32

    play(0, a);
    XSFLIB_CHECK(a.size() == 32);
    XSFLIB_CHECK(AllZero(a, 0, 16));
    XSFLIB_CHECK(SameBytes(a, 16, {12, 12})); // 64 * 13 / 32 + (-64 * 7 / 32)
}

void TestFifoB()
{
    // Voice 2 plays on FIFO B. Its start stores SOUNDCNT_H's high byte with FIFO B's reset bit and timer 1, and FIFO B
    // follows timer 1 from there on.
    Image m;
    m.OneTrack(6, {0x00, 0xBC, 0x01, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    m.Put16(kPeriods, 0x180);
    FifoSample(m, 5, 1, 0x300, Block({32}));
    const Rom rom = m.ToRom();

    Output output(rom, FifoInfo());
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.SoundWrites().back().address == kSoundCntHHigh);
    XSFLIB_CHECK(output.SoundWrites().back().value == 0xF3);
    XSFLIB_CHECK(output.FifoCycles(0) == 0 && output.FifoCycles(1) == 0x180);
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    XSFLIB_CHECK(a.empty() && b.size() == 32);
    XSFLIB_CHECK(AllZero(b, 0, 16));
    XSFLIB_CHECK(SameBytes(b, 16, {13, 13})); // 32 * 13 / 32
}

void TestFifoBend()
{
    // Voice 0: note (vol 12, map entry 1), wait 1, a bend of +16, wait 60, end. A bend gives the FIFO's timer a goal,
    // and the next DMA's interrupt moves the timer there.
    Image m;
    m.OneTrack(4, {0x00, 0xBC, 0x01, 0x01, 0xF2, 0x30, 0x3C, 0xFD}, Revision::kDungeonDiceMonsters);
    m.PutBytes(kMap, {0x00, 0x05});
    for (uint32_t p = 0; p < 0x200; p++)
    {
        m.Put16(kPeriods + 2 * p, uint16_t(0x4000 - p));
    }
    FifoSample(m, 5, 100, 0x300, {});
    const Rom rom = m.ToRom();

    Output output(rom, FifoInfo());
    output.Play(0);
    output.Frame();
    XSFLIB_CHECK(output.FifoCycles(0) == 0x4000);
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    output.Mix(a, b);
    output.Frame();
    const auto& r = output.Records()[4];
    XSFLIB_CHECK(r.flags == ntdlib::konami::kOutDungeonDicePeriod);
    XSFLIB_CHECK(output.FifoCycles(0) == 0x4000);
    output.Mix(a, b);
    XSFLIB_CHECK(output.FifoCycles(0) == uint32_t(uint16_t(r.pitch)));
}

// The reference. Small programs stand in for a game's routines.
constexpr uint32_t kReturn = kRomBase + 0x100; // bx lr
constexpr uint32_t kFrameEntry = kRomBase + 0x110;
constexpr uint32_t kStage = kRomBase + 0x130;
constexpr uint32_t kArmMixer = kRomBase + 0x150;
constexpr uint32_t kFifoIrq = kRomBase + 0x170;
constexpr uint32_t kMixerBuffer = 0x03001000;
constexpr uint32_t kDmaBuffer = 0x03002000;

Rom ReferenceRom()
{
    ntdlib::testing::agb_files::CartImage m(0x1000);
    m.PutHalfwords(kReturn, {0x4770});

    // push {lr} / sub sp, #40 / movs r0, #0x55 / add r1, sp, #8 / strb r0, [r1, #3], then a call of the output stage /
    // add sp, #40 / pop {r0} / bx r0
    m.PutHalfwords(kFrameEntry, {0xB500, 0xB08A, 0x2055, 0xA902, 0x70C8});
    m.PutBl(kFrameEntry + 10, kStage);
    m.PutHalfwords(kFrameEntry + 14, {0xB00A, 0xBC01, 0x4700});

    // The output stage stores FIFO A's reset bit, and stores what the register then reads as in NR51.
    // ldr r3, =SOUNDCNT_H's high byte / movs r0, #0x3B / strb r0, [r3] / ldrb r1, [r3] / ldr r3, =NR51 / strb r1, [r3]
    // / bx lr / nop
    m.PutHalfwords(kStage, {0x4B03, 0x203B, 0x7018, 0x7819, 0x4B02, 0x7019, 0x4770, 0x46C0});
    m.Put32(kStage + 16, kSoundCntHHigh);
    m.Put32(kStage + 20, kNr51);

    // The mixer, in ARM: it counts in the first byte of its buffer. ldr r0, [pc, #12] / ldrb r1, [r0] /
    // add r1, r1, #1 / strb r1, [r0] / bx lr
    m.Put32(kArmMixer, 0xE59F000C);
    m.Put32(kArmMixer + 4, 0xE5D01000);
    m.Put32(kArmMixer + 8, 0xE2811001);
    m.Put32(kArmMixer + 12, 0xE5C01000);
    m.Put32(kArmMixer + 16, 0xE12FFF1E);
    m.Put32(kArmMixer + 20, kMixerBuffer);

    // A DMA's interrupt routine counts in the first byte of the DMA's source.
    // ldr r0, =source / ldrb r1, [r0] / adds r1, #1 / strb r1, [r0] / bx lr
    m.PutHalfwords(kFifoIrq, {0x4803, 0x7801, 0x3101, 0x7001, 0x4770});
    m.Put32(kFifoIrq + 16, kDmaBuffer);
    return m.ToRom();
}

GameRoutines ReferenceRoutines()
{
    GameRoutines routines;
    routines.init = kReturn;
    routines.request_song = kReturn;
    routines.start_song = kReturn;
    routines.frame = kFrameEntry;
    routines.output_stage = kStage;
    routines.tracks = 2;
    routines.fifo_b = 0x20;
    return routines;
}

void TestReferenceFrame()
{
    const Rom rom = ReferenceRom();
    GameRoutines routines = ReferenceRoutines();
    routines.mixer = kArmMixer;
    routines.fifo_buffer = kMixerBuffer;
    GameDriver driver(rom, routines);
    driver.Play(0);

    // Timer 0's reload gives 0x400 cycles a point, and a block of 16 points 0x4000 cycles.
    driver.Gba().Store(ntdlib::agb::kRegTimer0Reload, 2, 0x10000 - 0x400);
    driver.Gba().Store(ntdlib::agb::kRegTimer0Reload + 2, 2, 0x80);
    XSFLIB_CHECK(driver.BlockCycles() == 0x4000);
    XSFLIB_CHECK(driver.Rate() == 16384.0);

    // The records lie 8 bytes above the stack pointer at the output stage. A store with FIFO A's reset bit reads back
    // without it, and the other bits stay.
    XSFLIB_CHECK(driver.Frame());
    XSFLIB_CHECK(driver.Records().size() == 2 && driver.Records()[0].vol == 0x55);
    XSFLIB_CHECK(SameWrites(driver.SoundWrites(), {{kSoundCntHHigh, 1, 0x3B}, {kNr51, 1, 0x33}}));

    // A store of the whole register loses both FIFOs' reset bits.
    driver.Gba().Store(kSoundCntH, 2, 0xBB0E);
    XSFLIB_CHECK(driver.Gba().Load(kSoundCntH, 2) == 0x330E);

    // A frame holds 17 blocks and 0x940 cycles. The cycles left over reach a whole block in the 7th frame: 18 blocks.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    int mixed = 0;
    for (int f = 0; f < 7; f++)
    {
        if (f != 0)
        {
            driver.Frame();
        }

        driver.Mix(a, b);
        XSFLIB_CHECK(a.size() == (f == 6 ? 18u : 17u) * 16);
        XSFLIB_CHECK(b.size() == a.size());
        mixed += int(a.size() / 16);
        XSFLIB_CHECK(a[a.size() - 16] == int8_t(mixed));
    }
}

void TestReferenceFifo()
{
    const Rom rom = ReferenceRom();
    GameRoutines routines = ReferenceRoutines();
    routines.fifo_irqs = {kFifoIrq, kFifoIrq};
    GameDriver driver(rom, routines);
    driver.Play(0);

    // Timer 0 runs at 0x100 cycles a sample. DMA 1 takes FIFO A's bytes from the buffer. Its timing bits keep it from
    // starting at the store. DMA 2 stays off. FIFO B takes nothing.
    driver.Gba().Store(ntdlib::agb::kRegTimer0Reload, 2, 0x10000 - 0x100);
    driver.Gba().Store(ntdlib::agb::kRegTimer0Reload + 2, 2, 0x80);
    driver.Gba().Store(0x040000BC, 4, kDmaBuffer);
    driver.Gba().Store(0x040000C6, 2, 0xB640);

    // An empty FIFO takes 16 bytes at the first sample, and has 15 at the next: 16 more. It then takes 16 bytes for
    // each 16 samples. The frame holds 1,097 samples of 0x100 cycles. Their DMAs come at samples 1, 2, 18, 34 and so on
    // up to 1,090: 70.
    std::vector<int8_t> a;
    std::vector<int8_t> b;
    driver.Frame();
    driver.Mix(a, b);
    XSFLIB_CHECK(a.size() == 70 * 16 && b.empty());
    XSFLIB_CHECK(a[0] == 0 && a[16] == 1 && a[32] == 2 && a[17] == 0);

    // The output stage's store of the reset bit empties the FIFO. The next frame's first samples behave as before. Its
    // first sample comes 192 cycles into the frame, and it holds 1,097 again.
    driver.Frame();
    driver.Mix(a, b);
    XSFLIB_CHECK(a.size() == 70 * 16);
}

} // namespace

int main()
{
    TestUltimateLevels(kRecords24, {-48, -1, 0, 47}, {-96, -1, 0, 95});
    TestUltimateLevels(kRecords28, {-47, 0, 0, 47}, {-95, 0, 0, 95});
    TestUltimateStep();
    TestDoubleLoop();
    TestPanRetrigger();
    TestPackedAndAdpcm();
    // The 24-byte build's echo: the value over 128 rounds down, and the product of the line and the feedback over 256
    // gets 1 added when it's negative. The 28-byte build rounds both towards zero first, with 128 added.
    TestEcho(kRecords24, 64, 32, 1);
    TestEcho(kRecords24, -64, -32, 0);
    TestEcho(kRecords28, 64, 32, 1);
    TestEcho(kRecords28, -64, -31, -1);
    TestStop();
    TestUltimateStores();
    // The 28-byte build keeps the loop that a note first took: the loop for equal levels. That loop gives both sides
    // the right side's value. The 24-byte build's next block takes the loop for other levels.
    TestWctLevels(kRecords28, {-15, 0, 0, 63}, {-15, 0, 0, 63}, {-15, 0, 0, 63});
    TestWctLevels(kRecords24, {-16, -1, 0, 63}, {-15, 0, 0, 63}, {-7, 0, 0, 31});
    TestWctPsg();
    TestLevelTable();
    TestRingLevels();
    TestRingWrap();
    TestRingLoop();
    TestRingNr51Twice();
    TestFifoVoice();
    TestFifoTwoVoices();
    TestFifoB();
    TestFifoBend();
    TestReferenceFrame();
    TestReferenceFifo();

    return xsflib::testing::Summary("Konami's output");
}
