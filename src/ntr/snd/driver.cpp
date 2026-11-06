// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/snd/driver.h"

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

#include "xsflib/text.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr::snd
{

namespace
{

constexpr uint32_t kMainMemory = 0x02000000;
constexpr uint32_t kMainMemoryEnd = 0x03000000;

// The sound unit's registers.
constexpr uint32_t kChannelRegisters = 0x04000400;
constexpr uint32_t kMasterVolumeRegister = 0x04000500;
constexpr uint32_t kMixRegister = 0x04000501;
constexpr uint32_t kCaptureControl = 0x04000508;
constexpr uint32_t kCaptureAddress = 0x04000510;
constexpr uint32_t kCaptureLength = 0x04000514;

uint32_t ChannelRegister(int channel, uint32_t offset)
{
    if (channel >= kChannelCount)
    {
        return kGrownChannelRegisters + static_cast<uint32_t>(channel - kChannelCount) * kGrownChannelBytes + offset;
    }

    return kChannelRegisters + static_cast<uint32_t>(channel) * 0x10 + offset;
}

// A channel's part: 0 plays waves alone, 1 plays a PSG wave too, and 2 noise too.
int PartOf(int twin)
{
    return twin >= 14 ? 2 : twin >= 8 ? 1 : 0;
}

// The channels whose volume the surround decay lowers: all but channels 1 and 3. Those two play the captures.
constexpr uint32_t kSurroundChannels = 0xFFF5;

// A sequence's clock: each player adds its tempo to its counter each frame, and plays a tick for each 240.
constexpr uint16_t kTickRate = 240;

// The envelope's attenuation at a note's start, and the level at which a release ends the channel.
constexpr int32_t kStartAttenuation = -92544;
constexpr int32_t kSilent = -723;

constexpr uint8_t kNoTrack = 0xFF;

enum ChannelType : uint8_t
{
    kPcm = 0,
    kPsg = 1,
    kNoise = 2,
};

enum Sync : uint8_t
{
    kSyncStart = 0x1,
    kSyncStop = 0x2,
    kSyncTimer = 0x4,
    kSyncVolume = 0x8,
    kSyncPan = 0x10,
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

constexpr int kKeySplits = 8;

// The places in a bank and a wave archive.
constexpr uint32_t kBankWaveArchives = 0x18;
constexpr uint32_t kBankCount = 0x38;
constexpr uint32_t kBankOffsets = 0x3C;
constexpr uint32_t kWaveArchiveCount = 0x38;
constexpr uint32_t kWaveArchiveOffsets = 0x3C;
constexpr uint32_t kWaveParametersSize = 12;

// The places in the shared work (SNDSharedWork).
constexpr uint32_t kFinishedCommandTag = 0x0;
constexpr uint32_t kPlayerStatus = 0x4;
constexpr uint32_t kChannelStatus = 0x8;
constexpr uint32_t kCaptureStatus = 0xA;
constexpr uint32_t kPlayerWork = 0x20;
constexpr uint32_t kPlayerWorkSize = 0x24;
constexpr uint32_t kTickCounter = 0x20;
constexpr uint32_t kGlobalVariables = 0x260;
constexpr uint32_t kVariableCount = 16;

// The layouts of a player (0x24 bytes) and a track (0x40 bytes) in the driver's memory, for the commands that write
// their fields by offset.
constexpr std::size_t kPlayerSize = 0x24;
constexpr std::size_t kTrackSize = 0x40;

void Put16(std::span<uint8_t> bytes, std::size_t at, uint16_t value)
{
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}

void Put32(std::span<uint8_t> bytes, std::size_t at, uint32_t value)
{
    Put16(bytes, at, static_cast<uint16_t>(value));
    Put16(bytes, at + 2, static_cast<uint16_t>(value >> 16));
}

uint16_t Get16(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

uint32_t Get32(std::span<const uint8_t> bytes, std::size_t at)
{
    return Get16(bytes, at) | static_cast<uint32_t>(Get16(bytes, at + 2)) << 16;
}

// Writes `size` bytes of `value` at `offset` into `bytes`. The driver's stores of 1, 2 and 4 bytes write the same way.
void Store(std::span<uint8_t> bytes, uint32_t offset, uint32_t value, int size)
{
    for (int i = 0; i < size && offset + static_cast<uint32_t>(i) < bytes.size(); i++)
    {
        bytes[offset + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void PutLfo(std::span<uint8_t> bytes, std::size_t at, const LfoParameters& lfo)
{
    bytes[at] = lfo.target;
    bytes[at + 1] = lfo.speed;
    bytes[at + 2] = lfo.depth;
    bytes[at + 3] = lfo.range;
    Put16(bytes, at + 4, lfo.delay);
}

LfoParameters GetLfo(std::span<const uint8_t> bytes, std::size_t at)
{
    return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3], Get16(bytes, at + 4)};
}

std::array<uint8_t, kPlayerSize> PlayerBytes(const Player& player)
{
    std::array<uint8_t, kPlayerSize> bytes = {};
    bytes[0] = static_cast<uint8_t>(player.active | player.prepared << 1 | player.paused << 2);
    bytes[1] = player.id;
    bytes[4] = player.priority;
    bytes[5] = player.volume;
    Put16(bytes, 6, static_cast<uint16_t>(player.external_fader));
    for (std::size_t i = 0; i < player.tracks.size(); i++)
    {
        bytes[8 + i] = player.tracks[i];
    }
    Put16(bytes, 0x18, player.tempo);
    Put16(bytes, 0x1A, player.tempo_ratio);
    Put16(bytes, 0x1C, player.tempo_counter);
    Put32(bytes, 0x20, player.bank);
    return bytes;
}

void SetPlayerBytes(Player& player, std::span<const uint8_t> bytes)
{
    player.active = (bytes[0] & 1) != 0;
    player.prepared = (bytes[0] & 2) != 0;
    player.paused = (bytes[0] & 4) != 0;
    player.id = bytes[1];
    player.priority = bytes[4];
    player.volume = bytes[5];
    player.external_fader = static_cast<int16_t>(Get16(bytes, 6));
    for (std::size_t i = 0; i < player.tracks.size(); i++)
    {
        player.tracks[i] = bytes[8 + i];
    }
    player.tempo = Get16(bytes, 0x18);
    player.tempo_ratio = Get16(bytes, 0x1A);
    player.tempo_counter = Get16(bytes, 0x1C);
    player.bank = Get32(bytes, 0x20);
}

std::array<uint8_t, kTrackSize> TrackBytes(const Track& track)
{
    std::array<uint8_t, kTrackSize> bytes = {};
    bytes[0] = static_cast<uint8_t>(track.active | track.note_wait << 1 | track.muted << 2 | track.tie << 3 |
                                    track.note_finish_wait << 4 | track.portamento << 5 | track.compare << 6 |
                                    track.channel_mask_set << 7);
    bytes[1] = track.pan_range;
    Put16(bytes, 2, track.program);
    bytes[4] = track.volume;
    bytes[5] = track.expression;
    bytes[6] = static_cast<uint8_t>(track.pitch_bend);
    bytes[7] = track.bend_range;
    bytes[8] = static_cast<uint8_t>(track.pan);
    bytes[9] = static_cast<uint8_t>(track.external_pan);
    Put16(bytes, 0xA, static_cast<uint16_t>(track.external_fader));
    Put16(bytes, 0xC, static_cast<uint16_t>(track.external_pitch));
    bytes[0xE] = track.attack;
    bytes[0xF] = track.decay;
    bytes[0x10] = track.sustain;
    bytes[0x11] = track.release;
    bytes[0x12] = track.priority;
    bytes[0x13] = static_cast<uint8_t>(track.transpose);
    bytes[0x14] = track.portamento_key;
    bytes[0x15] = track.portamento_time;
    Put16(bytes, 0x16, static_cast<uint16_t>(track.sweep_pitch));
    PutLfo(bytes, 0x18, track.modulation);
    Put16(bytes, 0x1E, track.channel_mask);
    Put32(bytes, 0x20, static_cast<uint32_t>(track.wait));
    Put32(bytes, 0x24, track.base);
    Put32(bytes, 0x28, track.position);
    for (std::size_t i = 0; i < track.calls.size(); i++)
    {
        Put32(bytes, 0x2C + 4 * i, track.calls[i]);
        bytes[0x38 + i] = track.loop_counts[i];
    }
    bytes[0x3B] = track.call_depth;
    return bytes;
}

// The fields of a track from its bytes. The list of its channels stays: the model keeps it by index.
void SetTrackBytes(Track& track, std::span<const uint8_t> bytes)
{
    track.active = (bytes[0] & 0x01) != 0;
    track.note_wait = (bytes[0] & 0x02) != 0;
    track.muted = (bytes[0] & 0x04) != 0;
    track.tie = (bytes[0] & 0x08) != 0;
    track.note_finish_wait = (bytes[0] & 0x10) != 0;
    track.portamento = (bytes[0] & 0x20) != 0;
    track.compare = (bytes[0] & 0x40) != 0;
    track.channel_mask_set = (bytes[0] & 0x80) != 0;
    track.pan_range = bytes[1];
    track.program = Get16(bytes, 2);
    track.volume = bytes[4];
    track.expression = bytes[5];
    track.pitch_bend = static_cast<int8_t>(bytes[6]);
    track.bend_range = bytes[7];
    track.pan = static_cast<int8_t>(bytes[8]);
    track.external_pan = static_cast<int8_t>(bytes[9]);
    track.external_fader = static_cast<int16_t>(Get16(bytes, 0xA));
    track.external_pitch = static_cast<int16_t>(Get16(bytes, 0xC));
    track.attack = bytes[0xE];
    track.decay = bytes[0xF];
    track.sustain = bytes[0x10];
    track.release = bytes[0x11];
    track.priority = bytes[0x12];
    track.transpose = static_cast<int8_t>(bytes[0x13]);
    track.portamento_key = bytes[0x14];
    track.portamento_time = bytes[0x15];
    track.sweep_pitch = static_cast<int16_t>(Get16(bytes, 0x16));
    track.modulation = GetLfo(bytes, 0x18);
    track.channel_mask = Get16(bytes, 0x1E);
    track.wait = static_cast<int32_t>(Get32(bytes, 0x20));
    track.base = Get32(bytes, 0x24);
    track.position = Get32(bytes, 0x28);
    for (std::size_t i = 0; i < track.calls.size(); i++)
    {
        track.calls[i] = Get32(bytes, 0x2C + 4 * i);
        track.loop_counts[i] = bytes[0x38 + i];
    }
    track.call_depth = bytes[0x3B];
}

LfoParameters DefaultLfo()
{
    return {.target = kLfoPitch, .speed = 16, .depth = 0, .range = 1, .delay = 0};
}

// The LFO's sine over a cycle of 128 steps, from the table's quarter.
int8_t Sine(int x)
{
    if (x < 0x20)
    {
        return tables::kLfoSine[x];
    }
    if (x < 0x40)
    {
        return tables::kLfoSine[0x40 - x];
    }
    if (x < 0x60)
    {
        return static_cast<int8_t>(-tables::kLfoSine[x - 0x40]);
    }

    return static_cast<int8_t>(-tables::kLfoSine[0x20 - (x - 0x60)]);
}

int32_t LfoValue(const Lfo& lfo)
{
    if (lfo.parameters.depth == 0 || lfo.delay_counter < lfo.parameters.delay)
    {
        return 0;
    }

    return Sine(lfo.counter >> 8) * lfo.parameters.depth * lfo.parameters.range;
}

void StepLfoCounter(Lfo& lfo)
{
    if (lfo.delay_counter < lfo.parameters.delay)
    {
        lfo.delay_counter++;
        return;
    }

    // The counter's low byte holds the fraction of a step, and the high byte the step of the cycle of 128.
    uint32_t step = (lfo.counter + (static_cast<uint32_t>(lfo.parameters.speed) << 6)) >> 8;
    while (step >= 0x80)
    {
        step -= 0x80;
    }
    lfo.counter = static_cast<uint16_t>(lfo.counter + (lfo.parameters.speed << 6));
    lfo.counter &= 0xFF;
    lfo.counter = static_cast<uint16_t>(lfo.counter | step << 8);
}

uint16_t DecayRate(int value)
{
    if (value == 127)
    {
        return 0xFFFF;
    }
    if (value == 126)
    {
        return 0x3C00;
    }
    if (value < 50)
    {
        return static_cast<uint16_t>(value * 2 + 1);
    }

    return static_cast<uint16_t>(0x1E00 / (126 - value));
}

// SOUNDxTMR's period for a wave's period `timer` moved by `pitch`, in 1/64 of a semitone.
uint16_t Timer(int timer, int pitch)
{
    int octave = 0;
    int fraction = -pitch;
    while (fraction < 0)
    {
        octave--;
        fraction += 768;
    }
    while (fraction >= 768)
    {
        octave++;
        fraction -= 768;
    }

    uint64_t result = (tables::kPitch[fraction] + 0x10000ull) * static_cast<uint32_t>(timer);
    int shift = octave - 16;
    if (shift <= 0)
    {
        result >>= -shift;
    }
    else if (shift < 32)
    {
        if ((result & (~0ull << (32 - shift))) != 0)
        {
            return 0xFFFF;
        }
        result <<= shift;
    }
    else
    {
        return 0xFFFF;
    }

    if (result < 0x10)
    {
        return 0x10;
    }
    if (result > 0xFFFF)
    {
        return 0xFFFF;
    }

    return static_cast<uint16_t>(result);
}

// SOUNDxCNT's volume and divider for a level in 0.1 dB.
uint16_t ChannelVolume(int level)
{
    level = std::clamp(level, kSilent, 0);
    const int volume = tables::kVolume[level - kSilent];
    const int divider = level < -240 ? 3 : level < -120 ? 2 : level < -60 ? 1 : 0;
    return static_cast<uint16_t>(volume | divider << 8);
}

} // namespace

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

Hang::Hang(int player, int track, uint32_t offset)
    : std::runtime_error("track " + std::to_string(track) + " of player " + std::to_string(player) + " runs " +
                         std::to_string(kCommandBudget) + " commands in a tick without waiting, up to offset " +
                         xsflib::Hex(offset, 0, true) + ": the sequence hangs"),
      player_(player),
      track_(track),
      offset_(offset)
{
}

int Hang::PlayerIndex() const
{
    return player_;
}

int Hang::TrackIndex() const
{
    return track_;
}

uint32_t Hang::Offset() const
{
    return offset_;
}

Build BuildOf(std::span<const uint8_t> arm7)
{
    // The tables start with the LFO's sine. The squares' levels follow on 4 bytes, then the levels of i/127 in the
    // builds that hold them, then the volume shifts.
    const auto sine = std::search(arm7.begin(), arm7.end(), std::begin(tables::kLfoSine), std::end(tables::kLfoSine),
                                  [](uint8_t byte, int8_t value) { return byte == static_cast<uint8_t>(value); });
    if (sine == arm7.end() ||
        std::search(sine + 1, arm7.end(), std::begin(tables::kLfoSine), std::end(tables::kLfoSine),
                    [](uint8_t byte, int8_t value) { return byte == static_cast<uint8_t>(value); }) != arm7.end())
    {
        throw std::runtime_error("the ARM7 binary doesn't hold the sound driver's tables once");
    }

    const auto at = static_cast<std::size_t>(sine - arm7.begin());
    const std::size_t squares = (at + std::size(tables::kLfoSine) + 3) & ~std::size_t{3};
    const std::size_t after = squares + 2 * std::size(tables::kDecibelSquare);
    if (after + std::size(tables::kVolumeShift) > arm7.size())
    {
        throw std::runtime_error("the ARM7 binary ends inside the sound driver's tables");
    }

    Build build;
    build.zero_level = static_cast<int16_t>(arm7[squares] | arm7[squares + 1] << 8);
    build.decibel_table = !std::equal(std::begin(tables::kVolumeShift), std::end(tables::kVolumeShift),
                                      arm7.begin() + static_cast<std::ptrdiff_t>(after));
    return build;
}

void InitSharedWork(std::span<uint8_t> work)
{
    std::fill(work.begin(), work.begin() + kSharedWorkSize, uint8_t{0});
    for (uint32_t player = 0; player < kPlayerCount; player++)
    {
        std::fill_n(work.begin() + kPlayerWork + kPlayerWorkSize * player, 2 * kVariableCount, uint8_t{0xFF});
    }
    std::fill_n(work.begin() + kGlobalVariables, 2 * kVariableCount, uint8_t{0xFF});
}

Driver::Driver(std::span<uint8_t> main_memory, Registers& registers, Build build)
    : main_memory_(main_memory), registers_(&registers), build_(build)
{
    // The tables lie one after another in the driver's binary. Values past the end of one read the next.
    for (int i = 0; i < 128; i++)
    {
        const int16_t level = i == 0 ? build.zero_level : tables::kDecibelSquare[i];
        tables_.push_back(static_cast<uint8_t>(level));
        tables_.push_back(static_cast<uint8_t>(static_cast<uint16_t>(level) >> 8));
    }
    if (build.decibel_table)
    {
        for (const int16_t level : tables::kDecibel)
        {
            tables_.push_back(static_cast<uint8_t>(level));
            tables_.push_back(static_cast<uint8_t>(static_cast<uint16_t>(level) >> 8));
        }
    }
    shifts_ = tables_.size();
    tables_.insert(tables_.end(), std::begin(tables::kVolumeShift), std::end(tables::kVolumeShift));
    tables_.insert(tables_.end(), std::begin(tables::kChannelOrder), std::end(tables::kChannelOrder));
    fast_attack_ = tables_.size();
    tables_.insert(tables_.end(), std::begin(tables::kFastAttack), std::end(tables::kFastAttack));
}

void Driver::Start()
{
    channels_.resize(kChannelCount);
    original_volumes_.resize(kChannelCount);
    original_pans_.resize(kChannelCount);
    for (int i = 0; i < kChannelCount; i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        channel.id = static_cast<uint16_t>(i);
        channel.twin = static_cast<uint16_t>(i);
        channel.sync = 0;
        channel.active = false;
    }
    locked_ = 0;
    weakly_locked_ = 0;
    for (int i = 0; i < kPlayerCount; i++)
    {
        players_[static_cast<std::size_t>(i)].active = false;
        players_[static_cast<std::size_t>(i)].id = static_cast<uint8_t>(i);
    }
    for (Track& track : tracks_)
    {
        track.active = false;
    }

    registers_->Write(kMixRegister, 1, registers_->Read(kMixRegister, 1) | 0x80);
    SetOutputSelector(0, 0, 0, 0);
    registers_->Write(kMasterVolumeRegister, 1, 0x7F);
}

void Driver::Frame(bool step, std::span<const CommandList> lists)
{
    UpdateChannels();
    for (const CommandList& list : lists)
    {
        for (const Command& command : list)
        {
            Run(command);
        }
        if (shared_work_ != 0)
        {
            Write32(shared_work_ + kFinishedCommandTag, Read32(shared_work_ + kFinishedCommandTag) + 1);
        }
    }
    StepPlayers(step);
    StepChannels(step);
    UpdateSharedWork();
    Random();
}

const Channel& Driver::GetChannel(int channel) const
{
    return channels_[static_cast<std::size_t>(channel)];
}

void Driver::GrowChannels(bool on)
{
    growing_ = on;
    if (on)
    {
        // The channels never move: a reference to one stays valid as others grow.
        channels_.reserve(kMostChannels);
    }
}

const Track& Driver::GetTrack(int track) const
{
    return tracks_[static_cast<std::size_t>(track)];
}

const Player& Driver::GetPlayer(int player) const
{
    return players_[static_cast<std::size_t>(player)];
}

uint8_t Driver::Read8(uint32_t address) const
{
    if (address < kMainMemory || address >= kMainMemoryEnd || main_memory_.empty())
    {
        return 0;
    }

    return main_memory_[(address - kMainMemory) % main_memory_.size()];
}

uint16_t Driver::Read16(uint32_t address) const
{
    return static_cast<uint16_t>(Read8(address) | Read8(address + 1) << 8);
}

uint32_t Driver::Read32(uint32_t address) const
{
    return Read16(address) | static_cast<uint32_t>(Read16(address + 2)) << 16;
}

void Driver::Write8(uint32_t address, uint8_t value)
{
    if (address >= kMainMemory && address < kMainMemoryEnd && !main_memory_.empty())
    {
        main_memory_[(address - kMainMemory) % main_memory_.size()] = value;
    }
}

void Driver::Write16(uint32_t address, uint16_t value)
{
    Write8(address, static_cast<uint8_t>(value));
    Write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void Driver::Write32(uint32_t address, uint32_t value)
{
    Write16(address, static_cast<uint16_t>(value));
    Write16(address + 2, static_cast<uint16_t>(value >> 16));
}

int16_t Driver::DecibelSquare(int index) const
{
    const std::size_t at = static_cast<std::size_t>(index) * 2;
    if (index < 0 || at + 1 >= tables_.size())
    {
        return 0;
    }

    return static_cast<int16_t>(tables_[at] | tables_[at + 1] << 8);
}

uint8_t Driver::FastAttack(int index) const
{
    const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(fast_attack_) + index;
    if (at < 0 || static_cast<std::size_t>(at) >= tables_.size())
    {
        return 0;
    }

    return tables_[static_cast<std::size_t>(at)];
}

void Driver::SetUpPcmChannel(int channel, uint32_t data, int format, int loop, int loop_start, int loop_length,
                             int volume, int divider, int timer, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 4,
                      static_cast<uint32_t>(format) << 29 | static_cast<uint32_t>(loop) << 27 |
                          static_cast<uint32_t>(pan) << 16 | static_cast<uint32_t>(divider) << 8 |
                          static_cast<uint32_t>(volume));
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
    registers_->Write(ChannelRegister(channel, 0xA), 2, static_cast<uint16_t>(loop_start));
    registers_->Write(ChannelRegister(channel, 0xC), 4, static_cast<uint32_t>(loop_length));
    registers_->Write(ChannelRegister(channel, 0x4), 4, data);
}

void Driver::SetUpPsgChannel(int channel, int duty, int volume, int divider, int timer, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 4,
                      0x60000000u | static_cast<uint32_t>(duty) << 24 | static_cast<uint32_t>(pan) << 16 |
                          static_cast<uint32_t>(divider) << 8 | static_cast<uint32_t>(volume));
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
}

void Driver::SetUpNoiseChannel(int channel, int volume, int divider, int timer, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 4,
                      0x60000000u | static_cast<uint32_t>(pan) << 16 | static_cast<uint32_t>(divider) << 8 |
                          static_cast<uint32_t>(volume));
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
}

void Driver::StopChannel(int channel, int hold)
{
    uint32_t control = registers_->Read(ChannelRegister(channel, 0x0), 4);
    control &= ~0x80000000u;
    if (hold & 1)
    {
        control |= 0x8000;
    }
    registers_->Write(ChannelRegister(channel, 0x0), 4, control);
}

void Driver::SetChannelVolume(int channel, int volume, int divider)
{
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        const int pan = static_cast<int>(registers_->Read(ChannelRegister(channel, 0x2), 1));
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 2, static_cast<uint16_t>(divider << 8 | volume));
}

void Driver::SetChannelTimer(int channel, int timer)
{
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
}

void Driver::SetChannelPan(int channel, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    registers_->Write(ChannelRegister(channel, 0x2), 1, static_cast<uint8_t>(pan));
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        registers_->Write(
            ChannelRegister(channel, 0x0), 1,
            static_cast<uint8_t>(SurroundDecay(original_volumes_[static_cast<std::size_t>(channel)], pan)));
    }
}

