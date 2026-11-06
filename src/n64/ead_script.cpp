// SPDX-License-Identifier: MIT

#include "ntdlib/n64/ead_script.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

// The fields of a command, one letter each:
//
//     b: an unsigned byte           c: a signed byte           s: an unsigned 16-bit field
//     v: a variable length          r: a signed byte that a relative jump reads
struct Form
{
    ScriptOp op = ScriptOp::kUnknown;
    const char* fields = "";
    uint8_t low_mask = 0; // the opcode's bits that hold a number
};

// The commands that change the flow of a script. The later engine's relative jumps fill 0xF2 to 0xF4.
Form FlowForm(uint8_t opcode, EadEngine engine)
{
    switch (opcode)
    {
    case 0xFF:
        return {ScriptOp::kEnd};
    case 0xFE:
        return {ScriptOp::kDelay1};
    case 0xFD:
        return {ScriptOp::kDelay, "v"};
    case 0xFC:
        return {ScriptOp::kCall, "s"};
    case 0xFB:
        return {ScriptOp::kJump, "s"};
    case 0xFA:
        return {ScriptOp::kBranchIfZero, "s"};
    case 0xF9:
        return {ScriptOp::kBranchIfNegative, "s"};
    case 0xF8:
        return {ScriptOp::kLoop, "b"};
    case 0xF7:
        return {ScriptOp::kLoopEnd};
    case 0xF6:
        return {ScriptOp::kBreak};
    case 0xF5:
        return {ScriptOp::kBranchIfNotNegative, "s"};
    default:
        break;
    }

    if (engine == EadEngine::kEarly)
    {
        return {};
    }

    switch (opcode)
    {
    case 0xF4:
        return {ScriptOp::kJump, "r"};
    case 0xF3:
        return {ScriptOp::kBranchIfZero, "r"};
    case 0xF2:
        return {ScriptOp::kBranchIfNegative, "r"};
    default:
        return {};
    }
}

Form EarlySequenceForm(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xF2:
        return {ScriptOp::kAllocateNotes, "b"};
    case 0xF1:
        return {ScriptOp::kFreeNotes};
    case 0xDF:
        return {ScriptOp::kSetTranspose, "c"};
    case 0xDE:
        return {ScriptOp::kAddTranspose, "c"};
    case 0xDD:
        return {ScriptOp::kTempo, "b"};
    case 0xDC:
        return {ScriptOp::kAddTempo, "c"};
    case 0xDB:
        return {ScriptOp::kVolume, "b"};
    case 0xDA:
        return {ScriptOp::kAddVolume, "c"};
    case 0xD7:
        return {ScriptOp::kInitChannels, "s"};
    case 0xD6:
        return {ScriptOp::kDisableChannels, "s"};
    case 0xD5:
        return {ScriptOp::kMuteScale, "c"};
    case 0xD4:
        return {ScriptOp::kMute};
    case 0xD3:
        return {ScriptOp::kMuteBehavior, "b"};
    case 0xD2:
        return {ScriptOp::kShortVelocityTable, "s"};
    case 0xD1:
        return {ScriptOp::kShortGateTable, "s"};
    case 0xD0:
        return {ScriptOp::kNoteAllocation, "b"};
    case 0xCC:
        return {ScriptOp::kLoadValue, "b"};
    case 0xC9:
        return {ScriptOp::kAndValue, "b"};
    case 0xC8:
        return {ScriptOp::kSubtractValue, "b"};
    default:
        break;
    }

    switch (opcode & 0xF0)
    {
    case 0x00:
        return {ScriptOp::kTestChannel, "", 0x0F};
    case 0x50:
        return {ScriptOp::kSubtractIo, "", 0x0F};
    case 0x70:
        return {ScriptOp::kWriteIo, "", 0x0F};
    case 0x80:
        return {ScriptOp::kReadIo, "", 0x0F};
    case 0x90:
        return {ScriptOp::kStartChannel, "s", 0x0F};
    default:
        return {};
    }
}

