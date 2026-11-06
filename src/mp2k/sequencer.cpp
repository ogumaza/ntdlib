// SPDX-License-Identifier: MIT

#include "ntdlib/mp2k/sequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::mp2k
{
using commands::Kind;
using ntdlib::agb::Rom;
using ntdlib::agb::WarnOnce;

namespace
{

// The player's priority whose notes take a priority of 255 in a later version of Camelot's sequencer.
constexpr int kCamelotTopPlayer = 120;

// The most points that a frame of the mixer holds: those of its highest rate.
constexpr int kMostFramePoints = 704;

// A track's flags.
constexpr uint8_t kFlagVolSet = 0x01;
constexpr uint8_t kFlagVolChange = 0x03;
constexpr uint8_t kFlagPitSet = 0x04;
constexpr uint8_t kFlagPitChange = 0x0C;
constexpr uint8_t kFlagVolPitChange = kFlagVolChange | kFlagPitChange;
constexpr uint8_t kFlagStart = 0x40;
constexpr uint8_t kFlagExist = 0x80;

// The settings that a track sets, for the conversion's reports.
constexpr uint8_t kSetVolume = 0x01;
constexpr uint8_t kSetPan = 0x02;
constexpr uint8_t kSetPitch = 0x04;

// 2^31 * 2^(n/12), rounded: a semitone's steps in the driver's table of sample rates, an octave below key 180.
constexpr uint32_t kFreqTable[12] = {2147483648u, 2275179671u, 2410468894u, 2553802834u, 2705659852u, 2866546760u,
                                     3037000500u, 3217589947u, 3408917802u, 3611622603u, 3826380858u, 4053909305u};

// The PSG's periods for the notes C2 to B2 at A4 = 440 Hz, negated: -131072 / f, rounded. A frequency setting is 2048
// less the period.
constexpr int16_t kPsgFreqTable[12] = {-2004, -1891, -1785, -1685, -1591, -1501,
                                       -1417, -1337, -1262, -1192, -1125, -1062};

// The registers of the PSG's channels, NRx0 to NRx4. Channels 2 and 4 have no NRx0. The driver takes the byte after the
// previous channel's NRx0 there, and doesn't write it.
struct PsgRegisters
{
    uint32_t nrx0, nrx1, nrx2, nrx3, nrx4;
};
constexpr PsgRegisters kPsgRegisters[4] = {
    {0x04000060, 0x04000062, 0x04000063, 0x04000064, 0x04000065},
    {0x04000061, 0x04000068, 0x04000069, 0x0400006C, 0x0400006D},
    {0x04000070, 0x04000072, 0x04000073, 0x04000074, 0x04000075},
    {0x04000071, 0x04000078, 0x04000079, 0x0400007C, 0x0400007D},
};
constexpr uint32_t kRegNr50 = 0x04000080;
constexpr uint32_t kRegNr51 =
    0x04000081; // each channel's outputs: the right side in the low 4 bits, the left in the high
constexpr uint32_t kRegSoundCntH = 0x04000082;
constexpr uint32_t kRegSoundCntX = 0x04000084;
constexpr uint32_t kRegSoundBias = 0x04000088;
constexpr uint32_t kRegWaveRam = 0x04000090;

// NRx2's direction bit: the hardware's envelope rises with it, and falls without it.
constexpr uint8_t kEnvelopeUp = 0x08;

// The wave channel's NR32 for each level of the envelope: mute, then 25%, 50%, 75% and the full volume. The driver's
// table holds 16 levels. The SDK's tables put the numbers 0 to 15 after it, so a level of 16 to 31 mutes the channel.
constexpr uint8_t kWaveVolumes[32] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40, 0x40, 0x40, 0x80,
                                      0x80, 0x80, 0x80, 0x20, 0x20, 0,    1,    2,    3,    4,    5,
                                      6,    7,    8,    9,    10,   11,   12,   13,   14,   15};

// Whether a PSG channel is the wave channel. The wave channel has no envelope in the hardware: NR32 sets its volume.
bool IsWaveChannel(const Channel& c)
{
    return (c.type & kVoicePsg) == 3;
}

// Asks for the frame's write of the channel's volume, with the hardware envelope's step time and direction `step`.
void SetEnvelopeStep(Channel& c, uint8_t step)
{
    c.modify |= kModifyVolume;
    if (!IsWaveChannel(c))
    {
        c.step = step;
    }
}

// The steps of a compressed sample's points, by the 4 bits that code each one.
constexpr int8_t kDeltas[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};

// A compressed sample's points come in blocks of 64 points in 33 bytes. A block starts with its first point. Then the
// low 4 bits of the next byte give the second point's step from the first. The two halves of each later byte give the
// next two steps, the high half first. The points wrap round at 8 bits.
constexpr uint32_t kBlockPoints = 64;
constexpr uint32_t kBlockSize = 33;

// The decoded block of a compressed sample that the mixer last read.
struct DecodedBlock
{
    uint32_t index = ~0u;
    std::array<int8_t, kBlockPoints> points = {};
};

// Returns point `index` of the compressed sample at `wave`. Decodes the point's block into `block` first, unless
// `block` holds it already.
int32_t CompressedPoint(const Rom& rom, uint32_t wave, uint32_t index, DecodedBlock& block)
{
    if (block.index != index / kBlockPoints)
    {
        block.index = index / kBlockPoints;
        const uint32_t data = wave + 16 + kBlockSize * block.index;
        uint8_t point = rom.U8(data);
        block.points[0] = int8_t(point);
        for (uint32_t i = 1; i < kBlockPoints; i++)
        {
            const uint8_t byte = rom.U8(data + i / 2 + 1);
            point = uint8_t(point + kDeltas[i % 2 ? byte & 0xF : byte >> 4]);
            block.points[i] = int8_t(point);
        }
    }

    return block.points[index % kBlockPoints];
}

// Adds `value` times `volume`, over 256 and rounded down, to a byte of the mixer's output. The byte wraps round.
void AddPoint(int8_t& out, int32_t value, int volume)
{
    out = int8_t(uint8_t(out) + uint8_t((value * volume) >> 8));
}

// Returns (a * b) >> 32.
uint32_t MulHigh(uint32_t a, uint32_t b)
{
    return uint32_t((uint64_t(a) * b) >> 32);
}

// Returns a key's entry in the driver's table of sample rates: the semitone in the low 4 bits, and the octaves below
// keys 168-179 in the high 4.
uint32_t ScaleEntry(int key)
{
    return uint32_t(((14 - key / 12) << 4) | (key % 12));
}

// Sets a PSG channel's pan and its envelope's full and sustain levels from its volumes.
void SetPsgVolume(Channel& c)
{
    // A note panned at least two to one plays on one side only, at up to the full level. Otherwise it plays on both.
    bool one_side = false;
    if (c.right >= c.left && c.right / 2 >= c.left)
    {
        c.pan = 0x0F;
        one_side = true;
    }
    else if (c.right < c.left && c.left / 2 >= c.right)
    {
        c.pan = 0xF0;
        one_side = true;
    }

    const uint32_t sum = uint32_t(c.left) + c.right;
    c.goal = uint8_t(sum / 16);
    if (!one_side)
    {
        c.pan = 0xFF;
    }
    else if (c.goal > 15)
    {
        c.goal = 15;
    }

    c.sustain_goal = uint8_t((c.goal * c.sustain + 15) >> 4);
    c.pan &= c.pan_mask;
}

// Each part of a PSG channel's envelope ends in a step of its counter, the end of the frame's envelope work, or the
// channel's end.
enum class Next
{
    kStep,
    kDone,
    kOff
};

// Starts the echo after a PSG note's release. A silent echo ends the channel.
Next StartEcho(Channel& c)
{
    c.envelope = uint8_t((c.goal * c.echo_volume + 0xFF) >> 8);
    if (!c.envelope)
    {
        return Next::kOff;
    }

    c.status |= kStatusEcho;
    SetEnvelopeStep(c, kEnvelopeUp);

    return Next::kDone;
}

// Starts a PSG channel's sustain, or its echo if the voice's sustain is 0.
Next StartSustain(Channel& c)
{
    if (c.sustain == 0)
    {
        c.status &= uint8_t(~kStatusEnvelope);
        return StartEcho(c);
    }

    c.status--;
    SetEnvelopeStep(c, kEnvelopeUp);
    c.envelope = c.sustain_goal;
    c.counter = 7;

    return Next::kStep;
}

// Starts a PSG channel's decay, or its sustain if the voice's decay is 0.
Next StartDecay(Channel& c)
{
    c.status--;
    c.counter = c.decay;
    if (!c.counter)
    {
        return StartSustain(c);
    }

    SetEnvelopeStep(c, c.decay);
    c.envelope = c.goal;

    return Next::kStep;
}

// Moves a PSG channel's envelope a level on in its phase once the counter has run out, and sets the counter again.
Next StepEnvelope(Channel& c)
{
    if (c.counter != 0)
    {
        return Next::kStep;
    }

    if (IsWaveChannel(c))
    {
        c.modify |= kModifyVolume;
    }
    SetPsgVolume(c);

    const uint8_t phase = c.status & kStatusEnvelope;
    if (phase == kPhaseRelease)
    {
        c.envelope--;
        if (int8_t(c.envelope) <= 0)
        {
            return StartEcho(c);
        }

        c.counter = c.release;
    }
    else if (phase == kPhaseSustain)
    {
        c.envelope = c.sustain_goal;
        c.counter = 7;
    }
    else if (phase == kPhaseDecay)
    {
        c.envelope--;
        if (int8_t(c.envelope) <= int8_t(c.sustain_goal))
        {
            return StartSustain(c);
        }

        c.counter = c.decay;
    }
    else
    {
        c.envelope++;
        if (c.envelope >= c.goal)
        {
            return StartDecay(c);
        }

        c.counter = c.attack;
    }

    return Next::kStep;
}

// The Metroid games' PSG routine (0x080011DC in Metroid Fusion) counts a channel's envelope a level at a time, once
// every `attack`, `decay` or `release` frames, with no extra step every 15th frame. It writes a square's or the noise
// channel's envelope only when a phase starts or the track's volume changes, and restarts the channel each time. The
// hardware envelope moves the level in between. The wave channel takes its volume at each step of the count, and
// restarts only when its note starts or its volume changes.

// Asks for the frame's write of the channel's level at the start of a phase, with the hardware envelope's step time and
// direction `step`. A square or the noise channel restarts. The wave channel restarts only if `restart`.
void MetroidPhase(Channel& c, uint8_t step, bool restart = false)
{
    c.modify |= kModifyVolume;
    c.lag = 0;
    if (!IsWaveChannel(c))
    {
        c.step = step;
    }
    else if (restart)
    {
        c.n4 |= 0x80;
    }
}

// Asks for the frame's write of the wave channel's level after a step of the count. The hardware envelope moves the
// other channels' levels.
void MetroidStep(Channel& c)
{
    if (IsWaveChannel(c))
    {
        c.modify |= kModifyVolume;
    }
}

// Starts the echo after a PSG note's release, at its level with a step time of 0. Without both an echo volume and an
// echo length, the note ends (0x080014F0).
Next MetroidEcho(Channel& c)
{
    c.status &= uint8_t(~kStatusEnvelope);
    if (!c.echo_volume || !c.echo_length)
    {
        return Next::kOff;
    }

    c.envelope = uint8_t((c.goal * c.echo_volume + 0xFF) >> 8);
    c.counter = c.echo_length;
    c.status |= kStatusEcho;
    MetroidPhase(c, 0);

    return Next::kStep;
}

// Starts a PSG channel's sustain, at the sustain level with a step time of 0 (0x08001474). The count goes back to the
// sustain level every frame.
Next MetroidSustain(Channel& c)
{
    c.status = uint8_t((c.status & ~kStatusEnvelope) | kPhaseSustain);
    c.envelope = c.sustain_goal;
    c.counter = 1;
    MetroidPhase(c, 0);

    return Next::kStep;
}

// Counts a level down in a PSG channel's decay. The sustain starts once the count reaches the sustain level
// (0x08001452).
Next MetroidDecayStep(Channel& c)
{
    c.envelope--;
    if (int8_t(c.envelope) <= int8_t(c.sustain_goal))
    {
        return MetroidSustain(c);
    }

    c.counter = c.decay;
    MetroidStep(c);

    return Next::kStep;
}

// Starts a PSG channel's decay, or its sustain if the voice's decay is 0. The count goes to the full level, then a
// level down at once, so the hardware restarts a level below the full level and falls (0x08001414).
Next MetroidDecay(Channel& c)
{
    if (!c.decay)
    {
        return MetroidSustain(c);
    }

    c.status = uint8_t((c.status & ~kStatusEnvelope) | kPhaseDecay);
    c.envelope = c.goal;
    MetroidPhase(c, c.decay);

    return MetroidDecayStep(c);
}

// Counts a level up in a PSG channel's attack. The decay starts once the count reaches the full level (0x08001402).
Next MetroidAttackStep(Channel& c)
{
    c.envelope++;
    if (int8_t(c.envelope) >= int8_t(c.goal))
    {
        return MetroidDecay(c);
    }

    c.counter = c.attack;
    MetroidStep(c);

    return Next::kStep;
}

// Counts a level down in a PSG channel's release. The echo starts once the count reaches 0 (0x080014D2).
Next MetroidReleaseStep(Channel& c)
{
    c.envelope--;
    if (int8_t(c.envelope) <= 0)
    {
        return MetroidEcho(c);
    }

    c.counter = c.release;
    MetroidStep(c);

    return Next::kStep;
}

// Starts a PSG note in the Metroid games (0x08001362). A voice with a sound length plays at the sustain level until
// the hardware's length counter stops it. Otherwise an attack restarts the hardware at 0, rising, while the count
// starts at 1, a level ahead of the hardware. Without an attack, the decay starts at once.
Next MetroidStart(Channel& c)
{
    if (c.length)
    {
        c.envelope = c.sustain_goal;
        c.status = uint8_t((c.status & ~kStatusEnvelope) | kPhaseSustain);
        c.counter = 0xFF;
        MetroidPhase(c, 0);
        return Next::kStep;
    }
    if (!c.attack)
    {
        return MetroidDecay(c);
    }

    c.envelope = 0;
    MetroidPhase(c, uint8_t(c.attack + kEnvelopeUp));
    if (!IsWaveChannel(c))
    {
        c.lag = 1;
    }

    return MetroidAttackStep(c);
}

// Moves a PSG channel's envelope on by a frame in the Metroid games, after its first. A change of the track's volume
// restarts the hardware at the count, with a step time of 0. A release restarts the hardware a level below the count,
// falling. A voice with a sound length has no release (0x080011DC).
Next StepMetroidEnvelope(Channel& c)
{
    if (c.modify & kModifyVolume)
    {
        SetPsgVolume(c);
        MetroidPhase(c, 0, true);
    }

    if (c.status & kStatusEcho)
    {
        return c.counter ? Next::kStep : Next::kOff;
    }
    if ((c.status & kStatusStop) && (c.status & kStatusEnvelope))
    {
        if (!c.release || c.length)
        {
            return MetroidEcho(c);
        }

        c.status &= uint8_t(~kStatusEnvelope);
        MetroidPhase(c, c.release);
        return MetroidReleaseStep(c);
    }
    if (c.counter)
    {
        return Next::kStep;
    }

    switch (c.status & kStatusEnvelope)
    {
    case kPhaseAttack:
        return MetroidAttackStep(c);
    case kPhaseDecay:
        return MetroidDecayStep(c);
    case kPhaseSustain:
        if (c.length)
        {
            c.counter = 0xFF;
        }
        else
        {
            c.envelope = c.sustain_goal;
            c.counter = 1;
        }
        return Next::kStep;
    default:
        return MetroidReleaseStep(c);
    }
}

} // namespace