bool Driver::IsChannelActive(int channel)
{
    return (registers_->Read(ChannelRegister(channel, 0x3), 1) & 0x80) != 0;
}

bool Driver::Surrounded(int channel) const
{
    return ((1u << channels_[static_cast<std::size_t>(channel)].twin) & kSurroundChannels) != 0;
}

void Driver::SetMasterPan(int pan)
{
    master_pan_ = pan;
    for (int i = 0; i < ChannelCount(); i++)
    {
        registers_->Write(ChannelRegister(i, 0x2), 1,
                          pan >= 0 ? static_cast<uint8_t>(pan) : original_pans_[static_cast<std::size_t>(i)]);
    }
}

void Driver::SetSurroundDecay(int decay)
{
    surround_decay_ = decay;
    for (int i = 0; i < ChannelCount(); i++)
    {
        if (Surrounded(i))
        {
            const int pan = static_cast<int>(registers_->Read(ChannelRegister(i, 0x2), 1));
            registers_->Write(ChannelRegister(i, 0x0), 1,
                              static_cast<uint8_t>(SurroundDecay(original_volumes_[static_cast<std::size_t>(i)], pan)));
        }
    }
}

int Driver::SurroundDecay(int volume, int pan) const
{
    if (pan < 24)
    {
        return volume * (surround_decay_ * (pan + 40) + ((0x7FFF - surround_decay_) << 6)) >> 21;
    }
    if (pan <= 104)
    {
        return volume;
    }

    return volume * (-surround_decay_ * (pan - 40) + ((surround_decay_ + 0x7FFF) << 6)) >> 21;
}

