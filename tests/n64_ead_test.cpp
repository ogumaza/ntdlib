// SPDX-License-Identifier: MIT

// Tests of the readers of Nintendo EAD's sound data for the Nintendo 64 (ntdlib/n64/ead_bank.h and ead_script.h). Each
// test builds its bytes by hand in the layouts that those headers describe, and the expected values were worked out by
// hand from the same layouts.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/ead_early.h"
#include "ntdlib/n64/ead_middle.h"
#include "ntdlib/n64/ead_script.h"
#include "ntdlib/n64/seq_file.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace n64 = ntdlib::n64;
using Bytes = std::vector<uint8_t>;
using n64::EadEngine;
using n64::ScriptLevel;
using n64::ScriptOp;

void Put16(Bytes& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void Put32(Bytes& bytes, uint32_t value)
{
    Put16(bytes, static_cast<uint16_t>(value >> 16));
    Put16(bytes, static_cast<uint16_t>(value));
}

// An early bank with one instrument and one drum. Their samples share a book and a loop.
Bytes EarlyBank()
{
    Bytes bank;
    Put32(bank, 1); // instruments
    Put32(bank, 1); // drums
    Put32(bank, 0);
    Put32(bank, 0x19960214);

    // The offsets below count from here, 0x10.
    Put32(bank, 0x10); // drums' table
    Put32(bank, 0x20); // instrument 0
    bank.resize(0x20, 0);

    // 0x10: the drums' table.
    Put32(bank, 0x40);
    bank.resize(0x30, 0);

    // 0x20: the instrument.
    bank.insert(bank.end(), {0, 20, 90, 0x10});
    Put32(bank, 0x50); // envelope
    Put32(bank, 0);
    Put32(bank, 0);
    Put32(bank, 0x60); // normal sample
    Put32(bank, 0x3F800000);
    Put32(bank, 0);
    Put32(bank, 0);

    // 0x40: the drum.
    bank.insert(bank.end(), {0x20, 64, 0, 0});
    Put32(bank, 0x60);
    Put32(bank, 0x40000000);
    Put32(bank, 0x50);

    // 0x50: the envelope: two points and a hang.
    Put16(bank, 2);
    Put16(bank, 32000);
    Put16(bank, 100);
    Put16(bank, 16000);
    Put16(bank, 0xFFFF);
    Put16(bank, 0);
    bank.resize(0x60 + 0x10, 0);

    // 0x60: the sample. Two bytes of flags, padding, an address, a loop, a book and a size.
    bank.insert(bank.end(), {0, 0, 0, 0});
    Put32(bank, 0x1000);
    Put32(bank, 0x80);
    Put32(bank, 0xC0);
    Put32(bank, 901);
    bank.resize(0x80 + 0x10, 0);

    // 0x80: a loop with a state.
    Put32(bank, 32);
    Put32(bank, 1600);
    Put32(bank, 0xFFFFFFFF);
    Put32(bank, 0);
    for (uint16_t i = 0; i < 16; i++)
    {
        Put16(bank, static_cast<uint16_t>(i + 100));
    }

    // 0xC0: a book of one predictor.
    bank.resize(0xC0 + 0x10, 0);
    Put32(bank, 2);
    Put32(bank, 1);
    for (uint16_t i = 0; i < 16; i++)
    {
        Put16(bank, i);
    }

    return bank;
}

void TestEarlyBank()
{
    const Bytes data = EarlyBank();

    const n64::EadBank bank = n64::EadBank::ParseEarly(data);

    XSFLIB_CHECK(bank.engine == EadEngine::kEarly);
    XSFLIB_CHECK(bank.program_instruments == std::vector<int32_t>{0});
    XSFLIB_CHECK(bank.drum_notes == std::vector<int32_t>{0});
    XSFLIB_CHECK(bank.samples.size() == 1 && bank.envelopes.size() == 1);
    XSFLIB_CHECK(bank.loops.size() == 1 && bank.books.size() == 1);
    if (bank.samples.size() != 1 || bank.envelopes.size() != 1 || bank.loops.size() != 1 || bank.books.size() != 1)
    {
        return;
    }

    XSFLIB_CHECK(bank.instruments[0].normal_range_high == 90 && bank.instruments[0].release_rate == 0x10);
    XSFLIB_CHECK(bank.instruments[0].normal.sample == 0 && bank.instruments[0].normal.tuning == 1.0F);
    XSFLIB_CHECK(bank.instruments[0].low.sample == n64::kNoObject);
    XSFLIB_CHECK(bank.drums[0].pan == 64 && bank.drums[0].sound.tuning == 2.0F);
    XSFLIB_CHECK(bank.envelopes[0].points.size() == 3 && bank.envelopes[0].points[2].delay == n64::kEnvelopeHang);
    XSFLIB_CHECK(bank.samples[0].address == 0x1000 && bank.samples[0].size == 901);
    XSFLIB_CHECK(bank.loops[0].start == 32 && bank.loops[0].state[15] == 115);
    XSFLIB_CHECK(bank.books[0].book.Coefficient(0, 1, 7) == 15);
    XSFLIB_CHECK(bank.Write() == data);

    // A changed field lands at its place.
    n64::EadBank edited = bank;
    edited.drums[0].pan = 10;
    edited.samples[0].address = 0x2000;
    const Bytes written = edited.Write();
    XSFLIB_CHECK(written[0x10 + 0x40 + 1] == 10);
    XSFLIB_CHECK(xsflib::LoadBe32(written.data() + 0x10 + 0x60 + 4) == 0x2000);

    // A count that no byte of the engine's tables could hold.
    Bytes bad = data;
    xsflib::StoreBe32(bad.data(), 0x1000);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::EadBank::ParseEarly(bad); }) ==
                 "an early bank's header holds 4096 instruments and 1 drums");
}

