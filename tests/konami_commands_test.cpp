// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the track commands of Konami's GBA driver (ntdlib/konami/commands.h). Each
// song is a string of bytes as the driver reads them, worked out by hand from the rules in ntdlib/konami/seqformat.h:
// <delay> { <command> <delay> }*, with each revision's opcodes. The decoder has to give the items that the bytes stand
// for, and the encoder the same bytes.

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/testing/agb_cart.h"

using xsflib::testing::Check;

namespace
{

namespace konami = ntdlib::konami;
using konami::Decoded;
using konami::DecodeSequence;
using konami::EncodeSequence;
using konami::Revision;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// Every track of a song of `revision` at `offset`, but for those that `first` gives.
std::vector<uint16_t> Offsets(Revision revision, uint16_t offset, std::vector<uint16_t> first = {})
{
    first.resize(size_t(konami::TrackCount(revision)), offset);
    return first;
}

// Whether the items encode back to `bytes`.
bool EncodesBack(const Decoded& decoded, const std::vector<uint8_t>& bytes, Revision revision)
{
    return EncodeSequence(decoded.sequence, decoded.entries, revision).bytes == bytes;
}

// The bytes of `item` alone, between the delays of a track of its revision: the song's first delay, the item, a delay
// and the end.
std::vector<uint8_t> BytesOfCommand(const Command& item, Revision revision)
{
    Command wait;
    wait.kind = Kind::kWait;
    Command end;
    end.kind = Kind::kEnd;
    ntdlib::commands::Sequence sequence;
    sequence.items = {wait, item, wait, end};
    const std::vector<uint32_t> entries(size_t(konami::TrackCount(revision)), 0);
    const std::vector<uint8_t> bytes = EncodeSequence(sequence, entries, revision).bytes;
    return std::vector<uint8_t>(bytes.begin() + 1, bytes.end() - 2);
}

bool Throws(const Command& item, Revision revision)
{
    try
    {
        static_cast<void>(BytesOfCommand(item, revision));
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// Each command of the Ultimate Masters revision on a PSG track.
void TestUltimateMasters()
{
    const std::vector<uint8_t> bytes = {
        0x05,             // delay 5
        0x84,             // duty 0x84: bits 0-5 << 6 = 0x00 after the shift of 4
        0x00,             // delay 0
        0xD3, 0x3C,       // PSG note 0x3C, volume 3
        0xE0, 0x10,       // delay 0x10, in the long form that its value doesn't require
        0xE5,             // a rest: the low nibble does nothing
        0xF1, 0x02,       // delay 0x102, in the form of 3 bytes: the high nibble does nothing
        0x01,             //
        0xF0, 0x20,       // pan 0x20
        0xE1, 0x23,       // delay 0x123
        0xF1, 0x40,       // vibrato 0x40
        0x00,             //
        0xF2, 0x41,       // a bend of +1
        0x00,             //
        0xF2, 0x84, 0x10, // a bend of 0x410 - 0x400 = +16
        0x00,             //
        0xC2,             // volume 2
        0x00,             //
        0xC8, 0x90,       // volume 0x90
        0x00,             //
        0xF3,             // a loop point, of 1 byte on a PSG track
        0x00,             //
        0xF7, 0x11,       // echo feedback
        0x00,             //
        0xF8, 0x22,       // echo delay
        0x00,             //
        0xF9, 0x31,       // echo route
        0x00,             //
        0xFF, 0x01,       // a loop of the song
        0x00,             // the sample tracks start here
        0xFE,             //
    };
    const Revision r = Revision::kUltimateMasters;
    const auto sample_tracks = uint16_t(bytes.size() - 2);
    const Decoded decoded = DecodeSequence(bytes, Offsets(r, sample_tracks, {0, 0, 0, 0}), r);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque && items.size() == 30,
          "the track's items: " + std::to_string(items.size()) + " " + decoded.reason);
    if (items.size() != 30)
    {
        return;
    }

    Check(items[0].kind == Kind::kWait && items[0].value.value == 5 * kOne, "a tick is a frame of a delay");
    Check(items[1].kind == Kind::kProgram && items[1].index == 0 && items[1].value.value == 0x00, "the duty byte");
    Check(items[1].encoding.opcode == (konami::kKeptBytes | 0x84),
          "a duty that the usual byte doesn't give keeps its byte");
    Check(items[3].kind == Kind::kNote && items[3].key == 0x3C && items[3].velocity == 3 * kOne && !items[3].sample &&
              items[3].value.value == 0,
          "a PSG note");
    Check(items[4].kind == Kind::kWait && items[4].value.value == 0x10 * kOne &&
              items[4].encoding.opcode == (konami::kKeptBytes | 0xE0) && items[4].bytes == std::vector<uint8_t>{0x10},
          "a delay in a long form keeps its bytes");
    Check(items[5].kind == Kind::kNoteOff && items[5].encoding.opcode == (konami::kKeptBytes | 0xE5), "a rest");
    Check(items[6].kind == Kind::kWait && items[6].value.value == 0x102 * kOne, "a delay of 3 bytes");
    Check(items[7].kind == Kind::kPan && items[7].value.value == (0x20 - 0x40) * kOne, "a pan with 0x40 the centre");
    Check(items[8].value.value == 0x123 * kOne && items[8].encoding.opcode == 0, "a delay in its usual form");
    Check(items[9].kind == Kind::kLfoDepth && items[9].value.value == 0x40 * kOne, "a vibrato");
    Check(items[11].kind == Kind::kPitchBend && items[11].value.value == kOne / 64,
          "a bend, on a range of 2 semitones");
    Check(items[13].kind == Kind::kPitchBend && items[13].value.value == 16 * (kOne / 64), "a long bend");
    Check(items[15].kind == Kind::kVolume && items[15].value.value == 2 * kOne, "a short volume");
    Check(items[17].kind == Kind::kVolume && items[17].value.value == 0x90 * kOne, "a volume with its byte");
    Check(items[19].kind == Kind::kLoopStart && items[19].bytes.empty(), "a loop point");
    Check(items[21].kind == Kind::kEchoVolume && items[21].value.value == 0x11 * kOne, "the echo's feedback");
    Check(items[23].kind == Kind::kEchoLength && items[23].value.value == 0x22 * kOne, "the echo's delay");
    Check(items[25].kind == Kind::kOther && items[25].encoding.opcode == 0xF9 &&
              items[25].bytes == std::vector<uint8_t>{0x31},
          "the echo's route, without a kind");
    Check(items[27].kind == Kind::kLoopEnd, "a loop of the song");
    Check(EncodesBack(decoded, bytes, r), "the items encode back to the bytes");

    // An edit of a value that the kept bytes don't hold gives the usual bytes, and the others stay.
    Decoded edited = decoded;
    edited.sequence.items[4].value.value = 0x11 * kOne;
    std::vector<uint8_t> expected = bytes;
    expected.erase(expected.begin() + 5, expected.begin() + 7);
    expected.insert(expected.begin() + 5, 0x11);
    Check(EncodesBack(edited, expected, r), "a new delay takes its usual byte");
}

// A sample track's commands in the Ultimate Masters revision and its later builds.
void TestSampleNotes()
{
    // A sample note holds its volume in the opcode up to 7, or in a byte. Bx adds semitones.
    {
        const std::vector<uint8_t> bytes = {
            0x00,             //
            0xA3, 0x20,       // sample 0x20, volume 3
            0x00,             //
            0xA8, 0x80, 0x21, // sample 0x21, volume 0x80
            0x00,             //
            0xB2, 0x22, 0xFE, // sample 0x22, volume 2, 2 semitones down
            0x00,             //
            0xB0, 0x23, 0x00, // sample 0x23, volume 0, semitones 0 in the longer form
            0x00,             //
            0xF3, 0x12, 0x34, // a loop point, of 3 bytes on a sample track
            0x00,             //
            0xFE,             // the end of the track
        };
        const Revision r = Revision::kUltimateMasters;
        std::vector<uint16_t> offsets = Offsets(r, 0);
        const std::vector<uint8_t> psg = {0x00, 0xFE};
        std::vector<uint8_t> all = bytes;
        all.insert(all.end(), psg.begin(), psg.end());
        for (int t = 0; t < konami::kPsgTracks; t++)
        {
            offsets[size_t(t)] = uint16_t(bytes.size());
        }
        const Decoded decoded = DecodeSequence(all, offsets, r);
        const std::vector<Command>& items = decoded.sequence.items;
        Check(!decoded.opaque && items.size() == 14, "the sample track's items");
        if (items.size() == 14)
        {
            Check(items[1].kind == Kind::kNote && items[1].sample == 0x20 && items[1].velocity == 3 * kOne &&
                      items[1].key == 0,
                  "a sample note");
            Check(items[3].sample == 0x21 && items[3].velocity == 0x80 * kOne, "a sample note with a volume's byte");
            Check(items[5].sample == 0x22 && items[5].key == 0xFE, "a sample note 2 semitones down");
            Check(
                items[7].sample == 0x23 && items[7].key == 0 && items[7].encoding.opcode == (konami::kKeptBytes | 0xB0),
                "a longer form keeps its bytes");
            Check(items[9].kind == Kind::kLoopStart && items[9].bytes == std::vector<uint8_t>({0x12, 0x34}),
                  "a sample track's loop point holds the bytes that the driver skips");
            Check(EncodesBack(decoded, all, r), "the sample track encodes back");
        }

        // A PSG track and a sample track read F3 with other lengths.
        const Decoded shared = DecodeSequence(bytes, Offsets(r, 0), r);
        Check(shared.opaque, "a PSG track and a sample track that share an F3 stay as data");
        Check(EncodesBack(shared, bytes, r), "the song kept as data encodes back");
    }

    // Live Start's sample numbers take a second byte from F0 up. Prince of Tennis 2004 reads such a byte as a sample.
    {
        const std::vector<uint8_t> bytes = {0x00, 0xA1, 0xF2, 0x34, 0x00, 0xFE};
        const Decoded live = DecodeSequence(bytes, Offsets(Revision::kLiveStart, 0), Revision::kLiveStart);
        Check(!live.opaque && live.sequence.items[1].sample == 0x234, "Live Start's long sample number");
        Check(EncodesBack(live, bytes, Revision::kLiveStart), "Live Start's note encodes back");

        const std::vector<uint8_t> other = {0x00, 0xA1, 0xF2, 0x00, 0xFE};
        const Decoded tennis = DecodeSequence(other, Offsets(Revision::kPrinceOfTennis, 0), Revision::kPrinceOfTennis);
        Check(!tennis.opaque && tennis.sequence.items[1].sample == 0xF2, "Prince of Tennis 2004's sample byte");

        // The Ultimate Masters revision doesn't read such a note.
        const Decoded plain = DecodeSequence(other, Offsets(Revision::kUltimateMasters, 0), Revision::kUltimateMasters);
        Check(plain.opaque, "a sample above EF stays as data in the Ultimate Masters revision");
    }
}

// The older revisions' commands, with what each makes of the same opcode.
void TestOlderRevisions()
{
    const Revision wct = Revision::kWct2004;
    const std::vector<uint8_t> bytes = {
        0x03,       //
        0x81,       // duty 1, in bits 6-7
        0x00,       //
        0x85,       // wave 1
        0x00,       //
        0xD5, 0x30, // PSG note 0x30, volume 5
        0xFF, 0x05, // delay (0x1F << 8) | 5
        0xC7,       // volume 7
        0x00,       //
        0xF0, 0x7B, // left 7, right 11
        0x00,       //
        0xF4, 0x10, // attack
        0x00,       //
        0xF5, 0x20, // decay
        0x00,       //
        0xF6, 0x02, // instrument 2
        0x00,       //
        0xFA, 0x01, // a volume scale from the game's setting
        0x00,       //
        0xFB,       // does nothing
        0x00,       //
        0xFE,       // a loop of the song
    };
    const Decoded decoded = DecodeSequence(bytes, Offsets(wct, 0), wct);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque && items.size() == 22, "WCT 2004's items: " + std::to_string(items.size()));
    if (items.size() == 22)
    {
        Check(items[1].kind == Kind::kProgram && items[1].index == 0 && items[1].value.value == 0x40 * kOne, "a duty");
        Check(items[3].kind == Kind::kProgram && items[3].index == 1 && items[3].value.value == kOne, "a wave");
        Check(items[5].kind == Kind::kNote && items[5].key == 0x30 && items[5].velocity == 5 * kOne, "a PSG note");
        Check(items[6].value.value == 0x1F05 * kOne, "a delay of 13 bits");
        Check(items[9].kind == Kind::kPan && items[9].value.value == ToUnit(4, konami::ScaleOf(Kind::kPan, wct)) &&
                  items[9].velocity == 11 * kOne,
              "a pan of levels: right minus left, and the larger level");
        Check(items[11].kind == Kind::kAttack && items[11].value.value == 0x10 * kOne, "an attack");
        Check(items[13].kind == Kind::kDecay && items[13].value.value == 0x20 * kOne, "a decay");
        Check(items[15].kind == Kind::kProgram && items[15].index == 2 && items[15].value.value == 2 * kOne, "F6");
        Check(items[17].kind == Kind::kOther && items[17].encoding.opcode == 0xFA, "FA keeps its byte in kOther");
        Check(items[19].kind == Kind::kOther && items[19].encoding.opcode == 0xFB && items[19].bytes.empty(), "a nop");
        Check(items[21].kind == Kind::kLoopEnd, "FE loops the song");
        Check(EncodesBack(decoded, bytes, wct), "WCT 2004's items encode back");
    }

    // A pan edit keeps the larger level: position 4 at level 11 is left 7 and right 11, and -4 is left 11 and right 7.
    Decoded panned = decoded;
    panned.sequence.items[9].value.value = ToUnit(-4, konami::ScaleOf(Kind::kPan, wct));
    std::vector<uint8_t> expected = bytes;
    expected[12] = 0xB7;
    Check(EncodesBack(panned, expected, wct), "a new pan keeps the level");

    // FA and FB in the other revisions.
    const std::vector<uint8_t> two = {0x00, 0xFA, 0x00, 0xFD};
    const Decoded kitaro = DecodeSequence(two, Offsets(Revision::kKitaro, 0), Revision::kKitaro);
    Check(kitaro.sequence.items[1].kind == Kind::kOther && kitaro.sequence.items[1].bytes.empty(),
          "the Kitaro revision's FA does nothing, in a byte");
    const std::vector<uint8_t> volume = {0x00, 0xFB, 0x08, 0x00, 0xFD};
    const Decoded minna = DecodeSequence(volume, Offsets(Revision::kMinnaNoOujisama, 0), Revision::kMinnaNoOujisama);
    Check(minna.sequence.items[1].kind == Kind::kMainVolume && minna.sequence.items[1].value.value == 8 * kOne,
          "Minna no Ouji-sama's song volume");
    Check(EncodesBack(minna, volume, Revision::kMinnaNoOujisama), "the song volume encodes back");

    // The later Ultimate Masters builds hold a track's output back with FA.
    const Decoded held = DecodeSequence(std::vector<uint8_t>{0x00, 0xFA, 0x01, 0x00, 0xFE},
                                        Offsets(Revision::kLiveStart, 0), Revision::kLiveStart);
    Check(held.sequence.items[1].kind == Kind::kMute && held.sequence.items[1].value.value == kOne, "FA holds output");

    // The revisions with Rave Master's commands: 00-7F do nothing, and F3 has no extra bytes on a sample track.
    const Revision rave = Revision::kRaveMaster;
    const std::vector<uint8_t> nop = {0x00, 0x12, 0x00, 0xF3, 0x00, 0xF7, 0x00, 0xFD};
    const Decoded rave_items = DecodeSequence(nop, Offsets(rave, 0), rave);
    Check(!rave_items.opaque && rave_items.sequence.items[1].kind == Kind::kOther &&
              rave_items.sequence.items[1].encoding.opcode == 0x12,
          "Rave Master's 12 does nothing");
    Check(rave_items.sequence.items[3].kind == Kind::kLoopStart && rave_items.sequence.items[3].bytes.empty(),
          "Rave Master's F3 is a byte on every track");
    Check(rave_items.sequence.items[5].kind == Kind::kOther, "Rave Master's F7 does nothing");
    Check(EncodesBack(rave_items, nop, rave), "Rave Master's items encode back");

    // Eternal Duelist reads F4-FC as F1, and F0 by the track. Its long delays are Fx yy.
    const Revision ed = Revision::kEternalDuelist;
    const std::vector<uint8_t> duelist = {
        0xF1, 0x23, // delay 0x123
        0xF5, 0x10, // a vibrato
        0xEF,       // delay 0xEF, in one byte
        0xF0, 0x31, // volume 1, and 3 for the next track
        0x00,       //
        0xFD,       //
        0x00,       // a PSG track
        0xF0, 0x11, // its NR51 bits
        0x00,       //
        0xFD,       //
    };
    std::vector<uint16_t> ed_offsets = Offsets(ed, 0);
    for (int t = 0; t < konami::kPsgTracks; t++)
    {
        ed_offsets[size_t(t)] = 9;
    }
    const Decoded ed_items = DecodeSequence(duelist, ed_offsets, ed);
    const std::vector<Command>& di = ed_items.sequence.items;
    Check(!ed_items.opaque && di.size() == 10, "Eternal Duelist's items");
    if (di.size() == 10)
    {
        Check(di[0].value.value == 0x123 * kOne, "Fx yy's delay");
        Check(di[1].kind == Kind::kLfoDepth && di[1].value.value == 0x10 * kOne &&
                  di[1].encoding.opcode == (konami::kKeptBytes | 0xF5),
              "F5 is a vibrato that keeps its opcode");
        Check(di[2].value.value == 0xEF * kOne && di[2].encoding.opcode == 0, "EF is a delay of a byte");
        Check(
            di[3].kind == Kind::kVolume && di[3].index == 2 && di[3].value.value == kOne && di[3].velocity == 3 * kOne,
            "a sample track's F0 sets two volumes");
        Check(di[7].kind == Kind::kOther && di[7].encoding.opcode == 0xF0 && di[7].bytes == std::vector<uint8_t>{0x11},
              "a PSG track's F0 sets its NR51 bits");
        Check(EncodesBack(ed_items, duelist, ed), "Eternal Duelist's items encode back");
    }
}

// Dungeon Dice Monsters' commands.
void TestDungeonDice()
{
    const Revision r = Revision::kDungeonDiceMonsters;
    const std::vector<uint8_t> bytes = {
        0x00,       //
        0x72,       // wave 2
        0x00,       //
        0x7D,       // duty 1, in bits 6-7
        0x00,       //
        0x20,       // a duty byte of 0x20
        0x00,       //
        0x85,       // vibrato 5
        0x00,       //
        0x93, 0x40, // the track's sample at note 0x40, volume 3
        0x00,       //
        0xA4, 0x41, // sample map entry 0x41, on the next track, volume 4
        0x00,       //
        0xB5, 0x42, // sample map entry 0x42, volume 5
        0x00,       //
        0xC6,       // restart at volume 6
        0x00,       //
        0xD7,       // volume 7, without a note
        0x00,       //
        0xE8, 0x43, // PSG note 0x43, volume 8
        0x00,       //
        0xF0, 0x77, // the PSG's volume on each side
        0x00,       //
        0xF1, 0x21, // a bend of 1 sixteenth on the next track
        0x00,       //
        0xF2, 0x1E, // a bend of -2 sixteenths
        0x00,       //
        0xF3, 0x22, // a bend of the note by 2 sixteenths
        0x00,       //
        0xF6,       // does nothing
        0x00,       //
        0xF8,       // a fade of this track
        0x00,       //
        0xF9,       // a fade of the next track
        0x00,       //
        0xFB,       // a release
        0x00,       //
        0xFC,       // a rest
        0x00,       //
        0xFF,       // a stop of the song
    };
    const Decoded decoded = DecodeSequence(bytes, Offsets(r, 0), r);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque && items.size() == 40, "Dungeon Dice Monsters' items: " + std::to_string(items.size()));
    if (items.size() == 40)
    {
        Check(items[1].kind == Kind::kProgram && items[1].index == 1 && items[1].value.value == 2 * kOne, "a wave");
        Check(items[3].kind == Kind::kProgram && items[3].index == 0 && items[3].value.value == 0x40 * kOne, "a duty");
        Check(items[5].index == 0 && items[5].value.value == 0x20 * kOne, "a duty byte");
        Check(items[7].kind == Kind::kLfoDepth && items[7].value.value == 5 * kOne, "a vibrato");
        Check(items[9].kind == Kind::kNote && items[9].key == 0x40 && items[9].index == 1 && !items[9].sample,
              "9x, the track's sample at a note");
        Check(items[11].kind == Kind::kNote && items[11].sample == 0x41 && items[11].index == 1,
              "Ax, on the next track");
        Check(items[13].kind == Kind::kNote && items[13].sample == 0x42 && items[13].index == 0, "Bx");
        Check(items[15].kind == Kind::kVolume && items[15].index == 0 && items[15].value.value == 6 * kOne, "Cx");
        Check(items[17].kind == Kind::kVolume && items[17].index == 1 && items[17].value.value == 7 * kOne, "Dx");
        Check(items[19].kind == Kind::kNote && items[19].key == 0x43 && items[19].index == 0 && !items[19].sample,
              "Ex");
        Check(items[21].kind == Kind::kOther && items[21].encoding.opcode == 0xF0, "the PSG's volume");
        Check(items[23].kind == Kind::kPitchBend && items[23].index == 2 && items[23].value.value == kOne / 32, "F1");
        Check(items[25].kind == Kind::kPitchBend && items[25].index == 1 && items[25].value.value == -2 * (kOne / 32),
              "F2");
        Check(items[27].kind == Kind::kPitchBend && items[27].index == 0 && items[27].value.value == 2 * (kOne / 32),
              "F3, a bend in 16ths of a semitone");
        Check(items[29].kind == Kind::kOther && items[29].encoding.opcode == 0xF6, "F6 does nothing");
        Check(items[31].kind == Kind::kRelease && items[31].index == 1, "F8");
        Check(items[33].kind == Kind::kRelease && items[33].index == 2, "F9");
        Check(items[35].kind == Kind::kRelease && items[35].index == 0, "FB");
        Check(items[37].kind == Kind::kNoteOff, "FC");
        Check(items[39].kind == Kind::kEnd && items[39].index == 2, "FF stops the song");
        Check(EncodesBack(decoded, bytes, r), "Dungeon Dice Monsters' items encode back");
    }