void Driver::SetOutputSelector(int left, int right, int channel1, int channel3)
{
    const int enable = (registers_->Read(kMixRegister, 1) & 0x80) ? 1 : 0;
    registers_->Write(kMixRegister, 1,
                      static_cast<uint8_t>(enable << 7 | channel3 << 5 | channel1 << 4 | right << 2 | left));
}

void Driver::SetUpCapture(int capture, int format, uint32_t address, int size, bool loop, int source, int add)
{
    const uint32_t offset = static_cast<uint32_t>(capture) * 8;
    registers_->Write(kCaptureControl + static_cast<uint32_t>(capture), 1,
                      static_cast<uint8_t>(format << 3 | (loop ? 0 : 1) << 2 | source << 1 | add));
    registers_->Write(kCaptureAddress + offset, 4, address);
    registers_->Write(kCaptureLength + offset, 2, static_cast<uint16_t>(size));
}

bool Driver::IsCaptureActive(int capture)
{
    return (registers_->Read(kCaptureControl + static_cast<uint32_t>(capture), 1) & 0x80) != 0;
}

void Driver::UpdateChannels()
{
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.sync == 0)
        {
            continue;
        }

        if (channel.sync & kSyncStop)
        {
            StopChannel(i, 0);
        }

        if (channel.sync & kSyncStart)
        {
            switch (channel.type)
            {
            case kPcm:
                SetUpPcmChannel(i, channel.data, channel.wave.format, channel.wave.loop ? 1 : 2,
                                channel.wave.loop_start, static_cast<int>(channel.wave.loop_length),
                                channel.volume & 0xFF, channel.volume >> 8, channel.timer, channel.pan);
                break;
            case kPsg:
                SetUpPsgChannel(i, static_cast<int>(channel.data), channel.volume & 0xFF, channel.volume >> 8,
                                channel.timer, channel.pan);
                break;
            case kNoise:
                SetUpNoiseChannel(i, channel.volume & 0xFF, channel.volume >> 8, channel.timer, channel.pan);
                break;
            default:
                break;
            }
        }
        else
        {
            if (channel.sync & kSyncTimer)
            {
                SetChannelTimer(i, channel.timer);
            }
            if (channel.sync & kSyncVolume)
            {
                SetChannelVolume(i, channel.volume & 0xFF, channel.volume >> 8);
            }
            if (channel.sync & kSyncPan)
            {
                SetChannelPan(i, channel.pan);
            }
        }
    }

    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.sync == 0)
        {
            continue;
        }

        if (channel.sync & kSyncStart)
        {
            registers_->Write(ChannelRegister(i, 0x3), 1, registers_->Read(ChannelRegister(i, 0x3), 1) | 0x80);
        }
        channel.sync = 0;
    }
}

void Driver::StepChannels(bool step)
{
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (!channel.active)
        {
            continue;
        }

        if (channel.start)
        {
            channel.sync |= kSyncStart;
            channel.start = false;
        }
        else if (!IsChannelActive(i))
        {
            // The sound unit has stopped the channel: a one-shot wave has ended.
            if (channel.owner >= 0)
            {
                Callback(channel, 1);
            }
            else
            {
                channel.priority = 0;
            }
            channel.volume = 0;
            channel.active = false;
            continue;
        }

        int32_t volume = DecibelSquare(channel.velocity);
        int32_t pitch = (channel.key - channel.root_key) * 0x40;
        int32_t pan = 0;
        volume += StepEnvelope(channel, step);
        pitch += StepSweep(channel, step);
        volume += channel.user_decay;
        volume += channel.user_decay2;
        pitch += channel.user_pitch;

        const int32_t lfo = StepLfo(channel, step);
        switch (channel.lfo.parameters.target)
        {
        case kLfoVolume:
            if (volume > -0x8000)
            {
                volume += lfo;
            }
            break;
        case kLfoPan:
            pan += lfo;
            break;
        case kLfoPitch:
            pitch += lfo;
            break;
        default:
            break;
        }

        pan += channel.initial_pan;
        if (channel.pan_range != 127)
        {
            pan = (pan * channel.pan_range + 0x40) >> 7;
        }
        pan += channel.user_pan;

        if (channel.envelope_stage == kRelease && volume <= kSilent)
        {
            channel.sync = kSyncStop;
            if (channel.owner >= 0)
            {
                Callback(channel, 1);
            }
            else
            {
                channel.priority = 0;
            }
            channel.volume = 0;
            channel.active = false;
            continue;
        }

        const uint16_t new_volume = ChannelVolume(volume);
        uint16_t timer = Timer(channel.wave.timer, pitch);
        if (channel.type == kPsg)
        {
            timer &= 0xFFFC;
        }
        pan = std::clamp(pan + 0x40, 0, 127);

        if (new_volume != channel.volume)
        {
            channel.volume = new_volume;
            channel.sync |= kSyncVolume;
        }
        if (timer != channel.timer)
        {
            channel.timer = timer;
            channel.sync |= kSyncTimer;
        }
        if (pan != channel.pan)
        {
            channel.pan = static_cast<uint8_t>(pan);
            channel.sync |= kSyncPan;
        }
    }
}

int32_t Driver::StepEnvelope(Channel& channel, bool step)
{
    if (step)
    {
        switch (channel.envelope_stage)
        {
        case kAttack:
            channel.attenuation = -((-channel.attenuation * channel.attack) >> 8);
            if (channel.attenuation == 0)
            {
                channel.envelope_stage = kDecay;
            }
            break;
        case kDecay:
            {
                const int32_t sustain = DecibelSquare(channel.sustain) * 128;
                channel.attenuation -= channel.decay;
                if (channel.attenuation <= sustain)
                {
                    channel.attenuation = sustain;
                    channel.envelope_stage = kSustain;
                }
                break;
            }
        case kRelease:
            channel.attenuation -= channel.release;
            break;
        default:
            break;
        }
    }

    return channel.attenuation >> 7;
}

int32_t Driver::StepSweep(Channel& channel, bool step)
{
    if (channel.sweep_pitch == 0 || channel.sweep_counter >= channel.sweep_length)
    {
        return 0;
    }

    const int64_t result = static_cast<int64_t>(channel.sweep_pitch) * (channel.sweep_length - channel.sweep_counter) /
                           channel.sweep_length;
    if (step && channel.auto_sweep)
    {
        channel.sweep_counter++;
    }

    return static_cast<int32_t>(result);
}

int32_t Driver::StepLfo(Channel& channel, bool step)
{
    int64_t result = LfoValue(channel.lfo);
    if (result != 0)
    {
        switch (channel.lfo.parameters.target)
        {
        case kLfoVolume:
            result *= 60;
            break;
        case kLfoPitch:
        case kLfoPan:
            result *= 64;
            break;
        default:
            break;
        }
        result >>= 14;
    }

    if (step)
    {
        StepLfoCounter(channel.lfo);
    }

    return static_cast<int32_t>(result);
}

void Driver::SetAttack(Channel& channel, int attack)
{
    channel.attack = attack < 109 ? static_cast<uint8_t>(255 - attack) : FastAttack(127 - attack);
}

void Driver::SetDecay(Channel& channel, int decay)
{
    channel.decay = DecayRate(decay);
}

void Driver::SetRelease(Channel& channel, int release)
{
    channel.release = DecayRate(release);
}

void Driver::StartChannel(Channel& channel, int32_t length)
{
    channel.attenuation = kStartAttenuation;
    channel.envelope_stage = kAttack;
    channel.length = length;
    channel.lfo.counter = 0;
    channel.lfo.delay_counter = 0;
    channel.start = true;
    channel.active = true;
}

int Driver::Allocate(uint32_t mask, int priority, bool weak_locks_too, int owner)
{
    mask &= ~locked_;
    if (!weak_locks_too)
    {
        mask &= ~weakly_locked_;
    }

    // The channel of the lowest priority, and of those the quietest, in the driver's order.
    const auto loudness = [this](const Channel& channel)
    {
        const std::size_t shift = shifts_ + static_cast<std::size_t>(channel.volume >> 8);
        return ((channel.volume & 0xFF) << 4) >> (shift < tables_.size() ? tables_[shift] : 0);
    };
    int chosen = -1;
    for (const uint8_t candidate : tables::kChannelOrder)
    {
        if (!(mask & (1u << candidate)))
        {
            continue;
        }
        const Channel& channel = channels_[candidate];
        if (chosen < 0)
        {
            chosen = candidate;
            continue;
        }
        const Channel& best = channels_[static_cast<std::size_t>(chosen)];
        if (channel.priority > best.priority)
        {
            continue;
        }
        if (channel.priority != best.priority || loudness(best) > loudness(channel))
        {
            chosen = candidate;
        }
    }

    if (chosen < 0)
    {
        return -1;
    }
    // A driver that grows its channels gives the note another channel in place of a channel whose note sounds before
    // its release, or where the note's priority takes no channel. A note in its release gives way, as on the console.
    const Channel& console_choice = channels_[static_cast<std::size_t>(chosen)];
    const bool sounding = console_choice.active && console_choice.envelope_stage != kRelease;
    if (growing_ && (sounding || priority < console_choice.priority))
    {
        if (const int grown = GrownChannel(chosen); grown >= 0)
        {
            chosen = grown;
        }
    }
    Channel& channel = channels_[static_cast<std::size_t>(chosen)];
    if (priority < channel.priority)
    {
        return -1;
    }

    if (channel.owner >= 0)
    {
        Callback(channel, 0);
    }
    channel.sync = kSyncStop;
    channel.active = false;

    channel.next = -1;
    channel.owner = owner;
    channel.length = 0;
    channel.priority = static_cast<uint8_t>(priority);
    channel.volume = 127;
    channel.start = false;
    channel.auto_sweep = true;
    channel.key = 60;
    channel.root_key = 60;
    channel.velocity = 127;
    channel.initial_pan = 0;
    channel.user_decay = 0;
    channel.user_decay2 = 0;
    channel.user_pitch = 0;
    channel.user_pan = 0;
    channel.pan_range = 127;
    channel.sweep_pitch = 0;
    channel.sweep_length = 0;
    channel.sweep_counter = 0;
    SetAttack(channel, 127);
    SetDecay(channel, 127);
    channel.sustain = 127;
    SetRelease(channel, 127);
    channel.lfo.parameters = DefaultLfo();
    return chosen;
}