// A later bank with one instrument and one sound effect, whose sample uses a raw codec and a loop without a state.
Bytes LaterBank()
{
    Bytes bank;
    Put32(bank, 0);    // no drums
    Put32(bank, 0x10); // effects
    Put32(bank, 0x20); // instrument 0
    bank.resize(0x10, 0);

    // 0x10: one sound effect.
    Put32(bank, 0x40);
    Put32(bank, 0x3F800000);
    bank.resize(0x20, 0);

    // 0x20: the instrument, with no envelope.
    bank.insert(bank.end(), {0, 0, 127, 0});
    Put32(bank, 0);
    Put32(bank, 0);
    Put32(bank, 0);
    Put32(bank, 0x40);
    Put32(bank, 0x40000000);
    Put32(bank, 0);
    Put32(bank, 0);

    // 0x40: the sample: codec 1, medium 2, size 0x123.
    Put32(bank, (1U << 28) | (2U << 26) | 0x123);
    Put32(bank, 0x500);
    Put32(bank, 0x50);
    Put32(bank, 0);

    // 0x50: a loop without a state.
    Put32(bank, 1);
    Put32(bank, 2);
    Put32(bank, 0);
    Put32(bank, 0);

    return bank;
}

void TestLaterBank()
{
    const Bytes data = LaterBank();

    const n64::EadBank bank = n64::EadBank::ParseLater(data, 1, 0, 1);

    XSFLIB_CHECK(bank.engine == EadEngine::kLater);
    XSFLIB_CHECK(bank.drum_notes.empty() && bank.effects.size() == 1 && bank.samples.size() == 1);
    if (bank.samples.size() != 1)
    {
        return;
    }

    XSFLIB_CHECK(bank.effects[0].sound.sample == 0 && bank.effects[0].sound.tuning == 1.0F);
    XSFLIB_CHECK(bank.samples[0].codec == n64::kEadCodecS8 && bank.samples[0].medium == 2);
    XSFLIB_CHECK(bank.samples[0].size == 0x123 && bank.samples[0].address == 0x500);
    XSFLIB_CHECK(bank.samples[0].book == n64::kNoObject);
    XSFLIB_CHECK(!bank.loops[0].has_state && bank.loops[0].end == 2);
    XSFLIB_CHECK(bank.Write() == data);
}

