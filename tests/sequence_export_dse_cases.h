// SPDX-License-Identifier: MIT

// The files, the sound effects and the helpers that the tests of DSE's conversion (ntdlib/sequence_export/dse)
// share, on the files of dse_song_files.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/testing/dse_files.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/sequence_export_check.h"

namespace sequence_export_dse_cases
{

namespace dse_files = ntdlib::testing::dse_files;
namespace files = ntdlib::testing::dse_song_files;
namespace sets = ntdlib::dse;
using namespace ntdlib::sequence_export::dse;
using dse_files::Bytes;
using xsflib::testing::Check;

// The name that the conversions' files give the program.
constexpr char kProgram[] = "xyz2midi";

// A tick of the driver in milliseconds: 5236 * 64 cycles of the system clock of 33,513,982 Hz. The game's settings give
// the tick of 10 ms (kTenMs).
constexpr double kTickMs = 1000.0 * 5236 * 64 / 33513982.0;
constexpr int kTenMs = ntdlib::dse::Driver::kTickMicroseconds;

// The test's song, its bank and the main bank, read.
struct TestFiles
{
    ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile());
    ntdlib::dse::WaveBank bank = ntdlib::dse::WaveBank::Parse(files::BankFile());
    ntdlib::dse::WaveBank main_bank = ntdlib::dse::WaveBank::Parse(files::MainBankFile());

    SequenceFiles Song() const
    {
        return {.song = &song, .banks = {&bank}, .main_bank = &main_bank};
    }
};

// A track of a sound effect: notes 48 and 52 of program 0 for 24 ticks each, at velocity 0x60.
inline Bytes EffectTrack()
{
    return files::Join({{1, 0, 0, 0},
                        {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                        files::Note(0x60, 0, 0, 0x18, 1),
                        {0x92, 0x18},
                        files::Note(0x60, 4, 0),
                        {0x92, 0x18, 0x98}});
}

// A bank of sound effects of id 5. Slot 0 is empty. Effect 1 plays EffectTrack on the song's bank. Effect 2 lists a
// bank that no file holds.
inline Bytes EffectsFile()
{
    return files::EffectsFile({std::nullopt, files::Effect{.tracks = {EffectTrack()}},
                               files::Effect{.tracks = {EffectTrack()}, .banks = {0x1234}}},
                              5);
}

// A track's events of `kind`.
inline std::vector<TrackEvent> EventsOf(const Performance& p, std::size_t track, EventKind kind)
{
    std::vector<TrackEvent> events;
    for (const TrackEvent& e : p.tracks.at(track))
    {
        if (e.kind == kind)
        {
            events.push_back(e);
        }
    }
    return events;
}

inline void CheckFiles(const Conversion& c, const std::string& name)
{
    const std::vector<std::string> midi = seqlib::midi::Problems(c.midi);
    const std::vector<std::string> sf2 = seqlib::sf2::Check(c.sf2);
    const std::string sfz =
        midi.empty() ? ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(seqlib::midi::Read(c.midi))) : "";
    Check(midi.empty() && sf2.empty() && sfz.empty(),
          name + ": seqlib's checks: " + (midi.empty() ? "" : midi[0]) + (sf2.empty() ? "" : sf2[0]) + sfz);
}

// A sound effect of `track` on `test`'s banks, converted.
inline Conversion ConvertEffect(const TestFiles& test, const Bytes& track)
{
    const ntdlib::dse::SoundEffects effects =
        ntdlib::dse::SoundEffects::Parse(files::EffectsFile({files::Effect{.tracks = {track}}}, 5));
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    return ConvertSequence(effect, "fx_000", {}, kProgram);
}

// The ticks and keys of a track's note-offs.
inline std::vector<std::pair<uint32_t, int>> NoteOffs(const Performance& p, std::size_t track)
{
    std::vector<std::pair<uint32_t, int>> offs;
    for (const TrackEvent& e : EventsOf(p, track, EventKind::kNoteOff))
    {
        offs.emplace_back(e.tick, e.key);
    }
    return offs;
}

constexpr char kCut[] = "note cut short by another note's voice (rings on in its release)";

} // namespace sequence_export_dse_cases
