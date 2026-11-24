// SPDX-License-Identifier: MIT

// Tests of sequence_export on synthetic archives: pieces of several sequences, and the companions that play beside
// them. Each test runs on every console whose engine it describes, with the settings of that console (see
// sequence_export_test.h).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/encoder.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/arrangements.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_pieces.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;
using xsflib::testing::ErrorOf;

namespace sf2 = seqlib::sf2;

namespace
{

// An archive of the sequences `commands`, labelled A, B, C and so on, that play one bank. A sequence after the first
// has the pan mode of `second_pan_mode`.
TestArchive PieceArchive(const std::vector<std::vector<uint8_t>>& commands, uint8_t second_pan_mode = 0)
{
    std::vector<ArchiveFile> files;
    ArchiveLists lists;
    const std::vector<uint32_t> kBanks = {0x03000000};
    for (std::size_t k = 0; k < commands.size(); k++)
    {
        files.push_back({Fseq(commands[k])});
        lists.sequences.push_back(
            {.file_id = static_cast<uint32_t>(k), .banks = kBanks, .pan_mode = k == 0 ? uint8_t{0} : second_pan_mode});
        lists.names.push_back(std::string(1, static_cast<char>('A' + k)));
    }
    files.push_back({OneRegionFbnk({{0x05000000, 0}})});
    files.push_back({Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)})});
    lists.banks = {static_cast<uint32_t>(commands.size())};
    lists.wave_archives = {static_cast<uint32_t>(commands.size() + 1)};

    return LoadTestArchive(Fsar(files, lists));
}

// Performs sounds `indices` of `archive` together, as one piece (PerformTogether), with the banks of the first.
Performance PerformPiece(const TestArchive& archive, const std::vector<uint32_t>& indices)
{
    if (archive.rsar_archive)
    {
        return WiiPerformPiece(*archive.rsar_archive, indices);
    }

    BankSet banks(*archive.sar_archive, archive.sar_archive->Sounds().at(0), console->settings);
    return PerformTogether(*archive.sar_archive, indices, banks, {}, console->settings);
}

// Converts sounds `indices` of `archive` together, as the piece `name` (ConvertTogether).
Conversion ConvertPiece(const TestArchive& archive, const std::vector<uint32_t>& indices, const std::string& name)
{
    if (archive.rsar_archive)
    {
        return WiiConvertPiece(*archive.rsar_archive, indices, name);
    }

    return ConvertTogether(*archive.sar_archive, indices, name, {}, console->settings, console->program);
}

// Converts the sound or the piece that `at` picks in `archive`, with `pieces` (ConvertSoundOrPiece).
Conversion ConvertArranged(const TestArchive& archive, const ArrangedSound& at,
                           const std::vector<ntdlib::SoundPiece>& pieces)
{
    if (archive.rsar_archive)
    {
        return WiiConvertArranged(*archive.rsar_archive, at, pieces);
    }

    return ConvertSoundOrPiece(*archive.sar_archive, at, pieces, {}, console->settings, console->program);
}

// A plays key 60 on track 0. B waits a tick and plays key 64. C plays nothing.
const std::vector<std::vector<uint8_t>> kPieceOfTwoNotes = {
    {0x3c, 0x64, 0x30, 0xff},
    {0x80, 0x01, 0x40, 0x64, 0x30, 0xff},
    {0x80, 0x30, 0xff},
};

// The ticks and keys of the notes that track `t` of `p` plays.
std::vector<std::pair<uint32_t, int>> NoteStarts(const Performance& p, int t)
{
    std::vector<std::pair<uint32_t, int>> notes;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            notes.emplace_back(e.tick, e.key);
        }
    }

    return notes;
}

void TestPieceOfSequencesKeepsTheirTracksWithNotes()
{
    // The piece holds A's track 0 and B's, in that order, named after their sequences and tracks. C plays no note and
    // adds no track. Both notes play the bank's instrument with the same settings. They share one preset. B's note
    // keeps its tick.
    const TestArchive archive = PieceArchive(kPieceOfTwoNotes);
    const std::vector<uint32_t> kAll = {0, 1, 2};

    const Performance p = PerformPiece(archive, kAll);

    const std::vector<std::pair<uint32_t, int>> kFirst = {{0, 60}};
    const std::vector<std::pair<uint32_t, int>> kSecond = {{1, 64}};
    XSFLIB_CHECK(NoteStarts(p, 0) == kFirst && NoteStarts(p, 1) == kSecond && p.tracks[2].empty());
    XSFLIB_CHECK(p.track_names[0] == "A Track 0" && p.track_names[1] == "B Track 0" && p.track_names[2].empty());
    XSFLIB_CHECK(p.presets.size() == 1 && !p.loop && p.warnings.empty());
}