int Driver::GrownChannel(int chosen)
{
    // A grown channel of the same part that plays no more, or else a new one.
    const int part = PartOf(chosen);
    for (int i = kChannelCount; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (PartOf(channel.twin) == part && !channel.active && channel.owner < 0 && channel.sync == 0 &&
            !IsChannelActive(i))
        {
            channel.twin = static_cast<uint16_t>(chosen);
            registers_->Write(ChannelRegister(i, kGrownChannelTwin), 4, static_cast<uint32_t>(chosen));
            return i;
        }
    }
    if (ChannelCount() >= kMostChannels)
    {
        return -1;
    }

    const int index = ChannelCount();
    Channel& channel = channels_.emplace_back();
    channel.id = static_cast<uint16_t>(index);
    channel.twin = static_cast<uint16_t>(chosen);
    original_volumes_.push_back(0);
    original_pans_.push_back(0);
    registers_->Write(ChannelRegister(index, kGrownChannelTwin), 4, static_cast<uint32_t>(chosen));
    return index;
}

void Driver::Free(Channel& channel)
{
    channel.owner = -1;
}

// The callback of a sequence's channel: status 0 when another note takes the channel, 1 when the channel ends.
void Driver::Callback(Channel& channel, int status)
{
    const int owner = channel.owner;
    if (status == 1)
    {
        channel.priority = 0;
        Free(channel);
    }

    Track& track = tracks_[static_cast<std::size_t>(owner)];
    const int index = channel.id;
    if (track.channels == index)
    {
        track.channels = channel.next;
        return;
    }
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        Channel& previous = channels_[static_cast<std::size_t>(at)];
        if (previous.next == index)
        {
            previous.next = channel.next;
            return;
        }
    }
}

void Driver::StopUnlockedChannels(uint32_t mask)
{
    for (int i = 0; i < kChannelCount && mask != 0; i++, mask >>= 1)
    {
        if (!(mask & 1) || (locked_ & (1u << i)))
        {
            continue;
        }

        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.owner >= 0)
        {
            Callback(channel, 0);
        }
        StopChannel(i, 0);
        channel.priority = 0;
        Free(channel);
        channel.sync = 0;
        channel.active = false;
    }
}

void Driver::LockChannels(uint32_t mask, uint32_t weak)
{
    StopUnlockedChannels(mask);
    if (weak & 1)
    {
        weakly_locked_ |= mask;
    }
    else
    {
        locked_ |= mask;
    }
}

void Driver::InvalidateWave(uint32_t start, uint32_t end)
{
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.active && channel.type == kPcm && start <= channel.data && channel.data <= end)
        {
            channel.start = false;
            StopChannel(i, 0);
        }
    }
}

bool Driver::ReadInstrument(uint32_t bank, int program, int key, Instrument& instrument) const
{
    if (program < 0 || static_cast<uint32_t>(program) >= Read32(bank + kBankCount))
    {
        return false;
    }

    const auto read_parameters = [this, &instrument](uint32_t at)
    {
        instrument.wave = {Read16(at), Read16(at + 2)};
        instrument.root_key = Read8(at + 4);
        instrument.attack = Read8(at + 5);
        instrument.decay = Read8(at + 6);
        instrument.sustain = Read8(at + 7);
        instrument.release = Read8(at + 8);
        instrument.pan = Read8(at + 9);
    };

    const uint32_t offset = Read32(bank + kBankOffsets + 4 * static_cast<uint32_t>(program));
    const uint32_t at = bank + (offset >> 8);
    instrument.type = static_cast<uint8_t>(offset);
    switch (instrument.type)
    {
    case kInstrumentPcm:
    case kInstrumentPsg:
    case kInstrumentNoise:
    case kInstrumentDirectPcm:
    case kInstrumentDummy:
        read_parameters(at);
        return true;
    case kInstrumentDrums:
        {
            const int low = Read8(at);
            const int high = Read8(at + 1);
            if (key < low || key > high)
            {
                return false;
            }
            const uint32_t region = at + 2 + 12 * static_cast<uint32_t>(key - low);
            instrument.type = Read8(region);
            read_parameters(region + 2);
            return true;
        }
    case kInstrumentKeySplit:
        {
            int i = 0;
            while (key > Read8(at + static_cast<uint32_t>(i)))
            {
                i++;
                if (i >= kKeySplits)
                {
                    return false;
                }
            }
            const uint32_t region = at + kKeySplits + 12 * static_cast<uint32_t>(i);
            instrument.type = Read8(region);
            read_parameters(region + 2);
            return true;
        }
    default:
        return false;
    }
}

uint32_t Driver::WaveAddress(uint32_t bank, int wave_archive, int wave) const
{
    const uint32_t archive = Read32(bank + kBankWaveArchives + 8 * static_cast<uint32_t>(wave_archive));
    if (archive == 0 || static_cast<uint32_t>(wave) >= Read32(archive + kWaveArchiveCount))
    {
        return 0;
    }

    // An offset below main memory counts from the wave archive's start. A larger one is the wave's address.
    uint32_t address = Read32(archive + kWaveArchiveOffsets + 4 * static_cast<uint32_t>(wave));
    if (address != 0 && address < kMainMemory)
    {
        address += archive;
    }

    return address;
}

bool Driver::NoteOn(Channel& channel, int key, int velocity, int32_t length, uint32_t bank,
                    const Instrument& instrument)
{
    uint8_t release = instrument.release;
    if (release == 0xFF)
    {
        length = -1;
        release = 0;
    }

    bool started = false;
    switch (instrument.type)
    {
    case kInstrumentPcm:
    case kInstrumentDirectPcm:
        {
            const uint32_t wave = instrument.type == kInstrumentPcm
                                      ? WaveAddress(bank, instrument.wave[1], instrument.wave[0])
                                      : static_cast<uint32_t>(instrument.wave[1]) << 16 | instrument.wave[0];
            if (wave != 0)
            {
                channel.type = kPcm;
                channel.wave = {Read8(wave),      Read8(wave + 1),  Read16(wave + 2),
                                Read16(wave + 4), Read16(wave + 6), Read32(wave + 8)};
                channel.data = wave + kWaveParametersSize;
                StartChannel(channel, length);
                started = true;
            }
            break;
        }
    case kInstrumentPsg:
        if (channel.twin >= 8 && channel.twin <= 13)
        {
            channel.type = kPsg;
            channel.data = instrument.wave[0];
            channel.wave.timer = 8006;
            StartChannel(channel, length);
            started = true;
        }
        break;
    case kInstrumentNoise:
        if (channel.twin >= 14 && channel.twin <= 15)
        {
            channel.type = kNoise;
            channel.wave.timer = 8006;
            StartChannel(channel, length);
            started = true;
        }
        break;
    default:
        break;
    }

    if (!started)
    {
        return false;
    }

    channel.key = static_cast<uint8_t>(key);
    channel.root_key = instrument.root_key;
    channel.velocity = static_cast<uint8_t>(velocity);
    SetAttack(channel, instrument.attack);
    SetDecay(channel, instrument.decay);
    channel.sustain = instrument.sustain;
    SetRelease(channel, release);
    channel.initial_pan = static_cast<int8_t>(instrument.pan - 0x40);
    return true;
}

void Driver::StepPlayers(bool step)
{
    uint32_t status = 0;
    for (int i = 0; i < kPlayerCount; i++)
    {
        Player& player = players_[static_cast<std::size_t>(i)];
        if (!player.active)
        {
            continue;
        }

        if (player.prepared)
        {
            if (step && !player.paused)
            {
                StepPlayer(player);
            }
            for (int index = 0; index < kTracksPerPlayer; index++)
            {
                if (Track* track = PlayerTrack(player, index))
                {
                    UpdateTrackChannels(*track, player, true);
                }
            }
        }
        if (player.active)
        {
            status |= 1u << i;
        }
    }

    if (shared_work_ != 0)
    {
        Write32(shared_work_ + kPlayerStatus, status);
    }
}

void Driver::PrepareSequence(int index, uint32_t sequence, uint32_t offset, uint32_t bank)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (player.active)
    {
        StopPlayer(player);
    }
    InitPlayer(player, bank);

    int allocated = AllocateTrack();
    if (allocated < 0)
    {
        return;
    }
    Track& first = tracks_[static_cast<std::size_t>(allocated)];
    InitTrack(first);
    first.base = sequence;
    first.position = sequence + offset;
    player.tracks[0] = static_cast<uint8_t>(allocated);

    // A sequence that starts with 0xFE allocates the tracks that its mask names.
    if (Read8(first.position) == 0xFE)
    {
        first.position++;
        uint16_t mask = static_cast<uint16_t>(ReadU16(first) >> 1);
        for (int track = 1; mask != 0; track++, mask >>= 1)
        {
            if (!(mask & 1))
            {
                continue;
            }
            allocated = AllocateTrack();
            if (allocated < 0)
            {
                break;
            }
            InitTrack(tracks_[static_cast<std::size_t>(allocated)]);
            player.tracks[static_cast<std::size_t>(track)] = static_cast<uint8_t>(allocated);
        }
    }

    player.active = true;
    player.prepared = false;
    if (shared_work_ != 0)
    {
        Write32(shared_work_ + kPlayerStatus, Read32(shared_work_ + kPlayerStatus) | 1u << index);
    }
}

void Driver::StopSequence(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (!player.active)
    {
        return;
    }

    StopPlayer(player);
    if (shared_work_ != 0)
    {
        Write32(shared_work_ + kPlayerStatus, Read32(shared_work_ + kPlayerStatus) & ~(1u << index));
    }
}

void Driver::PauseSequence(int index, uint32_t pause)
{
    // The flag takes the value's low bit, and the release of the notes the whole value.
    Player& player = players_[static_cast<std::size_t>(index)];
    player.paused = (pause & 1) != 0;
    if (pause == 0)
    {
        return;
    }

    for (int i = 0; i < kTracksPerPlayer; i++)
    {
        if (Track* track = PlayerTrack(player, i))
        {
            ReleaseTrackChannels(*track, player, 127);
            FreeTrackChannels(*track);
        }
    }
}

