// SPDX-License-Identifier: MIT

// Tests of the conversion of Nintendo SPD's sounds (ntdlib/sequence_export/spd/convert.h), on sound data made up with
// ntdlib/testing/spd_files.h. The expected values follow convert.h's mapping of the sound unit's registers: a MIDI tick
// for each frame, a sample's root key at the timer that its header gives, and the sample's rate from that timer. The
// local tool check_spd_player checks the model's register writes against the game's driver code.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sequence_export/spd/convert.h"
#include "ntdlib/spd/player.h"
#include "ntdlib/spd/song.h"
#include "ntdlib/testing/spd_files.h"

using xsflib::testing::Check;

namespace
{

namespace spd = ntdlib::spd;
namespace convert = ntdlib::sequence_export::spd;
using ntdlib::testing::SpdBlock;
using ntdlib::testing::SpdBlockBytes;
using ntdlib::testing::SpdEvent;
using ntdlib::testing::SpdSoundData;

// Bend range 2, no attack and no decay, a sustain at full level and no release.
constexpr uint32_t kEnvelope = 0x01F80002;

// The timer of the made-up sample at its root key: 16,756,991 Hz over 761 is 22,020 Hz.
constexpr uint16_t kSampleTimer = 761;

SpdBlock MakeBlock(std::vector<uint16_t> track, uint8_t root_key = 60)
{
    SpdBlock block;
    block.samples.push_back({0x08, root_key, kSampleTimer, 1, 2});
    block.programs.push_back({{0, kEnvelope, 3}});
    block.program_maps.push_back({0});
    block.sounds.push_back({0x7F0, {std::move(track)}});

    return block;
}

convert::Conversion Convert(const std::vector<uint8_t>& file, const spd::Block& block)
{
    convert::SoundData data;
    data.header = file;
    data.set_a = &block;

    return convert::ConvertSequence(data, 0, 1, "test", {}, "test");
}

void TestNote()
{
    const std::vector<uint16_t> track = {
        SpdEvent(0xF0, 160), SpdEvent(0xF2, 0), SpdEvent(0xF1, 0), SpdEvent(60, 127), 8,
        SpdEvent(0x80, 8),   SpdEvent(0xFF, 0)};
    const std::vector<uint8_t> block_bytes = SpdBlockBytes(MakeBlock(track));
    const std::vector<uint8_t> file = SpdSoundData({block_bytes});
    const spd::Block block(block_bytes);

    const convert::Conversion conversion = Convert(file, block);

    Check(seqlib::midi::Problems(conversion.midi).empty(), "the MIDI file is well formed");
    const seqlib::midi::File midi = seqlib::midi::Read(conversion.midi);
    const std::vector<seqlib::midi::Tempo> tempos = seqlib::midi::Tempos(midi);
    Check(tempos.size() == 1 &&
              tempos[0].micros_per_quarter == static_cast<uint32_t>(std::lround(48 * spd::kFrameSeconds * 1e6)),
          "tempo 160: a quarter note holds the 48 frames of 48 ticks");
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(midi);
    Check(notes.size() == 1 && notes[0].on == 0 && notes[0].key == 60, "the root key at the sample's timer");
    Check(!conversion.performance.loop, "a sound that ends has no loop");

    Check(seqlib::sf2::Check(conversion.sf2).empty(), "the SoundFont is well formed");
    const seqlib::sf2::Records records = seqlib::sf2::Read(conversion.sf2);
    Check(records.Count("shdr", seqlib::sf2::kSampleHeaderSize) == 2, "one sample, and the end");
    const uint8_t* header = records.Record("shdr", seqlib::sf2::kSampleHeaderSize, 0);
    Check(xsflib::LoadLe32(header + 36) == 22020 && header[40] == 60, "the sample's rate and root key");
}

// A sample whose root key isn't 60: the zone plays the sample at its root key.
void TestRootKey()
{
    const std::vector<uint16_t> track = {
        SpdEvent(0xF0, 160), SpdEvent(0xF2, 0), SpdEvent(0xF1, 0), SpdEvent(72, 127), 8,
        SpdEvent(0x80, 8),   SpdEvent(0xFF, 0)};
    const std::vector<uint8_t> block_bytes = SpdBlockBytes(MakeBlock(track, 72));
    const std::vector<uint8_t> file = SpdSoundData({block_bytes});
    const spd::Block block(block_bytes);

    const convert::Conversion conversion = Convert(file, block);

    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(seqlib::midi::Read(conversion.midi));
    Check(notes.size() == 1 && notes[0].key == 72, "the root key at the sample's timer");
    const seqlib::sf2::Records records = seqlib::sf2::Read(conversion.sf2);
    Check(records.Record("shdr", seqlib::sf2::kSampleHeaderSize, 0)[40] == 72, "the sample's root key");
    bool root_key = false;
    for (std::size_t i = 0; i < records.Count("igen", 4); i++)
    {
        const uint8_t* generator = records.Record("igen", 4, i);
        if (xsflib::LoadLe16(generator) == 58)
        {
            root_key = xsflib::LoadLe16(generator + 2) == 72;
        }
    }
    Check(root_key, "the zone's root key is the sample's");
}

void TestLoop()
{
    const std::vector<uint16_t> track = {SpdEvent(0xF0, 160), SpdEvent(0xF2, 0), SpdEvent(0xF1, 0),
                                         SpdEvent(0xF8, 0),   SpdEvent(60, 127), 4,
                                         SpdEvent(0x80, 8),   SpdEvent(0xF9, 0)};
    const std::vector<uint8_t> block_bytes = SpdBlockBytes(MakeBlock(track));
    const std::vector<uint8_t> file = SpdSoundData({block_bytes});
    const spd::Block block(block_bytes);

    const convert::Conversion conversion = Convert(file, block);

    const seqlib::midi::File midi = seqlib::midi::Read(conversion.midi);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);
    Check(markers.size() == 2 && markers[0].text == "loopStart" && markers[0].tick == 0 &&
              markers[1].text == "loopEnd" && markers[1].tick == 8 * midi.division / 48,
          "the loop runs from the frame of 0xF8 to the frame of 0xF9");
    Check(seqlib::midi::Notes(midi).size() == 1, "one pass");
}

