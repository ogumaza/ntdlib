// SPDX-License-Identifier: MIT

// Tests of the MusyX readers (ntdlib/musyx) on small synthetic files: a project with a song group and an effects group,
// a pool, a sample directory with its samples, and songs with and without sections. The expected values follow from
// the layouts that the readers' headers describe, and from DSP-ADPCM's definition.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/musyx/pool.h"
#include "ntdlib/musyx/project.h"
#include "ntdlib/musyx/samples.h"
#include "ntdlib/musyx/song.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace musyx = ntdlib::musyx;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// Big-endian data under construction.
class Bytes
{
public:
    std::size_t Size() const
    {
        return data_.size();
    }

    Bytes& U8(uint32_t v)
    {
        data_.push_back(static_cast<uint8_t>(v));

        return *this;
    }

    Bytes& U16(uint32_t v)
    {
        return U8(v >> 8).U8(v);
    }

    Bytes& U32(uint32_t v)
    {
        return U16(v >> 16).U16(v);
    }

    // Little-endian values, for the pool's envelope tables.
    Bytes& Le16(uint32_t v)
    {
        return U8(v).U8(v >> 8);
    }

    Bytes& Le32(uint32_t v)
    {
        return Le16(v).Le16(v >> 16);
    }

    Bytes& PadTo(std::size_t size)
    {
        data_.resize(std::max(data_.size(), size));

        return *this;
    }

    void Put16(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe16(data_.data() + at, static_cast<uint16_t>(v));
    }

    void Put32(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe32(data_.data() + at, v);
    }

    std::vector<uint8_t>& Data()
    {
        return data_;
    }

private:
    std::vector<uint8_t> data_;
};

// A project with a song group (ID 7) and an effects group (ID 9).
std::vector<uint8_t> MakeProject()
{
    Bytes b;

    // The song group's header. Its lists follow it.
    b.PadTo(0x28);
    b.Put16(4, 7);
    b.Put16(6, 0);

    // Macros 3, 5 to 8 (a range) and 10. Samples 1 and 2. No tables. Keymap 0 and layer 1.
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    b.U16(3).U16(0x8005).U16(8).U16(10).U16(0xffff);
    b.Put32(0x0c, static_cast<uint32_t>(b.Size()));
    b.U16(1).U16(2).U16(0xffff);
    b.Put32(0x14, static_cast<uint32_t>(b.Size()));
    b.U16(0).U16(0xffff);
    b.Put32(0x18, static_cast<uint32_t>(b.Size()));
    b.U16(1).U16(0xffff);

    // Two pages, then the page whose program is 0xff. No drum pages.
    b.Put32(0x1c, static_cast<uint32_t>(b.Size()));
    b.U16(3).U8(10).U8(4).U8(0).U8(0);
    b.U16(0x4000).U8(20).U8(8).U8(5).U8(0);
    b.U16(0).U8(0).U8(0).U8(0xff).U8(0);

    // One MIDI setup, for song 0x42. Channel 0's settings differ from MIDI's defaults. The rest have the defaults.
    b.Put32(0x24, static_cast<uint32_t>(b.Size()));
    b.U16(0x42).U16(0);
    b.U8(5).U8(100).U8(32).U8(40).U8(10);
    for (int c = 1; c < 16; c++)
    {
        b.U8(0).U8(127).U8(64).U8(0).U8(0);
    }

    // The effects group, at a 4-byte boundary, with macro 0x20 and two effects.
    b.PadTo((b.Size() + 3) / 4 * 4);
    const std::size_t effects_group = b.Size();
    b.Put32(0, static_cast<uint32_t>(effects_group));
    b.PadTo(effects_group + 0x28);
    b.Put16(effects_group + 4, 9);
    b.Put16(effects_group + 6, 1);
    b.Put32(effects_group + 0x08, static_cast<uint32_t>(b.Size()));
    b.U16(0x20).U16(0xffff);
    b.Put32(effects_group + 0x1c, static_cast<uint32_t>(b.Size()));
    b.U16(2).U16(0);
    b.U16(0x100).U16(0x20).U8(2).U8(50).U8(90).U8(30).U8(72).U8(3);
    b.U16(0x101).U16(0x20).U8(1).U8(40).U8(127).U8(64).U8(60).U8(0);

    // The end of the groups.
    b.Put32(effects_group, static_cast<uint32_t>(b.Size()));
    b.U32(0xffffffff);

    return b.Data();
}