void Driver::SkipSequence(int index, uint32_t ticks)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kTracksPerPlayer; i++)
    {
        if (Track* track = PlayerTrack(player, i))
        {
            ReleaseTrackChannels(*track, player, 127);
            FreeTrackChannels(*track);
        }
    }

    uint32_t skipped = 0;
    for (; skipped < ticks; skipped++)
    {
        if (StepTicks(player, false))
        {
            StopPlayer(player);
            break;
        }
    }
    if (shared_work_ != 0)
    {
        const uint32_t counter = shared_work_ + kPlayerWork + kPlayerWorkSize * player.id + kTickCounter;
        Write32(counter, Read32(counter) + skipped);
    }
}

void Driver::SetTrackMute(int index, uint32_t tracks, int mode)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kTracksPerPlayer && tracks != 0; i++, tracks >>= 1)
    {
        if (tracks & 1)
        {
            if (Track* track = PlayerTrack(player, i))
            {
                MuteTrack(*track, player, mode);
            }
        }
    }
}

void Driver::SetAllocatableChannels(int index, uint32_t tracks, uint32_t channels)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kTracksPerPlayer && tracks != 0; i++, tracks >>= 1)
    {
        if (tracks & 1)
        {
            if (Track* track = PlayerTrack(player, i))
            {
                track->channel_mask = static_cast<uint16_t>(channels);
                track->channel_mask_set = true;
            }
        }
    }
}

void Driver::InvalidateSequence(uint32_t start, uint32_t end)
{
    for (Player& player : players_)
    {
        if (!player.active)
        {
            continue;
        }
        for (int i = 0; i < kTracksPerPlayer; i++)
        {
            const Track* track = PlayerTrack(player, i);
            if (track && start <= track->position && track->position <= end)
            {
                StopPlayer(player);
                break;
            }
        }
    }
}

void Driver::InvalidateBank(uint32_t start, uint32_t end)
{
    for (Player& player : players_)
    {
        if (player.active && start <= player.bank && player.bank <= end)
        {
            StopPlayer(player);
        }
    }
}

void Driver::SetPlayerParameter(int index, uint32_t offset, uint32_t value, int size)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    std::array<uint8_t, kPlayerSize> bytes = PlayerBytes(player);
    Store(bytes, offset, value, size);
    SetPlayerBytes(player, bytes);
}

void Driver::SetTrackParameter(int index, uint32_t tracks, uint32_t offset, uint32_t value, int size)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kTracksPerPlayer && tracks != 0; i++, tracks >>= 1)
    {
        if (!(tracks & 1))
        {
            continue;
        }
        if (Track* track = PlayerTrack(player, i))
        {
            std::array<uint8_t, kTrackSize> bytes = TrackBytes(*track);
            Store(bytes, offset, value, size);
            SetTrackBytes(*track, bytes);
        }
    }
}

void Driver::InitPlayer(Player& player, uint32_t bank)
{
    player.paused = false;
    player.bank = bank;
    player.tempo = 120;
    player.tempo_ratio = 256;
    player.tempo_counter = kTickRate;
    player.volume = 0x7F;
    player.external_fader = 0;
    player.priority = 64;
    player.tracks.fill(kNoTrack);
    player_follows_[player.id] = {};

    if (shared_work_ != 0)
    {
        const uint32_t work = shared_work_ + kPlayerWork + kPlayerWorkSize * player.id;
        Write32(work + kTickCounter, 0);
        for (uint32_t i = 0; i < kTracksPerPlayer; i++)
        {
            Write16(work + 2 * i, 0xFFFF);
        }
    }
}

void Driver::InitTrack(Track& track)
{
    track_follows_[static_cast<std::size_t>(&track - tracks_.data())] = {};
    track.base = 0;
    track.position = 0;
    track.note_wait = true;
    track.muted = false;
    track.tie = false;
    track.note_finish_wait = false;
    track.portamento = false;
    track.compare = true;
    track.channel_mask_set = false;
    track.call_depth = 0;
    track.program = 0;
    track.priority = 64;
    track.volume = 127;
    track.expression = 127;
    track.external_fader = 0;
    track.pan = 0;
    track.external_pan = 0;
    track.pitch_bend = 0;
    track.external_pitch = 0;
    track.attack = 255;
    track.decay = 255;
    track.sustain = 255;
    track.release = 255;
    track.pan_range = 127;
    track.bend_range = 2;
    track.portamento_key = 60;
    track.portamento_time = 0;
    track.sweep_pitch = 0;
    track.transpose = 0;
    track.channel_mask = 0xFFFF;
    track.modulation = DefaultLfo();
    track.wait = 0;
    track.channels = -1;
}

int Driver::AllocateTrack()
{
    for (int i = 0; i < kTrackCount; i++)
    {
        Track& track = tracks_[static_cast<std::size_t>(i)];
        if (!track.active)
        {
            track.active = true;
            return i;
        }
    }

    return -1;
}

Track* Driver::PlayerTrack(Player& player, int index)
{
    if (index >= kTracksPerPlayer || player.tracks[static_cast<std::size_t>(index)] == kNoTrack)
    {
        return nullptr;
    }

    return &tracks_[player.tracks[static_cast<std::size_t>(index)] % kTrackCount];
}

void Driver::StepPlayer(Player& player)
{
    int ticks = 0;
    while (player.tempo_counter >= kTickRate)
    {
        player.tempo_counter = static_cast<uint16_t>(player.tempo_counter - kTickRate);
        ticks++;
    }

    int played = 0;
    for (; played < ticks; played++)
    {
        if (on_tick)
        {
            on_tick(player.id);
        }
        if (StepTicks(player, true))
        {
            StopPlayer(player);
            break;
        }
    }
    if (shared_work_ != 0)
    {
        const uint32_t counter = shared_work_ + kPlayerWork + kPlayerWorkSize * player.id + kTickCounter;
        Write32(counter, Read32(counter) + static_cast<uint32_t>(played));
    }

    const int increment = (player.tempo * player.tempo_ratio) >> 8;
    player.tempo_counter = static_cast<uint16_t>(player.tempo_counter + increment);
}

bool Driver::StepTicks(Player& player, bool play_notes)
{
    bool playing = false;
    for (int i = 0; i < kTracksPerPlayer; i++)
    {
        Track* track = PlayerTrack(player, i);
        if (!track || track->position == 0)
        {
            continue;
        }
        if (StepTrack(static_cast<int>(track - tracks_.data()), i, player, play_notes) != 0)
        {
            StopTrack(player, i);
        }
        else
        {
            playing = true;
        }
    }

    return !playing;
}

int Driver::StepTrack(int index, int number, Player& player, bool play_notes)
{
    Track& track = tracks_[static_cast<std::size_t>(index)];
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        Channel& channel = channels_[static_cast<std::size_t>(at)];
        if (channel.length > 0)
        {
            channel.length--;
        }
        if (!channel.auto_sweep && channel.sweep_counter < channel.sweep_length)
        {
            channel.sweep_counter++;
        }
    }

    if (track.note_finish_wait)
    {
        if (track.channels >= 0)
        {
            return 0;
        }
        track.note_finish_wait = false;
    }

    if (track.wait > 0)
    {
        track.wait--;
        if (track.wait > 0)
        {
            return 0;
        }
    }

    for (int commands = 0; track.wait == 0 && !track.note_finish_wait; commands++)
    {
        if (commands == kCommandBudget)
        {
            throw Hang(player.id, number, track.position - track.base);
        }

        const int result = following_ ? FollowCommand(index, number, player, play_notes)
                                      : RunCommand(index, number, player, play_notes);
        if (result != 0)
        {
            return result;
        }
    }

    return 0;
}