Form EarlyChannelForm(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xF3:
        return {ScriptOp::kHang};
    case 0xF2:
        return {ScriptOp::kAllocateNotes, "b"};
    case 0xF1:
        return {ScriptOp::kFreeNotes};
    case 0xE4:
        return {ScriptOp::kDynamicCall};
    case 0xE3:
        return {ScriptOp::kVibratoDelay, "b"};
    case 0xE2:
        return {ScriptOp::kVibratoDepthGradient, "bbb"};
    case 0xE1:
        return {ScriptOp::kVibratoRateGradient, "bbb"};
    case 0xE0:
        return {ScriptOp::kVolumeExpression, "b"};
    case 0xDF:
        return {ScriptOp::kChannelVolume, "b"};
    case 0xDE:
        return {ScriptOp::kFrequencyScale, "s"};
    case 0xDD:
        return {ScriptOp::kPan, "b"};
    case 0xDC:
        return {ScriptOp::kPanWeight, "b"};
    case 0xDB:
        return {ScriptOp::kChannelTranspose, "c"};
    case 0xDA:
        return {ScriptOp::kEnvelope, "s"};
    case 0xD9:
        return {ScriptOp::kReleaseRate, "b"};
    case 0xD8:
        return {ScriptOp::kVibratoDepth, "b"};
    case 0xD7:
        return {ScriptOp::kVibratoRate, "b"};
    case 0xD6:
        return {ScriptOp::kUpdatesPerFrame, "b"};
    case 0xD4:
        return {ScriptOp::kReverb, "b"};
    case 0xD3:
        return {ScriptOp::kBend, "c"};
    case 0xD2:
        return {ScriptOp::kSustain, "b"};
    case 0xD1:
        return {ScriptOp::kNoteAllocation, "b"};
    case 0xD0:
        return {ScriptOp::kStereoEffects, "b"};
    case 0xCC:
        return {ScriptOp::kLoadValue, "b"};
    case 0xCB:
        return {ScriptOp::kLoadFromSequence, "s"};
    case 0xCA:
        return {ScriptOp::kMuteBehavior, "b"};
    case 0xC9:
        return {ScriptOp::kAndValue, "b"};
    case 0xC8:
        return {ScriptOp::kSubtractValue, "b"};
    case 0xC7:
        return {ScriptOp::kStoreToSequence, "bs"};
    case 0xC6:
        return {ScriptOp::kFont, "b"};
    case 0xC5:
        return {ScriptOp::kDynamicTableLookup};
    case 0xC4:
        return {ScriptOp::kLargeNotes};
    case 0xC3:
        return {ScriptOp::kShortNotes};
    case 0xC2:
        return {ScriptOp::kDynamicTable, "s"};
    case 0xC1:
        return {ScriptOp::kInstrument, "b"};
    default:
        break;
    }

    switch (opcode & 0xF0)
    {
    case 0x00:
        return {ScriptOp::kTestLayer, "", 0x0F};
    case 0x10:
        return {ScriptOp::kStartOtherChannel, "s", 0x0F};
    case 0x20:
        return {ScriptOp::kStopOtherChannel, "", 0x0F};
    case 0x30:
        return {ScriptOp::kWriteOtherChannel, "b", 0x0F};
    case 0x40:
        return {ScriptOp::kReadOtherChannel, "b", 0x0F};
    case 0x50:
        return {ScriptOp::kSubtractIo, "", 0x0F};
    case 0x60:
        return {ScriptOp::kSetNotePriorityLow, "", 0x0F};
    case 0x70:
        return {ScriptOp::kWriteIo, "", 0x0F};
    case 0x80:
        return {ScriptOp::kReadIo, "", 0x0F};
    case 0x90:
        return {ScriptOp::kStartLayer, "s", 0x0F};
    case 0xA0:
        return {ScriptOp::kFreeLayer, "", 0x0F};
    case 0xB0:
        return {ScriptOp::kDynamicStartLayer, "", 0x0F};
    default:
        return {};
    }
}

