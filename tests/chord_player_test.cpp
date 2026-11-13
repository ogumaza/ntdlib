// SPDX-License-Identifier: MIT

// Unit tests for the player of a chord record's steps (jaudio/chord_player.h) and for a layer of records
// (jaudio/chord_layer.h), with made-up entries.

#include "ntdlib/jaudio/chord_player.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/jaudio/chord_layer.h"

using namespace ntdlib::jaudio;

namespace
{

void Put32(std::vector<uint8_t>& data, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        data.push_back(static_cast<uint8_t>(value >> shift));
    }
}

// A chord database with one entry: the steps block `steps` (a count, then each step's flow and level) and one part
// with one block that plays key 60 and stays. `records` gives the record table: four words for each record.
std::vector<uint8_t> MakeDatabase(const std::vector<uint8_t>& steps, const std::vector<uint32_t>& records = {})
{
    std::vector<uint8_t> data;
    Put32(data, 0);
    data.insert(data.end(), {'C', 'D', 'S', 'E'});
    Put32(data, 1);
    const uint32_t entry = 0x14;
    const uint32_t steps_at = entry + 12;
    const uint32_t part = steps_at + static_cast<uint32_t>(steps.size());
    const uint32_t block = part + 8;
    const uint32_t table = block + 21;
    Put32(data, table);
    Put32(data, entry);
    data.insert(data.end(), {0, 1, 48, 72});
    Put32(data, steps_at);
    Put32(data, part);
    data.insert(data.end(), steps.begin(), steps.end());
    data.insert(data.end(), {1, 1, 0, 0});
    Put32(data, block);
    const std::vector<uint8_t> bytes = {6, 60, 0, 0, 0, 0, 0, 0, 0, 0x7f, 0x7f, 0, 0};
    data.insert(data.end(), bytes.begin(), bytes.end());
    data.insert(data.end(), 8, 0x7e);
    Put32(data, static_cast<uint32_t>(records.size() / 4));
    for (const uint32_t word : records)
    {
        Put32(data, word);
    }

    return data;
}

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-6f;
}

struct Fixture
{
    std::vector<uint8_t> data;
    ChordDatabase database;
    ChordPlayer player;
    Chord chord{.root = 0, .kind = 0};
    ChordMoment moment{.chord = &chord};
    ChordRandom random;

    Fixture(const std::vector<uint8_t>& steps, int start, float level)
        : data(MakeDatabase(steps)), database(ReadChordDatabase(data)), player(data, database.entries[0], start, level)
    {
    }
};

void TestSteps()
{
    // Step 0 gives the first wait. Step 1 plays at full level, step 2 at 64/127 without the accent, and step 3 leaves
    // the effect playing and repeats.
    Fixture f({3, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x20, 0x02}, 0, 0.5f);
    XSFLIB_CHECK(f.player.Start(f.moment, f.random).empty());
    XSFLIB_CHECK(f.player.Waiting() == 0);

    std::vector<ChordStep> steps = f.player.Event(0.9f, f.moment, f.random);
    XSFLIB_REQUIRE(steps.size() == 1);
    XSFLIB_CHECK(steps[0].action == ChordStep::Action::kStart && steps[0].step == 1);
    XSFLIB_CHECK(Near(steps[0].level, 0.5f));
    XSFLIB_CHECK(steps[0].keys == std::vector<int>{60});
    XSFLIB_CHECK(f.player.NextStep() == 2 && f.player.Waiting() == 0);

    steps = f.player.Event(0.7f, f.moment, f.random);
    XSFLIB_REQUIRE(steps.size() == 1);
    XSFLIB_CHECK(Near(steps[0].level, 64.0f / 127.0f * 0.5f));

    steps = f.player.Event(1.0f, f.moment, f.random);
    XSFLIB_REQUIRE(steps.size() == 1);
    XSFLIB_CHECK(steps[0].action == ChordStep::Action::kKeep && steps[0].keys.empty());
    XSFLIB_CHECK(f.player.NextStep() == 3 && f.player.Waiting() == 0);
    steps = f.player.Event(1.0f, f.moment, f.random);
    XSFLIB_CHECK(steps.size() == 1 && steps[0].step == 3);
}

void TestAccent()
{
    // A level without bit 7 takes the accent.
    Fixture f({1, 0x01, 0x00, 0x01, 0x40}, 0, 1.0f);
    f.player.Start(f.moment, f.random);
    XSFLIB_CHECK(f.player.Waiting() == 1);
    const std::vector<ChordStep> steps = f.player.Event(0.9f, f.moment, f.random);
    XSFLIB_REQUIRE(steps.size() == 1);
    XSFLIB_CHECK(Near(steps[0].level, 64.0f / 127.0f * 0.9f));
}