// The capture reverb's level goes to each track as CC91, and 0 while the reverb is off.
void TestReverb()
{
    const std::vector<uint16_t> track = {
        SpdEvent(0xF0, 160), SpdEvent(0xF2, 0), SpdEvent(0xF1, 0), SpdEvent(60, 127), 8,
        SpdEvent(0x80, 8),   SpdEvent(0xFF, 0)};
    const std::vector<uint8_t> block_bytes = SpdBlockBytes(MakeBlock(track));
    const spd::Block block(block_bytes);
    const auto sends = [&](uint8_t level)
    {
        std::vector<uint8_t> file = SpdSoundData({block_bytes});
        file[6] = level; // the reverb in the header. The driver sets it at start-up.
        const seqlib::midi::File midi = seqlib::midi::Read(Convert(file, block).midi);
        std::vector<int> values;
        for (const seqlib::midi::Track& t : midi.tracks)
        {
            for (const seqlib::midi::Event& event : t.events)
            {
                if (event.bytes.size() == 3 && (event.bytes[0] & 0xF0) == 0xB0 && event.bytes[1] == 91)
                {
                    values.push_back(event.bytes[2]);
                }
            }
        }
        return values;
    };

    const std::vector<int> wet = sends(40);
    const std::vector<int> dry = sends(0);
    Check(!wet.empty() && std::ranges::all_of(wet, [](int v) { return v == 40; }), "CC91 is the reverb's level");
    Check(!dry.empty() && std::ranges::all_of(dry, [](int v) { return v == 0; }), "CC91 is 0 without the reverb");
}

} // namespace

int main()
{
    TestNote();
    TestRootKey();
    TestLoop();
    TestReverb();

    return xsflib::testing::Summary("spd_convert");
}