Form EarlyLayerForm(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xC0:
        return {ScriptOp::kLayerDelay, "v"};
    case 0xC1:
        return {ScriptOp::kShortVelocity, "b"};
    case 0xC2:
        return {ScriptOp::kLayerTranspose, "c"};
    case 0xC3:
        return {ScriptOp::kShortDelay, "v"};
    case 0xC4:
        return {ScriptOp::kLegato};
    case 0xC5:
        return {ScriptOp::kNoLegato};
    case 0xC6:
        return {ScriptOp::kLayerInstrument, "b"};
    case 0xC7:
        return {ScriptOp::kPortamento};
    case 0xC8:
        return {ScriptOp::kNoPortamento};
    case 0xC9:
        return {ScriptOp::kShortGate, "b"};
    case 0xCA:
        return {ScriptOp::kNotePan, "b"};
    default:
        break;
    }

    switch (opcode & 0xF0)
    {
    case 0xD0:
        return {ScriptOp::kShortVelocityFromTable, "", 0x0F};
    case 0xE0:
        return {ScriptOp::kShortGateFromTable, "", 0x0F};
    default:
        return {};
    }
}

// The commands that the middle engine changes or adds. The others are the early engine's.
Form MiddleSequenceForm(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xF1:
        return {ScriptOp::kAllocateNotes, "b"};
    case 0xF0:
        return {ScriptOp::kFreeNotes};
    case 0xDA:
        return {ScriptOp::kVolumeMode, "bs"};
    case 0xD9:
        return {ScriptOp::kVolumeScale, "b"};
    default:
        return EarlySequenceForm(opcode);
    }
}

Form MiddleChannelForm(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xF1:
        return {ScriptOp::kAllocateNotes, "b"};
    case 0xF0:
        return {ScriptOp::kFreeNotes};
    case 0xEC:
        return {ScriptOp::kVibratoReset};
    case 0xEB:
        return {ScriptOp::kFontInstrument, "bb"};
    case 0xEA:
        return {ScriptOp::kStopChannel};
    case 0xE9:
        return {ScriptOp::kNotePriority, "b"};
    case 0xE8:
        return {ScriptOp::kParameters, "bbbbbbbb"};
    case 0xE7:
        return {ScriptOp::kLoadParameters, "s"};
    case 0xE6:
        return {ScriptOp::kBookOffset, "b"};
    case 0xE5:
        return {ScriptOp::kReverbIndex, "b"};
    case 0xD6:
        return {};
    default:
        break;
    }

    if ((opcode & 0xF0) == 0x60)
    {
        return {ScriptOp::kChannelDelayShort, "", 0x0F};
    }

    return EarlyChannelForm(opcode);
}

Form MiddleLayerForm(uint8_t opcode)
{
    switch (opcode)
    {
    case 0xCB:
        return {ScriptOp::kLayerEnvelope, "sb"};
    case 0xCC:
        return {ScriptOp::kNoDrumPan};
    default:
        return EarlyLayerForm(opcode);
    }
}

