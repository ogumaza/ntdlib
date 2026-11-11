// SPDX-License-Identifier: MIT

// What the files of the SND driver model share: addresses in ARM7 memory, the channel registers and the work areas of
// the players, the kinds of channel, envelope stage, LFO target and instrument, the default LFO, the tempo step, and
// the fields of an instrument (Driver::Instrument).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
namespace ntdlib::ntr::snd
{

constexpr uint32_t kMainMemory = 0x02000000;

// The sound unit's registers.
constexpr uint32_t kChannelRegisters = 0x04000400;

constexpr uint32_t kMixRegister = 0x04000501;

constexpr uint32_t kCaptureControl = 0x04000508;

inline uint32_t ChannelRegister(int channel, uint32_t offset)
{
    if (channel >= kChannelCount)
    {
        return kGrownChannelRegisters + static_cast<uint32_t>(channel - kChannelCount) * kGrownChannelBytes + offset;
    }

    return kChannelRegisters + static_cast<uint32_t>(channel) * 0x10 + offset;
}

constexpr uint8_t kNoTrack = 0xFF;

enum ChannelType : uint8_t
{
    kPcm = 0,
    kPsg = 1,
    kNoise = 2,
};

enum EnvelopeStage : uint8_t
{
    kAttack = 0,
    kDecay = 1,
    kSustain = 2,
    kRelease = 3,
};

enum LfoTarget : uint8_t
{
    kLfoPitch = 0,
    kLfoVolume = 1,
    kLfoPan = 2,
};

// The instruments' types in a bank.
enum InstrumentType : uint8_t
{
    kInstrumentPcm = 1,
    kInstrumentPsg = 2,
    kInstrumentNoise = 3,
    kInstrumentDirectPcm = 4,
    kInstrumentDummy = 5,
    kInstrumentDrums = 16,
    kInstrumentKeySplit = 17,
};

constexpr uint32_t kPlayerWork = 0x20;

constexpr uint32_t kPlayerWorkSize = 0x24;

constexpr uint32_t kGlobalVariables = 0x260;

inline LfoParameters DefaultLfo()
{
    return {.target = kLfoPitch, .speed = 16, .depth = 0, .range = 1, .delay = 0};
}

// The ticks that a tempo and a tempo ratio (256 for 1) add to a player's counter each frame. The game's multiply
// wraps at 32 bits. The shift keeps the sign bit.
inline int TempoStep(uint16_t tempo, uint16_t ratio)
{
    return static_cast<int32_t>(static_cast<uint32_t>(tempo) * ratio) >> 8;
}

struct Driver::Instrument
{
    uint8_t type = 0;
    std::array<uint16_t, 2> wave = {};
    uint8_t root_key = 0;
    uint8_t attack = 0;
    uint8_t decay = 0;
    uint8_t sustain = 0;
    uint8_t release = 0;
    uint8_t pan = 0;
};

} // namespace ntdlib::ntr::snd