void TestPieceOfSequencesConvertsToOneSetOfFiles()
{
    // The piece's MIDI tracks and SFZ files take the names of its tracks. The SoundFont's comment lists the sequences
    // and the banks. A sequence converted alone keeps its comment.
    const TestArchive archive = PieceArchive(kPieceOfTwoNotes);
    const std::vector<uint32_t> kTwo = {0, 1};

    const Conversion c = ConvertPiece(archive, kTwo, "Piece");
    const Conversion alone = ConvertSound(archive, 0);

    const std::vector<std::string> tracks = seqlib::midi::TrackNames(seqlib::midi::Read(c.midi));
    std::vector<std::string> files;
    for (const SfzFile& f : c.sfz.files)
    {
        files.push_back(f.name);
    }
    const auto comment = [](const Conversion& conversion)
    {
        const seqlib::sf2::Records records = seqlib::sf2::Read(conversion.sf2);
        const auto found = records.chunks.find("ICMT");
        return found == records.chunks.end() ? std::string() : std::string(found->second.begin(), found->second.end());
    };
    const std::vector<std::string> kTracks = {"Piece", "A Track 0", "B Track 0"};
    const std::vector<std::string> kFiles = {"A Track 0.sfz", "B Track 0.sfz"};
    XSFLIB_CHECK(ntdlib::testing::CheckConversion(c, "Piece") == 0 && c.channels == 2);
    XSFLIB_CHECK(tracks == kTracks && files == kFiles);
    XSFLIB_CHECK(
        comment(c).starts_with("Converted by " + console->program + " from sequences A and B together. Banks:"));
    XSFLIB_CHECK(comment(alone).starts_with("Converted by " + console->program + " from sequence A. Banks:"));
}

void TestSoundThatStartsAPieceConvertsWithThePiece()
{
    // A and B form a piece. A converts together with B. B starts no piece and converts alone. An arrangement of A
    // converts alone too.
    const TestArchive archive = PieceArchive(kPieceOfTwoNotes);
    const std::vector<ntdlib::SoundPiece> pieces = {{"Piece", {0, 1}}};
    const std::vector<uint32_t> kTwo = {0, 1};
    const auto convert = [&archive, &pieces](const ArrangedSound& at)
    {
        return ConvertArranged(archive, at, pieces);
    };

    const Conversion first = convert({0, std::nullopt});
    const Conversion second = convert({1, std::nullopt});
    const Conversion arranged = convert({0, 0});

    const Conversion together = ConvertPiece(archive, kTwo, "Piece");
    const Conversion a = ConvertSound(archive, 0);
    const Conversion b = ConvertSound(archive, 1);
    XSFLIB_CHECK(together.midi != a.midi && together.sf2 != a.sf2);
    XSFLIB_CHECK(first.midi == together.midi && first.sf2 == together.sf2);
    XSFLIB_CHECK(second.midi == b.midi && second.sf2 == b.sf2);
    XSFLIB_CHECK(arranged.midi == a.midi && arranged.sf2 == a.sf2);
}

void TestPieceOfSequencesRequiresOneBankListAndPanMode()
{
    // A and B play one bank, but B's sound places stereo waves in another pan mode. A piece also requires a sequence.
    const TestArchive archive = PieceArchive({{0x3c, 0x64, 0x30, 0xff}, {0x3c, 0x64, 0x30, 0xff}}, 1);
    const auto message = [&archive](const std::vector<uint32_t>& indices)
    {
        return ErrorOf<std::invalid_argument>([&] { PerformPiece(archive, indices); });
    };

    XSFLIB_CHECK(message({0, 1}) == "A and B differ in banks or pan mode");
    XSFLIB_CHECK(message({}) == "no sequences to perform together");
}

// An archive of the one sequence `commands`, labelled `label`, that plays one bank of one wave.
SoundArchive OneSequenceArchive(const std::string& label, const std::vector<uint8_t>& commands)
{
    const std::vector<ArchiveFile> files = {{Fseq(commands)},
                                            {OneRegionFbnk({{0x05000000, 0}})},
                                            {Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)})}};
    ArchiveLists lists;
    lists.sequences.push_back({.file_id = 0, .banks = {0x03000000}});
    lists.names = {label};
    lists.banks = {1};
    lists.wave_archives = {2};

    return SoundArchive::Load(Fsar(files, lists), console->settings.archive);
}

