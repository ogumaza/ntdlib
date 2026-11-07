// SPDX-License-Identifier: MIT

// MP2K's track commands in the common form of `ntdlib::commands`. The sequencer (sequencer.h) reads each command
// through ReadPlayedCommand, and the decoder reads the commands the same way. A song's sequence is the bytes from its
// first track's start up to its header, where the tools put its tracks. Each track starts at an entry. The decoder
// follows every jump, pattern call, repeat and conditional jump from the entries. In the bytes that no track reaches,
// it reads the runs of commands that end cleanly. The other bytes stay as data. The encoder writes the items back, and
// gives the same bytes for a song that the decoder read.
//
// A command's bytes can leave out what the track's state gives. A byte below 0x80 repeats the running status: the last
// command from 0xBD up (VOICE) that the track ran, with that byte as its first argument (Encoding::repeated). A note
// and an end of a tie can leave out their last arguments, and then take the track's last key and velocity
// (Encoding::arguments). The decoder reads each command with the state of the tracks that reach it. Where two tracks
// reach a command with states that give it other values, the decoder keeps the song as data. The encoder writes the
// items as they say. Their bytes play as the items where each item that leaves something out follows commands that
// leave the track in the state that the item holds: the decoder then gives the items back.
//
// A note reads up to `note_arguments` bytes after its code: the SDK's library reads a key, a velocity and an extra
// length (3), and Metroid Fusion's sequencer a key and a velocity alone (2, NoteArguments). The decoder and the encoder
// take the driver's count. With 2, a note's length is the length of its code, and a byte below 0x80 after a note's
// velocity starts a note of the running status.
//
// A note's length is the length of its code (ClockLength) plus its extra length. A note of length 0 sounds until an
// end of a tie (kNoteOff) with its key: MP2K's TIE is the code of length 0. A wait's length has to be one of the codes'
// lengths, or come from the extended command of a long wait. Targets are the console's addresses: a target outside the
// song's bytes keeps its address (Command::target_outside).
//
// The values convert to the units of commands/units.h: the tempo is half the BPM, a key shift and a bend range are in
// semitones, the pan and the bend take 0x40 off, and the bend is in 64ths of its range. A fine tune is in 64ths of a
// semitone, with 0x40 the centre. The LFO's values stay in the driver's units, for the LFO runs in ticks: its speed in
// 256ths of a cycle each tick, its delay in ticks, and its depth in the driver's steps. MEMACC's bytes of the memory
// area are variables: their numbers are the bytes' offsets.

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/song.h"

namespace ntdlib::mp2k
{

// What a track keeps for the commands that leave bytes out: the running status, and the last key and velocity.
struct RunningState
{
    friend auto operator<=>(const RunningState&, const RunningState&) = default;

