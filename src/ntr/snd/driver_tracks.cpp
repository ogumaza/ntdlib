// SPDX-License-Identifier: MIT

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

constexpr uint32_t kPlayerStatus = 0x4;

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
    bytes[4] = player.priority;
    bytes[5] = player.volume;
    xsflib::StoreLe16(&bytes[6], static_cast<uint16_t>(player.external_fader));
    for (std::size_t i = 0; i < player.tracks.size(); i++)
    {
        bytes[8 + i] = player.tracks[i];
    }
    xsflib::StoreLe16(&bytes[0x18], player.tempo);
    xsflib::StoreLe16(&bytes[0x1A], player.tempo_ratio);
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
    player.priority = bytes[4];
    player.volume = bytes[5];
    player.external_fader = static_cast<int16_t>(xsflib::LoadLe16(&bytes[6]));
    for (std::size_t i = 0; i < player.tracks.size(); i++)
    {
        player.tracks[i] = bytes[8 + i];
    }
    player.tempo = xsflib::LoadLe16(&bytes[0x18]);
    player.tempo_ratio = xsflib::LoadLe16(&bytes[0x1A]);
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
    xsflib::StoreLe16(&bytes[0xA], static_cast<uint16_t>(track.external_fader));
    xsflib::StoreLe16(&bytes[0xC], static_cast<uint16_t>(track.external_pitch));
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
    track.external_fader = static_cast<int16_t>(xsflib::LoadLe16(&bytes[0xA]));
    track.external_pitch = static_cast<int16_t>(xsflib::LoadLe16(&bytes[0xC]));
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
        MarkRead(player, int(played.command.value.value));
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
                    int32_t shifted = 0;
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

} // namespace ntdlib::ntr::snd