    // FA's two bytes stay in the loop point. Only a song loop reads them: a delay and a command.
    {
        const std::vector<uint8_t> loop = {
            0x00,             //
            0xFA, 0x04, 0xFC, // a loop point: a song loop reads a delay of 4 and a rest there
            0x00,             //
            0xE1, 0x20,       //
            0x00,             //
            0xFE,             // a loop of the song
        };
        const Decoded looped = DecodeSequence(loop, Offsets(r, 0), r);
        Check(!looped.opaque && looped.sequence.items[1].kind == Kind::kLoopStart &&
                  looped.sequence.items[1].bytes == std::vector<uint8_t>({0x04, 0xFC}) &&
                  looped.sequence.items.size() == 6,
              "FA's bytes stay in the loop point");
        Check(EncodesBack(looped, loop, r), "FA's bytes encode back");

        // A command of two bytes there would read past the loop point.
        std::vector<uint8_t> past = loop;
        past[3] = 0xE1;
        const Decoded overlapped = DecodeSequence(past, Offsets(r, 0), r);
        Check(overlapped.opaque, "a song loop that reads past FA's bytes stays as data");
    }

    // F5 sets a duty or a wave from the delay after it, and counts that as the call's first command.
    {
        const std::vector<uint8_t> call = {
            0x00,                   // offset 0
            0xF5, 0x08, 0x00, 0x01, // a call of 1 command at offset 8, and 1 more for F5's setting
            0x10,                   // offset 5: the delay after the call and F5's setting
            0xFD,                   // offset 6
            0x00,                   // offset 7: unreached
            0x02,                   // offset 8: a delay
            0xFC,                   // offset 9: a rest
            0x03,                   // offset 10: a delay that the call's end leaves unread
        };
        const Decoded decoded_call = DecodeSequence(call, Offsets(r, 0), r);
        Check(!decoded_call.opaque && decoded_call.sequence.items[1].kind == Kind::kCall &&
                  decoded_call.sequence.items[1].key == 0xF5 && decoded_call.sequence.items[1].value.value == kOne,
              "F5 is a call: " + decoded_call.reason);
        Check(EncodesBack(decoded_call, call, r), "F5 encodes back");
    }
}