uint32_t KeyToFrequency(uint32_t frequency, int key, int fine)
{
    key &= 0xFF;
    uint32_t fraction = uint32_t(fine & 0xFF) << 24;
    if (key > 178)
    {
        key = 178;
        fraction = 255u << 24;
    }

    const uint32_t low = kFreqTable[ScaleEntry(key) & 0xF] >> (ScaleEntry(key) >> 4);
    const uint32_t high = kFreqTable[ScaleEntry(key + 1) & 0xF] >> (ScaleEntry(key + 1) >> 4);
    return MulHigh(frequency, low + MulHigh(high - low, fraction));
}

uint32_t CamelotKeyToFrequency(uint32_t frequency, int key, int fine)
{
    key &= 0xFF;
    uint32_t fraction = uint32_t(fine & 0xFF) << 24;
    if (key > 178)
    {
        key = 178;
        fraction = 255u << 24;
    }

    // The table holds each semitone's step to the next, which from B is to 2^32, and wraps to the same in 32 bits.
    const uint32_t semitone = ScaleEntry(key) & 0xF;
    const uint32_t octaves = ScaleEntry(key) >> 4;
    const uint32_t step = (semitone == 11 ? 0 : kFreqTable[semitone + 1]) - kFreqTable[semitone];
    return MulHigh(frequency, (kFreqTable[semitone] >> octaves) + MulHigh(step >> octaves, fraction));
}

uint32_t SampleFrequency(const DriverInfo& info, uint32_t frequency, int key, int fine)
{
    return info.camelot_sequencer ? CamelotKeyToFrequency(frequency, key, fine) : KeyToFrequency(frequency, key, fine);
}