void TestAtOnce()
{
    // A first wait of 4 plays step 1 at once, and the player then holds. A jump to step 0 does the same.
    Fixture f({1, 0x04, 0x00, 0x00, 0x40}, 0, 1.0f);
    std::vector<ChordStep> steps = f.player.Start(f.moment, f.random);
    XSFLIB_REQUIRE(steps.size() == 1);
    XSFLIB_CHECK(steps[0].step == 1 && Near(steps[0].level, 64.0f / 127.0f));
    XSFLIB_CHECK(!f.player.Waiting() && f.player.Playing());
    XSFLIB_CHECK(f.player.Event(1.0f, f.moment, f.random).empty());
    steps = f.player.Jump(0, f.moment, f.random);
    XSFLIB_CHECK(steps.size() == 1 && steps[0].step == 1);

    // A flow of 4 plays the step once more before the move.
    Fixture twice({2, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01}, 0, 1.0f);
    twice.player.Start(twice.moment, twice.random);
    steps = twice.player.Event(1.0f, twice.moment, twice.random);
    XSFLIB_REQUIRE(steps.size() == 2);
    XSFLIB_CHECK(steps[0].step == 1 && steps[1].step == 1);
    XSFLIB_CHECK(twice.player.NextStep() == 2 && !twice.player.Waiting());
}

void TestEndAndHold()
{
    // A flow of 5 ends the record. A level of 1 stops the effect.
    Fixture end({1, 0x00, 0x00, 0x05, 0x01}, 0, 1.0f);
    end.player.Start(end.moment, end.random);
    std::vector<ChordStep> steps = end.player.Event(1.0f, end.moment, end.random);
    XSFLIB_REQUIRE(steps.size() == 1);
    XSFLIB_CHECK(steps[0].action == ChordStep::Action::kStop);
    XSFLIB_CHECK(!end.player.Playing() && !end.player.Waiting());
    XSFLIB_CHECK(end.player.Jump(0, end.moment, end.random).empty());

    // Bit 3 holds the player at its step.
    Fixture hold({1, 0x00, 0x00, 0x08, 0x00}, 0, 1.0f);
    hold.player.Start(hold.moment, hold.random);
    hold.player.Event(1.0f, hold.moment, hold.random);
    XSFLIB_CHECK(hold.player.Playing() && !hold.player.Waiting() && hold.player.NextStep() == 1);
}

void TestStartStep()
{
    // A record that starts at step 1 waits by step 0's flow and plays step 2 first. The step leaves the effect playing:
    // the part, which starts at block 1, has one block.
    Fixture f({2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02}, 1, 1.0f);
    f.player.Start(f.moment, f.random);
    XSFLIB_CHECK(f.player.Waiting() == 2);
    const std::vector<ChordStep> steps = f.player.Event(1.0f, f.moment, f.random);
    XSFLIB_CHECK(steps.size() == 1 && steps[0].step == 2 && steps[0].action == ChordStep::Action::kKeep);
}

void TestRhythm()
{
    // The beats come in the game's order, whatever the order of the notes. The accent's velocity sets 1.0 or 0.9, and
    // wait 0's beat leaves 0.7 for the beats after it.
    ChordRhythm rhythm;
    XSFLIB_CHECK(!rhythm.TimerThreeRunning());
    rhythm.Note(0x24, 90);
    rhythm.Note(0x2a, 90);
    rhythm.Note(0x27, 90);
    rhythm.Note(0x25, 90);
    rhythm.Note(ChordRhythm::kAccentKey, 110);
    std::vector<ChordBeat> beats = rhythm.Frame();
    XSFLIB_REQUIRE(beats.size() == 4);
    XSFLIB_CHECK(beats[0].wait == 3 && Near(beats[0].accent, 0.9f));
    XSFLIB_CHECK(rhythm.TimerThreeRunning());
    XSFLIB_CHECK(beats[1].wait == 0 && Near(beats[1].accent, 0.9f));
    XSFLIB_CHECK(beats[2].wait == 1 && Near(beats[2].accent, 0.7f));
    XSFLIB_CHECK(beats[3].wait == 2 && Near(beats[3].accent, 0.7f));

    // The marks clear each frame. A velocity of 0 and other keys mark nothing.
    XSFLIB_CHECK(rhythm.Frame().empty());
    rhythm.Note(0x27, 0);
    rhythm.Note(0x29, 90);
    rhythm.Note(0x3c, 90);
    XSFLIB_CHECK(rhythm.Frame().empty());

    rhythm.Note(ChordRhythm::kAccentKey, 64);
    rhythm.Note(0x2a, 1);
    beats = rhythm.Frame();
    XSFLIB_CHECK(beats.size() == 1 && beats[0].wait == 1 && Near(beats[0].accent, 1.0f));
}