    uint8_t running = 0; // the last command from kCmdVoice up, or 0 for none
    uint8_t key = 0;
    uint8_t velocity = 0;
};

// A command as the driver reads it. Its values are the console's, not the units of units.h, and a target is the
// console's address.
struct PlayedCommand
{
    commands::Command command;
    uint8_t code = 0;        // the command byte: the running status's for a repeated command
    uint32_t extended = 0;   // an extended command's address: the byte after 0xCD, or the repeated byte
    uint32_t next = 0;       // the address after the command
    bool valid = true;       // false for a byte below 0x80 without a running status: the driver ends the track there
    RunningState after = {}; // the track's state after the command
};

namespace detail
{

// The extended commands' argument bytes, or -1 for those that end the track.
inline constexpr int kExtendedArguments[kXcmdCount] = {-1, 4, 1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 4};

// The kinds of the commands from 0xBA (PRIO) to 0xC8 (TUNE), with kOther where the code is something else.
commands::Kind KindOfCode(uint8_t code);

// The kind of extended command `x`, or kEnd where it ends the track.
commands::Kind KindOfExtended(uint8_t x);

} // namespace detail

// Reads the command at address `at` as the driver reads it, with the track's state `state`. `byte_at(address)` gives
// the byte at an address. A note reads up to `note_arguments` bytes: the SDK's library reads a key, a velocity and an
// extra length, and Metroid Fusion's sequencer only a key and a velocity.
template <typename ByteAt>
PlayedCommand ReadPlayedCommand(const ByteAt& byte_at, uint32_t at, const RunningState& state, int note_arguments = 3)
{
    using commands::Kind;
    using commands::Source;

    PlayedCommand played;
    played.after = state;
    commands::Command& command = played.command;
    const auto next_byte = [&byte_at, &at]
    {
        return static_cast<uint8_t>(byte_at(at++));
    };
    const auto u32 = [&next_byte]
    {
        uint32_t value = next_byte();
        value |= static_cast<uint32_t>(next_byte()) << 8;
        value |= static_cast<uint32_t>(next_byte()) << 16;
        value |= static_cast<uint32_t>(next_byte()) << 24;
        return value;
    };

    // A byte below 0x80 repeats the running status, with that byte as its first argument.
    uint8_t code = static_cast<uint8_t>(byte_at(at));
    if (code < 0x80)
    {
        if (state.running < kCmdVoice)
        {
            played.valid = false;
            played.code = code;
            played.next = at;
            return played;
        }

        code = state.running;
        command.encoding.repeated = true;
    }
    else
    {
        at++;
        if (code >= kCmdVoice)
        {
            played.after.running = code;
        }
    }
    played.code = code;

    // A note, or an end of a tie: the arguments that are there, each below 0x80.
    if (code >= kCmdTie || code == kCmdEot)
    {
        const int most = code == kCmdEot ? 1 : note_arguments;
        int count = 0;
        uint8_t arguments[3] = {};
        while (count < most && byte_at(at) < 0x80)
        {
            arguments[count++] = next_byte();
        }
        if (count >= 1)
        {
            played.after.key = arguments[0];
        }
        if (count >= 2)
        {
            played.after.velocity = arguments[1];
        }

        command.key = played.after.key;
        if (code == kCmdEot)
        {
            command.kind = Kind::kNoteOff;
            if (count != 1)
            {
                command.encoding.arguments = static_cast<uint8_t>(count);
            }
        }
        else
        {
            command.kind = Kind::kNote;
            command.velocity = played.after.velocity;
            const int length_code = code - kCmdTie;
            const int extra = count == 3 ? arguments[2] : 0;
            command.value.value = ClockLength(length_code) + extra;
            const int usual = extra != 0 ? 3 : 2;
            if (count != usual)
            {
                command.encoding.arguments = static_cast<uint8_t>(count);
            }

            // The code of the longest length up to the note's length gives the usual code.
            int usual_code = 0;
            for (int i = 0; i < 49 && ClockLength(i) <= command.value.value; i++)
            {
                usual_code = i;
            }
            if (length_code != usual_code)
            {
                command.encoding.length_code = static_cast<uint8_t>(length_code);
            }
        }

        played.next = at;
        return played;
    }

    if (code <= kCmdLastWait)
    {
        command.kind = Kind::kWait;
        command.value.value = ClockLength(code - kCmdWait);
        played.next = at;
        return played;
    }

    switch (code)
    {
    case kCmdGoto:
        command.kind = Kind::kJump;
        command.target = u32();
        break;

    case kCmdPatt:
        command.kind = Kind::kCall;
        command.target = u32();
        break;

    case kCmdPend:
        command.kind = Kind::kReturn;
        break;

    case kCmdRept:
        command.kind = Kind::kRepeat;
        command.value.value = next_byte();
        command.target = u32();
        break;

    case kCmdMemAcc:
        {
            // Operations 0 to 5 change a byte of the memory area, with the value or with the byte that the value names.
            // Operations 6 to 17 compare the byte in the same two ways, and jump.
            const uint8_t op = next_byte();
            const uint8_t index = next_byte();
            const uint8_t value = next_byte();
            if (op > 17)
            {
                command.kind = Kind::kOther;
                command.encoding.opcode = kCmdMemAcc;
                command.bytes = {op, index, value};
                break;
            }

            command.index = index;
            command.value.value = value;
            if (op < 6)
            {
                command.kind = static_cast<Kind>(static_cast<int>(Kind::kSetVariable) + op % 3);
                command.value.source = op >= 3 ? Source::kVariable : Source::kValue;
                break;
            }

            command.kind = static_cast<Kind>(static_cast<int>(Kind::kJumpIfEqual) + op % 6);
            command.value.source = op >= 12 ? Source::kVariable : Source::kValue;
            command.target = u32();
            break;
        }

    case kCmdTempo:
    case kCmdKeySh:
    case kCmdPrio:
    case kCmdVoice:
    case kCmdVol:
    case kCmdPan:
    case kCmdBend:
    case kCmdBendR:
    case kCmdLfoS:
    case kCmdLfoDl:
    case kCmdMod:
    case kCmdModT:
    case kCmdTune:
        command.kind = detail::KindOfCode(code);
        command.value.value = code == kCmdKeySh ? static_cast<int8_t>(next_byte()) : next_byte();
        break;

    case kCmdPort:
        command.kind = Kind::kRegister;
        command.index = next_byte();
        command.value.value = next_byte();
        break;

    case kCmdXcmd:
        {
            played.extended = at;
            const uint8_t x = next_byte();
            command.kind = detail::KindOfExtended(x);
            switch (x < kXcmdCount ? detail::kExtendedArguments[x] : -1)
            {
            case 1:
                command.value.value = next_byte();
                break;
            case 2:
                {
                    const uint32_t low = next_byte();
                    command.value.value = low | static_cast<uint32_t>(next_byte()) << 8;
                    break;
                }
            case 4:
                command.value.value = u32();
                break;
            default:
                break;
            }
            if (command.kind == Kind::kEnd || x == kXcmdWait)
            {
                command.encoding.opcode = static_cast<uint16_t>(kCmdXcmd << 8 | x);
            }
            break;
        }

    default:
        // kCmdFine, and the codes that the driver's command table gives its handler.
        command.kind = Kind::kEnd;
        if (code != kCmdFine)
        {
            command.encoding.opcode = code;
        }
        break;
    }

    played.next = at;
    return played;
}

// What the decoder made of a song's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item where each track starts