uint32_t KeyToPsgFrequency(int channel, int key, int fine)
{
    key &= 0xFF;
    fine &= 0xFF;

    // The noise settings go up a step a key from key 21: through ratios 7 to 4 of each clock shift from 13 down to 0,
    // then ratios 3 to 0 of shift 0 at keys 77 to 80.
    if (channel == 4)
    {
        const int index = key <= 20 ? 0 : std::min(key - 21, 59);
        return index < 56 ? uint32_t(((13 - index / 4) << 4) | (7 - index % 4)) : uint32_t(59 - index);
    }

    if (key <= 35)
    {
        fine = 0;
        key = 0;
    }
    else
    {
        key -= 36;
        if (key > 130)
        {
            key = 130;
            fine = 255;
        }
    }

    const int low = kPsgFreqTable[key % 12] >> (key / 12);
    const int high = kPsgFreqTable[(key + 1) % 12] >> ((key + 1) / 12);
    return uint32_t(low + ((fine * (high - low)) >> 8) + 2048);
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : rom_(rom), info_(info), direct_channels_(SongChannels(rom, info, song))
{
    // The buffer holds as many frames' parts as fit in it. A song whose header sets the reverb sets it for the mixer.
    // The game's code can set it before the start too. m4aMPlayVolumeControl gives each track a quarter of the player's
    // volume.
    if (info.samples_per_frame > 0)
    {
        dma_period_ = uint8_t(std::min<uint32_t>(kPcmBufferSize / uint32_t(info.samples_per_frame), 255));
    }
    reverb_ = uint8_t(info.reverb);
    vol_x_ = uint8_t(SongVolume(info, song) / 4);
    if (!ReadSong(rom, info, song, header_))
    {
        return;
    }
    reverb_ = uint8_t(SongReverb(info, song, header_));

    // The song's tracks go on the player's, as far as it has room for them. An unused player has no room.
    const int count = PlayedTracks(rom, info, song, header_);
    valid_ = count > 0;
    tracks_.resize(size_t(count));
    for (int t = 0; t < count; t++)
    {
        tracks_[size_t(t)].flags = kFlagExist | kFlagStart;
        tracks_[size_t(t)].position = header_.tracks[size_t(t)];
    }
    if (count < header_.track_count)
    {
        Warn("the song has " + std::to_string(header_.track_count) + " tracks, and its music player plays only " +
             std::to_string(count));
    }

    priority_ = header_.priority;

    // The SDK's init gives the PSG's channels an envelope of 0 and restarts them, turns the wave channel off, and sets
    // the master volumes and the outputs. The DAC's resolution goes in SOUNDBIAS's high bits. The BIOS leaves the bias
    // at 0x200.
    auto set = [this](uint32_t address, uint8_t value)
    {
        sound_registers_[address - agb::kRegSound1CntL] = value;
    };
    for (int n = 0; n < 4; n++)
    {
        if (n != 2)
        {
            set(kPsgRegisters[n].nrx2, 0x08);
            set(kPsgRegisters[n].nrx4, 0x80);
        }
    }
    set(kRegNr50, 0x77);
    set(kRegSoundCntH, 0x0E);
    set(kRegSoundCntH + 1, info_.mono ? 0x0B : 0xA9);
    set(kRegSoundCntX, 0x8F);
    set(kRegSoundBias + 1, uint8_t(info_.dac_bits | 0x02));

    for (int n = 0; n < 4; n++)
    {
        Channel& c = channels_[size_t(kFirstPsgChannel + n)];
        c.type = uint8_t(n + 1);
        c.part = uint8_t(n + 1);
        c.pan_mask = uint8_t(0x11 << n);
    }
}

void Sequencer::GrowChannels(bool on)
{
    growing_ = on;
    if (on)
    {
        channels_.reserve(kMostChannels);
        owners_.reserve(kMostChannels);
        enhanced_channels_.reserve(kMostChannels);
    }
}

int Sequencer::GrownChannel(int part)
{
    // A PSG channel's part lies in one unit. A new one takes the next unit that holds none of the part's channels.
    int units = 0;
    for (int c = kChannelCount; c < ChannelCount(); c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (ch.part == part)
        {
            if (!(ch.status & kStatusOn))
            {
                return c;
            }
            units++;
        }
    }
    if (ChannelCount() >= kMostChannels || (part && units + 1 >= agb::kMostPsgUnits))
    {
        return -1;
    }

    const int index = ChannelCount();
    Channel& ch = channels_.emplace_back();
    owners_.push_back(-1);
    enhanced_channels_.emplace_back();
    ch.part = uint8_t(part);
    if (part)
    {
        // The new unit starts as the SDK's init leaves the console's PSG: each channel but the wave channel restarted
        // with an envelope of 0.
        ch.type = uint8_t(part);
        ch.unit = uint16_t(units + 1);
        ch.pan_mask = uint8_t(0x11 << (part - 1));
        if (size_t(ch.unit) > grown_registers_.size())
        {
            grown_registers_.emplace_back();
            for (int n = 0; n < 4; n++)
            {
                if (n != 2)
                {
                    WriteSound(UnitAddress(ch.unit, kPsgRegisters[n].nrx2), 1, 0x08);
                    WriteSound(UnitAddress(ch.unit, kPsgRegisters[n].nrx4), 1, 0x80);
                }
            }
        }
    }
    return index;
}

void Sequencer::StartMixing()
{
    if (dma_period_ < 2)
    {
        throw std::invalid_argument("the mixer's buffer doesn't hold two frames' parts");
    }
    if (info_.camelot_mixer &&
        (info_.camelot_tap <= 0 || info_.camelot_tap >= info_.samples_per_frame || info_.camelot_tap % 2 != 0))
    {
        throw std::invalid_argument("the echo of Camelot's mixer doesn't reach back within a frame");
    }

    // Camelot's output stage can write two points for each of its mixer's points, into buffers twice as long.
    mixing_ = true;
    pcm_.assign(size_t(2 * kPcmBufferSize * PointsPerMixed()), 0);
    sums_.assign(size_t(2 * kMaxTracks * info_.samples_per_frame), 0);
    if (info_.camelot_mixer)
    {
        camelot_sums_.assign(size_t(info_.samples_per_frame), 0);
        camelot_channels_ = 0;
        camelot_last_part_ = 0;
    }
}

void Sequencer::StartEnhancedMixing()
{
    if (!mixing_)
    {
        throw std::logic_error("the enhanced mode mixes after StartMixing()");
    }
    enhanced_ = true;
    enhanced_channels_.assign(channels_.size(), EnhancedChannel{});
    enhanced_out_.assign(2 * size_t(agb::kEnhancedFrameSamples), 0.0f);
    enhanced_history_.assign(2 * size_t(dma_period_) * agb::kEnhancedFrameSamples, 0.0f);
    enhanced_echo_.assign(2 * size_t(agb::kEnhancedFrameSamples), 0.0f);
}

std::span<const float> Sequencer::EnhancedRight() const
{
    if (!enhanced_)
    {
        return {};
    }
    return std::span<const float>(enhanced_out_).first(agb::kEnhancedFrameSamples);
}

std::span<const float> Sequencer::EnhancedLeft() const
{
    if (!enhanced_)
    {
        return {};
    }
    return std::span<const float>(enhanced_out_)
        .subspan(info_.mono ? 0 : agb::kEnhancedFrameSamples, agb::kEnhancedFrameSamples);
}

int Sequencer::PointsPerMixed() const
{
    return info_.camelot_mixer ? info_.camelot_output_points : 1;
}

std::span<const int8_t> Sequencer::Right() const
{
    if (!mixing_)
    {
        return {};
    }

    return std::span<const int8_t>(pcm_).subspan(part_, size_t(info_.samples_per_frame * PointsPerMixed()));
}

std::span<const int8_t> Sequencer::Left() const
{
    if (!mixing_)
    {
        return {};
    }

    const size_t size = kPcmBufferSize * size_t(PointsPerMixed());
    return std::span<const int8_t>(pcm_).subspan((info_.mono ? 0 : size) + part_,
                                                 size_t(info_.samples_per_frame * PointsPerMixed()));
}

const std::vector<Action>& Sequencer::Step()
{
    actions_.clear();
    sound_writes_.clear();

    // m4aSoundVSync counts the frames down to the sound DMA's next start of the buffer.
    dma_counter_ = dma_counter_ > 1 ? uint8_t(dma_counter_ - 1) : dma_period_;

    // Each 150 of the tempo is a tick. The tracks that existed at the start of a tick count as playing, so the player
    // stops in the tick after its last track ended, and then skips the update of the channels.
    if (valid_ && !(status_ & kPlayerStopped))
    {
        tempo_c_ = uint16_t(tempo_c_ + tempo_i_);
        bool stopped = false;
        while (tempo_c_ >= kTickCounter)
        {
            if (clock_ == restate_from_)
            {
                for (Track& track : tracks_)
                {
                    track.restate = kSetVolume | kSetPan | kSetPitch;
                }
            }

            uint32_t playing = 0;
            for (int t = 0; t < TrackCount(); t++)
            {
                if (tracks_[size_t(t)].flags & kFlagExist)
                {
                    playing |= 1u << t;
                    RunTrack(t);
                }
            }

            clock_++;
            if (!playing)
            {
                status_ = kPlayerStopped;
                stopped = true;
                break;
            }

            status_ = playing;
            tempo_c_ = uint16_t(tempo_c_ - kTickCounter);
        }
        if (!stopped)
        {
            UpdateChannels();
        }
    }

    RunPsg();
    RunDirect();
    frame_++;

    return actions_;
}

bool Sequencer::Ended() const
{
    if (!TracksEnded() && valid_)
    {
        return false;
    }

    for (int c = 0; c < ChannelCount(); c++)
    {
        if ((c < direct_channels_ || c >= kFirstPsgChannel) && (channels_[size_t(c)].status & kStatusOn))
        {
            return false;
        }
    }

    return true;
}

void Sequencer::RunTrack(int t)
{
    Track& track = tracks_[size_t(t)];

    // Count down the notes' lengths, and take channels that have stopped off the track's list.
    for (int c = track.chan; c >= 0;)
    {
        Channel& ch = channels_[size_t(c)];
        const int next = ch.next;
        if (!(ch.status & kStatusOn))
        {
            UnlinkChannel(c);
        }
        else if (ch.gate && --ch.gate == 0)
        {
            ReleaseNote(t, c);
        }

        c = next;
    }

    if (track.flags & kFlagStart)
    {
        StartTrack(track);
    }

    // The Metroid games' sequencer counts a wait down and moves the LFO on before it runs the commands that are due. A
    // tick that runs commands moves the LFO on only if the track was waiting at its start.
    if (info_.metroid_sequencer && track.wait)
    {
        track.wait--;
        RunMetroidLfo(track);
    }

    // Run the commands that are due, up to the next wait. A track that ends doesn't move its LFO on.
    for (int commands = 0; track.wait == 0; commands++)
    {
        if (commands == kCommandBudget)
        {
            throw Hang(t, track.position);
        }

        if (!RunCommand(t))
        {
            return;
        }
    }

    if (info_.metroid_sequencer)
    {
        ReportTrack(t);
        return;
    }

    track.wait--;

    // The LFO's shape is a triangle wave from -64 to 64, scaled by the depth. Camelot's sequencer counts the LFO's
    // delay down before it looks at the speed, so it counts it down at a speed of 0 too.
    if ((track.lfo_speed || info_.camelot_sequencer) && track.mod)
    {
        if (track.lfo_delay_c)
        {
            track.lfo_delay_c--;
        }
        else if (track.lfo_speed)
        {
            const uint32_t phase = uint32_t(track.lfo_speed_c) + track.lfo_speed;
            track.lfo_speed_c = uint8_t(phase);
            const int32_t shape = int8_t(uint8_t(phase - 64)) >= 0 ? 128 - int32_t(phase) : int8_t(uint8_t(phase));
            const int32_t depth = (int32_t(track.mod) * shape) >> 6;
            if (uint8_t(depth) != uint8_t(track.mod_m))
            {
                track.mod_m = int8_t(uint8_t(depth));
                track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
            }
        }
    }

    ReportTrack(t);
}

void Sequencer::RunMetroidLfo(Track& track)
{
    // The delay counts down at any speed and depth. The LFO moves on only while the track's newest note sounds. The
    // swing is half as wide as in the SDK's library.
    if (track.lfo_delay_c)
    {
        track.lfo_delay_c--;
        return;
    }
    if (!track.lfo_speed || !track.mod || track.chan < 0 || !(channels_[size_t(track.chan)].status & kStatusOn))
    {
        return;
    }

    // The sequencer leaves the swing as it is when the triangle wave's new value, before the depth scales it, equals
    // the old swing (0x080016E0 in Metroid Fusion).
    const uint32_t phase = uint32_t(track.lfo_speed_c) + track.lfo_speed;
    track.lfo_speed_c = uint8_t(phase);
    const int32_t shape = int8_t(uint8_t(phase - 64)) >= 0 ? 128 - int32_t(uint8_t(phase)) : int8_t(uint8_t(phase));
    if (shape == track.mod_m)
    {
        return;
    }
    track.mod_m = int8_t(uint8_t(((int32_t(track.mod) + 1) * shape) >> 7));
    track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
}

bool Sequencer::RunCommand(int t)
{
    Track& track = tracks_[size_t(t)];
    if (!rom_.Contains(track.position))
    {
        Warn("track " + std::to_string(t) + " runs past the end of the ROM, so it stops there");
        EndTrack(t);
        return false;
    }

    // The decoder reads the command as the driver does, with the running status and the last key and velocity.
    const uint32_t address = track.position;
    const PlayedCommand played =
        ReadPlayedCommand([this](uint32_t at) { return rom_.U8(at); }, address,
                          RunningState{track.running, track.key, track.velocity}, info_.metroid_short_notes ? 2 : 3);
    if (!played.valid)
    {
        Warn("track " + std::to_string(t) +
             " has a byte below 0x80 before any command it could repeat, so it stops there");
        EndTrack(t);
        return false;
    }

    const commands::Command& command = played.command;
    const int64_t value = command.value.value;
    const bool waits_again = command.kind == Kind::kWait && track.timer != 0;
    if (record_commands_ && !waits_again)
    {
        Action a;
        a.kind = Action::kCommand;
        a.track = uint8_t(t);
        a.command = address;
        a.next = played.next;
        a.value = int32_t(value);
        a.command_kind = command.kind;
        AddAction(a);
    }

    track.running = played.after.running;
    track.key = played.after.key;
    track.velocity = played.after.velocity;
    track.position = played.next;
    switch (command.kind)
    {
    case Kind::kNote:
        track.gate = uint8_t(value);
        Note(t, address);
        return true;

    case Kind::kNoteOff:
        EndTie(t);
        break;

    case Kind::kWait:
        if (command.encoding.opcode == (kCmdXcmd << 8 | kXcmdWait))
        {
            // The extended wait runs again on each tick until its count runs out. The driver goes back 2 bytes from
            // its count, to the byte before the extended command's number.
            if (track.timer < value)
            {
                track.timer++;
                track.position = played.extended - 1;
                track.wait = 1;
            }
            else
            {
                track.timer = 0;
            }
            break;
        }
        track.wait = uint8_t(value);
        return true;

    case Kind::kJump:
        track.position = command.target;
        break;

    case Kind::kCall:
        // A pattern can call another, three deep. One more ends the track.
        if (track.pattern_level < 3)
        {
            track.stack[track.pattern_level++] = played.next;
            track.position = command.target;
        }
        else
        {
            EndTrack(t);
        }
        break;

    case Kind::kReturn:
        if (track.pattern_level)
        {
            track.position = track.stack[--track.pattern_level];
        }
        break;

    case Kind::kRepeat:
        // A count of 0 repeats for ever. The track has one count, for all of its repeats.
        if (value == 0 || ++track.repeats < value)
        {
            track.position = command.target;
        }
        else
        {
            track.repeats = 0;
        }
        break;

    case Kind::kSetVariable:
    case Kind::kAddVariable:
    case Kind::kSubtractVariable:
    case Kind::kJumpIfEqual:
    case Kind::kJumpIfNotEqual:
    case Kind::kJumpIfGreater:
    case Kind::kJumpIfGreaterOrEqual:
    case Kind::kJumpIfLessOrEqual:
    case Kind::kJumpIfLess:
        MemAcc(track, command);
        break;

    case Kind::kOther:
        // MEMACC with an operation past 17 does nothing.
        break;

    case Kind::kPriority:
        track.priority = uint8_t(value);
        break;

    case Kind::kTempo:
        {
            // The frame's progress has already been counted at the old tempo, and so has any of it left over for the
            // next tick.
            Action a;
            a.kind = Action::kTempo;
            a.track = uint8_t(t);
            a.old_tempo = tempo_i_;
            a.counter = tempo_c_ - kTickCounter;
            tempo_d_ = uint16_t(value * 2);
            tempo_i_ = uint16_t((uint32_t(tempo_d_) * tempo_u_) >> 8);
            a.value = tempo_i_;
            AddAction(a);
            break;
        }

    case Kind::kTranspose:
        track.key_shift = int8_t(value);
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kProgram:
        {
            // The track keeps a copy of the voice.
            const int voice = int(value);
            ReadVoice(rom_, header_.voices + 12 * uint32_t(voice), track.tone);
            track.voice = voice;
            track.modified = false;
            Action a;
            a.kind = Action::kVoice;
            a.track = uint8_t(t);
            a.value = voice;
            AddAction(a);
            break;
        }

    case Kind::kVolume:
        track.vol = uint8_t(value);
        track.flags |= kFlagVolChange;
        track.sets |= kSetVolume;
        break;

    case Kind::kPan:
        track.pan = int8_t(uint8_t(value - 0x40));
        track.flags |= kFlagVolChange;
        track.sets |= kSetPan;
        break;

    case Kind::kPitchBend:
        track.bend = int8_t(uint8_t(value - 0x40));
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kBendRange:
        track.bend_range = uint8_t(value);
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kLfoSpeed:
        // The Metroid games' sequencer halves the speed, rounding up, and keeps the LFO's swing.
        track.lfo_speed = info_.metroid_sequencer ? uint8_t((value + 1) >> 1) : uint8_t(value);
        if (!track.lfo_speed && !info_.metroid_sequencer)
        {
            ClearModulation(track);
        }
        break;

    case Kind::kLfoDelay:
        track.lfo_delay = uint8_t(value);
        break;

    case Kind::kLfoDepth:
        track.mod = uint8_t(value);
        if (!track.mod && !info_.metroid_sequencer)
        {
            ClearModulation(track);
        }
        break;

    case Kind::kLfoType:
        if (track.mod_t != uint8_t(value))
        {
            track.mod_t = uint8_t(value);
            track.flags |= kFlagVolPitChange;
        }
        break;

    case Kind::kTune:
        track.tune = int8_t(uint8_t(value - 0x40));
        track.flags |= kFlagPitChange;
        track.sets |= kSetPitch;
        break;

    case Kind::kRegister:
        {
            // The byte goes to the register that many bytes past NR10. A register past the wave RAM is left alone.
            const uint32_t register_address = agb::kRegSound1CntL + command.index;
            if (register_address < agb::kRegFifoA)
            {
                WriteSound(register_address, 1, uint8_t(value));
            }
            Warn("track " + std::to_string(t) + " writes to the sound registers, which the conversion leaves out");
            break;
        }

    case Kind::kSampleWave:
    case Kind::kVoiceType:
    case Kind::kAttack:
    case Kind::kDecay:
    case Kind::kSustain:
    case Kind::kRelease:
    case Kind::kEchoVolume:
    case Kind::kEchoLength:
    case Kind::kPsgLength:
    case Kind::kSweep:
    case Kind::kSampleOffset:
        ExtendedCommand(t, command);
        break;

    default:
        // kCmdFine, the codes that the driver's command table gives its handler, and the extended commands that end
        // the track.
        if (command.encoding.opcode >> 8 == kCmdXcmd && (command.encoding.opcode & 0xFF) >= kXcmdCount)
        {
            Warn("track " + std::to_string(t) + " has an unknown extended command, so it stops there");
        }
        EndTrack(t);
        break;
    }

    return track.flags != 0;
}

void Sequencer::StartTrack(Track& track)
{
    // The driver clears the first 64 bytes of the track's state, which leaves its position and pattern stack.
    Track fresh;
    fresh.position = track.position;
    fresh.stack = track.stack;
    fresh.flags = kFlagExist;
    fresh.bend_range = 2;
    fresh.vol_x = 64;

    // A game that gives the player a volume calls m4aMPlayImmInit first, which starts the tracks.
    // m4aMPlayVolumeControl then sets the started track's volX and the flag that recalculates its volumes.
    if (vol_x_ != fresh.vol_x)
    {
        fresh.vol_x = vol_x_;
        fresh.flags |= kFlagVolChange;
    }
    fresh.lfo_speed = 22;
    fresh.tone.type = 1;
    track = fresh;
}

void Sequencer::Note(int t, uint32_t command)
{
    Track& track = tracks_[size_t(t)];
    Action a;
    a.kind = Action::kNoteOn;
    a.track = uint8_t(t);
    a.command = command;
    a.key = track.key;
    a.velocity = track.velocity;
    a.gate = track.gate;
    a.program = track.voice;
    a.modified = track.modified || track.offset != 0;
    a.voice = track.voice >= 0 ? header_.voices + 12 * uint32_t(track.voice) : 0;

    // A key split or drum kit picks a voice for the key from its table. A drum kit's voice plays at its own key and
    // pan.
    Voice voice = track.tone;
    uint8_t key = track.key;
    int8_t rhythm_pan = 0;
    if (track.tone.IsSplit())
    {
        const uint32_t index =
            track.tone.type & kVoiceKeySplit ? rom_.U8(track.tone.key_table + track.key) : uint32_t(track.key);
        a.voice = track.tone.wave + 12 * index;
        ReadVoice(rom_, a.voice, voice);
        if (voice.IsSplit())
        {
            a.kind = Action::kNoteDropped;
            AddAction(a);
            return;
        }

        if (track.tone.type & kVoiceDrumKit)
        {
            rhythm_pan = voice.RhythmPan();
            key = voice.key;

            // The Metroid games' sequencer gives the track a drum's pan: the centre for a byte of 0, the byte less 0x80
            // from 0x80 up, and the track's pan otherwise (0x0800481C in Metroid Fusion).
            if (info_.metroid_sequencer)
            {
                rhythm_pan = 0;
                if (voice.pan_sweep == 0 || (voice.pan_sweep & 0x80))
                {
                    track.pan = voice.pan_sweep ? int8_t(uint8_t(voice.pan_sweep - 0xC0)) : int8_t(0);
                    track.sets |= kSetPan;
                }
            }
        }
    }

    // The Metroid games' sequencer sets the LFO's swing to 0 and works the track's levels and pitch out again at each
    // note, before it looks for a channel. The track's other notes keep theirs, unless a command changed them.
    if (info_.metroid_sequencer)
    {
        if (track.lfo_delay)
        {
            track.lfo_delay_c = track.lfo_delay;
        }
        track.mod_m = 0;
        track.flags |= kFlagVolSet | kFlagPitSet;
        UpdateTrack(track);
    }

    // A later version of Camelot's sequencer keeps 255 for the notes of a player whose priority is 120, and others go
    // up to 254 (0x0824ecd0 in Mario Tennis: Power Tour).
    int priority = std::min(255, priority_ + track.priority);
    if (info_.camelot_top_priority)
    {
        priority = priority_ == kCamelotTopPlayer ? 255 : std::min(priority, 254);
    }
    const int psg = voice.PsgChannel();
    int c = -1;
    if (psg <= 4)
    {
        c = psg || !info_.camelot_sequencer ? FindChannel(t, psg, priority) : FindCamelotChannel(t, priority);

        // A sequencer that grows its channels gives the note a grown channel in place of a channel whose note sounds
        // before its release. A note in its release gives way, as on the console.
        const uint8_t status = c >= 0 ? channels_[size_t(c)].status : 0;
        if (growing_ && (c < 0 || ((status & kStatusOn) && !(status & (kStatusStop | kStatusEcho)))))
        {
            if (const int grown = GrownChannel(psg); grown >= 0)
            {
                c = grown;
            }
        }
    }
    if (c < 0)
    {
        a.kind = Action::kNoteDropped;
        AddAction(a);
        return;
    }

    // The note takes the channel from any note that had it, and goes first in the track's list.
    Channel& ch = channels_[size_t(c)];
    if (ch.status & kStatusOn)
    {
        EndNote(c, !(ch.status & kStatusStart), true);
    }
    UnlinkChannel(c);
    LinkChannel(c, t);

    if (!info_.metroid_sequencer)
    {
        track.lfo_delay_c = track.lfo_delay;
        if (track.lfo_delay)
        {
            ClearModulation(track);
        }
        UpdateTrack(track);
    }

    ch.gate = track.gate;
    ch.midi_key = track.key;
    ch.velocity = track.velocity;
    ch.priority = uint8_t(priority);
    ch.key = key;
    ch.rhythm_pan = rhythm_pan;
    ch.start_pan = track.pan;
    ch.type = voice.type;
    ch.wave = voice.wave;
    ch.attack = voice.attack;
    ch.decay = voice.decay;
    ch.sustain = voice.sustain;
    ch.release = voice.release;
    ch.echo_volume = track.echo_volume;
    ch.echo_length = track.echo_length;
    SetChannelVolume(ch, track);

    SetChannelPitch(ch, track);
    if (!psg)
    {
        ch.count = int32_t(track.offset);
    }
    else
    {
        // Channel 1 sweeps by bits 4 to 6 of the voice's byte, unless they're 0 or bit 7 is set. 8 turns the sweep off.
        ch.length = voice.length;
        ch.sweep = (voice.pan_sweep & 0x80) || !(voice.pan_sweep & 0x70) ? 0x08 : voice.pan_sweep;
    }

    ch.status = kStatusStart;
    ch.synth = false;
    ch.audible = false;
    if (!info_.metroid_sequencer)
    {
        track.flags &= uint8_t(~kFlagVolPitChange);
    }

    a.channel = c;
    a.play_key = key;
    AddAction(a);
}

void Sequencer::EndTie(int t)
{
    // Release the newest of the track's notes with the key that hasn't been released yet.
    Track& track = tracks_[size_t(t)];
    for (int c = track.chan; c >= 0; c = channels_[size_t(c)].next)
    {
        Channel& ch = channels_[size_t(c)];
        if ((ch.status & (kStatusStart | kStatusEnvelope)) && !(ch.status & kStatusStop) && ch.midi_key == track.key)
        {
            ReleaseNote(t, c);
            break;
        }
    }
}

void Sequencer::EndTrack(int t)
{
    // The track's notes are released, and it lets go of its channels.
    Track& track = tracks_[size_t(t)];
    for (int c = track.chan; c >= 0;)
    {
        Channel& ch = channels_[size_t(c)];
        const int next = ch.next;
        if ((ch.status & kStatusOn) && !(ch.status & kStatusStop))
        {
            ReleaseNote(t, c);
        }

        UnlinkChannel(c);
        c = next;
    }

    track.flags = 0;
}

void Sequencer::ExtendedCommand(int t, const commands::Command& command)
{
    Track& track = tracks_[size_t(t)];
    const auto byte = uint8_t(command.value.value);

    // A key split keeps its key table where other voices keep their envelope.
    auto set_envelope = [&](uint8_t& field)
    {
        field = byte;
        track.tone.key_table = track.tone.attack | (track.tone.decay << 8) | (track.tone.sustain << 16) |
                               (uint32_t(track.tone.release) << 24);
        track.modified = true;
    };

    switch (command.kind)
    {
    case Kind::kSampleWave:
        track.tone.wave = uint32_t(command.value.value);
        track.modified = true;
        break;

    case Kind::kVoiceType:
        track.tone.type = byte;
        track.modified = true;
        break;

    case Kind::kAttack:
        set_envelope(track.tone.attack);
        break;

    case Kind::kDecay:
        set_envelope(track.tone.decay);
        break;

    case Kind::kSustain:
        set_envelope(track.tone.sustain);
        break;

    case Kind::kRelease:
        set_envelope(track.tone.release);
        break;

    case Kind::kEchoVolume:
        track.echo_volume = byte;
        break;

    case Kind::kEchoLength:
        track.echo_length = byte;
        break;

    case Kind::kPsgLength:
        track.tone.length = byte;
        track.modified = true;
        break;

    case Kind::kSweep:
        track.tone.pan_sweep = byte;
        track.modified = true;
        break;

    default:
        // kSampleOffset.
        track.offset = uint32_t(command.value.value);
        break;
    }
}

void Sequencer::MemAcc(Track& track, const commands::Command& command)
{
    // Operations 0-5 change the byte, with the value or with the byte the value names. Operations 6-17 jump if the byte
    // compares with them as each one says.
    uint8_t& byte = memory_[command.index];
    const auto value = uint8_t(command.value.value);
    const uint8_t operand = command.value.source == commands::Source::kVariable ? memory_[value] : value;
    bool jump = false;
    switch (command.kind)
    {
    case Kind::kSetVariable:
        byte = operand;
        return;
    case Kind::kAddVariable:
        byte = uint8_t(byte + operand);
        return;
    case Kind::kSubtractVariable:
        byte = uint8_t(byte - operand);
        return;
    case Kind::kJumpIfEqual:
        jump = byte == operand;
        break;
    case Kind::kJumpIfNotEqual:
        jump = byte != operand;
        break;
    case Kind::kJumpIfGreater:
        jump = byte > operand;
        break;
    case Kind::kJumpIfGreaterOrEqual:
        jump = byte >= operand;
        break;
    case Kind::kJumpIfLessOrEqual:
        jump = byte <= operand;
        break;
    default:
        jump = byte < operand;
        break;
    }

    if (jump)
    {
        track.position = command.target;
    }
}

void Sequencer::ClearModulation(Track& track)
{
    track.mod_m = 0;
    track.lfo_speed_c = 0;
    track.flags |= track.mod_t == 0 ? kFlagPitChange : kFlagVolChange;
    track.sets |= track.mod_t == 0 ? kSetPitch : track.mod_t == 1 ? kSetVolume : kSetPan;
}

Sequencer::SideLevels Sequencer::TrackLevels(const Track& track, bool metroid)
{
    // The Metroid games' sequencer gives a pan of -64 to 63 a side's level out of 128, from the centre's level out of
    // 64. Its tremolo multiplies the level by 65 plus the swing over 64, up to 255.
    if (metroid)
    {
        return MetroidLevels(track, track.pan);
    }

    // The volume and pan give each side a level. The pan moves the level in a straight line from one side to the
    // other. Each side's level is a byte. A level that the LFO raises past 255 wraps around.
    uint32_t level = (uint32_t(track.vol) * track.vol_x) >> 5;
    if (track.mod_t == 1)
    {
        level = (level * uint32_t(track.mod_m + 128)) >> 7;
    }

    int32_t pan = 2 * track.pan;
    if (track.mod_t == 2)
    {
        pan += track.mod_m;
    }
    pan = std::clamp(pan, -128, 127);

    return {uint8_t((uint32_t(127 - pan) * level) >> 8), uint8_t((uint32_t(pan + 128) * level) >> 8)};
}

Sequencer::SideLevels Sequencer::MetroidLevels(const Track& track, int pan)
{
    uint32_t level = (uint32_t(track.vol) * track.vol_x) >> 5;
    if (track.mod_t == 1)
    {
        level = uint32_t(std::min(255, (int32_t(level) * (track.mod_m + 65)) >> 6));
    }

    pan = std::clamp(pan, -64, 63);
    if (track.mod_t == 2)
    {
        pan = std::clamp(pan + track.mod_m, -64, 63);
    }

    return {uint8_t((uint32_t(64 - pan) * level) >> 7), uint8_t((uint32_t(pan + 64) * level) >> 7)};
}

void Sequencer::UpdateTrack(Track& track)
{
    if (track.flags & kFlagVolSet)
    {
        const SideLevels levels = TrackLevels(track, info_.metroid_sequencer);
        track.vol_mr = levels.right;
        track.vol_ml = levels.left;
    }

    // The pitch is in 256ths of a semitone: the key offset in the high bits, and the fine tune in the low 8.
    if (track.flags & kFlagPitSet)
    {
        const int32_t pitch = TrackPitch(track, info_.metroid_sequencer);
        track.key_m = int8_t(uint8_t(pitch >> 8));
        track.pit_m = uint8_t(pitch);
    }

    track.flags &= uint8_t(~(kFlagPitSet | kFlagVolSet));
}

void Sequencer::SetChannelVolume(Channel& c, const Track& track) const
{
    // The Metroid games' sequencer plays a note at its velocity plus 1 out of 128 of the track's levels. The mixer
    // works a drum kit's levels out again for each DirectSound note, at the pan that the note started with (0x08001006
    // in Metroid Fusion).
    if (info_.metroid_sequencer)
    {
        const SideLevels levels = (track.tone.type & kVoiceDrumKit) && !(c.type & kVoicePsg)
                                      ? MetroidLevels(track, c.start_pan)
                                      : SideLevels{track.vol_ml, track.vol_mr};
        c.right = uint8_t(std::min(((c.velocity + 1) * levels.right) >> 7, 255));
        c.left = uint8_t(std::min(((c.velocity + 1) * levels.left) >> 7, 255));
        return;
    }

    // Camelot's sequencer works out the left side's level from 128 less a drum kit voice's pan, where the driver takes
    // it from 127, so every note's left side comes out a little louder.
    const int right = (c.velocity * (128 + c.rhythm_pan) * track.vol_mr) >> 14;
    const int left = (c.velocity * ((info_.camelot_sequencer ? 128 : 127) - c.rhythm_pan) * track.vol_ml) >> 14;
    c.right = uint8_t(std::min(right, 255));
    c.left = uint8_t(std::min(left, 255));
}

void Sequencer::SetChannelPitch(Channel& c, const Track& track) const
{
    const int key = std::max(0, c.key + track.key_m);
    const int psg = c.type & kVoicePsg;
    if (psg && psg != 4 && info_.metroid_psg_table)
    {
        c.frequency = MetroidPsgFrequency(key, track.pit_m);
    }
    else if (psg)
    {
        c.frequency = KeyToPsgFrequency(psg, key, track.pit_m);
    }
    else
    {
        c.frequency = SampleFrequency(info_, rom_.U32(c.wave + 4), key, track.pit_m);
    }
}

uint32_t Sequencer::MetroidPsgFrequency(int key, int fine) const
{
    // The Metroid games take a square or wave channel's setting for a key from their table, and add (fine + 1)/256 of
    // the step to the next key's for a fine tune above 0. The table's keys below 36 repeat the octave from 36 up
    // (0x08001E48 in Metroid Fusion).
    key = std::min(key, 127);
    const uint32_t table = info_.metroid_psg_table;
    const int low = rom_.U16(table + 2 * uint32_t(key));
    if (!fine)
    {
        return uint32_t(low);
    }
    const int high = rom_.U16(table + 2 * uint32_t(key + 1));
    return uint32_t(low + (((high - low) * (fine + 1)) >> 8)) & 0xFFFF;
}

void Sequencer::UpdateChannels()
{
    for (int t = 0; t < TrackCount(); t++)
    {
        Track& track = tracks_[size_t(t)];
        if (!(track.flags & kFlagExist) || !(track.flags & kFlagVolPitChange))
        {
            continue;
        }

        UpdateTrack(track);
        for (int c = track.chan; c >= 0;)
        {
            Channel& ch = channels_[size_t(c)];
            const int next = ch.next;
            if (!(ch.status & kStatusOn))
            {
                UnlinkChannel(c);
                c = next;
                continue;
            }

            if (track.flags & kFlagVolChange)
            {
                SetChannelVolume(ch, track);
                if (ch.type & kVoicePsg)
                {
                    ch.modify |= kModifyVolume;
                }
            }
            if (track.flags & kFlagPitChange)
            {
                SetChannelPitch(ch, track);
                if (ch.type & kVoicePsg)
                {
                    ch.modify |= kModifyPitch;
                }
            }

            c = next;
        }

        // The Metroid games' mixer leaves a drum kit's track at the pan of the last DirectSound note whose levels it
        // works out. It works through the channels in order.
        if (info_.metroid_sequencer && (track.tone.type & kVoiceDrumKit) && (track.flags & kFlagVolChange))
        {
            int last = -1;
            for (int c = track.chan; c >= 0; c = channels_[size_t(c)].next)
            {
                if (!(channels_[size_t(c)].type & kVoicePsg))
                {
                    last = std::max(last, c);
                }
            }
            if (last >= 0)
            {
                track.pan = channels_[size_t(last)].start_pan;
            }
        }

        track.flags &= uint8_t(~kFlagVolPitChange);
    }
}

int Sequencer::FindChannel(int t, int psg, int priority) const
{
    // A PSG voice has one channel. It takes it if it's free, or plays a released note, or a note of lower priority, or
    // one of the same priority from this track or a later one.
    if (psg)
    {
        const int c = kFirstPsgChannel + psg - 1;
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn) || (ch.status & kStatusStop) || ch.priority < priority)
        {
            return c;
        }

        return ch.priority == priority && ch.track >= t ? c : -1;
    }

    // A sample takes the first free channel. Failing that, it takes a released note, or else a note of lower priority,
    // or of the same priority from this track or a later one: of those, the one with the lowest priority, from the
    // latest track, and the last in the list of channels.
    int found = -1;
    int lowest = priority;
    int latest = t + 1;
    bool released = false;
    for (int c = 0; c < direct_channels_; c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn))
        {
            return c;
        }

        const int order = ch.track + 1;
        if ((ch.status & kStatusStop) && !released)
        {
            released = true;
            lowest = ch.priority;
            latest = order;
            found = c;
            continue;
        }
        if (!(ch.status & kStatusStop) && released)
        {
            continue;
        }

        if (ch.priority < lowest)
        {
            lowest = ch.priority;
            latest = order;
            found = c;
        }
        else if (ch.priority == lowest && order >= latest)
        {
            latest = order;
            found = c;
        }
    }

    return found;
}