Form LaterSequenceForm(uint8_t opcode, EadEngine engine)
{
    switch (opcode)
    {
    case 0xF1:
        return {ScriptOp::kAllocateNotes, "b"};
    case 0xF0:
        return {ScriptOp::kFreeNotes};
    case 0xEF:
        return {ScriptOp::kChannelArgument, "sb"};
    case 0xDF:
        return {ScriptOp::kSetTranspose, "c"};
    case 0xDE:
        return {ScriptOp::kAddTranspose, "c"};
    case 0xDD:
        return {ScriptOp::kTempo, "b"};
    case 0xDC:
        return {ScriptOp::kAddTempo, "c"};
    case 0xDB:
        return {ScriptOp::kVolume, "b"};
    case 0xDA:
        return {ScriptOp::kVolumeMode, "bs"};
    case 0xD9:
        return {ScriptOp::kVolumeScale, "b"};
    case 0xD7:
        return {ScriptOp::kInitChannels, "s"};
    case 0xD6:
        return {ScriptOp::kDisableChannels, "s"};
    case 0xD5:
        return {ScriptOp::kMuteScale, "c"};
    case 0xD4:
        return {ScriptOp::kMute};
    case 0xD3:
        return {ScriptOp::kMuteBehavior, "b"};
    case 0xD2:
        return {ScriptOp::kShortVelocityTable, "s"};
    case 0xD1:
        return {ScriptOp::kShortGateTable, "s"};
    case 0xD0:
        return {ScriptOp::kNoteAllocation, "b"};
    case 0xCE:
        return {ScriptOp::kRandom, "b"};
    case 0xCD:
        return {ScriptOp::kDynamicJumpTable, "s"};
    case 0xCC:
        return {ScriptOp::kLoadValue, "b"};
    case 0xC9:
        return {ScriptOp::kAndValue, "b"};
    case 0xC8:
        return {ScriptOp::kSubtractValue, "b"};
    case 0xC7:
        return {ScriptOp::kStoreToSequence, "bs"};
    case 0xC6:
        return {ScriptOp::kStopSequence};
    case 0xC5:
        return {ScriptOp::kScriptCounter, "s"};
    case 0xC4:
        return {ScriptOp::kRunSequence, "bb"};
    case 0xC3:
        if (engine == EadEngine::kLaterMajora)
        {
            return {ScriptOp::kChannelArgument, "s"};
        }

        return {};
    case 0xC2:
        if (engine == EadEngine::kLaterMajora)
        {
            return {ScriptOp::kChannelArgument, "s"};
        }

        return {};
    default:
        break;
    }

    switch (opcode & 0xF0)
    {
    case 0x00:
        return {ScriptOp::kTestChannel, "", 0x0F};
    case 0x40:
        return {ScriptOp::kStopOtherChannel, "", 0x0F};
    case 0x50:
        return {ScriptOp::kSubtractIo, "", 0x0F};
    case 0x60:
        return {ScriptOp::kLoadResource, "bb", 0x0F};
    case 0x70:
        return {ScriptOp::kWriteIo, "", 0x0F};
    case 0x80:
        return {ScriptOp::kReadIo, "", 0x0F};
    case 0x90:
        return {ScriptOp::kStartChannel, "s", 0x0F};
    case 0xA0:
        return {ScriptOp::kStartChannelRel, "s", 0x0F};
    case 0xB0:
        return {ScriptOp::kLoadSequence, "bs", 0x0F};
    default:
        return {};
    }
}

