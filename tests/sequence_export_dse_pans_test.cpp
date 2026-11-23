// SPDX-License-Identifier: MIT

// Tests of the pans of the DSE conversion (ntdlib/sequence_export/dse): a change of pan in the oldest build, and the
// files for a mixer that pans linearly (PerformOptions::linear_pans).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/testing/dse_song_files.h"
#include "sequence_export_dse_cases.h"

namespace
{

using namespace sequence_export_dse_cases;

// The oldest build's writer never pans a sounding voice again (0x020C4DDC in Luminous Arc's ARM9 binary). Track 1 plays
// a note for 48 ticks at pan 0x40, then after 12 ticks sets pan 0x10 and plays a note for 12 ticks. In the oldest
// build, the second note takes a second part at the new pan, and the first part keeps its pan. The later build pans
// both notes on one part.
void TestOldestPan()
{
    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(
        files::SongFile({files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
                         files::Join({{1, 1, 0, 0},
                                      {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                      files::Note(0x7F, 0, 0, 48, 1),
                                      {0x92, 0x0C, 0xE8, 0x10},
                                      files::Note(0x7F, 4, 0, 12, 1),
                                      {0x92, 0x30, 0x98}})}));
    SequenceFiles files = test.Song();
    files.song = &song;
    const Performance oldest = Perform(files, {.build = ntdlib::dse::Build::kOldest});
    const Performance later = Perform(files);

    const auto values = [](const Performance& p, EventKind kind, uint8_t part)
    {
        std::set<uint8_t> found;
        for (const TrackEvent& e : EventsOf(p, 1, kind))
        {
            if (e.part == part)
            {
                found.insert(e.value);
            }
        }
        return found;
    };

    const auto note_parts = [](const Performance& p)
    {
        std::set<uint8_t> parts;
        for (const TrackEvent& e : EventsOf(p, 1, EventKind::kNoteOn))
        {
            parts.insert(e.part);
        }
        return parts;
    };

    const std::set<uint8_t> first = values(oldest, EventKind::kPan, 0);
    const std::set<uint8_t> second = values(oldest, EventKind::kPan, 1);
    Check(note_parts(oldest) == std::set<uint8_t>{0, 1} && first.size() == 1 && second.size() == 1 && first != second,
          "the oldest build: the second note on a second part, and the first part keeps its pan");
    Check(note_parts(later) == std::set<uint8_t>{0} && values(later, EventKind::kPan, 0).size() == 2,
          "the later build pans both notes on one part");
}

// The pan and attenuation generators of the zones of `sf2` that play the sample named `sample`, in the order of the
// instruments' zones. A generator that a zone leaves out is 0.
std::vector<std::pair<int, int>> PansAndAttenuations(const std::vector<uint8_t>& sf2, const std::string& sample)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<std::pair<int, int>> zones;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(z);
        const auto id = gens.find(seqlib::sf2::kSampleId);
        if (id == gens.end() ||
            std::string(reinterpret_cast<const char*>(records.Record("shdr", 46, id->second))) != sample)
        {
            continue;
        }

        const auto gen = [&](uint16_t op)
        {
            const auto found = gens.find(op);
            return found != gens.end() ? static_cast<int16_t>(found->second) : 0;
        };
        zones.emplace_back(gen(seqlib::sf2::kPan), gen(seqlib::sf2::kInitialAttenuation));
    }

    return zones;
}

// A sound effect for a mixer that pans linearly (PerformOptions::linear_pans). The track sets the channel pan to `pan`.
// It plays key 48 of program 0, whose split is at the centre, then key 48 of program 1, whose splits have pans of 0x20
// and 0x60. The DSE driver gives a voice the sum of the split pan and the channel pan, less 0x40 (dse::Driver, from the
// game's code). The sound unit of the DS plays a pan of p at (128 - p) / 128 on the left and p / 128 on the right.
// NITRO-Composer sums the pans of a track and a region in the same way. A file for such a mixer therefore keeps both
// pans as they are, and no level of a pan. CC10 contains the channel pan. The pan generator of a zone contains the
// position that CC10 gives the split pan by RP-036 (500 (p - 64) / 63). CC11 and the attenuations of the zones contain
// no gain of a pan and no cut.
Conversion ConvertPannedEffect(const TestFiles& test, uint8_t pan, bool linear_pans)
{
    const Bytes track = files::Join({{1, 0, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, pan},
                                     files::Note(0x7F, 0, 0, 0x18, 1),
                                     {0x92, 0x18, 0xAC, 0x01},
                                     files::Note(0x7F, 0, 0, 0x18, 1),
                                     {0x92, 0x18, 0x98}});
    const ntdlib::dse::SoundEffects effects =
        ntdlib::dse::SoundEffects::Parse(files::EffectsFile({files::Effect{.tracks = {track}}}, 5));
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;

    return ConvertSequence(effect, "fx_000", {.linear_pans = linear_pans}, kProgram);
}