// The command at the track's position. Returns -1 at the track's end (0xFF), and 0 otherwise.
int Driver::RunCommand(int index, int number, Player& player, bool play_notes)
{
    Track& track = tracks_[static_cast<std::size_t>(index)];
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const uint64_t now = player_follows_[player.id].commands_run;
    const auto byte_at = [this](uint32_t address)
    {
        return Read8(address);
    };
    const uint32_t start = track.position;
    const PlayedCommand played = ReadPlayedCommand(byte_at, track.position);
    track.position = played.next;
    const uint8_t command = played.code;
    const bool run = !played.command.conditional || track.compare;
    if (following_ && played.command.conditional)
    {
        follow.conditional_at = now;
    }

    // The driver works the value out as it reads the command, whether the command runs or not. A random value
    // takes a random number then.
    const int32_t argument = Resolve(played.command.value, player);
    if (following_ && played.command.value.source == commands::Source::kRandom)
    {
        follow.drew_at = now;
        if (hooks_.on_draw)
        {
            hooks_.on_draw(player.id, number);
        }
    }
    else if (following_ && played.command.value.source == commands::Source::kVariable)
    {
        MarkRead(player, played.command.value.value);
    }

    if ((command & 0x80) == 0)
    {
        const auto velocity = static_cast<uint8_t>(played.command.velocity);
        const int32_t length = argument;
        int key = command + track.transpose;
        if (!run)
        {
            return 0;
        }

        key = std::clamp(key, 0, 127);
        if (following_)
        {
            follow.noted_at = now;
            follow.holds = length >= kHeldTicks;
        }
        if (!track.muted && play_notes)
        {
            const int32_t played_length = length > 0 ? length : -1;
            const bool tie = track.tie && track.channels >= 0;
            const Channel* tied = tie ? &channels_[static_cast<std::size_t>(track.channels)] : nullptr;
            const uint8_t tied_key = tied ? tied->key : 0;
            const uint8_t tied_velocity = tied ? tied->velocity : 0;
            const int channel = PlayNote(index, player, key, velocity, played_length);
            if (following_ && channel >= 0 &&
                (!tie || tied_key != key || tied_velocity != velocity ||
                 channels_[static_cast<std::size_t>(channel)].sweep_pitch != 0))
            {
                MarkChange(index, player);
            }
            if (on_note)
            {
                on_note({
                    .player = player.id,
                    .track = number,
                    .offset = start - track.base,
                    .key = static_cast<uint8_t>(key),
                    .velocity = velocity,
                    .length = played_length,
                    .channel = channel,
                    .tie = tie,
                });
            }
        }
        track.portamento_key = static_cast<uint8_t>(key);
        if (track.note_wait)
        {
            follow.held_wait = following_ && follow.holds;
            track.wait = length;
            if (length == 0)
            {
                track.note_finish_wait = true;
            }
        }
        return 0;
    }

    switch (command & 0xF0)
    {
    case 0x80:
        {
            const int32_t value = argument;
            if (!run)
            {
                break;
            }
            if (command == 0x80)
            {
                track.wait = value;
            }
            else if (command == 0x81 && value < 0x10000)
            {
                track.program = static_cast<uint16_t>(value);
            }
            break;
        }
    case 0x90:
        if (command == 0x93)
        {
            const uint8_t opened = played.command.index;
            const uint32_t offset = played.command.target;
            if (!run)
            {
                break;
            }
            Track* other = PlayerTrack(player, opened);
            if (other && other != &track)
            {
                ReleaseTrackChannels(*other, player, -1);
                FreeTrackChannels(*other);
                other->base = track.base;
                other->position = track.base + offset;
                if (following_)
                {
                    MarkChange(index, player);
                    if (hooks_.on_open_track)
                    {
                        hooks_.on_open_track(player.id, number, opened);
                    }
                }
            }
        }
        else if (command == 0x94)
        {
            const uint32_t offset = played.command.target;
            if (run)
            {
                if (following_ &&
                    FollowsJumpAsLoop(index, offset, played.next - track.base, played.command.conditional))
                {
                    CountLoop(index, number, player, offset);
                }
                else if (following_)
                {
                    CountPoll(index, player, offset, played.next - track.base);
                }
                track.position = track.base + offset;
            }
        }
        else if (command == 0x95)
        {
            const uint32_t offset = played.command.target;
            if (run && track.call_depth < kCallDepth)
            {
                follow.loops[track.call_depth] = false;
                track.calls[track.call_depth] = track.position;
                track.call_depth++;
                track.position = track.base + offset;
            }
        }
        break;
    case 0xC0:
    case 0xD0:
        {
            const auto value = static_cast<uint8_t>(argument);
            if (!run)
            {
                break;
            }
            switch (command)
            {
            case 0xC0:
                track.pan = static_cast<int8_t>(value - 0x40);
                break;
            case 0xC1:
                track.volume = value;
                break;
            case 0xC2:
                player.volume = value;
                break;
            case 0xC3:
                track.transpose = static_cast<int8_t>(value);
                break;
            case 0xC4:
                track.pitch_bend = static_cast<int8_t>(value);
                break;
            case 0xC5:
                track.bend_range = value;
                break;
            case 0xC6:
                track.priority = value;
                break;
            case 0xC7:
                track.note_wait = (value & 1) != 0;
                break;
            case 0xC8:
                track.tie = (value & 1) != 0;
                ReleaseTrackChannels(track, player, -1);
                FreeTrackChannels(track);
                break;
            case 0xC9:
                track.portamento_key = static_cast<uint8_t>(value + track.transpose);
                track.portamento = true;
                break;
            case 0xCA:
                track.modulation.depth = value;
                break;
            case 0xCB:
                track.modulation.speed = value;
                break;
            case 0xCC:
                track.modulation.target = value;
                break;
            case 0xCD:
                track.modulation.range = value;
                break;
            case 0xCE:
                track.portamento = (value & 1) != 0;
                break;
            case 0xCF:
                track.portamento_time = value;
                break;
            case 0xD0:
                track.attack = value;
                break;
            case 0xD1:
                track.decay = value;
                break;
            case 0xD2:
                track.sustain = value;
                break;
            case 0xD3:
                track.release = value;
                break;
            case 0xD4:
                if (track.call_depth < kCallDepth)
                {
                    follow.loops[track.call_depth] = true;
                    track.calls[track.call_depth] = track.position;
                    track.loop_counts[track.call_depth] = value;
                    track.call_depth++;
                }
                break;
            case 0xD5:
                track.expression = value;
                break;
            case 0xD7:
                MuteTrack(track, player, value);
                break;
            default:
                break;
            }
            break;
        }
    case 0xE0:
        {
            const auto value = static_cast<int16_t>(argument);
            if (!run)
            {
                break;
            }
            if (command == 0xE0)
            {
                track.modulation.delay = static_cast<uint16_t>(value);
            }
            else if (command == 0xE1)
            {
                player.tempo = static_cast<uint16_t>(value);
            }
            else if (command == 0xE3)
            {
                track.sweep_pitch = value;
            }
            break;
        }
    case 0xB0:
        {
            const uint8_t variable_number = played.command.index;
            const auto value = static_cast<int16_t>(argument);
            const uint32_t variable = VariableAddress(player, variable_number);
            if (!run || variable == 0)
            {
                break;
            }

            const int16_t current = static_cast<int16_t>(Read16(variable));
            if (following_ && command >= 0xB8 && command <= 0xBD)
            {
                MarkRead(player, variable_number);
            }
            switch (command)
            {
            case 0xB0:
                Write16(variable, static_cast<uint16_t>(value));
                break;
            case 0xB1:
                Write16(variable, static_cast<uint16_t>(current + value));
                break;
            case 0xB2:
                Write16(variable, static_cast<uint16_t>(current - value));
                break;
            case 0xB3:
                Write16(variable, static_cast<uint16_t>(current * value));
                break;
            case 0xB4:
                if (value != 0)
                {
                    Write16(variable, static_cast<uint16_t>(current / value));
                }
                break;
            case 0xB5:
                {
                    // The ARM's shifts by a register take its low byte. A shift left by 32 or more gives 0, and an
                    // arithmetic shift right by 32 or more gives the sign.
                    const uint32_t amount = static_cast<uint32_t>(value >= 0 ? value : -value) & 0xFF;
                    int32_t shifted = current;
                    if (value >= 0)
                    {
                        shifted = amount >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(current) << amount);
                    }
                    else
                    {
                        shifted = amount >= 32 ? (current < 0 ? -1 : 0) : current >> amount;
                    }
                    Write16(variable, static_cast<uint16_t>(shifted));
                    break;
                }
            case 0xB6:
                {
                    if (following_)
                    {
                        follow.drew_at = now;
                        if (hooks_.on_draw)
                        {
                            hooks_.on_draw(player.id, number);
                        }
                    }
                    int16_t range = value;
                    const bool negative = range < 0;
                    if (negative)
                    {
                        range = static_cast<int16_t>(-range);
                    }
                    int32_t random = Random();
                    random = (random * (range + 1)) >> 16;
                    if (negative)
                    {
                        random = -random;
                    }
                    Write16(variable, static_cast<uint16_t>(random));
                    break;
                }
            case 0xB8:
                track.compare = current == value;
                break;
            case 0xB9:
                track.compare = current >= value;
                break;
            case 0xBA:
                track.compare = current > value;
                break;
            case 0xBB:
                track.compare = current <= value;
                break;
            case 0xBC:
                track.compare = current < value;
                break;
            case 0xBD:
                track.compare = current != value;
                break;
            default:
                break;
            }
            if (following_ && static_cast<int16_t>(Read16(variable)) != current)
            {
                follow.set_at = now;
            }
            break;
        }
    case 0xF0:
        if (!run)
        {
            break;
        }
        if (command == 0xFD)
        {
            if (track.call_depth != 0)
            {
                track.call_depth--;
                track.position = track.calls[track.call_depth];
            }
        }
        else if (command == 0xFC)
        {
            if (track.call_depth == 0)
            {
                break;
            }
            // A loop's count of 0 repeats forever. Otherwise the last pass leaves the loop. A length analysis counts
            // the passes of an endless loop.
            uint8_t count = track.loop_counts[track.call_depth - 1];
            const uint32_t loop_start = track.calls[track.call_depth - 1] - track.base;
            if (count != 0 && !LoopsForEver(player, number, loop_start))
            {
                count--;
                if (count == 0)
                {
                    track.call_depth--;
                    break;
                }
            }
            else if (following_)
            {
                CountLoop(index, number, player, loop_start);
            }
            track.loop_counts[track.call_depth - 1] = count;
            track.position = track.calls[track.call_depth - 1];
        }
        else if (command == 0xFF)
        {
            return -1;
        }
        break;
    default:
        break;
    }

    return 0;
}

int Driver::PlayNote(int index, Player& player, int key, int velocity, int32_t length)
{
    Track& track = tracks_[static_cast<std::size_t>(index)];
    Channel* channel = nullptr;
    if (track.tie && track.channels >= 0)
    {
        channel = &channels_[static_cast<std::size_t>(track.channels)];
        channel->key = static_cast<uint8_t>(key);
        channel->velocity = static_cast<uint8_t>(velocity);
    }

    if (!channel)
    {
        Instrument instrument;
        if (!ReadInstrument(player.bank, track.program, key, instrument))
        {
            return -1;
        }

        uint32_t allowed = 0;
        switch (instrument.type)
        {
        case kInstrumentPcm:
        case kInstrumentDirectPcm:
            allowed = 0xFFFF;
            break;
        case kInstrumentPsg:
            allowed = 0x3F00;
            break;
        case kInstrumentNoise:
            allowed = 0xC000;
            break;
        default:
            return -1;
        }
        allowed &= track.channel_mask;

        const int allocated = Allocate(allowed, player.priority + track.priority, track.channel_mask_set, index);
        if (allocated < 0)
        {
            return -1;
        }
        channel = &channels_[static_cast<std::size_t>(allocated)];
        if (!NoteOn(*channel, key, velocity, track.tie ? -1 : length, player.bank, instrument))
        {
            channel->priority = 0;
            Free(*channel);
            return -1;
        }
        channel->next = track.channels;
        track.channels = allocated;
    }

    if (track.attack != 0xFF)
    {
        SetAttack(*channel, track.attack);
    }
    if (track.decay != 0xFF)
    {
        SetDecay(*channel, track.decay);
    }
    if (track.sustain != 0xFF)
    {
        channel->sustain = track.sustain;
    }
    if (track.release != 0xFF)
    {
        SetRelease(*channel, track.release);
    }

    channel->sweep_pitch = track.sweep_pitch;
    if (track.portamento)
    {
        channel->sweep_pitch =
            static_cast<int16_t>(channel->sweep_pitch + static_cast<int16_t>((track.portamento_key - key) * 64));
    }

    if (track.portamento_time == 0)
    {
        channel->sweep_length = length;
        channel->auto_sweep = false;
    }
    else
    {
        int32_t sweep = track.portamento_time * track.portamento_time;
        sweep *= std::abs(static_cast<int32_t>(channel->sweep_pitch));
        sweep >>= 11;
        channel->sweep_length = sweep;
    }
    channel->sweep_counter = 0;
    return static_cast<int>(channel - channels_.data());
}

void Driver::UpdateTrackChannels(Track& track, const Player& player, bool release)
{
    int32_t volume = DecibelSquare(track.volume) + DecibelSquare(track.expression) + DecibelSquare(player.volume);
    int32_t fader = track.external_fader + player.external_fader;
    int32_t pitch = track.pitch_bend;
    pitch *= track.bend_range << 6;
    pitch >>= 7;
    pitch += track.external_pitch;
    int32_t pan = track.pan;
    if (track.pan_range != 127)
    {
        pan = (pan * track.pan_range + 0x40) >> 7;
    }
    pan += track.external_pan;

    volume = std::max(volume, -0x8000);
    fader = std::max(fader, -0x8000);
    pan = std::clamp(pan, -128, 127);

    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        Channel& channel = channels_[static_cast<std::size_t>(at)];
        channel.user_decay2 = static_cast<int16_t>(fader);
        if (channel.envelope_stage == kRelease)
        {
            continue;
        }

        channel.user_decay = static_cast<int16_t>(volume);
        channel.user_pitch = static_cast<int16_t>(pitch);
        channel.user_pan = static_cast<int8_t>(pan);
        channel.pan_range = track.pan_range;
        channel.lfo.parameters = track.modulation;
        if (channel.length == 0 && release)
        {
            channel.priority = 1;
            channel.envelope_stage = kRelease;
        }
    }
}

void Driver::ReleaseTrackChannels(Track& track, const Player& player, int release)
{
    UpdateTrackChannels(track, player, false);
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        Channel& channel = channels_[static_cast<std::size_t>(at)];
        if (channel.active)
        {
            if (release >= 0)
            {
                SetRelease(channel, release & 0xFF);
            }
            channel.priority = 1;
            channel.envelope_stage = kRelease;
        }
    }
}

void Driver::FreeTrackChannels(Track& track)
{
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        Free(channels_[static_cast<std::size_t>(at)]);
    }
    track.channels = -1;
}

void Driver::StopTrack(Player& player, int index)
{
    Track* track = PlayerTrack(player, index);
    if (!track)
    {
        return;
    }

    ReleaseTrackChannels(*track, player, -1);
    FreeTrackChannels(*track);
    track->active = false;
    player.tracks[static_cast<std::size_t>(index)] = kNoTrack;
}

void Driver::StopPlayer(Player& player)
{
    for (int i = 0; i < kTracksPerPlayer; i++)
    {
        StopTrack(player, i);
    }
    player.active = false;
}