Form LaterChannelForm(uint8_t opcode, EadEngine engine)
{
    switch (opcode)
    {
    case 0xF1:
        return {ScriptOp::kAllocateNotes, "b"};
    case 0xF0:
        return {ScriptOp::kFreeNotes};
    case 0xEE:
        return {ScriptOp::kBendFine, "c"};
    case 0xED:
        return {ScriptOp::kGain, "b"};
    case 0xEC:
        return {ScriptOp::kVibratoReset};
    case 0xEB:
        return {ScriptOp::kFontInstrument, "bb"};
    case 0xEA:
        return {ScriptOp::kStopChannel};
    case 0xE9:
        return {ScriptOp::kNotePriority, "b"};
    case 0xE8:
        return {ScriptOp::kParameters, "bbbbbbbb"};
    case 0xE7:
        return {ScriptOp::kLoadParameters, "s"};
    case 0xE6:
        return {ScriptOp::kBookOffset, "b"};
    case 0xE5:
        return {ScriptOp::kReverbIndex, "b"};
    case 0xE4:
        return {ScriptOp::kDynamicCall};
    case 0xE3:
        return {ScriptOp::kVibratoDelay, "b"};
    case 0xE2:
        return {ScriptOp::kVibratoDepthGradient, "bbb"};
    case 0xE1:
        return {ScriptOp::kVibratoRateGradient, "bbb"};
    case 0xE0:
        return {ScriptOp::kVolumeExpression, "b"};
    case 0xDF:
        return {ScriptOp::kChannelVolume, "b"};
    case 0xDE:
        return {ScriptOp::kFrequencyScale, "s"};
    case 0xDD:
        return {ScriptOp::kPan, "b"};
    case 0xDC:
        return {ScriptOp::kPanWeight, "b"};
    case 0xDB:
        return {ScriptOp::kChannelTranspose, "c"};
    case 0xDA:
        return {ScriptOp::kEnvelope, "s"};
    case 0xD9:
        return {ScriptOp::kReleaseRate, "b"};
    case 0xD8:
        return {ScriptOp::kVibratoDepth, "b"};
    case 0xD7:
        return {ScriptOp::kVibratoRate, "b"};
    case 0xD6:
    case 0xD5:
        return {ScriptOp::kChannelArgument, "b"};
    case 0xD4:
        return {ScriptOp::kReverb, "b"};
    case 0xD3:
        return {ScriptOp::kBend, "c"};
    case 0xD2:
        return {ScriptOp::kSustain, "b"};
    case 0xD1:
        return {ScriptOp::kNoteAllocation, "b"};
    case 0xD0:
        return {ScriptOp::kStereoEffects, "b"};
    case 0xCF:
        return {ScriptOp::kStorePointer, "s"};
    case 0xCE:
        return {ScriptOp::kLoadPointer, "s"};
    case 0xCD:
        return {ScriptOp::kStopOtherChannel, "b"};
    case 0xCC:
        return {ScriptOp::kLoadValue, "b"};
    case 0xCB:
        return {ScriptOp::kLoadFromSequence, "s"};
    case 0xCA:
        return {ScriptOp::kMuteBehavior, "b"};
    case 0xC9:
        return {ScriptOp::kAndValue, "b"};
    case 0xC8:
        return {ScriptOp::kSubtractValue, "b"};
    case 0xC7:
        return {ScriptOp::kStoreToSequence, "bs"};
    case 0xC6:
        return {ScriptOp::kFont, "b"};
    case 0xC5:
        return {ScriptOp::kDynamicTableLookup};
    case 0xC4:
        return {ScriptOp::kLargeNotes};
    case 0xC3:
        return {ScriptOp::kShortNotes};
    case 0xC2:
        return {ScriptOp::kDynamicTable, "s"};
    case 0xC1:
        return {ScriptOp::kInstrument, "b"};
    case 0xBD:
        if (engine == EadEngine::kLaterMajora)
        {
            return {ScriptOp::kSampleStart, "b"};
        }

        return {ScriptOp::kRandomPointer, "ss"};
    case 0xBC:
        return {ScriptOp::kAddPointer, "s"};
    case 0xBB:
        return {ScriptOp::kCombFilter, "bs"};
    case 0xBA:
        return {ScriptOp::kRandomGate, "b"};
    case 0xB9:
        return {ScriptOp::kRandomVelocity, "b"};
    case 0xB8:
        return {ScriptOp::kRandom, "b"};
    case 0xB7:
        return {ScriptOp::kRandomToPointer, "s"};
    case 0xB6:
        return {ScriptOp::kTableValue};
    case 0xB5:
        return {ScriptOp::kTableToPointer};
    case 0xB4:
        return {ScriptOp::kPointerToTable};
    case 0xB3:
        return {ScriptOp::kLoadFilter, "b"};
    case 0xB2:
        return {ScriptOp::kPointerFromSequence, "s"};
    case 0xB1:
        return {ScriptOp::kFreeFilter};
    case 0xB0:
        return {ScriptOp::kFilter, "s"};
    default:
        break;
    }

    if (engine == EadEngine::kLaterMajora)
    {
        switch (opcode)
        {
        case 0xBE:
        case 0xA7:
        case 0xA4:
            return {ScriptOp::kChannelArgument, "b"};
        case 0xA8:
            return {ScriptOp::kRandomPointer, "ss"};
        case 0xA6:
            return {ScriptOp::kChannelArgument, "bs"};
        case 0xA5:
        case 0xA3:
        case 0xA1:
            return {ScriptOp::kChannelArgument};
        case 0xA2:
        case 0xA0:
            return {ScriptOp::kChannelArgument, "s"};
        default:
            break;
        }
    }

    switch (opcode & 0xF8)
    {
    case 0x00:
    case 0x08:
        return {ScriptOp::kChannelDelayShort, "", 0x0F};
    case 0x10:
    case 0x18:
        return {ScriptOp::kLoadSample, "", 0x0F};
    case 0x20:
    case 0x28:
        return {ScriptOp::kStartOtherChannel, "s", 0x0F};
    case 0x30:
    case 0x38:
        return {ScriptOp::kWriteOtherChannel, "b", 0x0F};
    case 0x40:
    case 0x48:
        return {ScriptOp::kReadOtherChannel, "b", 0x0F};
    case 0x50:
    case 0x58:
        return {ScriptOp::kSubtractIo, "", 0x0F};
    case 0x60:
    case 0x68:
        return {ScriptOp::kReadIo, "", 0x0F};
    case 0x70:
        return {ScriptOp::kWriteIo, "", 0x07};
    case 0x78:
        return {ScriptOp::kStartLayerRel, "s", 0x07};
    case 0x80:
        return {ScriptOp::kTestLayer, "", 0x07};
    case 0x88:
        return {ScriptOp::kStartLayer, "s", 0x07};
    case 0x90:
        return {ScriptOp::kFreeLayer, "", 0x07};
    case 0x98:
        return {ScriptOp::kDynamicStartLayer, "", 0x07};
    default:
        return {};
    }
}