// Writes an envelope's points over the two points and the hang of EarlyBank().
Bytes EarlyBankWithEnvelope(const std::vector<n64::EadEnvelope::Point>& points)
{
    Bytes data = EarlyBank();
    std::size_t at = 0x10 + 0x50;
    for (const n64::EadEnvelope::Point& point : points)
    {
        xsflib::StoreBe16(data.data() + at, static_cast<uint16_t>(point.delay));
        xsflib::StoreBe16(data.data() + at + 2, static_cast<uint16_t>(point.target));
        at += 4;
    }

    return data;
}

// Pokémon Stadium 2's bank 0x17 holds at offset 0x880 an envelope that starts (-4, 0), (1, 32700), (163, 23452).
void TestEnvelopeWithNegativeDelay()
{
    const Bytes data =
        EarlyBankWithEnvelope({{n64::kEnvelopeSkip, 0}, {1, 32700}, {163, 23452}, {n64::kEnvelopeHang, 0}});

    const n64::EadBank bank = n64::EadBank::ParseEarly(data);

    XSFLIB_CHECK(bank.envelopes.size() == 1);
    if (bank.envelopes.size() != 1)
    {
        return;
    }

    const std::vector<n64::EadEnvelope::Point>& points = bank.envelopes[0].points;
    XSFLIB_CHECK(points.size() == 4);
    if (points.size() != 4)
    {
        return;
    }

    XSFLIB_CHECK(points[0].delay == -4 && points[0].target == 0);
    XSFLIB_CHECK(points[1].delay == 1 && points[1].target == 32700);
    XSFLIB_CHECK(points[2].delay == 163 && points[2].target == 23452);
    XSFLIB_CHECK(points[3].delay == -1);
    XSFLIB_CHECK(bank.Write() == data);

    // Any delay under -3 goes on too.
    const Bytes lower = EarlyBankWithEnvelope({{-1000, 5}, {2, 6}, {n64::kEnvelopeDisable, 0}, {7, 8}});
    const n64::EadBank lower_bank = n64::EadBank::ParseEarly(lower);
    XSFLIB_CHECK(lower_bank.envelopes.size() == 1 && lower_bank.envelopes[0].points.size() == 3);
    XSFLIB_CHECK(lower_bank.Write() == lower);
}

// The four delays that end a list, with a point left over after each. The bytes after the end stay in the bank.
void TestEnvelopeEnds()
{
    for (const int16_t end : {n64::kEnvelopeDisable, n64::kEnvelopeHang, n64::kEnvelopeGoto, n64::kEnvelopeRestart})
    {
        const Bytes data = EarlyBankWithEnvelope({{5, 100}, {end, 0}, {6, 200}, {n64::kEnvelopeHang, 0}});
        const n64::EadBank bank = n64::EadBank::ParseEarly(data);
        XSFLIB_CHECK(bank.envelopes.size() == 1 && bank.envelopes[0].points.size() == 2);
        XSFLIB_CHECK(bank.Write() == data);
    }
}

void TestLaterTable()
{
    Bytes data;
    Put16(data, 2);
    Put16(data, 3);
    Put32(data, 0x1000);
    Put32(data, 0xAABBCCDD);
    Put32(data, 0);

    Put32(data, 0);
    Put32(data, 0x100);
    data.insert(data.end(), {2, 1});
    Put16(data, 0x0100);
    Put16(data, 0x7F40);
    Put16(data, 5);

    Put32(data, 0x100);
    Put32(data, 0x80);
    data.insert(data.end(), {2, 0});
    Put16(data, 0);
    Put16(data, 0);
    Put16(data, 0);

    const n64::EadTable table = n64::EadTable::Parse(data);

    XSFLIB_CHECK(table.medium_parameter == 3 && table.base_address == 0x1000 && table.entries.size() == 2);
    XSFLIB_CHECK(table.entries[0].size == 0x100 && table.entries[0].data2 == 0x7F40 && table.entries[0].data3 == 5);
    XSFLIB_CHECK(table.entries[1].address == 0x100 && table.entries[1].medium == 2);
    XSFLIB_CHECK(table.Write() == data);
}