void Driver::MuteTrack(Track& track, const Player& player, int mode)
{
    switch (mode)
    {
    case 0:
        track.muted = false;
        break;
    case 1:
        track.muted = true;
        break;
    case 2:
        track.muted = true;
        ReleaseTrackChannels(track, player, -1);
        break;
    case 3:
        track.muted = true;
        ReleaseTrackChannels(track, player, 127);
        FreeTrackChannels(track);
        break;
    default:
        break;
    }
}

uint8_t Driver::ReadByte(Track& track)
{
    return Read8(track.position++);
}

uint16_t Driver::ReadU16(Track& track)
{
    const uint16_t low = ReadByte(track);
    return static_cast<uint16_t>(low | ReadByte(track) << 8);
}

int32_t Driver::Resolve(const commands::Argument& argument, const Player& player)
{
    switch (argument.source)
    {
    case commands::Source::kVariable:
        {
            const uint32_t variable = VariableAddress(player, static_cast<int>(argument.value));
            return variable != 0 ? static_cast<int16_t>(Read16(variable)) : 0;
        }
    case commands::Source::kRandom:
        {
            // A value from the low bound to the high bound, both signed 16-bit, by the random number's high 16 bits.
            const auto low = static_cast<int32_t>(argument.value);
            const auto high = static_cast<int32_t>(argument.high);
            const uint32_t random = Random();
            const int32_t span = high - low + 1;
            return (static_cast<int32_t>(random * static_cast<uint32_t>(span)) >> 16) + low;
        }
    case commands::Source::kValue:
        break;
    }

    return static_cast<int32_t>(argument.value);
}

uint32_t Driver::VariableAddress(const Player& player, int variable) const
{
    if (shared_work_ == 0)
    {
        return 0;
    }
    if (variable < 16)
    {
        return shared_work_ + kPlayerWork + kPlayerWorkSize * player.id + 2 * static_cast<uint32_t>(variable);
    }

    return shared_work_ + kGlobalVariables + 2 * static_cast<uint32_t>(variable - 16);
}

void Driver::Run(const Command& command)
{
    const std::array<uint32_t, 4>& a = command.args;
    const auto player = [&a]
    {
        return static_cast<int>(a[0] % kPlayerCount);
    };
    switch (command.id)
    {
    case CommandId::kStartSequence:
        PrepareSequence(player(), a[1], a[2], a[3]);
        players_[static_cast<std::size_t>(player())].prepared = true;
        break;
    case CommandId::kStopSequence:
        StopSequence(player());
        break;
    case CommandId::kPrepareSequence:
        PrepareSequence(player(), a[1], a[2], a[3]);
        break;
    case CommandId::kStartPreparedSequence:
        players_[static_cast<std::size_t>(player())].prepared = true;
        break;
    case CommandId::kPauseSequence:
        PauseSequence(player(), a[1]);
        break;
    case CommandId::kSkipSequence:
        SkipSequence(player(), a[1]);
        break;
    case CommandId::kPlayerParameter:
        SetPlayerParameter(player(), a[1], a[2], static_cast<int>(a[3]));
        break;
    case CommandId::kTrackParameter:
        SetTrackParameter(static_cast<int>((a[0] & 0xFFFFFF) % kPlayerCount), a[1], a[2], a[3],
                          static_cast<uint8_t>(a[0] >> 24));
        break;
    case CommandId::kMuteTrack:
        SetTrackMute(player(), a[1], static_cast<int>(a[2]));
        break;
    case CommandId::kAllocatableChannels:
        SetAllocatableChannels(player(), a[1], a[2]);
        break;
    case CommandId::kPlayerLocalVariable:
        if (shared_work_ != 0)
        {
            Write16(shared_work_ + kPlayerWork + kPlayerWorkSize * a[0] + 2 * a[1], static_cast<uint16_t>(a[2]));
        }
        break;
    case CommandId::kGlobalVariable:
        if (shared_work_ != 0)
        {
            Write16(shared_work_ + kGlobalVariables + 2 * a[0], static_cast<uint16_t>(a[1]));
        }
        break;
    case CommandId::kStartTimer:
        StartTimer(a[0], a[1]);
        break;
    case CommandId::kStopTimer:
        StopTimer(a[0], a[1], static_cast<int>(a[3]));
        break;
    case CommandId::kSetUpPcmChannel:
        SetUpPcmChannel(static_cast<int>(a[0] & 0xF), a[1] & 0x7FFFFFF, static_cast<int>((a[3] >> 24) & 0x3),
                        static_cast<int>((a[3] >> 26) & 0x3), static_cast<int>(a[3] & 0xFFFF),
                        static_cast<int>(a[2] & 0x3FFFFF), static_cast<int>((a[2] >> 24) & 0x7F),
                        static_cast<int>((a[2] >> 22) & 0x3), static_cast<int>((a[0] >> 16) & 0xFFFF),
                        static_cast<int>((a[3] >> 16) & 0x7F));
        break;
    case CommandId::kSetUpPsgChannel:
        SetUpPsgChannel(static_cast<int>(a[0] & 0xF), static_cast<int>(a[3]), static_cast<int>(a[1] & 0x7F),
                        static_cast<int>((a[1] >> 8) & 0x3), static_cast<int>((a[2] >> 8) & 0xFFFF),
                        static_cast<int>(a[2] & 0x7F));
        break;
    case CommandId::kSetUpNoiseChannel:
        SetUpNoiseChannel(static_cast<int>(a[0] & 0xF), static_cast<int>(a[1] & 0x7F),
                          static_cast<int>((a[1] >> 8) & 0x3), static_cast<int>((a[2] >> 8) & 0xFFFF),
                          static_cast<int>(a[2] & 0x7F));
        break;
    case CommandId::kSetUpCapture:
        SetUpCapture(static_cast<int>((a[2] >> 31) & 1), static_cast<int>((a[2] >> 30) & 1), a[0],
                     static_cast<int>(a[1]), ((a[2] >> 29) & 1) != 0, static_cast<int>((a[2] >> 28) & 1),
                     static_cast<int>((a[2] >> 27) & 1));
        break;
    case CommandId::kChannelTimer:
        for (uint32_t i = 0, mask = a[0]; i < kChannelCount && mask != 0; i++, mask >>= 1)
        {
            if (mask & 1)
            {
                SetChannelTimer(static_cast<int>(i), static_cast<int>(a[1]));
            }
        }
        break;
    case CommandId::kChannelVolume:
        for (uint32_t i = 0, mask = a[0]; i < kChannelCount && mask != 0; i++, mask >>= 1)
        {
            if (mask & 1)
            {
                SetChannelVolume(static_cast<int>(i), static_cast<int>(a[1]), static_cast<int>(a[2]));
            }
        }
        break;
    case CommandId::kChannelPan:
        for (uint32_t i = 0, mask = a[0]; i < kChannelCount && mask != 0; i++, mask >>= 1)
        {
            if (mask & 1)
            {
                SetChannelPan(static_cast<int>(i), static_cast<int>(a[1]));
            }
        }
        break;
    case CommandId::kSurroundDecay:
        SetSurroundDecay(static_cast<int>(a[0]));
        break;
    case CommandId::kMasterVolume:
        registers_->Write(kMasterVolumeRegister, 1, static_cast<uint8_t>(a[0]));
        break;
    case CommandId::kMasterPan:
        SetMasterPan(static_cast<int>(a[0]));
        break;
    case CommandId::kOutputSelector:
        SetOutputSelector(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]),
                          static_cast<int>(a[3]));
        break;
    case CommandId::kLockChannels:
        LockChannels(a[0], a[1]);
        break;
    case CommandId::kUnlockChannels:
        if (a[1] & 1)
        {
            weakly_locked_ &= ~a[0];
        }
        else
        {
            locked_ &= ~a[0];
        }
        break;
    case CommandId::kStopUnlockedChannels:
        StopUnlockedChannels(a[0]);
        break;
    case CommandId::kSetSharedWork:
        shared_work_ = a[0];
        break;
    case CommandId::kInvalidateSequence:
        InvalidateSequence(a[0], a[1]);
        break;
    case CommandId::kInvalidateBank:
        InvalidateBank(a[0], a[1]);
        break;
    case CommandId::kInvalidateWave:
        InvalidateWave(a[0], a[1]);
        break;
    case CommandId::kSetUpAlarm:
    case CommandId::kReadDriverInfo:
        break;
    }
}

void Driver::StartTimer(uint32_t channels, uint32_t captures)
{
    for (int i = 0; i < kChannelCount && channels != 0; i++, channels >>= 1)
    {
        if (channels & 1)
        {
            registers_->Write(ChannelRegister(i, 0x3), 1, registers_->Read(ChannelRegister(i, 0x3), 1) | 0x80);
        }
    }
    if (captures & 1)
    {
        if (captures & 2)
        {
            registers_->Write(kCaptureControl, 2, registers_->Read(kCaptureControl, 2) | 0x8080);
        }
        else
        {
            registers_->Write(kCaptureControl, 1, registers_->Read(kCaptureControl, 1) | 0x80);
        }
    }
    else if (captures & 2)
    {
        registers_->Write(kCaptureControl + 1, 1, registers_->Read(kCaptureControl + 1, 1) | 0x80);
    }
    UpdateSharedWork();
}

void Driver::StopTimer(uint32_t channels, uint32_t captures, int hold)
{
    for (int i = 0; i < kChannelCount && channels != 0; i++, channels >>= 1)
    {
        if (channels & 1)
        {
            StopChannel(i, hold);
        }
    }
    if (captures & 1)
    {
        registers_->Write(kCaptureControl, 1, 0);
    }
    if (captures & 2)
    {
        registers_->Write(kCaptureControl + 1, 1, 0);
    }
    UpdateSharedWork();
}

void Driver::UpdateSharedWork()
{
    if (shared_work_ == 0)
    {
        return;
    }

    uint16_t active = 0;
    uint16_t capturing = 0;
    for (int i = 0; i < kChannelCount; i++)
    {
        if (IsChannelActive(i))
        {
            active = static_cast<uint16_t>(active | 1u << i);
        }
    }
    if (IsCaptureActive(0))
    {
        capturing |= 1;
    }
    if (IsCaptureActive(1))
    {
        capturing |= 2;
    }
    Write16(shared_work_ + kChannelStatus, active);
    Write16(shared_work_ + kCaptureStatus, capturing);
}

void Driver::SetRandomState(uint32_t state)
{
    random_ = state;
}

uint16_t Driver::Random()
{
    random_ = random_ * 1664525u + 1013904223u;
    return static_cast<uint16_t>(random_ >> 16);
}

void Driver::FollowLengths(LengthHooks hooks, std::set<LoopPlace> endless_loops)
{
    hooks_ = std::move(hooks);
    endless_loops_ = std::move(endless_loops);
    following_ = static_cast<bool>(hooks_.on_loop);
}

bool Driver::IsSettled(int player) const
{
    const Player& state = players_[static_cast<std::size_t>(player)];
    const auto settled = [this](int track)
    {
        return WaitsForNotes(track) || (IsStill(track) && !HasEndingNote(track));
    };
    return ((state.tempo * state.tempo_ratio) >> 8) == 0 || EveryOpenTrack(player, settled);
}

bool Driver::IsHolding(int player) const
{
    const Player& state = players_[static_cast<std::size_t>(player)];
    const auto holding = [this](int track)
    {
        return WaitsForEver(track) || (IsStill(track) && !HasEndingNote(track));
    };
    return ((state.tempo * state.tempo_ratio) >> 8) == 0 || EveryOpenTrack(player, holding);
}