// A companion, Ctl, plays key 50 and waits until the game sets 16 to 1. It then sets 17 to 5, the hand-over, and plays
// key 60 on each beat. Once 18 holds 7, key 72 joins it from the next beat on. Its notes don't wait.
std::vector<uint8_t> CompanionCode()
{
    std::vector<uint8_t> commands;
    Append(commands, {0xc7, 0x00});               // 0: note wait off
    Append(commands, {0x32, 0x64, 0x01});         // 2: key 50 for a tick
    Append(commands, {0xf0, 0x90, 0x10, U16(1)}); // 5: 16 == 1?
    Append(commands, {0xa2, 0x89, U24(21)});      // 10: if so, jump to 21
    Append(commands, {0x80, 0x01});               // 15: wait a tick
    Append(commands, {0x89, U24(5)});             // 17: jump to 5
    Append(commands, {0xf0, 0x80, 0x11, U16(5)}); // 21: 17 = 5
    Append(commands, {0x3c, 0x64, 0x30});         // 26: key 60 for a beat
    Append(commands, {0x80, 0x30});               // 29: wait a beat
    Append(commands, {0xf0, 0x90, 0x12, U16(7)}); // 31: 18 == 7?
    Append(commands, {0xa2, 0x48, 0x64, 0x30});   // 36: if so, key 72 for a beat
    Append(commands, {0x89, U24(26)});            // 40: jump to 26

    return commands;
}

// The piece sets 18 to 7. It plays key 64 for a beat and a half when 17 holds 5, and key 61 otherwise. Its notes don't
// wait.
std::vector<uint8_t> BesidePieceCode()
{
    std::vector<uint8_t> commands;
    Append(commands, {0xc7, 0x00});               // 0: note wait off
    Append(commands, {0xf0, 0x80, 0x12, U16(7)}); // 2: 18 = 7
    Append(commands, {0xf0, 0x90, 0x11, U16(5)}); // 7: 17 == 5?
    Append(commands, {0xa2, 0x89, U24(21)});      // 12: if so, jump to 21
    Append(commands, {0x3d, 0x64, 0x30});         // 17: key 61 for a beat
    Append(commands, {0xff});                     // 20: the end
    Append(commands, {0x40, 0x64, 0x48});         // 21: key 64 for a beat and a half
    Append(commands, {0x80, 0x48});               // 24: wait a beat and a half
    Append(commands, {0xff});                     // 26: the end

    return commands;
}

// The game writes 1 to 16 in Ctl's frame 3. Ctl hands over to the piece when it sets 17 to `hand_over_value`.
CompanionScript BesideScript(int16_t hand_over_value = 5)
{
    return {.writes = {{.frame = 3, .index = 16, .value = 1}}, .hand_over = 17, .hand_over_value = hand_over_value};
}

void TestCompanionPlaysBesideThePiece()
{
    // Ctl sees the game's write in its first tick after frame 3 and hands over. The piece starts in that frame, with 17
    // at 5, and plays key 64. Ctl's key 50 comes before the start and is left out. Its key 60 of the hand-over's tick
    // starts with the piece. Ctl reads the piece's 18 a frame later and adds key 72 on the next beat. Ctl's notes from
    // the piece's end on are left out. Ctl's preset plays the companion's slot of the bank.
    const SoundArchive archive = OneSequenceArchive("Piece", BesidePieceCode());
    const SoundArchive companion = OneSequenceArchive("Ctl", CompanionCode());
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    BankSet companion_banks(companion, companion.Sounds()[0], console->settings);

    const Performance p =
        PerformBeside(archive, 0, banks, companion, 0, companion_banks, BesideScript(), {}, console->settings);

    // Ctl's events go to the starts of their frames in the piece's ticks. On the 3DS, a frame of 4.89 ms starts a
    // fraction of a tick before the beat, as it does for the piece's events.
    std::vector<std::pair<uint32_t, int>> ctl;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            ctl.emplace_back(static_cast<uint32_t>(std::lround(e.tick + e.after)), e.key);
        }
    }
    std::ranges::sort(ctl);
    const std::vector<std::pair<uint32_t, int>> kPiece = {{0, 64}};
    const std::vector<std::pair<uint32_t, int>> kCtl = {{0, 60}, {48, 60}, {48, 72}};
    XSFLIB_CHECK(NoteStarts(p, 0) == kPiece && ctl == kCtl && p.tracks[2].empty());
    XSFLIB_CHECK(p.track_names[0] == "Piece Track 0" && p.track_names[1] == "Ctl Track 0");
    XSFLIB_CHECK(!p.loop && p.warnings.empty());
    XSFLIB_CHECK(p.presets.size() == 2 && p.presets[0].bank_slot == 0 &&
                 p.presets[1].bank_slot == BankSet::kCompanionSlot);
}