void TestEarlyFiles()
{
    Bytes file;
    Put16(file, n64::kEarlyBankFileRevision);
    Put16(file, 1);
    Put32(file, 12);
    Put32(file, 2);
    file.insert(file.end(), {7, 8});

    XSFLIB_CHECK(n64::SeqFile::Matches(file, n64::kEarlyBankFileRevision));
    XSFLIB_CHECK(!n64::SeqFile::Matches(file));
    const n64::SeqFile parsed = n64::SeqFile::Parse(file, n64::kEarlyBankFileRevision);
    XSFLIB_CHECK(parsed.Write() == file);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::SeqFile::Parse(file, n64::kEarlySampleFileRevision); }) ==
                 "not a file of revision 2");

    // Two sequences: sequence 0 uses banks 5 and 6, and sequence 1 uses bank 7.
    const Bytes sets = {0, 4, 0, 7, 2, 5, 6, 1, 7};
    const std::vector<std::vector<uint8_t>> parsed_sets = n64::ParseEarlyBankSets(sets, 2);
    XSFLIB_CHECK(parsed_sets.size() == 2);
    XSFLIB_CHECK((parsed_sets[0] == std::vector<uint8_t>{5, 6}) && (parsed_sets[1] == std::vector<uint8_t>{7}));
}

// A file of one sequence that holds one note on one channel, and the bank of EarlyBank.
n64::EadEarlyFiles MiddleFiles()
{
    const Bytes sequence = {
        0xD7, 0x00, 0x01,             // 0: init channel 0
        0xDB, 0x7F,                   // 3: the volume
        0xDD, 0x78,                   // 5: a tempo of 120
        0x90, 0x00, 0x10,             // 7: start channel 0 at 0x10
        0xFD, 0x8F, 0xFF,             // 10: a delay
        0xFF,                         // 13: the end
        0xEE, 0xEE,                   // 14
        0xC4,                         // 16: large notes
        0xC1, 0x00,                   // 17: instrument 0
        0xDF, 0x7F,                   // 19: the channel's volume
        0xDD, 0x40,                   // 21: a pan
        0x90, 0x00, 0x20,             // 23: start layer 0 at 0x20
        0xFD, 0x8F, 0xFF,             // 26: a delay
        0xFF,                         // 29: the end
        0xEE, 0xEE,                   // 30
        0x27, 0x8F, 0xFF, 0x7F, 0x00, // 32: pitch 39, delay 0xFFF, velocity 0x7F, gate 0
        0xFF,                         // 37: the end
    };

    Bytes file;
    Put16(file, n64::kSeqFileRevision);
    Put16(file, 1);
    Put32(file, 12);
    Put32(file, static_cast<uint32_t>(sequence.size()));
    file.insert(file.end(), sequence.begin(), sequence.end());

    n64::EadEarlyFiles files;
    files.sequences = n64::SeqFile::Parse(file);
    files.banks.push_back(n64::EadBank::ParseEarly(EarlyBank()));
    files.sample_places = {0};
    files.bank_sets = {{0}};

    return files;
}

struct MiddleRig
{
    MiddleRig(const n64::EadMiddleConfig& config, const n64::EadEarlyFiles& files)
        : memory(image, 0x400000), player(config, files, memory)
    {
        output = memory.Allocate(0x1000);
        player.Start(0, 0);
    }

    std::vector<n64::AudioCommand> Frame()
    {
        n64::AudioList list;
        player.RunFrame(list, memory, output, 544);

        return list.commands;
    }

