// SPDX-License-Identifier: MIT

// Tests of the DS performance's records for an editor (ntdlib/sequence_export/ntr/performance.h): each track's
// stretches of commands, each note's command, each note's channel, the notes that get no channel, and each track's
// changes of its volume and pan. The archives come from sdat::BuildArchive. The expected values are worked out by hand
// from the driver's commands: a note waits for its length while the track's note wait is on, as it is when a track
// opens. A call's return goes on after the call, and a loop's end goes back to the command after its start until its
// count runs out. A note takes the channel of the lowest priority that its track may take, when its priority is as
// high, and otherwise gets none.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/sdat_files.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace sdat = ntdlib::sdat;
using namespace ntdlib::sequence_export::ntr;
using files::Bytes;
using ntdlib::sequence_export::Entry;
using ntdlib::sequence_export::Mix;
using ntdlib::sequence_export::MixChange;
using ntdlib::sequence_export::NoteCommand;
using ntdlib::sequence_export::Stretch;
using xsflib::testing::Check;

constexpr uint16_t kNone = 0xFFFF;

// An archive whose sequence 0 plays `bytecode` on a bank whose program 0 plays a PCM8 square that loops, with release
// `release`. The player gives its sequences the channels of `channels`, or any for 0.
Bytes ArchiveOf(const Bytes& bytecode, uint16_t channels = 0, uint8_t release = 127)
{
    const files::Wave square{.loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, channels, 0, ""}};
    model.files = {files::Sequence(bytecode), files::Bank({{.type = 1, .region = {.wave = 0, .release = release}}}),
                   files::WaveArchive({square})};

    return sdat::BuildArchive(model);
}

Performance PerformBytecode(const Bytes& bytecode, uint16_t channels = 0, uint8_t release = 127)
{
    const Bytes bytes = ArchiveOf(bytecode, channels, release);
    const sdat::Archive archive = sdat::Archive::Parse(bytes);

    return Perform(archive, ntdlib::ntr::SequenceRef{0});
}

Stretch StretchOf(uint32_t tick, uint32_t first, uint32_t last, Entry entry, uint8_t depth)
{
    Stretch stretch;
    stretch.tick = tick;
    stretch.first = first;
    stretch.last = last;
    stretch.entry = entry;
    stretch.depth = depth;
    stretch.note_wait = true;

    return stretch;
}

// A volume and a pan at tick 0, a call of a subroutine that plays note 62 and returns, then note 60 and the end:
//
//    0: C1 64        volume 100
//    2: C0 20        pan 0x20: 32 to the left of the centre
//    4: 95 0C 00 00  call 12
//    8: 3C 7F 18     note 60 for 24 ticks
//   11: FF           end
//   12: 3E 7F 18     note 62 for 24 ticks
//   15: FD           return
//
// The subroutine's note plays at tick 0 and waits 24 ticks. The return then runs at tick 24, and note 60 plays there.
void TestCallStretches()
{
    const Performance performance = PerformBytecode(
        {0xC1, 0x64, 0xC0, 0x20, 0x95, 0x0C, 0x00, 0x00, 0x3C, 0x7F, 0x18, 0xFF, 0x3E, 0x7F, 0x18, 0xFD});

    const std::vector<Stretch> expected = {
        StretchOf(0, 0, 4, Entry::kOpen, 0),
        StretchOf(0, 12, 15, Entry::kCall, 1),
        StretchOf(24, 8, 11, Entry::kReturn, 0),
    };
    Check(performance.stretches[0] == expected, "the call's stretches");
    for (std::size_t t = 1; t < performance.stretches.size(); t++)
    {
        Check(performance.stretches[t].empty(), "no other track runs commands");
    }

    // The notes' IDs follow the order of their note-ons: note 62 first.
    Check(performance.note_commands == std::vector<NoteCommand>{{12, 1}, {8, 2}}, "the notes' commands");
    uint32_t notes = 0;
    for (const auto& event : performance.tracks[0])
    {
        if (event.kind == seqlib::midi::EventKind::kNoteOn)
        {
            notes++;
            const NoteCommand& command = performance.note_commands[event.note - 1];
            Check(event.key == (command.offset == 12 ? 62 : 60), "each note leads to its command");
            Check(event.tick == (command.offset == 12 ? 0u : 24u), "each note at its tick");
        }
    }
    Check(notes == 2, "two notes");

    const std::vector<MixChange> mix = {{0, Mix::kVolume, 100}, {0, Mix::kPan, -32}};
    Check(performance.mix[0] == mix, "the volume and the pan");
}

// A loop of two passes around note 60, then the end:
//
//   0: D4 02     loop start, 2 passes
//   2: 3C 7F 18  note 60 for 24 ticks
//   5: FC        loop end
//   6: FF        end
//
// The second pass begins at tick 24 with the loop's end, inside the loop. The loop's end then leaves the loop, and the
// end follows it in the same stretch. The track keeps volume 127 and the centre.
void TestLoopStretches()
{
    const Performance performance = PerformBytecode({0xD4, 0x02, 0x3C, 0x7F, 0x18, 0xFC, 0xFF});

    const std::vector<Stretch> expected = {
        StretchOf(0, 0, 5, Entry::kOpen, 0),
        StretchOf(24, 2, 6, Entry::kLoop, 1),
    };
    Check(performance.stretches[0] == expected, "the loop's stretches");
    Check(performance.note_commands == std::vector<NoteCommand>{{2, 0}, {2, 1}}, "each pass's note");
    Check(performance.mix[0].empty(), "no change of the mix");
}