void TestProject()
{
    const std::vector<uint8_t> file = MakeProject();
    const std::vector<musyx::Group> groups = musyx::ReadProject(file);
    if (!Check(groups.size() == 2, "the project's two groups"))
    {
        return;
    }

    const musyx::Group& songs = groups[0];
    Check(songs.id == 7 && songs.type == musyx::GroupType::kSongs, "the song group's ID and type");
    Check(songs.macros == std::vector<uint16_t>{3, 5, 6, 7, 8, 10}, "the macros, with a range");
    Check(songs.samples == std::vector<uint16_t>{1, 2}, "the samples");
    Check(songs.tables.empty(), "no tables");
    Check(songs.keymaps == std::vector<uint16_t>{0x4000}, "a keymap's ID has bit 14");
    Check(songs.layers == std::vector<uint16_t>{0x8001}, "a layer's ID has bit 15");
    Check(musyx::KindOf(0x0003) == musyx::ObjectKind::kMacro && musyx::KindOf(0x4000) == musyx::ObjectKind::kKeymap &&
              musyx::KindOf(0x8001) == musyx::ObjectKind::kLayer,
          "the kinds of objects");
    if (Check(songs.pages.size() == 2, "two pages before the end"))
    {
        Check(songs.pages[0].object == 3 && songs.pages[0].priority == 10 && songs.pages[0].max_voices == 4 &&
                  songs.pages[0].program == 0,
              "the first page");
        Check(songs.pages[1].object == 0x4000 && songs.pages[1].program == 5, "the second page");
    }
    Check(songs.drum_pages.empty(), "no drum pages");
    if (Check(songs.midi_setups.size() == 1, "one MIDI setup"))
    {
        const musyx::MidiSetup& setup = songs.midi_setups[0];
        const musyx::ChannelSetup& first = setup.channels[0];
        Check(setup.song == 0x42, "the setup's song");
        Check(first.program == 5 && first.volume == 100 && first.pan == 32 && first.reverb == 40 && first.chorus == 10,
              "channel 0's setup");
        Check(setup.channels[15].volume == 127 && setup.channels[15].pan == 64, "channel 15's setup");
    }
    Check(songs.effects.empty(), "a song group has no effects");

    const musyx::Group& effects = groups[1];
    Check(effects.id == 9 && effects.type == musyx::GroupType::kEffects, "the effects group's ID and type");
    Check(effects.macros == std::vector<uint16_t>{0x20}, "the effects group's macro");
    Check(effects.pages.empty() && effects.midi_setups.empty(), "an effects group has no pages");
    if (Check(effects.effects.size() == 2, "two effects"))
    {
        const musyx::Effect& e = effects.effects[0];
        Check(e.id == 0x100 && e.object == 0x20 && e.max_voices == 2 && e.priority == 50 && e.volume == 90 &&
                  e.pan == 30 && e.key == 72 && e.volume_group == 3,
              "the first effect");
        Check(effects.effects[1].id == 0x101 && effects.effects[1].key == 60, "the second effect");
    }

    // A group of neither type, and groups that run in a circle.
    std::vector<uint8_t> bad_type = file;
    xsflib::StoreBe16(bad_type.data() + 6, 2);
    Check(ErrorOf([&] { musyx::ReadProject(bad_type); }) == "project file has a group of unknown type 2",
          "a group of unknown type");
    std::vector<uint8_t> circle = file;
    const uint32_t effects_group = xsflib::LoadBe32(file.data());
    xsflib::StoreBe32(circle.data() + effects_group, 0);
    Check(ErrorOf([&] { musyx::ReadProject(circle); }) == "project file's groups run in a circle",
          "groups in a circle");
    Check(!ErrorOf([&] { musyx::ReadProject(std::span(file).first(file.size() - 4)); }).empty(),
          "a project without its end");
}