Form LaterLayerForm(uint8_t opcode, EadEngine engine)
{
    switch (opcode)
    {
    case 0xC0:
        return {ScriptOp::kLayerDelay, "v"};
    case 0xC1:
        return {ScriptOp::kShortVelocity, "b"};
    case 0xC2:
        return {ScriptOp::kLayerTranspose, "c"};
    case 0xC3:
        return {ScriptOp::kShortDelay, "v"};
    case 0xC4:
        return {ScriptOp::kLegato};
    case 0xC5:
        return {ScriptOp::kNoLegato};
    case 0xC6:
        return {ScriptOp::kLayerInstrument, "b"};
    case 0xC7:
        return {ScriptOp::kPortamento};
    case 0xC8:
        return {ScriptOp::kNoPortamento};
    case 0xC9:
        return {ScriptOp::kShortGate, "b"};
    case 0xCA:
        return {ScriptOp::kNotePan, "b"};
    case 0xCB:
        return {ScriptOp::kLayerEnvelope, "sb"};
    case 0xCC:
        return {ScriptOp::kNoDrumPan};
    case 0xCD:
        return {ScriptOp::kLayerStereo, "b"};
    case 0xCE:
        return {ScriptOp::kLayerBendFine, "c"};
    case 0xCF:
        return {ScriptOp::kLayerReleaseRate, "b"};
    case 0xF0:
        if (engine == EadEngine::kLaterMajora)
        {
            return {ScriptOp::kLayerUnknownF0, "s"};
        }

        return {};
    case 0xF1:
        if (engine == EadEngine::kLaterMajora)
        {
            return {ScriptOp::kSurroundEffect, "b"};
        }

        return {};
    default:
        break;
    }

    switch (opcode & 0xF0)
    {
    case 0xD0:
        return {ScriptOp::kShortVelocityFromTable, "", 0x0F};
    case 0xE0:
        return {ScriptOp::kShortGateFromTable, "", 0x0F};
    default:
        return {};
    }
}

Form FormOf(uint8_t opcode, EadEngine engine, ScriptLevel level)
{
    const Form flow = FlowForm(opcode, engine);
    if (flow.op != ScriptOp::kUnknown)
    {
        return flow;
    }

    if (engine == EadEngine::kEarly)
    {
        switch (level)
        {
        case ScriptLevel::kSequence:
            return EarlySequenceForm(opcode);
        case ScriptLevel::kChannel:
            return EarlyChannelForm(opcode);
        case ScriptLevel::kLayer:
            return EarlyLayerForm(opcode);
        }
    }

    if (engine == EadEngine::kMiddle)
    {
        switch (level)
        {
        case ScriptLevel::kSequence:
            return MiddleSequenceForm(opcode);
        case ScriptLevel::kChannel:
            return MiddleChannelForm(opcode);
        case ScriptLevel::kLayer:
            return MiddleLayerForm(opcode);
        }
    }

    switch (level)
    {
    case ScriptLevel::kSequence:
        return LaterSequenceForm(opcode, engine);
    case ScriptLevel::kChannel:
        return LaterChannelForm(opcode, engine);
    case ScriptLevel::kLayer:
        return LaterLayerForm(opcode, engine);
    }

    return {};
}

uint32_t ReadVariableLength(const xsflib::Reader& reader, uint32_t& position)
{
    const uint8_t first = reader.U8(position++);
    if ((first & 0x80) == 0)
    {
        return first;
    }

    const uint8_t second = reader.U8(position++);

    return (static_cast<uint32_t>(first & 0x7F) << 8) | second;
}