// A layer of record 0, which plays the entry as effect 0x50001, over a chord file with one chord (C major) in order 0.
struct LayerFixture
{
    std::vector<uint8_t> data;
    ChordLayer layer;

    LayerFixture(const std::vector<uint8_t>& steps, uint32_t stage = 1, uint32_t sound = 0x50001)
        : data(MakeDatabase(steps, {0, sound, 0, 0})),
          layer(data, ChordFile{.orders = {{0}}, .chords = {Chord{.root = 0, .kind = 0}}}, stage,
                std::vector<uint32_t>{0})
    {
    }
};

void TestLayer()
{
    // Nothing starts before the position is in. The first frame with one starts the record, and its wait-0 beats then
    // play the steps.
    LayerFixture f({3, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x20, 0x02});
    XSFLIB_CHECK(f.layer.Effects() == 1);
    f.layer.Note(0x27, 90);
    XSFLIB_CHECK(f.layer.Frame(std::nullopt).empty());
    XSFLIB_CHECK(f.layer.Frame(0).empty());

    f.layer.Note(0x2a, 90);
    XSFLIB_CHECK(f.layer.Frame(0).empty());

    f.layer.Note(0x27, 90);
    std::vector<ChordLayerSound> sounds = f.layer.Frame(std::nullopt);
    XSFLIB_REQUIRE(sounds.size() == 1);
    XSFLIB_CHECK(sounds[0].action == ChordLayerSound::Action::kStart && sounds[0].effect == 0);
    XSFLIB_CHECK(sounds[0].sound == 0x50001 && Near(sounds[0].level, 1.0f));
    XSFLIB_CHECK(sounds[0].ports == (std::map<uint8_t, uint16_t>{{0xb, 60}}));

    // Step 2 starts the effect again. Step 3 leaves it playing.
    f.layer.Note(0x27, 90);
    XSFLIB_CHECK(f.layer.Frame(0).size() == 1);
    f.layer.Note(0x27, 90);
    XSFLIB_CHECK(f.layer.Frame(0).empty());
}

void TestLayerTimerThree()
{
    // A player that waits for timer 3 waits in timer 0's list until a beat of timer 3 has come.
    LayerFixture early({1, 0x03, 0x00, 0x00, 0x40});
    early.layer.Frame(0);
    early.layer.Note(0x25, 90);
    XSFLIB_CHECK(early.layer.Frame(0).empty());
    early.layer.Note(0x27, 90);
    XSFLIB_CHECK(early.layer.Frame(0).size() == 1);

    LayerFixture late({1, 0x03, 0x00, 0x00, 0x40});
    late.layer.Note(0x25, 90);
    late.layer.Frame(std::nullopt);
    late.layer.Frame(0);
    late.layer.Note(0x27, 90);
    XSFLIB_CHECK(late.layer.Frame(0).empty());
    late.layer.Note(0x25, 90);
    XSFLIB_CHECK(late.layer.Frame(0).size() == 1);
}

void TestLayerStages()
{
    XSFLIB_CHECK(ChordEffectAtStage(0x50042, 0x2c) == 0x50063);
    XSFLIB_CHECK(ChordEffectAtStage(0x50045, 0x35) == 0x50062);
    XSFLIB_CHECK(ChordEffectAtStage(0x50045, 0x41) == 0x50062);
    XSFLIB_CHECK(ChordEffectAtStage(0x50042, 0x2d) == 0x50042);
    XSFLIB_CHECK(ChordEffectAtStage(0x50001, 0x2c) == 0x50001);

    LayerFixture sax({1, 0x00, 0x00, 0x00, 0x40}, 0x2c, 0x50042);
    sax.layer.Frame(0);
    sax.layer.Note(0x27, 90);
    const std::vector<ChordLayerSound> sounds = sax.layer.Frame(0);
    XSFLIB_CHECK(sounds.size() == 1 && sounds[0].sound == 0x50063);

    // A record past the table throws.
    const std::vector<uint8_t> data = MakeDatabase({1, 0x00, 0x00, 0x00, 0x40});
    const std::vector<uint32_t> records = {0};
    bool threw = false;
    try
    {
        ChordLayer(data, ChordFile{}, 1, records);
    }
    catch (const xsflib::FormatError&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

} // namespace

int main()
{
    TestSteps();
    TestAccent();
    TestAtOnce();
    TestEndAndHold();
    TestStartStep();
    TestRhythm();
    TestLayer();
    TestLayerTimerThree();
    TestLayerStages();

    return xsflib::testing::Summary("chord player");
}