// A pool with three macros' steps, two envelope tables, a keymap and a layer.
std::vector<uint8_t> MakePool()
{
    Bytes b;
    b.PadTo(16);

    // Macro 3: SetAdsr with table 5, a WaitMs whose bytes test the step's accessors, and End.
    b.Put32(0, static_cast<uint32_t>(b.Size()));
    b.U32(8 + 3 * 8).U16(3).U16(0);
    b.U32(0x0000050c).U32(0);
    b.U32(0x12345687).U32(0xabcdef01);
    b.U32(0).U32(0);
    b.U32(0xffffffff);

    // Table 5: a linear envelope. Table 6: a DLS envelope. Both are little-endian.
    b.Put32(4, static_cast<uint32_t>(b.Size()));
    b.U32(8 + 8).U16(5).U16(0);
    b.Le16(0x10).Le16(0x20).Le16(0xc00).Le16(0x40);
    b.U32(8 + 20).U16(6).U16(0);
    b.Le32(static_cast<uint32_t>(-100000)).Le32(200000).Le16(0x800).Le16(0x1234).Le32(0).Le32(0x10000);
    b.U32(0xffffffff);

    // Keymap 0x4000: key 60 plays macro 3 two keys higher, panned left, with a lower priority. The other keys play
    // nothing.
    b.Put32(8, static_cast<uint32_t>(b.Size()));
    b.U32(8 + 128 * 8).U16(0x4000).U16(0);
    for (int k = 0; k < 128; k++)
    {
        if (k == 60)
        {
            b.U16(3).U8(2).U8(10).U16(0xfffe).U16(0);
        }
        else
        {
            b.U16(0xffff).U8(0).U8(64).U16(0).U16(0);
        }
    }
    b.U32(0xffffffff);

    // Layer 0x8001: macro 3 over keys 0 to 59 and keymap 0x4000 over 60 to 127.
    b.Put32(12, static_cast<uint32_t>(b.Size()));
    b.U32(8 + 4 + 2 * 12).U16(0x8001).U16(0);
    b.U32(2);
    b.U16(3).U8(0).U8(59).U8(0xf4).U8(100).U16(1).U8(20).U8(0).U16(0);
    b.U16(0x4000).U8(60).U8(127).U8(0).U8(127).U16(0).U8(64).U8(0).U16(0);
    b.U32(0xffffffff);

    return b.Data();
}

void TestPool()
{
    const std::vector<uint8_t> file = MakePool();
    const musyx::Pool pool = musyx::ReadPool(file);

    if (Check(pool.macros.contains(3) && pool.macros.at(3).size() == 3, "macro 3's three steps"))
    {
        const std::vector<musyx::MacroStep>& steps = pool.macros.at(3);
        Check(steps[0].Op() == musyx::MacroOp::kSetAdsr && steps[0].Half(1) == 5, "SetAdsr's table");
        const musyx::MacroStep& wait = steps[1];
        Check(wait.Op() == musyx::MacroOp::kWaitMs, "the opcode is the low 7 bits of the first word");
        Check(wait.Byte(0) == 0x87 && wait.Byte(1) == 0x56 && wait.Byte(2) == 0x34 && wait.Byte(3) == 0x12,
              "the first word's bytes, from the low one");
        Check(wait.Byte(4) == 0x01 && wait.Byte(5) == 0xef && wait.Byte(6) == 0xcd && wait.Byte(7) == 0xab,
              "the second word's bytes, from the low one");
        Check(wait.Half(1) == 0x3456 && wait.Half(4) == 0xef01 && wait.Half(6) == 0xabcd,
              "the halves are little-endian pairs of bytes");
        Check(steps[2].Op() == musyx::MacroOp::kEnd, "End");
    }

    if (Check(pool.tables.contains(5) && pool.tables.contains(6), "the two tables"))
    {
        const std::optional<musyx::LinearAdsr> linear = musyx::ReadLinearAdsr(pool.tables.at(5));
        Check(linear && linear->attack == 0x10 && linear->decay == 0x20 && linear->sustain == 0xc00 &&
                  linear->release == 0x40,
              "the linear envelope");
        const std::optional<musyx::DlsAdsr> dls = musyx::ReadDlsAdsr(pool.tables.at(6));
        Check(dls && dls->attack == -100000 && dls->decay == 200000 && dls->sustain == 0x800 &&
                  dls->release == 0x1234 && dls->velocity_to_attack == 0 && dls->key_to_decay == 0x10000,
              "the DLS envelope");
        Check(!musyx::ReadDlsAdsr(pool.tables.at(5)), "a linear envelope is too short for a DLS one");
    }

    if (Check(pool.keymaps.contains(0x4000), "the keymap"))
    {
        const auto& keymap = pool.keymaps.at(0x4000);
        Check(keymap[60].object == 3 && keymap[60].transpose == 2 && keymap[60].pan == 10 &&
                  keymap[60].priority_offset == -2,
              "key 60's entry");
        Check(keymap[61].object == 0xffff, "a key without an object");
    }

    if (Check(pool.layers.contains(0x8001) && pool.layers.at(0x8001).size() == 2, "the layer's two entries"))
    {
        const musyx::LayerEntry& low = pool.layers.at(0x8001)[0];
        const musyx::LayerEntry& high = pool.layers.at(0x8001)[1];
        Check(low.object == 3 && low.low_key == 0 && low.high_key == 59 && low.transpose == -12 && low.volume == 100 &&
                  low.priority_offset == 1 && low.pan == 20,
              "the layer's first entry");
        Check(high.object == 0x4000 && high.low_key == 60 && high.high_key == 127 && high.pan == 64,
              "the layer's second entry");
    }

    // A record shorter than its header.
    std::vector<uint8_t> short_record = file;
    xsflib::StoreBe32(short_record.data() + 16, 4);
    Check(ErrorOf([&] { musyx::ReadPool(short_record); }) == "pool file has a record of 4 bytes",
          "a record shorter than its header");
}