int Sequencer::FindCamelotChannel(int t, int priority) const
{
    // A sample takes the third free channel, in the list's order. With only one or two free, it takes the last of them,
    // unless one of this track's released notes has a channel: then it takes the last of those.
    const int count = direct_channels_;
    int free_count = 0;
    int last_free = -1;
    uint32_t released = 0;
    for (int c = 0; c < count; c++)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(ch.status & kStatusOn))
        {
            if (++free_count == 3)
            {
                return c;
            }

            last_free = c;
        }
        else if ((ch.status & kStatusStop) && !(info_.camelot_top_priority && ch.priority == 255))
        {
            // A later version's search leaves out a released note of priority 255 (0x0824ed3e in Mario Tennis: Power
            // Tour).
            released |= 1u << c;
        }
    }

    if (last_free >= 0)
    {
        for (int c = count - 1; c >= 0; c--)
        {
            if ((released & (1u << c)) && channels_[size_t(c)].track == t)
            {
                return c;
            }
        }

        return last_free;
    }

    // With none free, it takes a released note if there is one, or else a note of lower priority, or of the same
    // priority from this track or a later one: of those, the one with the lowest priority, from the latest track, and
    // the first in the list of channels.
    const uint32_t candidates = released ? released : (1u << count) - 1;
    int found = -1;
    int lowest = released ? 256 : priority;
    int latest = released ? 0 : t + 1;
    for (int c = count - 1; c >= 0; c--)
    {
        const Channel& ch = channels_[size_t(c)];
        if (!(candidates & (1u << c)))
        {
            continue;
        }

        const int order = ch.track + 1;
        if (ch.priority < lowest)
        {
            lowest = ch.priority;
            latest = order;
            found = c;
        }
        else if (ch.priority == lowest && order >= latest)
        {
            latest = order;
            found = c;
        }
    }

    return found;
}