// A call counts from its track's start in the song table.
void TestCalls()
{
    const Revision r = Revision::kWct2004;
    const std::vector<uint8_t> bytes = {
        0xFD,                   // offset 0: unreached
        0x00,                   // offset 1: track 0 starts here
        0x9F, 0x06, 0x00, 0x02, // offset 2: a call of 2 commands at offset 6 from the track's start: offset 7
        0x01,                   // offset 6: the delay after the call
        0xFD,                   // offset 7: a delay of 2 bytes, from the call's fragment, that runs past the bytes
    };
    const Decoded bad = DecodeSequence(bytes, Offsets(r, 1), r);
    Check(bad.opaque, "a call's fragment that runs past the bytes stays as data");
    Check(EncodesBack(bad, bytes, r), "the song kept as data gives its bytes back");

    const std::vector<uint8_t> good = {
        0x7F,                   // offset 0: unreached
        0x00,                   // offset 1: every track starts here
        0x9F, 0x08, 0x00, 0x02, // offset 2: a call of 2 commands, from offset 1 + 8 = 9
        0x05,                   // offset 6: the delay after the call
        0xFD,                   // offset 7: the end
        0x7E,                   // offset 8: unreached
        0x01,                   // offset 9: a delay
        0xC1,                   // offset 10: volume 1
        0x02,                   // offset 11: a delay
        0xC2,                   // offset 12: volume 2, the call's last command
        0x00,                   // offset 13: unreached, for the track goes back without reading a delay here
    };
    const Decoded decoded = DecodeSequence(good, Offsets(r, 1), r);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque, "the call's song reads: " + decoded.reason);
    Check(items.size() == 11, "the call's items: " + std::to_string(items.size()));
    if (items.size() == 11)
    {
        Check(items[0].kind == Kind::kData && items[0].bytes == std::vector<uint8_t>{0x7F}, "bytes before the track");
        Check(items[2].kind == Kind::kCall && items[2].key == 0x9F && items[2].index == 0 &&
                  items[2].value.value == 2 * kOne && !items[2].target_outside && items[2].target == 6,
              "the call names its fragment's first item");
        Check(items[5].kind == Kind::kData && items[5].bytes == std::vector<uint8_t>{0x7E}, "bytes between");
        Check(items[10].kind == Kind::kData && items[10].bytes == std::vector<uint8_t>{0x00},
              "the track goes back after the call's last command");
        Check(decoded.entries == std::vector<uint32_t>(16, 1), "the tracks' entries");
        Check(EncodesBack(decoded, good, r), "the call's song encodes back");

        // An item before the fragment moves the target's offset, and the call follows.
        Decoded edited = decoded;
        Command wait;
        wait.kind = Kind::kWait;
        Command volume;
        volume.kind = Kind::kVolume;
        volume.value.value = 3 * kOne;
        edited.sequence.items.insert(edited.sequence.items.begin() + 3, {wait, volume});
        edited.sequence.items[2].target = 8;
        const std::vector<uint8_t> encoded = EncodeSequence(edited.sequence, edited.entries, r).bytes;
        Check(encoded[3] == 0x0A && encoded[4] == 0, "the call's offset follows its target");
    }

    // A call of 0 commands that sets a duty first never reads its fragment, and keeps its offset as it is.
    const std::vector<uint8_t> none = {0x00, 0x90, 0x34, 0x12, 0x00, 0x00, 0xFD};
    const Decoded empty = DecodeSequence(none, Offsets(r, 0), r);
    Check(!empty.opaque && empty.sequence.items[1].target_outside && empty.sequence.items[1].target == 0x1234,
          "a call without commands keeps its offset");
    Check(EncodesBack(empty, none, r), "a call without commands encodes back");

    // Two tracks that start apart read a call with other targets.
    const std::vector<uint8_t> shared = {
        0x00,                   // offset 0: most tracks start here
        0xC1,                   //
        0x00,                   // offset 2: track 1 starts here
        0x9F, 0x09, 0x00, 0x01, // a call of 1 command, from offset 9 for most tracks and 11 for track 1
        0x00,                   // offset 7
        0xFD,                   //
        0x00,                   // offset 9
        0xC1,                   //
        0x00,                   // offset 11
        0xC2,                   //
    };
    std::vector<uint16_t> apart = Offsets(r, 0);
    apart[1] = 2;
    const Decoded split = DecodeSequence(shared, apart, r);
    Check(split.opaque, "a call that two tracks read with other targets stays as data");
}