// DSP-ADPCM coefficients whose predictor 0 predicts nothing and whose predictor 1 repeats the sample before.
std::array<int16_t, 16> Coefficients()
{
    std::array<int16_t, 16> c{};
    c[2] = 2048;

    return c;
}

// A sample directory and its samples file.
struct SampleFiles
{
    std::vector<uint8_t> directory;
    std::vector<uint8_t> samples;
};

// A sample directory with a DSP-ADPCM sample, a PCM16 sample and a PCM8 sample, and the samples file. The DSP-ADPCM
// sample has 28 samples in two frames. Its first frame uses predictor 0 with scale 2^1, and the second predictor 1
// with scale 2^0. It loops from sample 20 to its end. Its settings give the loop's history as 7, then `loop_hist1`.
SampleFiles MakeSamples(int16_t loop_hist1)
{
    // The samples file: the DSP-ADPCM sample's two frames, then 4 PCM16 samples, then 4 PCM8 samples. Every nibble of
    // the first frame is 1. Its samples are then 2. Every nibble of the second frame is 1 too. Each of its samples is
    // then 1 more than the one before: 3, 4, ..., 16.
    Bytes samples;
    samples.U8(0x01);
    for (int i = 0; i < 7; i++)
    {
        samples.U8(0x11);
    }
    samples.U8(0x10);
    for (int i = 0; i < 7; i++)
    {
        samples.U8(0x11);
    }
    samples.U16(0x1234).U16(0x8000).U16(0x7fff).U16(0xffff);
    samples.U8(0x01).U8(0x80).U8(0x7f).U8(0xff);

    Bytes d;

    // The DSP-ADPCM sample: ID 1, key 64, 22050 Hz, 28 samples, a loop of 8 from 20.
    d.U16(1).U16(0).U32(0).U32(0);
    d.U32(64u << 24 | 22050).U32(0u << 24 | 28).U32(20).U32(8).U32(0);
    const std::size_t settings_field = d.Size() - 4;

    // The PCM16 sample: ID 2, key 60, 32000 Hz, 4 samples, no loop.
    d.U16(2).U16(0).U32(16).U32(0);
    d.U32(60u << 24 | 32000).U32(2u << 24 | 4).U32(0).U32(0).U32(0);

    // The PCM8 sample: ID 3, key 72, 8000 Hz, 4 samples, no loop.
    d.U16(3).U16(0).U32(24).U32(0);
    d.U32(72u << 24 | 8000).U32(3u << 24 | 4).U32(0).U32(0).U32(0);
    d.U16(0xffff);
    d.PadTo((d.Size() + 3) / 4 * 4);

    // The DSP-ADPCM settings: the coefficients' count, the first frame's scale, the loop's scale, the loop's history
    // (the sample two before the loop's start, then the one just before) and the coefficients.
    d.Put32(settings_field, static_cast<uint32_t>(d.Size()));
    d.U16(8).U8(0x01).U8(0x10).U16(7).U16(static_cast<uint16_t>(loop_hist1));
    for (int16_t c : Coefficients())
    {
        d.U16(static_cast<uint16_t>(c));
    }

    return {d.Data(), samples.Data()};
}

