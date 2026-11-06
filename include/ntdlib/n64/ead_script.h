// SPDX-License-Identifier: MIT

// The sequences of Nintendo EAD's sound engines for the Nintendo 64. A sequence is a program in three levels. The
// script of the sequence starts at offset 0 and starts channels. A channel's script sets the channel's sound and starts
// note layers. A layer's script plays notes. The levels have separate commands. The commands at 0xF2 and up that
// change the flow of a script are shared. Offsets in a sequence are big-endian and count from its start. A variable
// length has one byte, or two when the top bit of the first is set: 15 bits, the high bits first.
//
// The early engine (Super Mario 64, Wave Race 64 and Mario Kart 64) and the later engine (Ocarina of Time, Majora's
// Mask and the two Pokémon Stadiums) share the notes and most commands. The later engine renames some and adds others.
//
// ReadScriptCommand reads one command as the engine's player does. WalkScript follows every path from a sequence's
// start that doesn't depend on the values the scripts compute. It gives the commands it reached and the offsets where
// it found a command that the engine doesn't have.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace ntdlib::n64
{

// The engine that a sequence plays on.
enum class EadEngine : uint8_t
{
    kEarly,      // Super Mario 64's American and Japanese builds
    kMiddle,     // Wave Race 64 and Mario Kart 64: the early engine's files with more commands
    kLater,      // Ocarina of Time and Pokémon Stadium
    kLaterMajora // Majora's Mask and Pokémon Stadium 2
};

enum class ScriptLevel : uint8_t
{
    kSequence,
    kChannel,
    kLayer
};

// What a command does, whichever opcode holds it on an engine. Arguments are in `ScriptCommand::args`, in the order of
// the command's bytes.
enum class ScriptOp : uint8_t
{
    kUnknown,

    // Flow, on every level. A jump's or a call's target is an offset from the sequence's start. A branch tests the
    // script's value.
    kEnd,
    kDelay1,
    kDelay,
    kCall,
    kJump,
    kBranchIfZero,
    kBranchIfNegative,
    kBranchIfNotNegative,
    kLoop,
    kLoopEnd,
    kBreak,
    kHang, // the early engine's channel script stops, and its layers go on

    // Values, on the sequence's and the channels' levels.
    kLoadValue,
    kAndValue,
    kSubtractValue,
    kRandom,
    kStoreToSequence,   // writes the value plus args[0] to args[1] in the sequence
    kLoadFromSequence,  // reads args[0] plus the value from the sequence
    kTestChannel,       // low bits: the channel
    kReadIo,            // low bits: the port
    kWriteIo,           // low bits: the port
    kSubtractIo,        // low bits: the port
    kWriteOtherChannel, // low bits: the channel; args[0] is the port
    kReadOtherChannel,  // low bits: the channel; args[0] is the port

    // The sequence's level.
    kAllocateNotes,
    kFreeNotes,
    kSetTranspose,
    kAddTranspose,
    kTempo,
    kAddTempo,
    kVolume,
    kAddVolume,       // the early engine
    kVolumeMode,      // the later engine
    kVolumeScale,     // the later engine
    kInitChannels,    // args[0] is a mask
    kDisableChannels, // args[0] is a mask
    kMuteScale,
    kMute,
    kMuteBehavior,
    kShortVelocityTable,
    kShortGateTable,
    kNoteAllocation,
    kStartChannel,    // low bits: the channel; args[0] is the script
    kStartChannelRel, // low bits: the channel; args[0] is a distance from the command's end
    kLoadSequence,    // low bits: the port; args[0] is a sequence's number; args[1] the place for its bytes
    kLoadResource,    // low bits: the port; args[0] is its kind; args[1] its number
    kRunSequence,     // args[0] is the player; args[1] the sequence's number
    kStopSequence,    // the later engine's end of a script without its release
    kScriptCounter,
    kDynamicJumpTable, // args[0] is a table of offsets that the value indexes

    // The channels' level.
    kInstrument,
    kShortNotes,
    kLargeNotes,
    kDynamicTable,
    kDynamicTableLookup,
    kFont,
    kFontInstrument,
    kPan,
    kPanWeight,
    kVolumeExpression,
    kChannelVolume,
    kFrequencyScale,
    kBend,
    kBendFine,
    kChannelTranspose,
    kEnvelope,
    kReleaseRate,
    kVibratoRate,
    kVibratoDepth,
    kVibratoRateGradient,
    kVibratoDepthGradient,
    kVibratoDelay,
    kVibratoReset,
    kUpdatesPerFrame, // the early engine: unused
    kReverb,
    kReverbIndex,
    kBookOffset,
    kParameters,
    kLoadParameters,
    kNotePriority,
    kStopChannel,
    kStereoEffects,
    kSustain,
    kGain,
    kDynamicCall,
    kFilter,
    kLoadFilter,
    kFreeFilter,
    kCombFilter,
    kLoadPointer,
    kStorePointer,
    kAddPointer,
    kRandomPointer,
    kPointerFromSequence,
    kPointerToTable,
    kTableToPointer,
    kTableValue,
    kRandomToPointer,
    kRandomVelocity,
    kRandomGate,
    kChannelDelayShort,  // low bits: the delay
    kLoadSample,         // low bits: the port, and in bit 3 the kind of sound
    kStartLayer,         // low bits: the layer; args[0] is the script
    kStartLayerRel,      // low bits: the layer; args[0] is a distance from the command's end
    kDynamicStartLayer,  // low bits: the layer
    kFreeLayer,          // low bits: the layer
    kTestLayer,          // low bits: the layer
    kSetNotePriorityLow, // the early engine: low bits: the note's priority
    kStartOtherChannel,  // low bits: the channel; args[0] is the script
    kStopOtherChannel,   // low bits: the channel
    kSampleStart,        // Majora's Mask: the point in a sample that notes start from
    kChannelArgument,    // an argument that a later command of the channel reads

    // The layers' level. A note's low 6 bits hold its pitch. args holds what its form reads: delay, velocity and gate
    // time, -1 where the form reads nothing.
    kNote,
    kLayerDelay,
    kShortVelocity,
    kLayerTranspose,
    kShortDelay,
    kLegato,
    kNoLegato,
    kLayerInstrument,
    kPortamento,
    kNoPortamento,
    kShortGate,
    kNotePan,
    kLayerEnvelope,
    kNoDrumPan,
    kLayerStereo,
    kLayerBendFine,
    kLayerReleaseRate,
    kShortVelocityFromTable, // low bits: the index
    kShortGateFromTable,     // low bits: the index
    kLayerUnknownF0,
    kSurroundEffect
};

// A command as an engine's player reads it.
struct ScriptCommand
{
    friend bool operator==(const ScriptCommand&, const ScriptCommand&) = default;

    uint32_t offset = 0;
    uint32_t next = 0; // the offset after the command
    uint8_t opcode = 0;
    uint8_t low_bits = 0; // the part of the opcode that holds a number, where the command has one
    ScriptOp op = ScriptOp::kUnknown;

    // The arguments in the order of the bytes. An unused one holds -1. Offsets and 16-bit fields come out as their
    // unsigned values, and 8-bit signed fields as their signed values. kParameters holds its 8 bytes in `bytes`.
    std::array<int32_t, 3> args = {-1, -1, -1};
    std::array<uint8_t, 8> bytes = {};
};

// Reads the command at `offset`. On a channel's level `large_notes` doesn't matter. On a layer's level it gives the
// form of the notes, by the channel's kLargeNotes and kShortNotes. A command that runs past `sequence` throws
// xsflib::FormatError. An opcode that the engine doesn't have comes out as ScriptOp::kUnknown.
ScriptCommand ReadScriptCommand(std::span<const uint8_t> sequence, uint32_t offset, EadEngine engine, ScriptLevel level,
                                bool large_notes);

// A command that WalkScript reached, with the place in the script's structure.
struct WalkedCommand
{
    friend bool operator==(const WalkedCommand&, const WalkedCommand&) = default;

    ScriptCommand command;
    ScriptLevel level = ScriptLevel::kSequence;
    int8_t channel = -1;
    int8_t layer = -1;
};

struct ScriptWalk
{
    std::vector<WalkedCommand> commands; // in the order of their offsets
    std::vector<uint32_t> bad_offsets;   // where a path met an opcode that the engine lacks, or left the sequence
    std::map<uint32_t, ScriptLevel> bad_levels; // the level of the script at each of those offsets
    std::size_t notes = 0;

    // The tables that dynamic commands index. Their lengths aren't in the bytes. A path through such a table isn't
    // followed.
    std::vector<uint32_t> dynamic_tables;
};

// Follows every path through the sequence.
ScriptWalk WalkScript(std::span<const uint8_t> sequence, EadEngine engine);

} // namespace ntdlib::n64