    Bytes image = Bytes(0x1000, 0);
    n64::AudioMemory memory;
    n64::EadMiddlePlayer player;
    uint32_t output = 0;
};

// The first word's low half of the first command with this high half.
uint32_t ValueOf(const std::vector<n64::AudioCommand>& commands, uint32_t high_half)
{
    const auto found = std::find_if(commands.begin(), commands.end(), [high_half](const n64::AudioCommand& command)
                                    { return (command.w0 >> 16) == high_half; });
    XSFLIB_CHECK(found != commands.end());

    return found->w0 & 0xFFFF;
}

// The middle engine's lists for the standard microcode. Wave Race 64 builds this form.
//
// A frame's 533 samples align to 544. The form takes an update for each 160 samples: (544 + 16) / 160 + 1 = 4. The
// other form takes 192: (544 + 16) / 192 + 1 = 3. A voice has an envelope mixer in each update.
void TestMiddleStandardForm()
{
    const n64::EadEarlyFiles files = MiddleFiles();
    n64::EadMiddleConfig config;
    config.standard_microcode = true;
    config.dry_gain = 0x5000;

    MiddleRig rig(config, files);
    XSFLIB_CHECK(rig.player.UpdatesPerFrame() == 4);
    n64::EadMiddleConfig other;
    MiddleRig other_rig(other, files);
    XSFLIB_CHECK(other_rig.player.UpdatesPerFrame() == 3);

    rig.Frame();
    const std::vector<n64::AudioCommand> second = rig.Frame();
    const auto mixers = std::count_if(second.begin(), second.end(), [](const n64::AudioCommand& command)
                                      { return command.Opcode() == n64::kAudioEnvMixer; });
    XSFLIB_CHECK(mixers == 4);

    // Each voice takes the session's gain for its dry output.
    constexpr uint32_t kSetAux = (uint32_t{n64::kAudioSetVolume} << 8) | n64::kAudioFlagAux;
    XSFLIB_CHECK(ValueOf(second, kSetAux) == 0x5000);

    // The form doesn't square a channel's volume. A volume of 0.5 gives half of the target.
    MiddleRig half(config, files);
    half.Frame();
    half.player.SetChannelVolume(0, 0, 0.5F);
    half.Frame();
    MiddleRig full(config, files);
    full.Frame();
    full.player.SetChannelVolume(0, 0, 1.0F);
    full.Frame();
    constexpr uint32_t kSetTargetRight = uint32_t{n64::kAudioSetVolume} << 8;
    const std::vector<n64::AudioCommand> half_list = half.Frame();
    const std::vector<n64::AudioCommand> full_list = full.Frame();
    const int half_target = static_cast<int>(ValueOf(half_list, kSetTargetRight));
    const int full_target = static_cast<int>(ValueOf(full_list, kSetTargetRight));
    XSFLIB_CHECK(full_target > 0);
    XSFLIB_CHECK(std::abs((half_target * 2) - full_target) <= 2);
}