// A note: its pitch in the low 6 bits, and a form in the top 2 bits.
void ReadNote(const xsflib::Reader& reader, uint32_t& position, uint8_t opcode, bool large_notes,
              ScriptCommand& command)
{
    command.op = ScriptOp::kNote;
    command.low_bits = static_cast<uint8_t>(opcode & 0x3F);

    const int form = opcode >> 6;
    if (form != 2)
    {
        if (large_notes || form == 0)
        {
            command.args[0] = static_cast<int32_t>(ReadVariableLength(reader, position));
        }
    }

    if (large_notes)
    {
        command.args[1] = reader.U8(position++);
        if (form != 1)
        {
            command.args[2] = reader.U8(position++);
        }
    }
}

void ReadPortamento(const xsflib::Reader& reader, uint32_t& position, ScriptCommand& command)
{
    command.args[0] = reader.U8(position++);
    command.args[1] = reader.U8(position++);
    command.args[2] = (command.args[0] & 0x80) != 0 ? reader.U8(position++)
                                                    : static_cast<int32_t>(ReadVariableLength(reader, position));
}

void ReadFields(const xsflib::Reader& reader, uint32_t& position, const char* fields, ScriptCommand& command)
{
    std::size_t count = 0;
    for (const char* field = fields; *field != '\0'; field++, count++)
    {
        int32_t value = 0;
        switch (*field)
        {
        case 'b':
            value = reader.U8(position);
            position += 1;
            break;
        case 'c':
            value = static_cast<int8_t>(reader.U8(position));
            position += 1;
            break;
        case 's':
            value = reader.U16(position);
            position += 2;
            break;
        case 'v':
            value = static_cast<int32_t>(ReadVariableLength(reader, position));
            break;
        case 'r':
            {
                // A relative jump counts from the end of the command.
                const int32_t distance = static_cast<int8_t>(reader.U8(position));
                position += 1;
                value = static_cast<int32_t>(position) + distance;
                break;
            }
        default:
            break;
        }

        if (count < command.bytes.size())
        {
            command.bytes[count] = static_cast<uint8_t>(value);
        }

        if (count < command.args.size())
        {
            command.args[count] = value;
        }
    }
}

// The place in a sequence that a path of WalkScript has reached.
struct Place
{
    friend auto operator<=>(const Place&, const Place&) = default;

    uint32_t offset = 0;
    ScriptLevel level = ScriptLevel::kSequence;
    int8_t channel = -1;
    int8_t layer = -1;
    bool large_notes = false;
};

class Walker
{
public:
    Walker(std::span<const uint8_t> sequence, EadEngine engine) : sequence_(sequence), engine_(engine)
    {
    }

    ScriptWalk Run()
    {
        std::vector<Place> pending = {{0, ScriptLevel::kSequence, -1, -1, false}};
        while (!pending.empty())
        {
            Place place = pending.back();
            pending.pop_back();
            Follow(place, pending);
        }

        std::sort(walk_.bad_offsets.begin(), walk_.bad_offsets.end());
        walk_.bad_offsets.erase(std::unique(walk_.bad_offsets.begin(), walk_.bad_offsets.end()),
                                walk_.bad_offsets.end());
        std::sort(walk_.dynamic_tables.begin(), walk_.dynamic_tables.end());
        walk_.dynamic_tables.erase(std::unique(walk_.dynamic_tables.begin(), walk_.dynamic_tables.end()),
                                   walk_.dynamic_tables.end());
        for (auto& [offset, command] : found_)
        {
            walk_.commands.push_back(std::move(command));
        }

        return std::move(walk_);
    }

private:
    void Follow(Place place, std::vector<Place>& pending)
    {
        while (true)
        {
            if (!visited_.insert(place).second)
            {
                return;
            }

            ScriptCommand command;
            try
            {
                command = ReadScriptCommand(sequence_, place.offset, engine_, place.level, place.large_notes);
            }
            catch (const xsflib::FormatError&)
            {
                walk_.bad_offsets.push_back(place.offset);
                walk_.bad_levels[place.offset] = place.level;
                return;
            }

            if (command.op == ScriptOp::kUnknown)
            {
                walk_.bad_offsets.push_back(place.offset);
                walk_.bad_levels[place.offset] = place.level;
                return;
            }

            if (command.op == ScriptOp::kNote && found_.find(place.offset) == found_.end())
            {
                walk_.notes++;
            }

            found_.try_emplace(place.offset, WalkedCommand{command, place.level, place.channel, place.layer});
            if (!Step(command, place, pending))
            {
                return;
            }
        }
    }