void TestSamples()
{
    const SampleFiles files = MakeSamples(8);
    const std::vector<musyx::SampleInfo> directory = musyx::ReadSampleDirectory(files.directory);
    if (!Check(directory.size() == 3, "three samples"))
    {
        return;
    }

    const musyx::SampleInfo& adpcm = directory[0];
    Check(adpcm.id == 1 && adpcm.offset == 0 && adpcm.key == 64 && adpcm.rate == 22050 &&
              adpcm.format == musyx::SampleFormat::kAdpcm && adpcm.length == 28 && adpcm.loop_start == 20 &&
              adpcm.loop_length == 8,
          "the DSP-ADPCM sample's entry");
    Check(adpcm.adpcm && adpcm.adpcm->initial_scale == 0x01 && adpcm.adpcm->loop_scale == 0x10 &&
              adpcm.adpcm->loop_hist2 == 7 && adpcm.adpcm->loop_hist1 == 8 &&
              adpcm.adpcm->coefficients == Coefficients(),
          "the DSP-ADPCM settings");
    Check(directory[1].format == musyx::SampleFormat::kPcm16 && directory[1].key == 60 && directory[1].rate == 32000 &&
              !directory[1].adpcm,
          "the PCM16 sample's entry");
    Check(directory[2].format == musyx::SampleFormat::kPcm8 && directory[2].offset == 24, "the PCM8 sample's entry");

    // The first frame decodes to 2s, and the second counts up from 3. The loop's history matches the first pass's
    // samples 18 and 19 (7 and 8). The decoder then adds no second pass.
    const musyx::DecodedSample first = musyx::DecodeSample(adpcm, files.samples);
    std::vector<int16_t> expected(14, 2);
    for (int16_t s = 3; s <= 16; s++)
    {
        expected.push_back(s);
    }
    Check(first.pcm == expected, "the DSP-ADPCM sample's samples");
    Check(first.loop && first.loop_start == 20 && first.loop_end == 28, "the loop as the directory gives it");

    // With another history, each repeat of the loop counts up from the history's sample instead: 21 to 28 after a
    // history of 20. The decoder keeps the first pass and loops over a second.
    const SampleFiles other = MakeSamples(20);
    const musyx::DecodedSample repeated =
        musyx::DecodeSample(musyx::ReadSampleDirectory(other.directory)[0], other.samples);
    std::vector<int16_t> unrolled = expected;
    for (int16_t s = 21; s <= 28; s++)
    {
        unrolled.push_back(s);
    }
    Check(repeated.pcm == unrolled, "a loop whose repeats differ from the first pass");
    Check(repeated.loop && repeated.loop_start == 28 && repeated.loop_end == 36, "the loop over the second pass");

    const musyx::DecodedSample pcm16 = musyx::DecodeSample(directory[1], files.samples);
    Check(pcm16.pcm == std::vector<int16_t>{0x1234, -32768, 32767, -1} && !pcm16.loop, "the PCM16 sample");
    const musyx::DecodedSample pcm8 = musyx::DecodeSample(directory[2], files.samples);
    Check(pcm8.pcm == std::vector<int16_t>{0x0100, -32768, 0x7f00, -256}, "the PCM8 sample");

    // A sample that runs past the samples file's end.
    Check(ErrorOf([&] { musyx::DecodeSample(adpcm, std::span(files.samples).first(15)); }) ==
              "sample 1 runs past the end of the samples file",
          "a sample past the file's end");
}