void TestCompanionTrackWithoutNotesIsLeftOut()
{
    // This companion plays key 50, hands over a tick later and ends. Its one note comes before the piece's start. The
    // performance then holds the piece's track alone.
    std::vector<uint8_t> commands;
    Append(commands, {0xc7, 0x00});               // 0: note wait off
    Append(commands, {0x32, 0x64, 0x01});         // 2: key 50 for a tick
    Append(commands, {0x80, 0x01});               // 5: wait a tick
    Append(commands, {0xf0, 0x80, 0x11, U16(5)}); // 7: 17 = 5
    Append(commands, {0xff});                     // 12: the end
    const SoundArchive archive = OneSequenceArchive("Piece", BesidePieceCode());
    const SoundArchive companion = OneSequenceArchive("Ctl", commands);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    BankSet companion_banks(companion, companion.Sounds()[0], console->settings);
    const CompanionScript script = {.hand_over = 17, .hand_over_value = 5};

    const Performance p =
        PerformBeside(archive, 0, banks, companion, 0, companion_banks, script, {}, console->settings);

    const std::vector<std::pair<uint32_t, int>> kPiece = {{0, 64}};
    XSFLIB_CHECK(NoteStarts(p, 0) == kPiece && p.tracks[1].empty() && p.track_names[1].empty());
    XSFLIB_CHECK(p.warnings.empty());
}

// A piece that sets 18 to 7 and plays key 64 on each beat for ever. Its main loop is a beat long.
std::vector<uint8_t> LoopingPieceCode()
{
    std::vector<uint8_t> commands;
    Append(commands, {0xc7, 0x00});               // 0: note wait off
    Append(commands, {0xf0, 0x80, 0x12, U16(7)}); // 2: 18 = 7
    Append(commands, {0x40, 0x64, 0x30});         // 7: key 64 for a beat
    Append(commands, {0x80, 0x30});               // 10: wait a beat
    Append(commands, {0x89, U24(7)});             // 12: jump to 7

    return commands;
}

void TestCompanionThatRepeatsKeepsTheLoop()
{
    // This companion hands over as Ctl does and then plays key 60 on each beat, whatever 18 holds. Each pass of the
    // piece's loop holds the same notes of the companion. The piece keeps its loop.
    std::vector<uint8_t> commands;
    Append(commands, {0xc7, 0x00});               // 0: note wait off
    Append(commands, {0x32, 0x64, 0x01});         // 2: key 50 for a tick
    Append(commands, {0xf0, 0x90, 0x10, U16(1)}); // 5: 16 == 1?
    Append(commands, {0xa2, 0x89, U24(21)});      // 10: if so, jump to 21
    Append(commands, {0x80, 0x01});               // 15: wait a tick
    Append(commands, {0x89, U24(5)});             // 17: jump to 5
    Append(commands, {0xf0, 0x80, 0x11, U16(5)}); // 21: 17 = 5
    Append(commands, {0x3c, 0x64, 0x30});         // 26: key 60 for a beat
    Append(commands, {0x80, 0x30});               // 29: wait a beat
    Append(commands, {0x89, U24(26)});            // 31: jump to 26
    const SoundArchive archive = OneSequenceArchive("Piece", LoopingPieceCode());
    const SoundArchive companion = OneSequenceArchive("Ctl", commands);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    BankSet companion_banks(companion, companion.Sounds()[0], console->settings);

    const Performance p =
        PerformBeside(archive, 0, banks, companion, 0, companion_banks, BesideScript(), {}, console->settings);

    XSFLIB_CHECK(p.loop && !NoteStarts(p, 1).empty() && p.warnings.empty());
}