    // Queues the paths that the command starts. Returns whether the path goes on after the command, with `place`
    // updated.
    bool Step(const ScriptCommand& command, Place& place, std::vector<Place>& pending)
    {
        const auto target = static_cast<uint32_t>(command.args[0]);
        switch (command.op)
        {
        case ScriptOp::kEnd:
        case ScriptOp::kHang:
        case ScriptOp::kStopSequence:
        case ScriptOp::kStopChannel:
            return false;
        case ScriptOp::kJump:
            place.offset = target;
            return true;
        case ScriptOp::kCall:
        case ScriptOp::kBranchIfZero:
        case ScriptOp::kBranchIfNegative:
        case ScriptOp::kBranchIfNotNegative:
            {
                Place side = place;
                side.offset = target;
                pending.push_back(side);
                break;
            }
        case ScriptOp::kStartChannel:
            pending.push_back({target, ScriptLevel::kChannel, static_cast<int8_t>(command.low_bits), -1, false});
            break;
        case ScriptOp::kStartChannelRel:
            pending.push_back({command.next + static_cast<uint32_t>(static_cast<int16_t>(target)),
                               ScriptLevel::kChannel, static_cast<int8_t>(command.low_bits), -1, false});
            break;
        case ScriptOp::kStartOtherChannel:
            pending.push_back({target, ScriptLevel::kChannel, static_cast<int8_t>(command.low_bits), -1, false});
            break;
        case ScriptOp::kStartLayer:
            pending.push_back(
                {target, ScriptLevel::kLayer, place.channel, static_cast<int8_t>(command.low_bits), place.large_notes});
            break;
        case ScriptOp::kStartLayerRel:
            pending.push_back({command.next + static_cast<uint32_t>(static_cast<int16_t>(target)), ScriptLevel::kLayer,
                               place.channel, static_cast<int8_t>(command.low_bits), place.large_notes});
            break;
        case ScriptOp::kLargeNotes:
            place.large_notes = true;
            break;
        case ScriptOp::kShortNotes:
            place.large_notes = false;
            break;
        case ScriptOp::kDynamicTable:
        case ScriptOp::kDynamicJumpTable:
            walk_.dynamic_tables.push_back(target);
            break;
        default:
            break;
        }

        place.offset = command.next;

        return true;
    }

    std::span<const uint8_t> sequence_;
    EadEngine engine_;
    ScriptWalk walk_;
    std::set<Place> visited_;
    std::map<uint32_t, WalkedCommand> found_;
};

} // namespace

ScriptCommand ReadScriptCommand(std::span<const uint8_t> sequence, uint32_t offset, EadEngine engine, ScriptLevel level,
                                bool large_notes)
{
    const xsflib::Reader reader(sequence, true);
    uint32_t position = offset;

    ScriptCommand command;
    command.offset = offset;
    command.opcode = reader.U8(position++);
    if (level == ScriptLevel::kLayer && command.opcode < 0xC0)
    {
        ReadNote(reader, position, command.opcode, large_notes, command);
        command.next = position;

        return command;
    }

    const Form form = FormOf(command.opcode, engine, level);
    command.op = form.op;
    command.low_bits = static_cast<uint8_t>(command.opcode & form.low_mask);
    if (form.op == ScriptOp::kPortamento)
    {
        ReadPortamento(reader, position, command);
    }
    else
    {
        ReadFields(reader, position, form.fields, command);
    }

    command.next = position;

    return command;
}

ScriptWalk WalkScript(std::span<const uint8_t> sequence, EadEngine engine)
{
    return Walker(sequence, engine).Run();
}

} // namespace ntdlib::n64