// The encoder's refusals, and the usual bytes of new commands.
void TestNewCommands()
{
    const auto command = [](Kind kind, int64_t value, uint8_t index = 0)
    {
        Command item;
        item.kind = kind;
        item.value.value = value * kOne;
        item.index = index;
        return item;
    };

    // The usual delays of each form.
    ntdlib::commands::Sequence waits;
    Command end;
    end.kind = Kind::kEnd;
    const auto wait_of = [&](int64_t frames, Revision r)
    {
        waits.items = {command(Kind::kWait, frames), end};
        const std::vector<uint32_t> entries(size_t(konami::TrackCount(r)), 0);
        std::vector<uint8_t> bytes = EncodeSequence(waits, entries, r).bytes;
        bytes.pop_back();
        return bytes;
    };
    const auto bytes_of = [](std::initializer_list<int> list)
    {
        std::vector<uint8_t> bytes;
        for (const int b : list)
        {
            bytes.push_back(uint8_t(b));
        }
        return bytes;
    };

    Check(wait_of(0xDF, Revision::kUltimateMasters) == bytes_of({0xDF}), "a delay of a byte");
    Check(wait_of(0xE0, Revision::kUltimateMasters) == bytes_of({0xE0, 0xE0}), "Ultimate Masters' Ex yy");
    Check(wait_of(0x1000, Revision::kUltimateMasters) == bytes_of({0xF0, 0x00, 0x10}), "Ultimate Masters' Fx lo hi");
    Check(wait_of(0x1FFF, Revision::kWct2004) == bytes_of({0xFF, 0xFF}), "WCT 2004's bb yy");
    Check(wait_of(0xEF, Revision::kEternalDuelist) == bytes_of({0xEF}), "Eternal Duelist's delay of a byte");
    Check(wait_of(0xF0, Revision::kEternalDuelist) == bytes_of({0xF0, 0xF0}), "Eternal Duelist's Fx yy");

    // A new pan takes level 15 on each side, less the position on the side that it moves away from.
    Command pan = command(Kind::kPan, 0);
    Check(BytesOfCommand(pan, Revision::kWct2004) == bytes_of({0xF0, 0xFF}), "a new pan at the centre");
    pan.value.value = ToUnit(4, konami::ScaleOf(Kind::kPan, Revision::kWct2004));
    Check(BytesOfCommand(pan, Revision::kWct2004) == bytes_of({0xF0, 0xBF}), "a new pan to the right");
    pan.value.value = ToUnit(-4, konami::ScaleOf(Kind::kPan, Revision::kWct2004));
    Check(BytesOfCommand(pan, Revision::kWct2004) == bytes_of({0xF0, 0xFB}), "a new pan to the left");

    Command note = command(Kind::kNote, 0);
    note.key = 0x3C;
    note.velocity = 9 * kOne;
    Check(BytesOfCommand(note, Revision::kUltimateMasters) == bytes_of({0xD8, 9, 0x3C}),
          "a PSG note with a volume's byte");
    note.velocity = 5 * kOne;
    Check(BytesOfCommand(note, Revision::kUltimateMasters) == bytes_of({0xD5, 0x3C}), "a short PSG note");
    Check(BytesOfCommand(note, Revision::kWct2004) == bytes_of({0xD5, 0x3C}), "WCT 2004's PSG note");
    Check(BytesOfCommand(note, Revision::kDungeonDiceMonsters) == bytes_of({0xE5, 0x3C}),
          "Dungeon Dice Monsters' note");

    note.sample = 0x20;
    note.key = 0;
    Check(BytesOfCommand(note, Revision::kWct2004) == bytes_of({0xA5, 0x20}), "a sample note");
    note.key = 0xFE;
    Check(BytesOfCommand(note, Revision::kWct2004) == bytes_of({0xB5, 0x20, 0xFE}), "a sample note with semitones");
    note.sample = 0x234;
    note.key = 0;
    Check(BytesOfCommand(note, Revision::kLiveStart) == bytes_of({0xA5, 0xF2, 0x34}), "Live Start's sample number");
    Check(Throws(note, Revision::kWct2004), "a sample past a byte in WCT 2004");

    Check(BytesOfCommand(command(Kind::kVolume, 0x20), Revision::kUltimateMasters) == bytes_of({0xC8, 0x20}),
          "a volume of a byte");
    Check(Throws(command(Kind::kVolume, 0x20), Revision::kWct2004), "a volume past 15 in WCT 2004");
    Check(BytesOfCommand(command(Kind::kVolume, 3, 1), Revision::kDungeonDiceMonsters) == bytes_of({0xD3}), "Dx");

    Check(BytesOfCommand(command(Kind::kNoteOff, 0), Revision::kWct2004) == bytes_of({0xE0}), "a rest");
    Check(BytesOfCommand(command(Kind::kNoteOff, 0), Revision::kDungeonDiceMonsters) == bytes_of({0xFC}), "FC");

    Command bend = command(Kind::kPitchBend, 0);
    bend.value.value = 0x20 * (kOne / 64);
    Check(BytesOfCommand(bend, Revision::kUltimateMasters) == bytes_of({0xF2, 0x60}), "a short bend");
    bend.value.value = 0x41 * (kOne / 64);
    Check(BytesOfCommand(bend, Revision::kUltimateMasters) == bytes_of({0xF2, 0x84, 0x41}), "a long bend");
    Check(BytesOfCommand(bend, Revision::kWct2004) == bytes_of({0xF2, 0x81}), "WCT 2004's bend of a byte");

    Check(BytesOfCommand(command(Kind::kAttack, 1), Revision::kWct2004) == bytes_of({0xF4, 1}), "F4");
    Check(Throws(command(Kind::kAttack, 1), Revision::kUltimateMasters), "the Ultimate Masters revision has no F4");
    Check(Throws(command(Kind::kAttack, 1), Revision::kEternalDuelist), "Eternal Duelist reads F4 as a vibrato");
    Check(Throws(command(Kind::kTempo, 120), Revision::kWct2004), "the driver has no tempo");

    // A new loop point takes the bytes that the driver skips.
    Check(konami::NewLoopPoint(0, Revision::kUltimateMasters).bytes.empty(), "a PSG track's loop point");
    Check(konami::NewLoopPoint(4, Revision::kUltimateMasters).bytes.size() == 2, "a sample track's loop point");
    Check(konami::NewLoopPoint(4, Revision::kRaveMaster).bytes.empty(), "Rave Master's loop point");
    Check(konami::NewLoopPoint(0, Revision::kDungeonDiceMonsters).bytes.size() == 2, "FA's bytes");

    // A sequence that the driver wouldn't read as its items say.
    const Revision r = Revision::kWct2004;
    const std::vector<uint32_t> entries(16, 0);
    const auto refused = [&](std::vector<Command> items)
    {
        ntdlib::commands::Sequence sequence;
        sequence.items = std::move(items);
        try
        {
            static_cast<void>(EncodeSequence(sequence, entries, r));
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        return false;
    };
    Check(refused({end}), "a track that starts with a command");
    Check(refused({command(Kind::kWait, 1), command(Kind::kWait, 1), end}), "two waits in a row");
    Command data;
    data.kind = Kind::kData;
    data.bytes = {0xFD};
    Check(refused({command(Kind::kWait, 1), data}), "a track that reaches data");
    Check(refused({command(Kind::kWait, 1), command(Kind::kVolume, 1)}), "a track that runs past the bytes");
    Check(!refused({command(Kind::kWait, 1), end, data}), "data that no track reaches");
}

// A song of a cart: its bytes end with the last that a track reads, or at the next song where that lies close.
void TestItemOffsets()
{
    // A wait, a rest, a wait and an end, as bytes, then a command that the revision lacks.
    const ntdlib::commands::Sequence sequence = {
        {Command{.kind = Kind::kWait, .value = {.value = 2 * kOne}}, Command{.kind = Kind::kData, .bytes = {1, 2, 3}},
         Command{.kind = Kind::kNoteOff}, Command{.kind = Kind::kWait, .value = {.value = 0}},
         Command{.kind = Kind::kEnd}},
        {}};
    const std::vector<uint32_t> offsets = konami::ItemOffsets(sequence, Revision::kWct2004);
    Check(offsets == std::vector<uint32_t>{0, 1, 4, 5, 6, 7}, "each item's offset, then the end");

    // The same items as bytes give the same offsets.
    const konami::Encoded encoded =
        konami::EncodeSequence({{Command{.kind = Kind::kWait}, Command{.kind = Kind::kNoteOff},
                                 Command{.kind = Kind::kWait}, Command{.kind = Kind::kEnd}},
                                {}},
                               std::vector<uint32_t>{0}, Revision::kWct2004);
    const std::vector<uint32_t> again =
        konami::ItemOffsets({{Command{.kind = Kind::kWait}, Command{.kind = Kind::kNoteOff},
                              Command{.kind = Kind::kWait}, Command{.kind = Kind::kEnd}},
                             {}},
                            Revision::kWct2004);
    Check(again == encoded.offsets, "the offsets are the encoder's");

    bool refused = false;
    try
    {
        konami::ItemOffsets({{Command{.kind = Kind::kMainVolume}}, {}}, Revision::kKitaro);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    Check(refused, "a command that the revision lacks has no offset");
}

void TestSongsOfCart()
{
    using ntdlib::testing::agb_files::kRomBase;
    const Revision r = Revision::kEternalDuelist;
    const uint32_t table = 0x100;
    konami::DriverInfo info;
    info.revision = r;
    info.song_table = kRomBase + table;
    info.song_count = 3;

    std::vector<uint8_t> image(0x400, 0xEE);
    const auto put_song = [&](int song, uint32_t base, std::vector<uint16_t> offsets)
    {
        const uint32_t entry = table + uint32_t(song) * konami::SongEntrySize(r);
        const uint32_t address = kRomBase + base;
        for (int i = 0; i < 4; i++)
        {
            image[entry + uint32_t(i)] = uint8_t(address >> (8 * i));
        }
        offsets.resize(10, 0);
        for (std::size_t t = 0; t < offsets.size(); t++)
        {
            image[entry + 4 + 2 * t] = uint8_t(offsets[t]);
            image[entry + 5 + 2 * t] = uint8_t(offsets[t] >> 8);
        }
    };
    const auto put_bytes = [&](uint32_t at, std::vector<uint8_t> bytes)
    {
        std::copy(bytes.begin(), bytes.end(), image.begin() + at);
    };

    // Song 0's tracks end 2 bytes before song 1. Song 1's end at song 2. No song follows song 2.
    put_song(0, 0x180, {});
    put_bytes(0x180, {0x00, 0xFD, 0x00, 0xFD});
    put_song(1, 0x184, {});
    put_bytes(0x184, {0x01, 0xE0, 0x00, 0xFD});
    put_song(2, 0x188, {});
    put_bytes(0x188, {0x02, 0xFD, 0xAA, 0xBB});

    konami::Rom rom;
    rom.Assign(image);
    const Decoded first = konami::DecodeSong(rom, info, 0);
    Check(first.sequence.items.size() == 3 && first.sequence.items[2].kind == Kind::kData &&
              first.sequence.items[2].bytes == std::vector<uint8_t>({0x00, 0xFD}),
          "the bytes before the next song count as the song's data");
    const Decoded second = konami::DecodeSong(rom, info, 1);
    Check(second.sequence.items.size() == 4 && !second.opaque, "a song without a gap");
    const Decoded last = konami::DecodeSong(rom, info, 2);
    Check(last.sequence.items.size() == 2, "the last song ends with its last byte that a track reads");

    konami::DriverInfo missing = info;
    missing.song_table = kRomBase + 0x300;
    rom.Assign(std::vector<uint8_t>(0x400, 0));
    bool threw = false;
    try
    {
        static_cast<void>(konami::DecodeSong(rom, missing, 0));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "an empty entry has no bytes");
}

} // namespace

int main()
{
    TestUltimateMasters();
    TestSampleNotes();
    TestOlderRevisions();
    TestDungeonDice();
    TestCalls();
    TestNewCommands();
    TestItemOffsets();
    TestSongsOfCart();

    return xsflib::testing::Summary("Konami's commands");
}