void Sequencer::LinkChannel(int index, int t)
{
    Channel& ch = channels_[size_t(index)];
    Track& track = tracks_[size_t(t)];
    ch.prev = -1;
    ch.next = track.chan;
    if (track.chan >= 0)
    {
        channels_[size_t(track.chan)].prev = index;
    }

    track.chan = index;
    ch.track = t;
    owners_[size_t(index)] = int8_t(t);
}

void Sequencer::UnlinkChannel(int index)
{
    Channel& ch = channels_[size_t(index)];
    if (ch.track < 0)
    {
        return;
    }

    if (ch.prev >= 0)
    {
        channels_[size_t(ch.prev)].next = ch.next;
    }
    else
    {
        tracks_[size_t(ch.track)].chan = ch.next;
    }
    if (ch.next >= 0)
    {
        channels_[size_t(ch.next)].prev = ch.prev;
    }

    ch.track = -1;
}

void Sequencer::SetEnvelopeCounter(int counter)
{
    if (counter < 0 || counter > 14)
    {
        throw std::invalid_argument("the PSG envelopes' frame counter runs from 0 to 14");
    }
    c15_ = uint8_t(counter);
}

void Sequencer::RunPsg()
{
    // The steps of the hardware's envelopes are 64 a second, so every 15th frame takes two.
    c15_ = c15_ ? uint8_t(c15_ - 1) : 14;

    // The console's PSG channels, then the grown ones.
    for (int index = kFirstPsgChannel; index < ChannelCount(); index++)
    {
        Channel& c = channels_[size_t(index)];
        const int n = c.part;
        if (!n || !(c.status & kStatusOn))
        {
            continue;
        }

        // The hardware envelope's step time and direction stay as the last write left them, unless the frame changes
        // them.
        c.step = SoundRegister(UnitAddress(c.unit, kPsgRegisters[n - 1].nrx2));

        // A note released before the channel started it never sounds.
        Next next = Next::kStep;
        const bool never_started = (c.status & kStatusStart) && (c.status & kStatusStop);
        if (never_started)
        {
            next = Next::kOff;
        }
        else if (c.status & kStatusStart)
        {
            c.status = kPhaseAttack;
            c.modify = kModifyVolume | kModifyPitch;
            SetPsgVolume(c);
            StartPsgRegisters(n, c);
            c.counter = c.attack;
            c.lag = 0;
            if (info_.metroid_sequencer)
            {
                next = MetroidStart(c);
            }
            else if (c.attack)
            {
                c.envelope = 0;
            }
            else
            {
                next = StartDecay(c);
            }
        }
        else if (info_.metroid_sequencer)
        {
            next = StepMetroidEnvelope(c);
        }
        else if (c.status & kStatusEcho)
        {
            c.echo_length--;
            next = int8_t(c.echo_length) <= 0 ? Next::kOff : Next::kDone;
        }
        else if ((c.status & kStatusStop) && (c.status & kStatusEnvelope))
        {
            c.status &= uint8_t(~kStatusEnvelope);
            c.counter = c.release;
            if (!c.release)
            {
                next = StartEcho(c);
            }
            else
            {
                SetEnvelopeStep(c, c.release);
            }
        }
        else
        {
            next = StepEnvelope(c);
        }

        // The counter steps once a frame, and twice every 15th frame. The Metroid games' counter steps once a frame.
        int c15 = c15_;
        if (info_.metroid_sequencer && next == Next::kStep)
        {
            c.counter--;
            next = Next::kDone;
        }
        while (next == Next::kStep)
        {
            c.counter--;
            if (c15 != 0)
            {
                next = Next::kDone;
                break;
            }

            c15--;
            next = StepEnvelope(c);
        }
        if (next == Next::kOff)
        {
            StopPsg(n, c.unit);
            c.status = 0;
            c.modify = 0;
            EndNote(index, !never_started);
            continue;
        }

        CheckAudible(index);

        // A fixed-pitch voice's frequency is rounded to suit the output's resolution. The driver rounds it when it
        // changes, and rounding it again changes nothing.
        if (n < 4 && (c.type & kVoiceFixed))
        {
            if (info_.dac_bits < 0x40)
            {
                c.frequency = (c.frequency + 2) & 0x7FC;
            }
            else if (info_.dac_bits < 0x80)
            {
                c.frequency = (c.frequency + 1) & 0x7FE;
            }
        }

        WritePsgRegisters(n, c);
        c.modify = 0;
    }
}

void Sequencer::StartPsgRegisters(int n, Channel& c)
{
    // A square channel takes its duty in NRx1's top 2 bits. The wave channel takes its pattern when another pattern is
    // in its RAM: NR30's bit 6 has the channel play the other bank meanwhile. The noise channel takes its type in
    // NR43's bit 3. The length's enable goes in NRx4 with the pitch.
    const PsgRegisters& r = kPsgRegisters[n - 1];
    const auto at = [&c](uint32_t address)
    {
        return UnitAddress(c.unit, address);
    };
    switch (n)
    {
    case 3:
        if (c.wave != c.pattern)
        {
            WriteSound(at(r.nrx0), 1, 0x40);
            for (uint32_t i = 0; i < 4; i++)
            {
                WriteSound(at(kRegWaveRam + 4 * i), 4, rom_.U32(c.wave + 4 * i));
            }
            c.pattern = c.wave;
        }
        WriteSound(at(r.nrx0), 1, 0);
        WriteSound(at(r.nrx1), 1, c.length);
        c.n4 = c.length ? 0xC0 : 0x80;
        return;
    case 4:
        WriteSound(at(r.nrx1), 1, c.length);
        WriteSound(at(r.nrx3), 1, c.wave << 3);
        break;
    default:
        if (n == 1)
        {
            WriteSound(at(r.nrx0), 1, c.sweep);
        }
        WriteSound(at(r.nrx1), 1, (c.wave << 6) + c.length);
        break;
    }

    c.step = uint8_t(c.attack + kEnvelopeUp);
    c.n4 = c.length ? 0x40 : 0x00;
}

void Sequencer::WritePsgRegisters(int n, Channel& c)
{
    const PsgRegisters& r = kPsgRegisters[n - 1];
    const auto at = [&c](uint32_t address)
    {
        return UnitAddress(c.unit, address);
    };
    if (c.modify & kModifyPitch)
    {
        // The noise channel keeps its type in NR43's bit 3. The frequency's high bits go in NRx4.
        WriteSound(at(r.nrx3), 1, n == 4 ? (SoundRegister(at(r.nrx3)) & 0x08) | c.frequency : c.frequency);
        c.n4 = uint8_t((c.n4 & 0xC0) + ((c.frequency >> 8) & 0xFF));
        WriteSound(at(r.nrx4), 1, c.n4);
    }

    if (c.modify & kModifyVolume)
    {
        WriteSound(at(kRegNr51), 1, (SoundRegister(at(kRegNr51)) & ~c.pan_mask) | c.pan);
        if (n == 3)
        {
            // The envelope never goes above 31. The first volume after a note starts restarts the wave channel.
            WriteSound(at(r.nrx2), 1, kWaveVolumes[c.envelope & 31]);
            if (c.n4 & 0x80)
            {
                WriteSound(at(r.nrx0), 1, 0x80);
                WriteSound(at(r.nrx4), 1, c.n4);
                c.n4 &= 0x7F;
            }
        }
        else
        {
            // The other channels restart at the envelope's level, and the hardware envelope moves on from there. The
            // Metroid games' count can run a level ahead of the hardware (see MetroidStart()).
            // Channel 1 restarts a second time unless NR10 has its sweep lower the pitch.
            WriteSound(at(r.nrx2), 1, (c.step & 0x0F) + (uint32_t(c.envelope - c.lag) << 4));
            WriteSound(at(r.nrx4), 1, c.n4 | 0x80);
            if (n == 1 && !(SoundRegister(at(r.nrx0)) & 0x08))
            {
                WriteSound(at(r.nrx4), 1, c.n4 | 0x80);
            }
        }
    }
}

void Sequencer::StopPsg(int n, int unit)
{
    const PsgRegisters& r = kPsgRegisters[n - 1];
    if (n == 3)
    {
        WriteSound(UnitAddress(unit, r.nrx0), 1, 0);
        return;
    }

    // A rising envelope with a step time of 0 stays at its level of 0.
    WriteSound(UnitAddress(unit, r.nrx2), 1, kEnvelopeUp);
    WriteSound(UnitAddress(unit, r.nrx4), 1, 0x80);
}

uint32_t Sequencer::UnitAddress(int unit, uint32_t address)
{
    return unit == 0
               ? address
               : agb::kGrownPsgRegisters + uint32_t(unit - 1) * agb::kGrownPsgBytes + (address - agb::kRegSound1CntL);
}

void Sequencer::SetSoundRegisters(std::span<const uint8_t> registers)
{
    if (registers.size() != sound_registers_.size())
    {
        throw std::invalid_argument("the sound registers take " + std::to_string(sound_registers_.size()) + " bytes");
    }

    std::copy(registers.begin(), registers.end(), sound_registers_.begin());
}

void Sequencer::WriteSound(uint32_t address, int size, uint32_t value)
{
    if (size < 4)
    {
        value &= (1u << (8 * size)) - 1;
    }
    for (int i = 0; i < size; i++)
    {
        const uint32_t byte = address + uint32_t(i);
        if (byte >= agb::kGrownPsgRegisters)
        {
            const uint32_t offset = byte - agb::kGrownPsgRegisters;
            grown_registers_[offset / agb::kGrownPsgBytes][offset % agb::kGrownPsgBytes] = uint8_t(value >> (8 * i));
        }
        else
        {
            sound_registers_[byte - agb::kRegSound1CntL] = uint8_t(value >> (8 * i));
        }
    }
    sound_writes_.push_back({address, uint8_t(size), value});
}

uint8_t Sequencer::SoundRegister(uint32_t address) const
{
    if (address >= agb::kGrownPsgRegisters)
    {
        const uint32_t offset = address - agb::kGrownPsgRegisters;
        return grown_registers_[offset / agb::kGrownPsgBytes][offset % agb::kGrownPsgBytes];
    }
    return sound_registers_[address - agb::kRegSound1CntL];
}