void TestEarlyCommands()
{
    // The sequence: a tempo, a channel's start, a delay, a loop and an end.
    const Bytes sequence = {
        0xD3, 0x80,       // 0: mute behaviour
        0xDD, 0x78,       // 2: a tempo of 120
        0x90, 0x00, 0x10, // 4: start channel 0 at 0x10
        0xFD, 0x81, 0x00, // 7: a delay of 0x100
        0xFF,             // 10: the end
        0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
        0xC4,             // 16: large notes
        0xC1, 0x05,       // 17: instrument 5
        0xDD, 0x20,       // 19: a pan
        0xDB, 0xFE,       // 21: a transpose of -2
        0x90, 0x00, 0x20, // 23: start layer 0 at 0x20
        0xFE,             // 26: a delay of 1
        0xFF,             // 27
        0xEE, 0xEE, 0xEE, 0xEE, 0x05,
        0x18, 0x60, 0x40, // 32: a large note, form 0: pitch 5, delay 24, velocity 0x60, gate 0x40
        0x46, 0x18, 0x61, // 36: a large note, form 1: pitch 6, delay 24, velocity 0x61
        0x87, 0x62, 0x41, // 39: a large note, form 2: pitch 7, velocity 0x62, gate 0x41
        0xC0, 0x80, 0x90, // 42: a delay with 2 bytes: 0x90
        0xFF,             // 45
    };

    const n64::ScriptCommand tempo =
        n64::ReadScriptCommand(sequence, 2, EadEngine::kEarly, ScriptLevel::kSequence, false);
    XSFLIB_CHECK(tempo.op == ScriptOp::kTempo && tempo.args[0] == 0x78 && tempo.next == 4);

    const n64::ScriptCommand start =
        n64::ReadScriptCommand(sequence, 4, EadEngine::kEarly, ScriptLevel::kSequence, false);
    XSFLIB_CHECK(start.op == ScriptOp::kStartChannel && start.low_bits == 0 && start.args[0] == 0x10);

    const n64::ScriptCommand delay =
        n64::ReadScriptCommand(sequence, 7, EadEngine::kEarly, ScriptLevel::kSequence, false);
    XSFLIB_CHECK(delay.op == ScriptOp::kDelay && delay.args[0] == 0x100 && delay.next == 10);

    const n64::ScriptCommand transpose =
        n64::ReadScriptCommand(sequence, 21, EadEngine::kEarly, ScriptLevel::kChannel, false);
    XSFLIB_CHECK(transpose.op == ScriptOp::kChannelTranspose && transpose.args[0] == -2);

    const n64::ScriptCommand first = n64::ReadScriptCommand(sequence, 32, EadEngine::kEarly, ScriptLevel::kLayer, true);
    XSFLIB_CHECK(first.op == ScriptOp::kNote && first.low_bits == 5);
    XSFLIB_CHECK(first.args[0] == 0x18 && first.args[1] == 0x60 && first.args[2] == 0x40 && first.next == 36);

    const n64::ScriptCommand second =
        n64::ReadScriptCommand(sequence, 36, EadEngine::kEarly, ScriptLevel::kLayer, true);
    XSFLIB_CHECK(second.low_bits == 6 && second.args[1] == 0x61 && second.args[2] == -1 && second.next == 39);

    const n64::ScriptCommand third = n64::ReadScriptCommand(sequence, 39, EadEngine::kEarly, ScriptLevel::kLayer, true);
    XSFLIB_CHECK(third.low_bits == 7 && third.args[0] == -1 && third.args[1] == 0x62 && third.args[2] == 0x41);

    // The same bytes read as short notes: a form 0 holds only a delay, and the other forms hold nothing.
    const n64::ScriptCommand short_first =
        n64::ReadScriptCommand(sequence, 32, EadEngine::kEarly, ScriptLevel::kLayer, false);
    XSFLIB_CHECK(short_first.args[0] == 0x18 && short_first.args[1] == -1 && short_first.next == 34);
    const n64::ScriptCommand short_second =
        n64::ReadScriptCommand(sequence, 36, EadEngine::kEarly, ScriptLevel::kLayer, false);
    XSFLIB_CHECK(short_second.args[0] == -1 && short_second.next == 37);

    const n64::ScriptWalk walk = n64::WalkScript(sequence, EadEngine::kEarly);
    XSFLIB_CHECK(walk.bad_offsets.empty());
    XSFLIB_CHECK(walk.notes == 3);
    XSFLIB_CHECK(walk.commands.size() == 5 + 7 + 5);
    const auto layer_note = std::find_if(walk.commands.begin(), walk.commands.end(),
                                         [](const n64::WalkedCommand& c) { return c.command.offset == 39; });
    XSFLIB_CHECK(layer_note != walk.commands.end() && layer_note->channel == 0 && layer_note->layer == 0);
}