// A jump over a note to a note, and a note whose length a random prefix gives:
//
//    0: 94 07 00 00  jump 7
//    4: 3C 7F 18     note 60, never played
//    7: A0 3E 7F 18 00 18 00  note 62 for a random length from 24 to 24
//   14: FF           end
//
// The prefixed note's command starts at its prefix: at 7, the jump's target.
void TestJumpStretches()
{
    const Performance performance =
        PerformBytecode({0x94, 0x07, 0x00, 0x00, 0x3C, 0x7F, 0x18, 0xA0, 0x3E, 0x7F, 0x18, 0x00, 0x18, 0x00, 0xFF});

    const std::vector<Stretch> expected = {
        StretchOf(0, 0, 0, Entry::kOpen, 0),
        StretchOf(0, 7, 14, Entry::kJump, 0),
    };
    Check(performance.stretches[0] == expected, "the jump's stretches");
    Check(performance.note_commands == std::vector<NoteCommand>{{7, 1}}, "the prefixed note's command");
}

// Three notes on channel 4 alone, the first that the driver's order gives:
//
//    0: C7 00     note wait off
//    2: 3C 7F 30  note 60 for 48 ticks
//    5: 80 18     wait 24
//    7: 3E 7F 18  note 62 for 24 ticks
//   10: C6 00     priority 0
//   12: 40 7F 18  note 64 for 24 ticks
//   15: 80 30     wait 48
//   17: FF        end
//
// The driver takes the channel of the lowest priority for a note whose priority is as high: the player's priority plus
// the track's. Note 60 takes the free channel at tick 0. Note 62 takes it from note 60 at tick 24, at the same
// priority, and note 60 ends there. The track's priority then falls by 64, and note 64 gets no channel.
void TestChannels()
{
    const Performance performance = PerformBytecode(
        {0xC7, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x18, 0x3E, 0x7F, 0x18, 0xC6, 0x00, 0x40, 0x7F, 0x18, 0x80, 0x30, 0xFF},
        0x0010);

    Check(performance.note_commands == std::vector<NoteCommand>{{2, 0}, {7, 0}}, "the started notes' commands");
    Check(performance.note_channels == std::vector<NoteChannel>{{4, 24}, {4, std::nullopt}},
          "the notes' channel and cut");
    Check(performance.unstarted ==
              std::vector<UnstartedNote>{
                  {.tick = 24, .length = 24, .track = 0, .key = 64, .velocity = 127, .offset = 12}},
          "the note without a channel");

    uint32_t notes = 0;
    for (const auto& event : performance.tracks[0])
    {
        if (event.kind == seqlib::midi::EventKind::kNoteOff)
        {
            notes++;
            Check(event.tick == 24u + (event.key == 62 ? 24u : 0u), "note 60 ends where note 62 takes its channel");
        }
    }
    Check(notes == 2, "two MIDI notes");
}

// Two notes in turn on channel 4 alone, with the note wait on:
//
//   0: 3C 7F 18  note 60 for 24 ticks
//   3: 3E 7F 18  note 62 for 24 ticks
//   6: FF        end
//
// Note 62 takes the channel at tick 24, before the driver releases note 60. Note 60's length has run out there, and no
// cut shortens it.
void TestChannelAtLengthEnd()
{
    const Performance performance = PerformBytecode({0x3C, 0x7F, 0x18, 0x3E, 0x7F, 0x18, 0xFF}, 0x0010);

    Check(performance.note_channels == std::vector<NoteChannel>{{4, std::nullopt}, {4, std::nullopt}},
          "two notes on channel 4 in turn");
    Check(performance.unstarted.empty(), "every note starts");
}

// Two notes at tick 0 on channel 4 alone, with the note wait off:
//
//    0: C7 00     note wait off
//    2: 3C 7F 30  note 60 for 48 ticks
//    5: 3E 7F 18  note 62 for 24 ticks
//    8: 80 30     wait 48
//   10: FF        end
//
// Note 62 takes the channel from note 60 in the same tick, at the same priority. Note 60's cut is then at tick 0, and
// note 62 has no cut.
void TestCutAtFirstTick()
{
    const Performance performance =
        PerformBytecode({0xC7, 0x00, 0x3C, 0x7F, 0x30, 0x3E, 0x7F, 0x18, 0x80, 0x30, 0xFF}, 0x0010);

    Check(performance.note_channels == std::vector<NoteChannel>{{4, 0}, {4, std::nullopt}},
          "a cut at tick 0, and no cut");
    Check(performance.unstarted.empty(), "both notes start");
}

// Two notes in turn on channel 4 alone, on an instrument whose release is 0xFF:
//
//   0: 3C 7F 18  note 60 for 24 ticks
//   3: 80 18     wait 24
//   5: 3E 7F 18  note 62 for 24 ticks
//   8: FF        end
//
// The driver gives the channel of such an instrument no length. Note 60 then plays its looping square past its 24
// ticks. Note 62 takes the channel at tick 48 and cuts note 60 there.
void TestCutPastTheCommandsLength()
{
    const Performance performance =
        PerformBytecode({0x3C, 0x7F, 0x18, 0x80, 0x18, 0x3E, 0x7F, 0x18, 0xFF}, 0x0010, 0xFF);

    Check(performance.note_channels == std::vector<NoteChannel>{{4, 48}, {4, std::nullopt}},
          "a cut of a note whose channel has no length");
    uint32_t first_end = 0;
    for (const auto& event : performance.tracks[0])
    {
        if (event.kind == seqlib::midi::EventKind::kNoteOff && event.key == 60)
        {
            first_end = event.tick;
        }
    }
    Check(first_end == 48, "note 60 ends where note 62 takes its channel");
}

} // namespace

int main()
{
    TestCallStretches();
    TestLoopStretches();
    TestJumpStretches();
    TestChannels();
    TestChannelAtLengthEnd();
    TestCutAtFirstTick();
    TestCutPastTheCommandsLength();

    return xsflib::testing::Summary("DS performance for an editor");
}