void Sequencer::RunDirect()
{
    const bool metering = mixing_ && metering_;
    if (mixing_)
    {
        StartPart();
    }
    if (enhanced_)
    {
        StartEnhancedPart();
    }
    if (metering)
    {
        std::fill(sums_.begin(), sums_.end(), int16_t(0));
    }

    // The console's DirectSound channels, then the grown ones.
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& c = channels_[size_t(i)];
        if ((i >= direct_channels_ && (i < kChannelCount || c.part)) || !(c.status & kStatusOn))
        {
            continue;
        }

        // The mixer starts a note's sample, then moves its envelope on: up by the attack each frame, down by the decay
        // to the sustain level, and down by the release after the note's release, each a fraction out of 256. After the
        // release, the note can carry on at its echo's level for the echo's length.
        Wave wave;
        ReadWave(rom_, c.wave, wave);
        int envelope = c.envelope;
        bool echo = false;
        const bool starting = (c.status & kStatusStart) != 0;
        if (starting)
        {
            if (c.status & kStatusStop)
            {
                c.status = 0;
                EndNote(i, false);
                continue;
            }

            // The enhanced mode counts the position from the sample's first point. The note starts c.count points in.
            enhanced_channels_[size_t(i)].position = uint64_t(uint32_t(c.count)) << 23;
            c.status = kPhaseAttack;
            c.position = wave.Data() + uint32_t(c.count);
            c.count = int32_t(wave.size - uint32_t(c.count));
            c.fraction = 0;
            envelope = 0;
            if (wave.loops)
            {
                c.status |= kStatusLoop;
            }
        }
        else if (c.status & kStatusEcho)
        {
            if (c.echo_length-- <= 1)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }
        }
        else if (c.status & kStatusStop)
        {
            // Camelot's mixer takes 256 less the release off the level each frame, in a straight line.
            envelope = info_.camelot_mixer ? envelope + c.release - 256 : (envelope * c.release) >> 8;
            echo = info_.metroid_sequencer ? envelope <= 0 : envelope <= c.echo_volume;
        }
        else if ((c.status & kStatusEnvelope) == kPhaseDecay)
        {
            envelope = (envelope * c.decay) >> 8;
            if (envelope <= c.sustain)
            {
                envelope = c.sustain;
                if (envelope == 0)
                {
                    echo = true;
                }
                else
                {
                    c.status--;
                }
            }
        }

        if ((c.status & (kStatusStop | kStatusEcho | kStatusEnvelope)) == kPhaseAttack)
        {
            envelope += c.attack;
            if (envelope >= 255)
            {
                envelope = 255;
                c.status--;

                // The Metroid games' mixer moves the level down by the decay in the frame that the attack ends.
                if (info_.metroid_sequencer)
                {
                    envelope = (envelope * c.decay) >> 8;
                    if (envelope <= c.sustain)
                    {
                        envelope = c.sustain;
                        if (envelope == 0)
                        {
                            echo = true;
                        }
                        else
                        {
                            c.status--;
                        }
                    }
                }
            }
        }
        // The Metroid games' mixer ends a note whose level reaches 0, unless it has both an echo volume and an echo
        // length. Such a note is silent for that frame, then plays on at its last level for the length less 1
        // (0x080010EA in Metroid Fusion).
        if (echo && info_.metroid_sequencer)
        {
            if (!c.echo_volume || !c.echo_length)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }
            c.status |= kStatusEcho;
            continue;
        }
        if (echo)
        {
            envelope = c.echo_volume;
            if (envelope == 0)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }

            c.status |= kStatusEcho;
        }

        c.envelope = uint8_t(envelope);
        CheckAudible(i);

        // A muted channel plays at volume 0.
        const int owner = owners_[size_t(i)];
        const bool muted = owner >= 0 && (muted_tracks_ >> owner & 1);
        const int meter = metering && !muted ? owner : -1;

        // Camelot's mixer plays each side at 9/8 of the envelope. It leaves out a note that comes out silent on both
        // sides. Such a note doesn't move on through its sample (0x03000846 to 0x03000888 in Golden Sun: The Lost Age).
        if (info_.camelot_mixer)
        {
            const int level = envelope + (envelope >> 3);
            const uint32_t right = uint32_t((c.right * level) >> 9);
            const uint32_t left = uint32_t((c.left * level) >> 9);
            if (right == 0 && left == 0)
            {
                enhanced_channels_[size_t(i)].volumes = {};
                continue;
            }
            if (mixing_)
            {
                // The enhanced mode's volumes: each side's volume at 9/8 of the envelope, over 512, and the output
                // stage's 128 to a point.
                if (enhanced_)
                {
                    const float scale = muted ? 0.0f : float(envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
                    MixEnhanced(i, c, wave, {float(c.right) * scale, float(c.left) * scale}, starting);
                }
                MixCamelot(c, wave, muted ? 0 : right | left << 16, meter);
                if (!c.status)
                {
                    EndNote(i, true);
                }
                continue;
            }
        }

        // The master volume scales the envelope's level, and the level scales each side's volume. The mono mixer plays
        // the channel at the average of its volumes.
        const int level = muted ? 0 : (envelope * (info_.master_volume + 1)) >> 4;
        if (enhanced_)
        {
            // The enhanced mode's volumes: the master volume's share of the envelope, times each side's volume, over
            // 2^20. The mono mixer's one side takes the average of the two.
            const float scale = muted ? 0.0f : float(envelope * (info_.master_volume + 1)) / 1048576.0f;
            const std::array<float, 2> volumes =
                info_.mono ? std::array<float, 2>{float(c.right + c.left) * scale * 0.5f,
                                                  float(c.right + c.left) * scale * 0.5f}
                           : std::array<float, 2>{float(c.right) * scale, float(c.left) * scale};
            MixEnhanced(i, c, wave, volumes, starting);
        }
        if (info_.mono)
        {
            const int volume = ((c.right + c.left) * level) >> 9;
            MixChannel(c, wave, volume, volume, meter);
        }
        else
        {
            MixChannel(c, wave, (c.right * level) >> 8, (c.left * level) >> 8, meter);
        }
        if (!c.status)
        {
            EndNote(i, true);
        }
    }

    if (mixing_ && info_.camelot_mixer)
    {
        OutputCamelot();
    }
    if (enhanced_)
    {
        EndEnhancedPart();
    }

    // The mono mixer's left side is its right side.
    peaks_ = {};
    if (metering)
    {
        const size_t points = size_t(info_.samples_per_frame);
        for (size_t t = 0; t < tracks_.size(); t++)
        {
            for (size_t side = 0; side < 2; side++)
            {
                const int16_t* sums = &sums_[(2 * t + (info_.mono ? 0 : side)) * points];
                for (size_t i = 0; i < points; i++)
                {
                    peaks_[t][1 - side] = std::max<int32_t>(peaks_[t][1 - side], std::abs(sums[i]));
                }
            }
        }
    }
}

bool Sequencer::MixerSilent() const
{
    const auto silent = [](std::span<const int8_t> points)
    {
        return std::all_of(points.begin(), points.end(), [](int8_t point) { return point == 0; });
    };
    if (!mixing_)
    {
        return true;
    }

    // Camelot's echo comes from the buffer's earlier output, and from the next frame's sums that the output stage
    // starts. With one point for each of the mixer's, the stage starts each half of a silent sum at 64 less, and its
    // rounding gives the 64 back.
    if (info_.camelot_mixer)
    {
        const uint32_t start = info_.camelot_output_points == 1 ? 0xFFBFFFC0 : 0;
        return silent(pcm_) && std::all_of(camelot_sums_.begin(), camelot_sums_.end(),
                                           [start](uint32_t s) { return s == 0 || s == start; });
    }
    return reverb_ ? silent(pcm_) : silent(Right()) && silent(Left());
}

void Sequencer::StartPart()
{
    // The frame's part follows the parts that the DMA has played since it last started the buffer: the buffer's start
    // when the DMA starts it again in the next frame. Camelot's SoundMain works out the place in the same way, from the
    // copy of the DMA counter that the game's VBlank interrupt makes (0x08000682 in Golden Sun: The Lost Age).
    const uint32_t points = uint32_t(info_.samples_per_frame);
    part_ = dma_counter_ > 1 ? (uint32_t(dma_period_) - (dma_counter_ - 1u)) * points * uint32_t(PointsPerMixed()) : 0;
    if (info_.camelot_mixer)
    {
        return;
    }

    int8_t* right = &pcm_[part_];
    int8_t* left = right + kPcmBufferSize;
    if (!reverb_)
    {
        std::fill(right, right + points, int8_t(0));
        if (!info_.mono)
        {
            std::fill(left, left + points, int8_t(0));
        }
        return;
    }

    // The reverb adds both sides' points of the part, from the DMA's last pass over the buffer, to those of the next
    // part, or of the buffer's first part after its last. The part's points on both sides take the sum times the
    // reverb, over 512 and rounded down. A result whose bit 7 is set goes up by 1. The mono mixer adds its one side's
    // points, and takes the sum times the reverb over 256.
    const uint32_t next = dma_counter_ == 2 ? 0 : part_ + points;
    if (info_.mono)
    {
        for (uint32_t i = 0; i < points; i++)
        {
            int32_t value = ((right[i] + pcm_[next + i]) * reverb_) >> 8;
            if (value & 0x80)
            {
                value++;
            }
            right[i] = int8_t(value);
        }
        return;
    }

    for (uint32_t i = 0; i < points; i++)
    {
        const int32_t sum = right[i] + left[i] + pcm_[next + i] + pcm_[kPcmBufferSize + next + i];
        int32_t value = (sum * reverb_) >> 9;
        if (value & 0x80)
        {
            value++;
        }
        right[i] = int8_t(value);
        left[i] = int8_t(value);
    }
}

void Sequencer::MixChannel(Channel& c, const Wave& wave, int right, int left, int meter)
{
    const int samples = info_.samples_per_frame;

    // Each point that the channel plays adds to the frame's part on each side, or on the mono mixer's one side. A
    // track's sums for its meter add the points without wrapping round.
    int8_t* out_right = mixing_ ? &pcm_[part_] : nullptr;
    int8_t* out_left = mixing_ && !info_.mono ? &pcm_[kPcmBufferSize + part_] : nullptr;
    int16_t* sums_right = meter >= 0 ? &sums_[size_t(2 * meter * samples)] : nullptr;
    int16_t* sums_left = sums_right ? sums_right + samples : nullptr;
    const auto mix = [&](int i, int32_t value)
    {
        if (out_right)
        {
            AddPoint(out_right[i], value, right);
        }
        if (out_left)
        {
            AddPoint(out_left[i], value, left);
        }
        if (sums_right)
        {
            sums_right[i] = int16_t(sums_right[i] + ((value * right) >> 8));
            sums_left[i] = int16_t(sums_left[i] + ((value * left) >> 8));
        }
    };

    // Camelot's mixer takes a sample that has no points left when it comes to mix it, as one of no length is from the
    // start, as a synth voice from then on, unless it plays at a fixed pitch.
    if (info_.camelot_mixer && !(c.type & kVoiceFixed) && (c.synth || c.count == 0))
    {
        c.synth = true;
        MixSynth(c);
        return;
    }

    // A newer driver's mixer plays a reversed sample backwards from its end, and counts its way through a compressed
    // sample by the point rather than the address. A compressed voice with an uncompressed sample isn't mixed.
    const bool special = info_.special_samples && (c.type & (kVoiceReverse | kVoiceCompressed));
    const bool reverse = special && (c.type & kVoiceReverse);
    if (special && !(c.status & kStatusSpecial))
    {
        c.status |= kStatusSpecial;
        if (reverse)
        {
            c.position = 2 * c.wave + 32 + wave.size - c.position;
        }
        if (wave.type != 0)
        {
            c.position -= c.wave + 16;
        }
    }
    if (special && !reverse && wave.type == 0)
    {
        return;
    }

    // Otherwise a fixed-pitch sample plays a point for each point of the output. The driver takes a loop of no length
    // as no loop.
    const uint32_t loop_start = special ? wave.loop_start : wave.Data() + wave.loop_start;
    const int32_t loop_length = int32_t(wave.size - wave.loop_start);
    const bool loops = !reverse && (c.status & kStatusLoop) && loop_length > 0;
    if (!special && (c.type & kVoiceFixed))
    {
        for (int i = 0; i < samples; i++)
        {
            mix(i, rom_.S8(c.position));
            c.position++;
            if (--c.count == 0)
            {
                if (!loops)
                {
                    c.status = 0;
                    return;
                }

                c.position = loop_start;
                c.count = loop_length;
            }
        }

        return;
    }

    // Other samples move on by their rate, in 2^-23 points, for each point of the output. A sample that ends stops the
    // channel there, without saving its progress, and a loop goes back by its length as many times as it takes. A
    // reversed sample doesn't loop.
    //
    // Each point of the output lies between two of the sample's points, at the fraction. A reversed sample's position
    // is one past its point, and its next point lies before that. The fraction keeps the bits above 29 that the steps
    // carry into it, and the product with the difference wraps round at 32 bits.
    const uint32_t step = special && (c.type & kVoiceFixed) ? 0x800000u : c.frequency * info_.step_scale;
    const bool compressed = special && wave.type != 0;
    DecodedBlock block;
    const auto point = [&](uint32_t at)
    {
        return compressed ? CompressedPoint(rom_, c.wave, at, block) : int32_t(rom_.S8(at));
    };
    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t position = c.position;
    for (int i = 0; i < samples; i++)
    {
        if (out_right)
        {
            const int32_t s0 = reverse ? point(position - 1) : point(position);
            const int32_t s1 = reverse ? point(position - 2) : point(position + 1);
            mix(i, s0 + (int32_t(fraction * uint32_t(s1 - s0)) >> 23));
        }

        fraction += step;
        const int32_t advance = int32_t(fraction >> 23);
        if (advance == 0)
        {
            continue;
        }

        fraction &= ~0x3F800000u;
        count -= advance;
        if (count > 0)
        {
            position = reverse ? position - uint32_t(advance) : position + uint32_t(advance);
            continue;
        }
        if (!loops)
        {
            c.status = 0;
            return;
        }

        int32_t overshoot = -count;
        count += loop_length;
        while (count <= 0)
        {
            overshoot -= loop_length;
            count += loop_length;
        }

        position = loop_start + uint32_t(overshoot);
    }

    c.fraction = fraction;
    c.count = count;
    c.position = position;
}

void Sequencer::MixSynth(Channel& c) const
{
    // A synth voice's data follows its sample's 16-byte header. It starts 0x80 and its type: 0 for a pulse wave, 1 for
    // a saw wave, and anything else for a triangle wave.
    const uint32_t data = c.wave + 16;
    const int type = int8_t(rom_.U8(data + 1));
    const uint32_t step = SynthStep(c.frequency, info_.step_scale);
    const int samples = info_.samples_per_frame;

    // A pulse wave's duty follows a triangle wave whose phase is the top byte of the count, which moves on by the
    // fourth byte of the sample's data each frame.
    if (type == 0)
    {
        c.count = int32_t(uint32_t(c.count) + DutyStep(rom_, data));
        c.fraction += step * uint32_t(samples);
        return;
    }

    // A saw wave goes through a filter that adds half its last output to each point, which the count keeps.
    if (type == 1)
    {
        int32_t filter = c.count;
        for (int i = 0; i < samples; i++)
        {
            c.fraction += step;
            filter = FilterSaw(SawPoint(c.fraction), filter);
        }

        c.count = filter;
        return;
    }

    c.fraction += step * uint32_t(samples);
}

bool Sequencer::CamelotSynthPoints(Channel& c, uint32_t step, int32_t* points, int count) const
{
    const int type = rom_.S8(c.position + 1);
    if (type == 0)
    {
        // A pulse wave's duty follows a triangle wave, at the top of the count. The wave is high below the duty
        // (0x03000eec).
        c.count = int32_t(uint32_t(c.count) + (uint32_t(rom_.U8(c.position + 3)) << 24));
        uint32_t phase = uint32_t(c.count) + (uint32_t(rom_.U8(c.position + 5)) << 24);
        if (int32_t(phase) < 0)
        {
            phase = ~phase;
        }
        const uint32_t duty = (phase >> 8) * rom_.U8(c.position + 4) + (uint32_t(rom_.U8(c.position + 2)) << 24);
        for (int i = 0; i < count; i++)
        {
            points[i] = c.fraction < duty ? 64 : -64;
            c.fraction += step << 3;
        }
        return false;
    }
    if (type == 1)
    {
        // A saw wave goes through its filter.
        int32_t filter = c.count;
        for (int i = 0; i < count; i++)
        {
            c.fraction += step << 3;
            filter = FilterSaw(SawPoint(c.fraction), filter);
            points[i] = filter;
        }
        c.count = filter;
        return true;
    }

    // A triangle wave (0x03001044).
    for (int i = 0; i < count; i++)
    {
        c.fraction += step << 3;
        const int32_t phase = int32_t(c.fraction);
        points[i] = phase >= 0 ? (phase >> 23) - 128 : 384 - int32_t(c.fraction >> 23);
    }
    return false;
}