    // Commands that overlap, a target or an entry inside a command, or a command that two tracks read with other
    // values. The decoder then keeps the whole song as data, split at the entries.
    bool opaque = false;
};

// Decodes `data`, a song's bytes from address `base`, with its tracks at the addresses `entries`. A note reads up to
// `note_arguments` bytes (NoteArguments). Throws std::invalid_argument for an entry outside `data`.
Decoded DecodeSequence(std::span<const uint8_t> data, uint32_t base, std::span<const uint32_t> entries,
                       int note_arguments = 3);

// Decodes the song with `header`: the bytes from its first track's start up to the header. Throws
// std::invalid_argument where the tracks don't lie in the ROM before the header.
Decoded DecodeSong(const Rom& rom, const SongHeader& header, int note_arguments = 3);

// Whether a track goes on to the next command after `command`. A jump, a repeat for ever and an end end it. A return
// goes on where the track runs it outside a pattern.
bool FallsThrough(const commands::Command& command);

// The bytes of a song, and each item's offset in them. `offsets` holds one more entry than the sequence has items: the
// end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence` at address `base`, with the console's values nearest its units, for a driver whose notes read up
// to `note_arguments` bytes (NoteArguments). Throws std::invalid_argument for a command that the bytes can't hold,
// such as a wait of no code's length, a value past the console's range, a target past the items, or a note that leaves
// out its arguments before a byte below 0x80. With 2 note arguments, a note of no code's length throws too: such a
// note takes a TIE and an end of the tie.
Encoded EncodeSequence(const commands::Sequence& sequence, uint32_t base, int note_arguments = 3);

// The scale of the value of a command of `kind` (commands::UnitOf). A note's velocity takes the unchanged scale.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::mp2k
