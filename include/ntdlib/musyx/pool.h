// SPDX-License-Identifier: MIT

// A MusyX pool file: the SoundMacros that play notes and effects, the tables that they read, the keymaps and the
// layers. A SoundMacro is a list of 8-byte steps that a voice runs. A keymap gives each key a macro, and a layer plays
// several macros over key ranges. Tables hold envelopes and curves. On the GameCube, the pool's records and a step's
// two words are big-endian. The envelope tables are little-endian there.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib::musyx
{

// The commands of SoundMacro steps, by MusyX's names for them. The low 7 bits of a step's first word hold the command.
enum class MacroOp : uint8_t
{
    kEnd = 0x00,
    kStop = 0x01,
    kSplitKey = 0x02,
    kSplitVelocity = 0x03,
    kWaitTicks = 0x04,
    kLoop = 0x05,
    kGoto = 0x06,
    kWaitMs = 0x07,
    kPlayMacro = 0x08,
    kSendKeyOff = 0x09,
    kSplitModulation = 0x0a,
    kPianoPan = 0x0b,
    kSetAdsr = 0x0c,
    kScaleVolume = 0x0d,
    kPanning = 0x0e,
    kEnvelope = 0x0f,
    kStartSample = 0x10,
    kStopSample = 0x11,
    kKeyOff = 0x12,
    kSplitRandom = 0x13,
    kFadeIn = 0x14,
    kSurroundPanning = 0x15,
    kSetAdsrFromControllers = 0x16,
    kRandomKey = 0x17,
    kAddKey = 0x18,
    kSetKey = 0x19,
    kLastKey = 0x1a,
    kPortamento = 0x1b,
    kVibrato = 0x1c,
    kPitchSweep1 = 0x1d,
    kPitchSweep2 = 0x1e,
    kSetPitch = 0x1f,
    kSetPitchAdsr = 0x20,
    kScaleVolumeDls = 0x21,
    kModulationToVibrato = 0x22,
    kSetupTremolo = 0x23,
    kReturn = 0x24,
    kGosub = 0x25,
    kTrapEvent = 0x28,
    kUntrapEvent = 0x29,
    kSendMessage = 0x2a,
    kGetMessage = 0x2b,
    kGetVoiceId = 0x2c,
    kAddAgeCounter = 0x30,
    kSetAgeCounter = 0x31,
    kSendFlag = 0x32,
    kPitchWheelRange = 0x33,
    kScaleReverb = 0x34,
    kPitchBendAfterKeyOff = 0x35,
    kSetPriority = 0x36,
    kAddPriority = 0x37,
    kAgeCounterSpeed = 0x38,
    kAgeCounterByVolume = 0x39,
    kVolumeSelect = 0x40,
    kPanningSelect = 0x41,
    kPitchWheelSelect = 0x42,
    kModWheelSelect = 0x43,
    kPedalSelect = 0x44,
    kPortamentoSelect = 0x45,
    kReverbSelect = 0x46,
    kSurroundPanningSelect = 0x47,
    kDopplerSelect = 0x48,
    kTremoloSelect = 0x49,
    kPreAuxASelect = 0x4a,
    kPreAuxBSelect = 0x4b,
    kPostAuxBSelect = 0x4c,
    kAuxAEffectSelect = 0x4d,
    kAuxBEffectSelect = 0x4e,
    kSetupLfo = 0x50,
    kModeSelect = 0x58,
    kSetKeyGroup = 0x59,
    kSrcModeSelect = 0x5a,
    kAddVariables = 0x60,
    kSubtractVariables = 0x61,
    kMultiplyVariables = 0x62,
    kDivideVariables = 0x63,
    kAddImmediate = 0x64,
    kSetVariable = 0x65,
    kIfEqual = 0x70,
    kIfLess = 0x71,
};

// A step of a SoundMacro: two 32-bit words. Its fields are numbered as bytes from the low end of each word. Byte 0 is
// the first word's lowest byte and holds the command. Bytes 1 to 3 are the first word's higher bytes, and bytes 4 to 7
// are the second word's, lowest first. A 16-bit field takes two such bytes, the lower-numbered one as its low byte.
struct MacroStep
{
    MacroOp Op() const;

    // Byte `index` (0 to 7) of the step.
    uint8_t Byte(int index) const;

    // The 16-bit field whose low byte is byte `index` (0 to 6).
    uint16_t Half(int index) const;

    uint32_t word0 = 0;
    uint32_t word1 = 0;
};

// An envelope in the linear form: times in milliseconds, and the sustain level in 4096ths of full amplitude.
struct LinearAdsr
{
    uint16_t attack = 0;
    uint16_t decay = 0;
    uint16_t sustain = 0;
    uint16_t release = 0;
};

// An envelope in the DLS form. The attack and decay times are in 65536ths of a timecent, and INT32_MIN means no time
// at all. The sustain level is in 4096ths before a square law, and the release time is in milliseconds. The two scales
// lengthen the attack with the velocity and the decay with the key, in the same units. INT32_MIN turns one off.
struct DlsAdsr
{
    int32_t attack = INT32_MIN;
    int32_t decay = INT32_MIN;
    uint16_t sustain = 0;
    uint16_t release = 0;
    int32_t velocity_to_attack = INT32_MIN;
    int32_t key_to_decay = INT32_MIN;
};

// Reads table `data` as an envelope in the linear form. Returns nothing if it's shorter than one (8 bytes).
std::optional<LinearAdsr> ReadLinearAdsr(std::span<const uint8_t> data);

// Reads table `data` as an envelope in the DLS form. Returns nothing if it's shorter than one (20 bytes).
std::optional<DlsAdsr> ReadDlsAdsr(std::span<const uint8_t> data);

// A key of a keymap: the object it plays, with a transposition, a pan and an offset to the voices' priority.
struct KeymapEntry
{
    uint16_t object = 0xffff;
    int8_t transpose = 0;
    uint8_t pan = 64;
    int16_t priority_offset = 0;
};

// An entry of a layer: an object that plays over a range of keys, with a transposition, a volume, a pan and an offset
// to the voices' priority.
struct LayerEntry
{
    uint16_t object = 0xffff;
    uint8_t low_key = 0;
    uint8_t high_key = 127;
    int8_t transpose = 0;
    uint8_t volume = 127;
    int16_t priority_offset = 0;
    uint8_t pan = 64;
};

// A pool's objects by ID. Keymap IDs include bit 14, and layer IDs include bit 15.
struct Pool
{
    std::map<uint16_t, std::vector<MacroStep>> macros;
    std::map<uint16_t, std::vector<uint8_t>> tables;
    std::map<uint16_t, std::array<KeymapEntry, 128>> keymaps;
    std::map<uint16_t, std::vector<LayerEntry>> layers;
};

// Reads the pool file `file`. Throws FormatError if it's damaged.
Pool ReadPool(std::span<const uint8_t> file);

} // namespace ntdlib::musyx