bool Sequencer::PlaysCamelotSteps(const Channel& c) const
{
    return info_.camelot_adpcm && (c.type & kVoiceFixed) && (c.count < 0 || (c.type & kVoiceCompressed));
}

const std::vector<float>& Sequencer::CamelotStepPoints(const Wave& wave)
{
    std::shared_ptr<const std::vector<float>>& known = camelot_step_points_[wave.address];
    if (known)
    {
        return *known;
    }

    const std::vector<int32_t> points = DecodeCamelotSteps(rom_, wave, uint32_t(info_.samples_per_frame));
    known = std::make_shared<const std::vector<float>>(points.begin(), points.end());
    return *known;
}

void Sequencer::MixCamelot(Channel& c, const Wave& wave, uint32_t volumes, int meter)
{
    // The addresses below are those of the mixer's code in Golden Sun: The Lost Age, in IWRAM. The mixer counts the
    // channels that it mixes in a frame (0x0300088c). Each point adds the volumes times the point to the point's sums,
    // in a word that wraps round at 32 bits. A negative right side borrows from the left.
    camelot_channels_++;
    const int points = info_.samples_per_frame;
    uint32_t* sums = camelot_sums_.data();
    int16_t* meter_right = meter >= 0 ? &sums_[size_t(2 * meter * points)] : nullptr;
    int16_t* meter_left = meter_right ? meter_right + points : nullptr;
    const auto add = [&](int i, uint32_t volume, int32_t value)
    {
        sums[i] += volume * uint32_t(value);
        if (meter_right)
        {
            meter_right[i] = int16_t(meter_right[i] + int32_t((int64_t(volume & 0xFFFF) * value) >> 7));
            meter_left[i] = int16_t(meter_left[i] + int32_t((int64_t(volume >> 16) * value) >> 7));
        }
    };

    // A looped sample goes back to its loop's start at its end. A sample without a loop stops the channel there, and
    // so does a loop of no length (0x0300085a).
    const uint32_t loop_start = wave.Data() + wave.loop_start;
    const int32_t loop_length = (c.status & kStatusLoop) ? int32_t(wave.size - wave.loop_start) : 0;

    // A fixed-pitch sample plays a point for each point of the output, at the channel's volumes (0x03000c1c).
    if (c.type & kVoiceFixed)
    {
        // A mixer with 4-bit steps plays a sample of a negative size in steps, a byte for each two points. Such a
        // sample has no loop. The channel's fraction keeps the steps' state between frames: the index in its low bits
        // and the point above them. The voice's type marks the channel (0x03000c1c to 0x03000da0 in Mario Tennis: Power
        // Tour). A negative count takes the mark and the first state.
        if (PlaysCamelotSteps(c))
        {
            uint32_t state = c.fraction;
            if (c.count < 0)
            {
                c.type |= kVoiceCompressed;
                c.count = -c.count;
                state = kCamelotStepsStart;
            }

            int32_t point = int32_t(state) >> 16;
            uint32_t index = state & 0xFF;
            const int mixed = std::min(c.count, points);
            c.count -= mixed;
            int i = 0;
            do
            {
                const uint32_t byte = uint32_t(rom_.S8(c.position++));
                CamelotStep(byte << 24 & 0xF0000000u, point, index);
                add(i, volumes, point);
                CamelotStep(byte << 28, point, index);
                add(i + 1, volumes, point);
                i += 2;
            } while (i < mixed);

            c.fraction = index | uint32_t(point) << 16;
            if (c.count == 0)
            {
                c.status = 0;
            }
            return;
        }

        for (int i = 0; i < points; i++)
        {
            add(i, volumes, rom_.S8(c.position));
            c.position++;
            if (--c.count == 0)
            {
                if (loop_length == 0)
                {
                    c.status = 0;
                    return;
                }
                c.count = loop_length;
                c.position = loop_start;
            }
        }
        return;
    }

    // A sample with no points left becomes a synth voice (0x030008a4). Its data follows at its position: 0x80, its
    // type, and a pulse wave's duty, speed, depth and offset. The phase moves on by 8 times a sample's step.
    const uint32_t step = c.frequency * info_.step_scale;
    if (c.synth || c.count == 0)
    {
        // A saw wave plays at half the volumes, rounded down on each side (0x03000fc0).
        c.synth = true;
        std::array<int32_t, kMostFramePoints> synth_points;
        const bool saw = CamelotSynthPoints(c, step, synth_points.data(), points);
        const uint32_t volume = saw ? (volumes >> 1) & ~0xFF00u : volumes;
        for (int i = 0; i < points; i++)
        {
            add(i, volume, synth_points[size_t(i)]);
        }
        return;
    }

    // Other samples play at each side's volume halved, rounded up (0x030008d0). Each point lies between two of the
    // sample's points, at the fraction in 2^-23 points.
    uint32_t volume = ((volumes >> 1) + 0x8000 + (volumes & 1)) & ~0xFF00u;
    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t next = c.position + 1; // the address of the next point
    int32_t point = rom_.S8(c.position);
    int32_t difference = rom_.S8(next) - point;

    // When the sample has more points left than the frame takes, the mixer runs a loop of 8 points with the code of
    // its step: below 1, from 1 to 2, or from 2 up (0x03000940). The code below 1 keeps the fraction less 1 and
    // interpolates to 9 bits. The others interpolate to 8 bits at twice the volumes. The first point of each 8
    // multiplies the fraction by the difference. Each later one does so unless the step has just gone on to a point
    // whose difference is 0. The point then takes the last point's result, shifted right by 22 or 23 bits: 0, or -1
    // when it was negative.
    const uint64_t frame_steps = fraction + uint64_t(step) * uint64_t(points);
    const uint32_t advance = uint32_t(frame_steps >> 23);
    if (count > int32_t(advance))
    {
        enum class Code
        {
            kBelowOne,
            kOneToTwo,
            kTwoUp,
        };
        Code code = Code::kBelowOne;
        uint32_t increment = step - 0x800000;
        if (int32_t(increment) >= 0)
        {
            volume <<= 1;
            code = int32_t(step - 0x1000000) < 0 ? Code::kOneToTwo : Code::kTwoUp;
            increment = code == Code::kOneToTwo ? step - 0x1000000 : step - 0x800000;
        }

        uint32_t product = 0;
        bool multiply = true;
        for (int i = 0; i < points; i++)
        {
            if (i % 8 == 0 || multiply)
            {
                product = fraction * uint32_t(difference);
            }
            const int32_t value =
                code == Code::kBelowOne ? (int32_t(product) >> 22) + 2 * point : point + (int32_t(product) >> 23);
            product = uint32_t(value);
            add(i, volume, value);

            fraction += increment;
            if (code == Code::kBelowOne)
            {
                multiply = true;
                if (int32_t(fraction) >= 0)
                {
                    point += difference;
                    next++;
                    difference = rom_.S8(next) - point;
                    multiply = difference != 0;
                }
            }
            else
            {
                if (code == Code::kOneToTwo)
                {
                    point += difference;
                    if (int32_t(fraction) >= 0)
                    {
                        next++;
                        point = rom_.S8(next);
                    }
                }
                else
                {
                    next += fraction >> 23;
                    point = rom_.S8(next);
                }
                next++;
                difference = rom_.S8(next) - point;
                multiply = difference != 0;
            }
            fraction &= 0x7FFFFF;
        }

        c.fraction = fraction;
        c.count = count - int32_t(advance);
        c.position += advance;
        return;
    }

    // Otherwise each point goes on by the step, and the sample's end loops or stops the channel (0x03000b44). A loop
    // goes back to its start, past it by the points that the step overshot.
    for (int i = 0; i < points; i++)
    {
        add(i, volume, (int32_t(fraction * uint32_t(difference)) >> 22) + 2 * point);
        fraction += step;
        const uint32_t steps = fraction >> 23;
        if (steps == 0)
        {
            continue;
        }

        count -= int32_t(steps);
        if (count <= 0)
        {
            if (loop_length == 0)
            {
                c.status = 0;
                return;
            }
            const int32_t overshoot = -count;
            count += loop_length;
            next = loop_start + uint32_t(overshoot);
            point = rom_.S8(next);
        }
        else if (steps == 1)
        {
            point += difference;
        }
        else
        {
            next += steps - 1;
            point = rom_.S8(next);
        }
        next++;
        difference = rom_.S8(next) - point;
        fraction &= ~0x3F800000u;
    }

    c.fraction = fraction;
    c.count = count;
    c.position = next - 1;
}

void Sequencer::OutputCamelot()
{
    // The output stage (0x03000d40) writes two points for each sum on each side, or one in Mario Tennis: Power Tour
    // (0x03000dec). The two sides' sums share a word, 16 bits each, and the output stage works on both halves at once.
    // A sum that doesn't fit in 15 bits saturates (0x03000e90).
    constexpr uint32_t kSigns = 0x80008000;
    constexpr uint32_t kRound = kSigns >> 9;
    constexpr uint32_t kBytes = 0xFF00FF00;
    const auto rotate = [](uint32_t value, int bits)
    {
        return value >> bits | value << (32 - bits);
    };
    const auto saturate = [](uint32_t sum)
    {
        const uint32_t swapped = sum >> 16 | sum << 16;
        if ((swapped ^ swapped << 1) & 0x80000000)
        {
            const bool high = sum & 0x4000;
            sum = (sum | 0x4000) & ~0x3FC0u;
            if (high)
            {
                sum -= 0x40;
            }
        }
        if ((sum ^ sum << 1) & 0x80000000)
        {
            const bool high = sum & 0x40000000;
            sum = (sum | 0x40000000) & ~0x3FC00000u;
            if (high)
            {
                sum -= 0x400000;
            }
        }
        return sum;
    };
    // The points of both halves in a word's bytes: the left side's first and second points, then the right side's. A
    // negative half takes 64 more first. The first point holds bits 7 to 14 of the half, and the second those of the
    // half less 64.
    const auto points = [&](uint32_t sum)
    {
        const uint32_t up = sum + ((sum & kSigns) >> 9);
        return (rotate(up - kRound, 15) & kBytes) | (rotate(up, 15) & kBytes) >> 8;
    };

    // The sums take 64 more before they turn into points. With 4 channels or more in the frame, only the first two do.
    // The stage writes the jump back of its loop to match (0x03000d4c). It also shifts the count up by 4 bits for the
    // next frame. Only the next frame's count reaches the low 4 bits that the stage reads.
    const bool many = (camelot_channels_ & 12) != 0;
    camelot_channels_ = 0;

    // The echo of the left side takes the right side's points from camelot_tap points before: the last frame's last
    // part, then the frame's first points.
    const uint32_t frame_points = uint32_t(info_.samples_per_frame);
    const uint32_t tap = uint32_t(info_.camelot_tap);
    const uint32_t scale = uint32_t(PointsPerMixed());
    const uint32_t size = scale * kPcmBufferSize;
    int8_t* right = pcm_.data();
    int8_t* left = right + size;
    uint32_t place = part_;
    uint32_t echo = camelot_last_part_ + scale * (frame_points - tap);
    camelot_last_part_ = part_;

    // Each side of the next frame's sums starts with a share of its old points, and a share of the other side's. The
    // constants spread over both halves.
    const uint32_t echo_right = uint32_t(info_.camelot_cross_echo) << 16 | uint32_t(info_.camelot_echo);
    const uint32_t echo_left = (uint32_t(info_.camelot_echo) << 16) + uint32_t(info_.camelot_cross_echo);
    for (uint32_t i = 0; i < frame_points; i += 2)
    {
        if (i == tap)
        {
            echo = part_;
        }

        uint32_t first = camelot_sums_[i];
        uint32_t second = camelot_sums_[i + 1];
        if (i == 0 || !many)
        {
            first += kRound;
            second += kRound;
        }
        if (((first ^ first << 1) | (second ^ second << 1)) & kSigns)
        {
            first = saturate(first);
            second = saturate(second);
        }

        // With one point for each sum, a word holds the left side's point in its low byte and the right side's above
        // it. The sums start with shares of the buffer's old points, a quarter of the frame's right side and a quarter
        // of the right side's tap. A half takes 64 less, or 64 more when it's negative.
        if (scale == 1)
        {
            const int32_t first_word = int32_t(rotate(first, 16)) >> 7;
            const int32_t second_word = int32_t(rotate(second, 16)) >> 7;
            uint32_t first_echo = uint32_t(right[place]) * echo_right + uint32_t(left[place]) * echo_left;
            uint32_t second_echo = uint32_t(right[place + 1]) * echo_right + uint32_t(left[place + 1]) * echo_left;
            left[place] = int8_t(first_word);
            left[place + 1] = int8_t(second_word);
            const int32_t first_right = first_word >> 16;
            const int32_t second_right = second_word >> 16;
            right[place] = int8_t(first_right);
            right[place + 1] = int8_t(second_right);
            place += 2;

            first_echo += uint32_t(first_right) << 5;
            second_echo += uint32_t(second_right) << 5;
            first_echo += uint32_t(right[echo]) << 21;
            second_echo += uint32_t(right[echo + 1]) << 21;
            echo += 2;
            camelot_sums_[i] = first_echo - kRound + ((first_echo & kSigns) >> 8);
            camelot_sums_[i + 1] = second_echo - kRound + ((second_echo & kSigns) >> 8);
            continue;
        }

        const uint32_t first_points = points(first);
        const uint32_t second_points = points(second);

        // The shares come from the first points of the buffer's part: 3 frames before this one in Golden Sun: The Lost
        // Age.
        uint32_t first_echo = uint32_t(right[place]) * echo_right + uint32_t(left[place]) * echo_left;
        uint32_t second_echo = uint32_t(right[place + 2]) * echo_right + uint32_t(left[place + 2]) * echo_left;
        left[place] = int8_t(first_points);
        left[place + 1] = int8_t(first_points >> 8);
        left[place + 2] = int8_t(second_points);
        left[place + 3] = int8_t(second_points >> 8);
        right[place] = int8_t(first_points >> 16);
        right[place + 1] = int8_t(first_points >> 24);
        right[place + 2] = int8_t(second_points >> 16);
        right[place + 3] = int8_t(second_points >> 24);
        place += 4;

        // The right side takes a quarter of its second points from this frame, plus 1/128 of that. The left side takes
        // a quarter of the right side's first points from camelot_tap points before (0x03000e34).
        uint32_t quarter = ((second_points & 0xFFFF0000) | first_points >> 16) & kBytes;
        quarter += quarter >> 7;
        second_echo += uint32_t(int32_t(quarter) >> 19);
        first_echo += uint32_t(int32_t(rotate(quarter, 16)) >> 19);
        first_echo += uint32_t(int32_t(right[echo])) << 21;
        second_echo += uint32_t(int32_t(right[echo + 2])) << 21;
        echo += 4;
        camelot_sums_[i] = first_echo;
        camelot_sums_[i + 1] = second_echo;
    }
}