void TestLinearPans()
{
    const TestFiles test;
    const Conversion side = ConvertPannedEffect(test, 0x20, true);
    const Conversion centre = ConvertPannedEffect(test, 0x40, true);
    const Conversion sin_cos_side = ConvertPannedEffect(test, 0x20, false);
    const Conversion sin_cos_centre = ConvertPannedEffect(test, 0x40, false);

    // The MIDI file: CC10 contains the channel pan, and CC11 doesn't follow it. With the sin/cos law, CC11 at the
    // centre takes the level of the centre against the level of a side.
    const auto values = [](const Conversion& c, EventKind kind)
    {
        std::set<uint8_t> found;
        for (const TrackEvent& e : EventsOf(c.performance, 0, kind))
        {
            found.insert(e.value);
        }
        return found;
    };
    Check(values(side, EventKind::kPan) == std::set<uint8_t>{0x20} &&
              values(centre, EventKind::kPan) == std::set<uint8_t>{0x40},
          "linear pans: CC10 holds the channel's pan");
    Check(values(sin_cos_side, EventKind::kPan) == std::set<uint8_t>{ntdlib::sequence_export::PanController(0x20)},
          "by default, CC10 holds the pan's position for the sin/cos law");
    Check(!values(side, EventKind::kExpression).empty() &&
              values(side, EventKind::kExpression) == values(centre, EventKind::kExpression),
          "linear pans: CC11 takes no level of the channel's pan");
    Check(values(sin_cos_centre, EventKind::kExpression) != values(centre, EventKind::kExpression),
          "by default, CC11 takes the level of the channel's pan");

    // The SoundFont: each zone at its split's pan and at the square of its volume, without a cut. With the sin/cos law,
    // a zone at the centre takes the cut.
    const auto attenuation = [&test](uint16_t program, std::size_t split, double cut)
    {
        const std::vector<PresetZone> zones = PresetZones(test.bank, {.bank = files::kBankId, .program = program});
        const double volume = zones.size() > split ? zones[split].volume / 127.0 : 1.0;
        return static_cast<int>(std::lround(2.5 * (cut - 200.0 * std::log10(volume * volume))));
    };
    const auto all_at = [](const std::vector<std::pair<int, int>>& zones, std::pair<int, int> expected)
    {
        return !zones.empty() && std::ranges::all_of(zones, [&](const auto& z) { return z == expected; });
    };
    Check(all_at(PansAndAttenuations(side.sf2, "w32512_0"), {0, attenuation(0, 0, 0.0)}),
          "linear pans: the centre's zone at its volume");
    Check(all_at(PansAndAttenuations(side.sf2, "w32512_1"), {-254, attenuation(1, 0, 0.0)}),
          "linear pans: pan 0x20 at -254, at its volume");
    Check(all_at(PansAndAttenuations(side.sf2, "w32512_2"), {254, attenuation(1, 1, 0.0)}),
          "linear pans: pan 0x60 at 254, at its volume");
    Check(all_at(PansAndAttenuations(sin_cos_centre.sf2, "w32512_0"),
                 {0, attenuation(0, 0, ntdlib::sequence_export::PanCut())}),
          "by default, the centre's zone takes the cut");

    Check(side.sfz.files.empty() && !sin_cos_side.sfz.files.empty(), "linear pans: no SFZ files");
    CheckFiles(sin_cos_side, "the effect with the sin/cos law");
}

} // namespace

int main()
{
    TestOldestPan();
    TestLinearPans();

    return xsflib::testing::Summary("DSE conversion: pans");
}