// A song without sections. Track 0 plays pattern 0 on channel 3 at tick 0, with program 5, volume 100, two keys higher
// and 3 softer, and loops back to that entry at tick 768. Track 5 plays pattern 1 on channel 9 at tick 384, then ends.
// The tempo starts at 120 and changes to 90 at tick 384.
std::vector<uint8_t> MakeSong(bool sections)
{
    Bytes b;
    b.PadTo(0x58);
    b.Put32(0x10, (sections ? 0x80000000u : 0u) | 120);
    b.Put32(0x14, 384);
    if (sections)
    {
        for (uint32_t s = 1; s < 16; s++)
        {
            b.Put32(0x14 + 4 * s, 100 * s);
        }
    }

    // The track table: 64 offsets.
    b.Put32(0x00, static_cast<uint32_t>(b.Size()));
    const std::size_t tracks = b.Size();
    b.PadTo(tracks + 64 * 4);

    // The channel map, and the section map.
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    const std::size_t channels = b.Size();
    b.PadTo(channels + 64);
    b.Data()[channels + 0] = 3;
    b.Data()[channels + 5] = 9;
    if (sections)
    {
        b.Put32(0x54, static_cast<uint32_t>(b.Size()));
        const std::size_t section_map = b.Size();
        b.PadTo(section_map + 64);
        b.Data()[section_map + 5] = 2;
    }

    // The tempo table.
    b.Put32(0x0c, static_cast<uint32_t>(b.Size()));
    b.U32(384).U32(90).U32(0xffffffff).U32(0);

    // Track 0's entries.
    b.Put32(tracks + 0, static_cast<uint32_t>(b.Size()));
    b.U32(0).U8(5).U8(100).U16(0).U16(0).U8(2).U8(0xfd);
    b.U32(768).U8(0xff).U8(0xff).U16(0).U16(0xfffe).U16(0);

    // Track 5's entries.
    b.Put32(tracks + 5 * 4, static_cast<uint32_t>(b.Size()));
    b.U32(384).U8(0xff).U8(0xff).U16(0).U16(1).U8(0).U8(0);
    b.U32(768).U8(0xff).U8(0xff).U16(0).U16(0xffff).U16(0);

    // Pattern 0: a note, a rest, a program change, a volume change (controller 7), controller 130, a command that the
    // game ignores, another note, and the end. Its pitch-wheel stream rises by 0x20 at tick 10 with a one-byte delta,
    // falls by 0x1000 at tick 300 with two-byte ones, and falls by 1 at tick 310.
    const std::size_t pattern0 = b.Size();
    b.U32(12).U32(0).U32(0);
    b.U16(0).U8(60).U8(100).U16(96);
    b.U16(96).U8(0).U8(0);
    b.U16(0).U8(0x80 | 7).U8(0);
    b.U16(10).U8(0x80 | 90).U8(0x80 | 7);
    b.U16(0).U8(0x80 | 1).U8(1);
    b.U16(0).U8(0x80 | 2).U8(2);
    b.U16(86).U8(64).U8(80).U16(192);
    b.U16(400).U8(0xff).U8(0xff);
    const std::size_t pitch = b.Size();
    b.U8(10).U8(0x20);
    b.U8(0x81).U8(0x22).U8(0xf0).U8(0x00);
    b.U8(10).U8(0x7f);
    b.U8(0x80).U8(0x00);
    b.Put32(pattern0 + 4, static_cast<uint32_t>(pitch));

    // Pattern 1: one note and the end. Its mod-wheel stream rises to 0x7f at tick 0.
    const std::size_t pattern1 = b.Size();
    b.U32(12).U32(0).U32(0);
    b.U16(0).U8(36).U8(127).U16(48);
    b.U16(384).U8(0xff).U8(0xff);
    const std::size_t mod = b.Size();
    b.U8(0).U8(0x80).U8(0x7f);
    b.U8(0x80).U8(0x00);
    b.Put32(pattern1 + 8, static_cast<uint32_t>(mod));

    // The pattern table.
    b.Put32(0x04, static_cast<uint32_t>(b.Size()));
    b.U32(static_cast<uint32_t>(pattern0)).U32(static_cast<uint32_t>(pattern1));

    return b.Data();
}