void Sequencer::StartEnhancedPart()
{
    // The part of the frame in the buffer, and in the enhanced mode's history: 0 to the frames that the buffer holds.
    const size_t samples = agb::kEnhancedFrameSamples;
    const size_t parts = dma_period_;
    const size_t part = part_ / (uint32_t(info_.samples_per_frame) * uint32_t(PointsPerMixed()));
    float* right = enhanced_out_.data();
    float* left = right + samples;

    // Camelot's sums start with the echo that the last frame worked out.
    if (info_.camelot_mixer)
    {
        std::copy(enhanced_echo_.begin(), enhanced_echo_.end(), enhanced_out_.begin());
        return;
    }
    if (!reverb_)
    {
        std::fill(enhanced_out_.begin(), enhanced_out_.end(), 0.0f);
        return;
    }

    // The reverb reads the parts that the console's reverb reads: the frame's part and the next, from the last pass.
    const size_t next = (part + 1) % parts;
    const float* history_right = enhanced_history_.data();
    const float* history_left = history_right + parts * samples;
    const float* here_right = history_right + part * samples;
    const float* here_left = history_left + part * samples;
    const float* next_right = history_right + next * samples;
    const float* next_left = history_left + next * samples;
    if (info_.mono)
    {
        const float scale = float(reverb_) / 256.0f;
        for (size_t i = 0; i < samples; i++)
        {
            right[i] = (here_right[i] + next_right[i]) * scale;
            left[i] = right[i];
        }
        return;
    }
    const float scale = float(reverb_) / 512.0f;
    for (size_t i = 0; i < samples; i++)
    {
        right[i] = ((here_right[i] + here_left[i]) + (next_right[i] + next_left[i])) * scale;
        left[i] = right[i];
    }
}

void Sequencer::MixEnhanced(int index, const Channel& c, const Wave& wave, std::array<float, 2> to, bool starting)
{
    EnhancedChannel& e = enhanced_channels_[size_t(index)];
    const std::array<float, 2> from = starting ? to : e.volumes;
    e.volumes = to;
    const bool sounds = enhanced_frames_ && (from[0] != 0.0f || from[1] != 0.0f || to[0] != 0.0f || to[1] != 0.0f);
    const size_t samples = agb::kEnhancedFrameSamples;
    const uint32_t points = uint32_t(info_.samples_per_frame);
    float* right = enhanced_out_.data();
    float* left = right + samples;
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    const auto add = [&](size_t i, float value)
    {
        right[i] += value * (from[0] + float(i) * slopes[0]);
        left[i] += value * (from[1] + float(i) * slopes[1]);
    };

    // A synth voice of Camelot's mixer: its points of the frame, after its last points of the last frame, and the
    // first points of the next frame at the same pitch. The saw wave plays at half the volumes.
    if (info_.camelot_mixer && !(c.type & kVoiceFixed) && (c.synth || c.count == 0))
    {
        constexpr size_t kAhead = 32;
        std::array<int32_t, size_t(kMostFramePoints) + kAhead> raw;
        Channel voice = c;
        const uint32_t step = c.frequency * info_.step_scale;
        const bool saw = CamelotSynthPoints(voice, step, raw.data(), int(points));
        CamelotSynthPoints(voice, step, raw.data() + points, int(kAhead));
        std::array<float, 32 + size_t(kMostFramePoints) + kAhead> input;
        if (starting)
        {
            e.history = {};
        }
        std::copy(e.history.begin(), e.history.end(), input.begin());
        for (size_t i = 0; i < points + kAhead; i++)
        {
            input[32 + i] = saw ? float(raw[i]) * 0.5f : float(raw[i]);
        }
        std::copy(input.begin() + points, input.begin() + points + 32, e.history.begin());
        if (!sounds)
        {
            return;
        }

        const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(points) / double(samples));
        const size_t width = size_t(kernel.HalfWidth());
        for (size_t i = 0; i < samples; i++)
        {
            const uint64_t position = (uint64_t(i) * points << 23) / samples;
            const size_t point = size_t(position >> 23);
            add(i, kernel.At(&input[32 + point + 1 - width], uint32_t(position & 0x7FFFFF) >> 7));
        }
        return;
    }

    // The sample as the console's mixer plays it: forwards or backwards, compressed or not, with or without a loop,
    // and its step for each of the console's points. A newer driver's mixer leaves a compressed voice with an
    // uncompressed sample out. In Camelot's mixer, as in its driver, a loop of no length plays as no loop.
    const bool special = !info_.camelot_mixer && info_.special_samples && (c.type & (kVoiceReverse | kVoiceCompressed));
    const bool reverse = special && (c.type & kVoiceReverse);
    if (special && !reverse && wave.type == 0)
    {
        return;
    }
    const bool compressed = special && wave.type != 0;

    // A sample of 4-bit steps in Camelot's mixer plays its decoded points, and has no loop.
    const std::vector<float>* steps =
        PlaysCamelotSteps(c) && int32_t(wave.size) < 0 ? &CamelotStepPoints(wave) : nullptr;
    const uint32_t wave_size = steps ? uint32_t(steps->size()) : wave.size;
    const uint32_t loop_length = wave.size - wave.loop_start;
    const bool loops = !reverse && !steps && (c.status & kStatusLoop) && loop_length > 0;
    const uint64_t step = (c.type & kVoiceFixed) ? 0x800000u : uint64_t(uint32_t(c.frequency * info_.step_scale));
    const uint64_t move = step * points;
    const uint64_t start = e.position;
    e.position += move;
    if (!sounds)
    {
        return;
    }

    // The sample's points that the frame's positions reach, with the interpolator's span on each side.
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * 8388608.0));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(start >> 23) - (width - 1);
    const int64_t last = int64_t((start + move) >> 23) + width;
    std::vector<float> input(size_t(last - first + 1));
    DecodedBlock block;
    for (int64_t n = first; n <= last; n++)
    {
        float value = 0.0f;
        if (n >= 0)
        {
            uint64_t at = uint64_t(n);
            if (at >= wave.size && loops)
            {
                at = wave.loop_start + (at - wave.loop_start) % loop_length;
            }
            if (at < wave_size)
            {
                const uint32_t point = reverse ? wave.size - 1 - uint32_t(at) : uint32_t(at);
                if (steps)
                {
                    value = (*steps)[point];
                }
                else
                {
                    value =
                        float(compressed ? CompressedPoint(rom_, c.wave, point, block) : rom_.S8(wave.Data() + point));
                }
            }
        }
        input[size_t(n - first)] = value;
    }

    for (size_t i = 0; i < samples; i++)
    {
        const uint64_t position = start + uint64_t(i) * move / samples;
        const int64_t point = int64_t(position >> 23);
        add(i, kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position & 0x7FFFFF) >> 7));
    }
}

void Sequencer::EndEnhancedPart()
{
    // A frame without the enhanced mode's mixing is silent. The reverb and the echo take it as silence.
    const size_t samples = agb::kEnhancedFrameSamples;
    const size_t parts = dma_period_;
    const size_t part = part_ / (uint32_t(info_.samples_per_frame) * uint32_t(PointsPerMixed()));
    if (!enhanced_frames_)
    {
        std::fill(enhanced_out_.begin(), enhanced_out_.end(), 0.0f);
    }
    const float* right = enhanced_out_.data();
    const float* left = right + samples;
    float* history_right = enhanced_history_.data();
    float* history_left = history_right + parts * samples;
    const auto keep = [](float value)
    {
        return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
    };

    // Camelot's echo for the next frame: from the part of the buffer's frames before this one, before this frame takes
    // its place, from this frame's right side, and for the left side from the right side camelot_tap points before.
    if (info_.camelot_mixer)
    {
        const size_t tap = samples * size_t(info_.camelot_tap) / size_t(info_.samples_per_frame);
        const float level = float(info_.camelot_echo);
        const float cross = float(info_.camelot_cross_echo);
        const float share = info_.camelot_output_points == 1 ? 1.0f / 4.0f : 1.0f / 4.0f + 1.0f / 512.0f;
        const float* last_right = history_right + (part + parts - 1) % parts * samples;
        float* echo_right = enhanced_echo_.data();
        float* echo_left = echo_right + samples;
        for (size_t i = 0; i < samples; i++)
        {
            const float old_right = history_right[part * samples + i];
            const float old_left = history_left[part * samples + i];
            const float delayed = i >= tap ? right[i - tap] : last_right[samples - tap + i];
            echo_right[i] = keep((level * old_right + cross * old_left) / 128.0f + right[i] * share);
            echo_left[i] = keep((level * old_left + cross * old_right) / 128.0f + delayed * (1.0f / 4.0f));
        }
    }

    for (size_t i = 0; i < samples; i++)
    {
        history_right[part * samples + i] = keep(right[i]);
        history_left[part * samples + i] = keep(info_.mono ? right[i] : left[i]);
    }
}

void Sequencer::ReportTrack(int t)
{
    Track& track = tracks_[size_t(t)];
    Action a;
    a.track = uint8_t(t);
    const uint8_t again = track.sets & track.restate;
    track.sets = 0;

    // The levels on each side. The driver computes them from the track's settings at the end of the frame, or when the
    // track starts a note.
    // A drum kit's notes in the Metroid games keep the pans that they started with. The levels follow the newest.
    const bool drums = info_.metroid_sequencer && (track.tone.type & kVoiceDrumKit) && track.chan >= 0;
    const SideLevels levels = drums ? MetroidLevels(track, channels_[size_t(track.chan)].start_pan)
                                    : TrackLevels(track, info_.metroid_sequencer);
    if (levels.left != track.last_left || levels.right != track.last_right || (again & (kSetVolume | kSetPan)))
    {
        track.restate &= uint8_t(~(kSetVolume | kSetPan));
        track.last_left = levels.left;
        track.last_right = levels.right;
        a.kind = Action::kLevels;
        a.left = levels.left;
        a.right = levels.right;
        AddAction(a);
    }

    const int pitch = TrackPitch(track, info_.metroid_sequencer);
    if (pitch != track.last_pitch || (again & kSetPitch))
    {
        track.restate &= uint8_t(~kSetPitch);
        track.last_pitch = pitch;
        a.kind = Action::kPitch;
        a.value = pitch;
        AddAction(a);
    }
}

int32_t Sequencer::TrackPitch(const Track& track, bool metroid)
{
    // The Metroid games' sequencer adds 1 to an upward bend. The highest bend then reaches the bend range. It moves the
    // tune in 16ths of a semitone, and the LFO's swing in steps of 48, while the LFO has a speed and a depth.
    if (metroid)
    {
        const int32_t bend = track.bend > 0 ? track.bend + 1 : track.bend;
        int32_t pitch = bend * track.bend_range * 4 + track.key_shift * 256 + track.tune * 16;
        if (track.mod_t == 0 && track.lfo_speed && track.mod)
        {
            pitch += 48 * track.mod_m;
        }
        return pitch;
    }

    int32_t pitch = (track.tune + track.bend * track.bend_range) * 4 + track.key_shift * 256;
    if (track.mod_t == 0)
    {
        pitch += 16 * track.mod_m;
    }

    return pitch;
}

void Sequencer::AddAction(Action action)
{
    action.tick = clock_;
    action.frame = frame_;
    actions_.push_back(action);
}

void Sequencer::ReleaseNote(int t, int channel)
{
    channels_[size_t(channel)].status |= kStatusStop;

    Action a;
    a.kind = Action::kRelease;
    a.track = uint8_t(t);
    a.channel = channel;
    AddAction(a);
}

void Sequencer::EndNote(int channel, bool sounded, bool taken)
{
    Action a;
    a.kind = Action::kNoteEnd;
    a.channel = channel;
    a.sounded = sounded;
    a.taken = taken;
    AddAction(a);
}

void Sequencer::CheckAudible(int channel)
{
    Channel& c = channels_[size_t(channel)];
    if (!c.audible && c.envelope > c.lag)
    {
        c.audible = true;
        Action a;
        a.kind = Action::kAudible;
        a.channel = channel;
        AddAction(a);
    }
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

Hang::Hang(int track, uint32_t address)
    : std::runtime_error("track " + std::to_string(track) + " runs " + std::to_string(kCommandBudget) +
                         " commands in a tick without waiting, up to address " + xsflib::Hex(address, 0, true) +
                         ": the song hangs"),
      track_(track),
      address_(address)
{
}

int Hang::TrackIndex() const
{
    return track_;
}

uint32_t Hang::Address() const
{
    return address_;
}

} // namespace ntdlib::mp2k