void TestLaterCommands()
{
    // A relative start of a channel and a relative jump.
    const Bytes sequence = {
        0xA1, 0x00, 0x03, // 0: start channel 1, 3 bytes after this command
        0xF4, 0x02,       // 3: a jump over the next two bytes
        0xEE, 0xEE,       // 5
        0xFF,             // 7: the end
    };
    const n64::ScriptCommand relative =
        n64::ReadScriptCommand(sequence, 0, EadEngine::kLater, ScriptLevel::kSequence, false);
    XSFLIB_CHECK(relative.op == ScriptOp::kStartChannelRel && relative.low_bits == 1 && relative.args[0] == 3);

    const n64::ScriptCommand jump =
        n64::ReadScriptCommand(sequence, 3, EadEngine::kLater, ScriptLevel::kSequence, false);
    XSFLIB_CHECK(jump.op == ScriptOp::kJump && jump.args[0] == 7 && jump.next == 5);

    // The early engine lacks the relative jumps.
    const n64::ScriptCommand early =
        n64::ReadScriptCommand(sequence, 3, EadEngine::kEarly, ScriptLevel::kSequence, false);
    XSFLIB_CHECK(early.op == ScriptOp::kUnknown);

    const Bytes channel = {0xE8, 1, 2, 3, 4, 5, 6, 7, 8, 0xEB, 3, 9, 0x7B, 0x00, 0x10, 0x8A, 0x00, 0x40};
    const n64::ScriptCommand parameters =
        n64::ReadScriptCommand(channel, 0, EadEngine::kLater, ScriptLevel::kChannel, false);
    XSFLIB_CHECK(parameters.op == ScriptOp::kParameters && parameters.bytes[7] == 8 && parameters.next == 9);

    const n64::ScriptCommand font = n64::ReadScriptCommand(channel, 9, EadEngine::kLater, ScriptLevel::kChannel, false);
    XSFLIB_CHECK(font.op == ScriptOp::kFontInstrument && font.args[0] == 3 && font.args[1] == 9);

    const n64::ScriptCommand layer_rel =
        n64::ReadScriptCommand(channel, 12, EadEngine::kLater, ScriptLevel::kChannel, false);
    XSFLIB_CHECK(layer_rel.op == ScriptOp::kStartLayerRel && layer_rel.low_bits == 3 && layer_rel.args[0] == 0x10);

    const n64::ScriptCommand layer =
        n64::ReadScriptCommand(channel, 15, EadEngine::kLater, ScriptLevel::kChannel, false);
    XSFLIB_CHECK(layer.op == ScriptOp::kStartLayer && layer.low_bits == 2 && layer.args[0] == 0x40);

    // A portamento's time has one byte when the mode's top bit is set.
    const Bytes portamento = {0xC7, 0x81, 40, 0x20, 0xC7, 0x01, 40, 0x81, 0x00};
    const n64::ScriptCommand special =
        n64::ReadScriptCommand(portamento, 0, EadEngine::kLater, ScriptLevel::kLayer, false);
    XSFLIB_CHECK(special.op == ScriptOp::kPortamento && special.args[2] == 0x20 && special.next == 4);
    const n64::ScriptCommand plain =
        n64::ReadScriptCommand(portamento, 4, EadEngine::kLater, ScriptLevel::kLayer, false);
    XSFLIB_CHECK(plain.args[2] == 0x100 && plain.next == 9);

    // A command that leaves the sequence.
    const Bytes cut = {0xDD};
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>(
                      [&] { n64::ReadScriptCommand(cut, 0, EadEngine::kEarly, ScriptLevel::kSequence, false); })
                      .empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_ead",
                                    []
                                    {
                                        TestEarlyBank();
                                        TestLaterBank();
                                        TestEnvelopeWithNegativeDelay();
                                        TestEnvelopeEnds();
                                        TestLaterTable();
                                        TestEarlyFiles();
                                        TestEarlyCommands();
                                        TestLaterCommands();
                                        TestMiddleStandardForm();
                                    });
}