bool Driver::IsStuck(int player) const
{
    const Player& state = players_[static_cast<std::size_t>(player)];
    const auto stuck = [this](int track)
    {
        return WaitsForEver(track) || (IsStill(track) && track_follows_[static_cast<std::size_t>(track)].quiet);
    };
    return ((state.tempo * state.tempo_ratio) >> 8) == 0 || EveryOpenTrack(player, stuck);
}

uint64_t Driver::ChangedAt(int player) const
{
    return player_follows_[static_cast<std::size_t>(player)].changed_at;
}

bool Driver::IsTrackOpen(int player, int track) const
{
    const Track* open = TrackOf(player, track);
    return open && open->position != 0;
}

bool Driver::IsTrackIdle(int player, int track) const
{
    const Track* open = TrackOf(player, track);
    return open && IsIdle(static_cast<int>(open - tracks_.data()));
}

bool Driver::WaitsForEver(int player, int track) const
{
    const Track* open = TrackOf(player, track);
    return open && WaitsForEver(static_cast<int>(open - tracks_.data()));
}

std::set<LoopPlace> Driver::OutermostCountedLoops(int player) const
{
    std::set<LoopPlace> loops;
    for (int number = 0; number < kTracksPerPlayer; number++)
    {
        if (!IsTrackOpen(player, number))
        {
            continue;
        }

        const Track& track = *TrackOf(player, number);
        const TrackFollow& follow = track_follows_[static_cast<std::size_t>(&track - tracks_.data())];
        for (int depth = 0; depth < track.call_depth; depth++)
        {
            if (follow.loops[static_cast<std::size_t>(depth)])
            {
                if (track.loop_counts[static_cast<std::size_t>(depth)] != 0)
                {
                    loops.insert({player, number, track.calls[static_cast<std::size_t>(depth)] - track.base});
                }
                break;
            }
        }
    }

    return loops;
}

bool Driver::AnyChannelChanging() const
{
    return std::ranges::any_of(
        channels_, [](const Channel& channel) { return channel.active && channel.envelope_stage != kSustain; });
}

int Driver::ActiveChannels() const
{
    return static_cast<int>(std::ranges::count_if(channels_, [](const Channel& channel) { return channel.active; }));
}

Driver::SoundState Driver::StateOf(const Track& track, const Player& player) const
{
    return {
        .program = track.program,
        .transpose = track.transpose,
        .bend_range = track.bend_range,
        .priority = track.priority,
        .portamento_key = track.portamento_key,
        .portamento_time = track.portamento_time,
        .envelope = {track.attack, track.decay, track.sustain, track.release},
        .sweep_pitch = track.sweep_pitch,
        .modulation = track.modulation,
        .muted = track.muted,
        .tie = track.tie,
        .portamento = track.portamento,
        .note_wait = track.note_wait,
        .volume = track.volume,
        .expression = track.expression,
        .pitch_bend = track.pitch_bend,
        .pan = track.pan,
        .player_volume = player.volume,
        .tempo = player.tempo,
        .channels = track.channels,
    };
}

// While the driver follows lengths: the command's place in the order that the player's tracks run them, and whether it
// changes the track's sound.
int Driver::FollowCommand(int index, int number, Player& player, bool play_notes)
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const uint64_t now = ++player_follows_[player.id].commands_run;
    follow.run[track.position - track.base] = now;
    follow.held_wait = false;
    if (hooks_.on_command_start)
    {
        hooks_.on_command_start(player.id, number);
    }

    const SoundState before = StateOf(track, player);
    const int result = RunCommand(index, number, player, play_notes);
    if (StateOf(track, player) != before)
    {
        MarkChange(index, player);
    }

    return result;
}

void Driver::MarkChange(int track, const Player& player)
{
    PlayerFollow& follow = player_follows_[player.id];
    track_follows_[static_cast<std::size_t>(track)].changed_at = follow.commands_run;
    follow.changed_at = follow.commands_run;
}

void Driver::MarkRead(const Player& player, int variable)
{
    if (variable >= 0 && variable < 32)
    {
        player_follows_[player.id].variables_read |= 1u << variable;
    }
}

// A pass of a loop of track `number` back to `target`. The pass began when the track last ran `target`. A still pass
// also left the variables that the tracks read with the values that the pass before left. The first pass compares
// them with 0. The pass played a note if the track has played one since the pass began.
void Driver::CountLoop(int index, int number, const Player& player, uint32_t target)
{
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const PlayerFollow& player_follow = player_follows_[player.id];
    const auto ran = follow.run.find(target);
    const uint64_t begun = ran != follow.run.end() ? ran->second : 0;
    std::array<int16_t, 32> variables{};
    for (int i = 0; i < static_cast<int>(variables.size()); i++)
    {
        const uint32_t address = (player_follow.variables_read >> i & 1) != 0 ? VariableAddress(player, i) : 0;
        variables[static_cast<std::size_t>(i)] = address != 0 ? static_cast<int16_t>(Read16(address)) : 0;
    }
    follow.idle = follow.changed_at < begun;
    follow.still = follow.idle && follow.drew_at < begun && variables == follow.loop_variables;
    follow.loop_variables = variables;
    follow.looped_at = player_follow.commands_run;
    follow.polled_at = 0;

    const bool noted = follow.noted_at != 0 && follow.noted_at >= begun;
    const bool alone = HoldsSustainedNote(index) && OthersRest(player, number);
    if (!follow.idle && (number == 0 || (noted && IsMainTrack(player, number)) || alone))
    {
        hooks_.on_loop(player.id, number, target);
    }

    follow.quiet = !noted;
    if (hooks_.on_track_loop)
    {
        hooks_.on_track_loop(player.id, number, target);
    }
}

// A jump of track `index` back to `target` that isn't a loop: a pass of a poll inside a subroutine (`next` is the
// offset after the jump). The pass began when the track last ran `target`. It's idle when it changed nothing in the
// sound, changed no variable and drew no random number. The track then does the same on every pass until another track
// or the game changes a variable. A track that polls the game's variables once a tick, while later tracks play the
// music, makes such passes. A countdown between a track's notes changes its counter on each pass. The pass is never a
// loop. nw::snd's model has the same rule (nwsnd::SequenceTrack::CountPoll).
void Driver::CountPoll(int index, const Player& player, uint32_t target, uint32_t next)
{
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const auto ran = follow.run.find(target);
    if (target >= next || ran == follow.run.end())
    {
        return;
    }

    // After a pass that isn't idle, IsIdle follows the track's loops again.
    const uint64_t begun = ran->second;
    const bool idle = follow.changed_at < begun && follow.set_at < begun && follow.drew_at < begun;
    follow.polled_at = idle ? player_follows_[player.id].commands_run : 0;
}

// A jump back to a command that the track has run is a loop, outside a subroutine. Inside a subroutine, the jump is a
// loop only when it's unconditional and the track holds a note that sounds until the game stops it, or the track has
// run no conditional command since the jump's target.
bool Driver::FollowsJumpAsLoop(int index, uint32_t target, uint32_t next, bool conditional) const
{
    const TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const auto ran = follow.run.find(target);
    if (target >= next || ran == follow.run.end())
    {
        return false;
    }

    const Track& track = tracks_[static_cast<std::size_t>(index)];
    bool in_call = false;
    for (int depth = 0; depth < track.call_depth; depth++)
    {
        in_call = in_call || !follow.loops[static_cast<std::size_t>(depth)];
    }
    const bool unconditioned = follow.conditional_at < ran->second;
    return !in_call || (!conditional && (HoldsSustainedNote(index) || unconditioned));
}

bool Driver::LoopsForEver(const Player& player, int number, uint32_t offset) const
{
    return following_ && endless_loops_.contains({player.id, number, offset});
}

const Track* Driver::TrackOf(int player, int number) const
{
    const uint8_t track = players_[static_cast<std::size_t>(player)].tracks[static_cast<std::size_t>(number)];
    return track == kNoTrack ? nullptr : &tracks_[track];
}

bool Driver::IsIdle(int track) const
{
    // A track that polls in a subroutine is idle too after an idle pass of its poll (CountPoll).
    const TrackFollow& follow = track_follows_[static_cast<std::size_t>(track)];
    return (follow.idle && follow.changed_at < follow.looped_at) ||
           (follow.polled_at != 0 && follow.changed_at < follow.polled_at);
}

bool Driver::IsStill(int track) const
{
    // A poll's passes don't count.
    const TrackFollow& follow = track_follows_[static_cast<std::size_t>(track)];
    return follow.idle && follow.changed_at < follow.looped_at && follow.still && follow.drew_at < follow.looped_at;
}

// A note or wait whose length comes from a variable left at -1 makes a negative wait.
bool Driver::WaitsForEver(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    if (track.wait < 0 || track_follows_[static_cast<std::size_t>(index)].held_wait)
    {
        return true;
    }
    if (!track.note_finish_wait)
    {
        return false;
    }

    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(at)];
        if (channel.length < 0 && ChannelWaveLoops(channel))
        {
            return true;
        }
    }

    return false;
}

bool Driver::WaitsForNotes(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    return (track.note_finish_wait && track.channels >= 0) || WaitsForEver(index);
}

// A held note's length doesn't count: the game stops it first.
bool Driver::HasEndingNote(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    if (track_follows_[static_cast<std::size_t>(index)].holds)
    {
        return false;
    }

    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        if (channels_[static_cast<std::size_t>(at)].length > 0)
        {
            return true;
        }
    }

    return false;
}

bool Driver::HoldsSustainedNote(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    const bool holds = track_follows_[static_cast<std::size_t>(index)].holds;
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(at)];
        if ((holds || channel.length < 0) && channel.envelope_stage == kSustain && ChannelWaveLoops(channel))
        {
            return true;
        }
    }

    return false;
}

// A PCM wave loops when its settings say so. A rectangular wave and noise play on for ever.
bool Driver::ChannelWaveLoops(const Channel& channel) const
{
    return channel.type != kPcm || channel.wave.loop != 0;
}

// A track after track 0 is the main track when every track before it is idle, waits for ever, or passes without
// notes.
bool Driver::IsMainTrack(const Player& player, int number) const
{
    for (int i = 0; i < number; i++)
    {
        const Track* track = TrackOf(player.id, i);
        if (!track || track->position == 0)
        {
            continue;
        }

        const int index = static_cast<int>(track - tracks_.data());
        if (!IsIdle(index) && !WaitsForEver(index) &&
            (i == 0 || !track_follows_[static_cast<std::size_t>(index)].quiet))
        {
            return false;
        }
    }

    return true;
}

bool Driver::OthersRest(const Player& player, int number) const
{
    for (int i = 0; i < kTracksPerPlayer; i++)
    {
        const Track* track = TrackOf(player.id, i);
        if (i == number || !track || track->position == 0)
        {
            continue;
        }

        const int index = static_cast<int>(track - tracks_.data());
        if (!IsIdle(index) && !WaitsForEver(index))
        {
            return false;
        }
    }

    return true;
}

// Whether `test` passes for the driver's index of each open track of player `player`. It passes when no track is open.
template <typename Test>
bool Driver::EveryOpenTrack(int player, const Test& test) const
{
    for (int number = 0; number < kTracksPerPlayer; number++)
    {
        const Track* track = TrackOf(player, number);
        if (track && track->position != 0 && !test(static_cast<int>(track - tracks_.data())))
        {
            return false;
        }
    }

    return true;
}

} // namespace ntdlib::ntr::snd