void TestCompanionThatChangesDropsTheLoop()
{
    // Ctl adds key 72 from its second beat on. The first pass of the piece's loop doesn't hold it, and the second
    // does. The piece has no loop, with a warning.
    const SoundArchive archive = OneSequenceArchive("Piece", LoopingPieceCode());
    const SoundArchive companion = OneSequenceArchive("Ctl", CompanionCode());
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    BankSet companion_banks(companion, companion.Sounds()[0], console->settings);

    const Performance p =
        PerformBeside(archive, 0, banks, companion, 0, companion_banks, BesideScript(), {}, console->settings);

    const std::vector<std::string> kWarnings = {
        "Ctl doesn't repeat with the loop of Piece: the piece has no loop markers"};
    XSFLIB_CHECK(!p.loop && p.warnings == kWarnings);
}

void TestCompanionConvertsWithThePiece()
{
    // The MIDI file and the SFZ files hold both sounds' tracks. Ctl's wave comes from another archive. The name of its
    // sample starts with c. The SoundFont's comment names both sequences and lists Ctl's bank.
    const SoundArchive archive = OneSequenceArchive("Piece", BesidePieceCode());
    const SoundArchive companion = OneSequenceArchive("Ctl", CompanionCode());

    const Conversion c =
        ConvertBeside(archive, 0, companion, 0, BesideScript(), {}, console->settings, console->program);

    const std::vector<std::string> tracks = seqlib::midi::TrackNames(seqlib::midi::Read(c.midi));
    std::vector<std::string> files;
    for (const SfzFile& f : c.sfz.files)
    {
        files.push_back(f.name);
    }
    std::vector<std::string> samples;
    for (const SfzSample& s : c.sfz.samples)
    {
        samples.push_back(s.name);
    }
    std::ranges::sort(samples);
    const seqlib::sf2::Records records = seqlib::sf2::Read(c.sf2);
    const auto found = records.chunks.find("ICMT");
    const std::string comment =
        found == records.chunks.end() ? std::string() : std::string(found->second.begin(), found->second.end());
    const std::vector<std::string> kTracks = {"Piece", "Piece Track 0", "Ctl Track 0"};
    const std::vector<std::string> kFiles = {"Piece Track 0.sfz", "Ctl Track 0.sfz"};
    const std::vector<std::string> kSamples = {"c0_0.wav", "w0_0.wav"};
    XSFLIB_CHECK(ntdlib::testing::CheckConversion(c, "Piece") == 0 && c.channels == 2);
    XSFLIB_CHECK(tracks == kTracks && files == kFiles && samples == kSamples);
    XSFLIB_CHECK(
        comment.starts_with("Converted by " + console->program + " from sequence Piece beside Ctl. Banks: b0 = ") &&
        comment.find("; companion b0 = ") != std::string::npos);
}

void TestCompanionThatDoesNotHandOverIsReported()
{
    // Ctl never sets 17 to 6.
    const SoundArchive archive = OneSequenceArchive("Piece", BesidePieceCode());
    const SoundArchive companion = OneSequenceArchive("Ctl", CompanionCode());
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    BankSet companion_banks(companion, companion.Sounds()[0], console->settings);

    const std::string message = ErrorOf<xsflib::FormatError>(
        [&]
        { PerformBeside(archive, 0, banks, companion, 0, companion_banks, BesideScript(6), {}, console->settings); });

    XSFLIB_CHECK(message == "Ctl doesn't hand over to Piece within 30 seconds");
}

// The tests that hold on every console, the Wii included.
const Test kEveryConsoleTests[] = {
    TestSoundThatStartsAPieceConvertsWithThePiece,
};

// The tests that hold on the Wii U, the Switch and the 3DS.
const Test kWiiUSwitchAndCtrTests[] = {
    TestPieceOfSequencesKeepsTheirTracksWithNotes,
    TestPieceOfSequencesConvertsToOneSetOfFiles,
    TestPieceOfSequencesRequiresOneBankListAndPanMode,
    TestCompanionPlaysBesideThePiece,
    TestCompanionTrackWithoutNotesIsLeftOut,
    TestCompanionThatRepeatsKeepsTheLoop,
    TestCompanionThatChangesDropsTheLoop,
    TestCompanionConvertsWithThePiece,
    TestCompanionThatDoesNotHandOverIsReported,
};

} // namespace

int main()
{
    RunOn(WiiU(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests}));
    RunOn(Switch(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests}));
    RunOn(Ctr(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests}));
    RunOn(Wii(), kEveryConsoleTests);

    return xsflib::testing::Summary("sequence_export_pieces");
}