void TestSong()
{
    const std::vector<uint8_t> file = MakeSong(false);
    const musyx::Song song = musyx::ReadSong(file);
    Check(song.tempo == 120 && !song.fine_tempo && !song.sections, "the tempo, without sections");
    Check(song.loop_starts[0] == 384 && song.loop_starts[1] == 0, "one loop start");
    Check(song.tempo_changes.size() == 1 && song.tempo_changes[0].tick == 384 && song.tempo_changes[0].tempo == 90,
          "the tempo change");
    if (!Check(song.tracks.size() == 2, "two tracks") || !Check(song.patterns.size() == 2, "two patterns"))
    {
        return;
    }

    const musyx::Track& first = song.tracks[0];
    Check(first.number == 0 && first.channel == 3 && first.section == 0, "track 0's channel");
    if (Check(first.entries.size() == 2, "track 0's two entries"))
    {
        const musyx::TrackEntry& e = first.entries[0];
        Check(e.tick == 0 && e.pattern == 0 && e.program == 5 && e.volume == 100 && e.transpose == 2 &&
                  e.velocity_add == -3,
              "track 0's pattern entry");
        Check(first.entries[1].tick == 768 && first.entries[1].pattern == musyx::kLoopBack &&
                  first.entries[1].loop_target == 0,
              "track 0's loop entry");
    }
    const musyx::Track& second = song.tracks[1];
    Check(second.number == 5 && second.channel == 9, "track 5's channel");
    Check(second.entries.size() == 2 && second.entries[0].program == 0xff && second.entries[0].volume == 0xff &&
              second.entries[1].pattern == musyx::kEndOfTrack,
          "track 5's entries");

    // Pattern 0's events, with the rest's time counted and the ignored command left out.
    const musyx::Pattern& p = song.patterns[0];
    Check(p.end_tick == 592, "pattern 0's end");
    if (Check(p.events.size() == 5, "pattern 0's five events"))
    {
        Check(p.events[0].kind == musyx::PatternEventKind::kNote && p.events[0].tick == 0 && p.events[0].key == 60 &&
                  p.events[0].velocity == 100 && p.events[0].length == 96,
              "the first note");
        Check(p.events[1].kind == musyx::PatternEventKind::kProgram && p.events[1].tick == 96 && p.events[1].value == 7,
              "the program change after the rest");
        Check(p.events[2].kind == musyx::PatternEventKind::kController && p.events[2].tick == 106 &&
                  p.events[2].controller == 7 && p.events[2].value == 90,
              "the volume change");
        Check(p.events[3].kind == musyx::PatternEventKind::kController && p.events[3].controller == 130 &&
                  p.events[3].value == 1,
              "controller 130");
        Check(p.events[4].kind == musyx::PatternEventKind::kNote && p.events[4].tick == 192 && p.events[4].key == 64 &&
                  p.events[4].length == 192,
              "the second note");
    }
    if (Check(p.pitch_wheel.size() == 3, "the pitch wheel's three points"))
    {
        Check(p.pitch_wheel[0].tick == 10 && p.pitch_wheel[0].value == 0x2020, "a one-byte rise");
        Check(p.pitch_wheel[1].tick == 300 && p.pitch_wheel[1].value == 0x1020, "a two-byte fall");
        Check(p.pitch_wheel[2].tick == 310 && p.pitch_wheel[2].value == 0x101f, "a one-byte fall");
    }
    Check(p.mod_wheel.empty(), "no mod wheel in pattern 0");

    const musyx::Pattern& q = song.patterns[1];
    Check(q.end_tick == 384 && q.events.size() == 1 && q.pitch_wheel.empty(), "pattern 1");
    Check(q.mod_wheel.size() == 1 && q.mod_wheel[0].tick == 0 && q.mod_wheel[0].value == 0x7f, "pattern 1's mod wheel");

    // With sections: each track's section, and a loop start for each section.
    const musyx::Song with_sections = musyx::ReadSong(MakeSong(true));
    Check(with_sections.sections && with_sections.tempo == 120, "the tempo, with sections");
    Check(with_sections.tracks.size() == 2 && with_sections.tracks[0].section == 0 &&
              with_sections.tracks[1].section == 2,
          "the tracks' sections");
    Check(with_sections.loop_starts[0] == 384 && with_sections.loop_starts[15] == 1500, "the sections' loop starts");

    // A loop to the loop entry itself, and a track on channel 16.
    std::vector<uint8_t> bad_loop = file;
    const uint32_t track0 = xsflib::LoadBe32(file.data() + xsflib::LoadBe32(file.data()));
    xsflib::StoreBe16(bad_loop.data() + track0 + 12 + 10, 1);
    Check(ErrorOf([&] { musyx::ReadSong(bad_loop); }) == "song has a track that loops to entry 1 of 2",
          "a loop that goes nowhere earlier");
    std::vector<uint8_t> bad_channel = file;
    bad_channel[xsflib::LoadBe32(file.data() + 8)] = 16;
    Check(ErrorOf([&] { musyx::ReadSong(bad_channel); }) == "song has track 0 on channel 16 in section 0",
          "a track on channel 16");
}

} // namespace

int main()
{
    TestProject();
    TestPool();
    TestSamples();
    TestSong();

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("musyx tests passed\n");

    return 0;
}
