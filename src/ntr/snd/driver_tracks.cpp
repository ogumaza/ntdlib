// SPDX-License-Identifier: MIT

// The players and the tracks of the model of the DS's sound driver: the commands that start, stop, pause and change a
// player's sequence, the set-up of players and tracks and their fields in the driver's memory, a player's tempo, and a
// note's start on a channel (see driver.h).

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

#include "driver_shared.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr::snd
{

namespace
{

// A sequence's clock: each player adds its tempo to its counter each frame, and plays a tick for each 240.
constexpr uint16_t kTickRate = 240;

constexpr uint32_t kWaveParametersSize = 12;

constexpr uint32_t kTickCounter = 0x20;

// The layouts of a player (0x24 bytes) and a track (0x40 bytes) in the driver's memory, for the commands that write
// their fields by offset.
constexpr std::size_t kPlayerSize = 0x24;

constexpr std::size_t kTrackSize = 0x40;

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
    xsflib::StoreLe16(&bytes[at + 4], lfo.delay);
}

LfoParameters GetLfo(std::span<const uint8_t> bytes, std::size_t at)
{
    return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3], xsflib::LoadLe16(&bytes[at + 4])};
}

std::array<uint8_t, kPlayerSize> PlayerBytes(const Player& player)
{
    std::array<uint8_t, kPlayerSize> bytes = {};
    bytes[0] = static_cast<uint8_t>(player.active | player.prepared << 1 | player.paused << 2);
    bytes[1] = player.id;
    bytes[kPlayerPriorityOffset] = player.priority;
    bytes[5] = player.volume;
    xsflib::StoreLe16(&bytes[kPlayerFaderOffset], static_cast<uint16_t>(player.external_fader));
    for (std::size_t i = 0; i < player.tracks.size(); i++)
    {
        bytes[8 + i] = player.tracks[i];
    }
    xsflib::StoreLe16(&bytes[0x18], player.tempo);
    xsflib::StoreLe16(&bytes[kPlayerTempoRatioOffset], player.tempo_ratio);
    xsflib::StoreLe16(&bytes[0x1C], player.tempo_counter);
    xsflib::StoreLe32(&bytes[0x20], player.bank);

    return bytes;
}

void SetPlayerBytes(Player& player, std::span<const uint8_t> bytes)
{
    player.active = (bytes[0] & 1) != 0;
    player.prepared = (bytes[0] & 2) != 0;
    player.paused = (bytes[0] & 4) != 0;
    player.id = bytes[1];
    player.priority = bytes[kPlayerPriorityOffset];
    player.volume = bytes[5];
    player.external_fader = static_cast<int16_t>(xsflib::LoadLe16(&bytes[kPlayerFaderOffset]));
    for (std::size_t i = 0; i < player.tracks.size(); i++)
    {
        player.tracks[i] = bytes[8 + i];
    }
    player.tempo = xsflib::LoadLe16(&bytes[0x18]);
    player.tempo_ratio = xsflib::LoadLe16(&bytes[kPlayerTempoRatioOffset]);
    player.tempo_counter = xsflib::LoadLe16(&bytes[0x1C]);
    player.bank = xsflib::LoadLe32(&bytes[0x20]);
}

std::array<uint8_t, kTrackSize> TrackBytes(const Track& track)
{
    std::array<uint8_t, kTrackSize> bytes = {};
    bytes[0] = static_cast<uint8_t>(track.active | track.note_wait << 1 | track.muted << 2 | track.tie << 3 |
                                    track.note_finish_wait << 4 | track.portamento << 5 | track.compare << 6 |
                                    track.channel_mask_set << 7);
    bytes[1] = track.pan_range;
    xsflib::StoreLe16(&bytes[2], track.program);
    bytes[4] = track.volume;
    bytes[5] = track.expression;
    bytes[6] = static_cast<uint8_t>(track.pitch_bend);
    bytes[7] = track.bend_range;
    bytes[8] = static_cast<uint8_t>(track.pan);
    bytes[9] = static_cast<uint8_t>(track.external_pan);
    xsflib::StoreLe16(&bytes[kTrackFaderOffset], static_cast<uint16_t>(track.external_fader));
    xsflib::StoreLe16(&bytes[kTrackPitchOffset], static_cast<uint16_t>(track.external_pitch));
    bytes[0xE] = track.attack;
    bytes[0xF] = track.decay;
    bytes[0x10] = track.sustain;
    bytes[0x11] = track.release;
    bytes[0x12] = track.priority;
    bytes[0x13] = static_cast<uint8_t>(track.transpose);
    bytes[0x14] = track.portamento_key;
    bytes[0x15] = track.portamento_time;
    xsflib::StoreLe16(&bytes[0x16], static_cast<uint16_t>(track.sweep_pitch));
    PutLfo(bytes, 0x18, track.modulation);
    xsflib::StoreLe16(&bytes[0x1E], track.channel_mask);
    xsflib::StoreLe32(&bytes[0x20], static_cast<uint32_t>(track.wait));
    xsflib::StoreLe32(&bytes[0x24], track.base);
    xsflib::StoreLe32(&bytes[0x28], track.position);
    for (std::size_t i = 0; i < track.calls.size(); i++)
    {
        xsflib::StoreLe32(&bytes[0x2C + 4 * i], track.calls[i]);
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
    track.program = xsflib::LoadLe16(&bytes[2]);
    track.volume = bytes[4];
    track.expression = bytes[5];
    track.pitch_bend = static_cast<int8_t>(bytes[6]);
    track.bend_range = bytes[7];
    track.pan = static_cast<int8_t>(bytes[8]);
    track.external_pan = static_cast<int8_t>(bytes[9]);
    track.external_fader = static_cast<int16_t>(xsflib::LoadLe16(&bytes[kTrackFaderOffset]));
    track.external_pitch = static_cast<int16_t>(xsflib::LoadLe16(&bytes[kTrackPitchOffset]));
    track.attack = bytes[0xE];
    track.decay = bytes[0xF];
    track.sustain = bytes[0x10];
    track.release = bytes[0x11];
    track.priority = bytes[0x12];
    track.transpose = static_cast<int8_t>(bytes[0x13]);
    track.portamento_key = bytes[0x14];
    track.portamento_time = bytes[0x15];
    track.sweep_pitch = static_cast<int16_t>(xsflib::LoadLe16(&bytes[0x16]));
    track.modulation = GetLfo(bytes, 0x18);
    track.channel_mask = xsflib::LoadLe16(&bytes[0x1E]);
    track.wait = static_cast<int32_t>(xsflib::LoadLe32(&bytes[0x20]));
    track.base = xsflib::LoadLe32(&bytes[0x24]);
    track.position = xsflib::LoadLe32(&bytes[0x28]);
    for (std::size_t i = 0; i < track.calls.size(); i++)
    {
        track.calls[i] = xsflib::LoadLe32(&bytes[0x2C + 4 * i]);
        track.loop_counts[i] = bytes[0x38 + i];
    }
    track.call_depth = bytes[0x3B];
}

} // namespace

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

    const int increment = TempoStep(player.tempo, player.tempo_ratio);
    player.tempo_counter = static_cast<uint16_t>(player.tempo_counter + increment);
}

} // namespace ntdlib::ntr::snd
