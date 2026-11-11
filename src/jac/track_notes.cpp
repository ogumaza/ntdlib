// SPDX-License-Identifier: MIT

// The tracks' notes and oscillators: the notes' start and stop, the gates, the oscillators' set-up and the jumps
// back, and the parser's notes and their ends (see track.cpp).

#include "ntdlib/jac/track.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "jac/track_internal.h"

namespace ntdlib::jac
{

namespace
{

// jamosc's tables of points, from main's data: the release of the default envelope (REL_TABLE, 0x6afdc8), a vibrato
// (VIB_TABLE, 0x6afdd4), a tremolo (TRE_TABLE, 0x6afdf8), and a simple ADSR (ADS_TABLE, 0x5bf3de). Each point is a
// mode, a time and a value.
constexpr std::array<OscPoint, 2> kRelTable = {{{0, 10, 0}, {kPointStop, 1, 0}}};
constexpr std::array<OscPoint, 6> kVibTable = {
    {{0, 0, 0}, {0, 20, 0x7fff}, {0, 20, 0}, {0, 20, -0x4000}, {0, 20, 0}, {kPointJump, 0, 1}}};
constexpr std::array<OscPoint, 6> kTreTable = {
    {{0, 0, 0x7fff}, {0, 20, 0}, {0, 20, -0x7fff}, {0, 20, 0}, {0, 20, 0x7fff}, {kPointJump, 0, 1}}};
constexpr std::array<OscPoint, 4> kAdsTable = {{{0, 0, 0x7fff}, {0, 0, 0x7fff}, {0, 0, 0}, {kPointHold, 0, 0}}};

template <std::size_t N>
constexpr OscTable TableOf(const std::array<OscPoint, N>& points)
{
    return {points.data(), N};
}

// jamosc's settings (main's data): VIBRATO_DEF (0x6afd68), TREMOLO_DEF (0x6afd88), ENVELOPE_DEF (0x6afda8) and
// ADSR_DEF. Each holds the target, the rate, the two tables, the width and the vertex.
constexpr OscData kVibratoDef{1, 0.8f, TableOf(kVibTable), TableOf(kVibTable), 0.0f, 1.0f};
constexpr OscData kTremoloDef{0, 1.0f, TableOf(kTreTable), TableOf(kTreTable), 0.0f, 1.0f};
constexpr OscData kEnvelopeDef{0, 1.0f, {}, TableOf(kRelTable), 1.0f, 0.0f};
constexpr OscData kAdsrDef{0, 1.0f, {}, {}, 1.0f, 0.0f};

} // namespace

// noteon.

int32_t Tracks::NoteOn(uint32_t index, uint8_t slot, uint8_t key, uint8_t velocity, int32_t time)
{
    // NoteON: a channel from the track's manager, or from the nearest manager up the tree that has a free one, which
    // moves to the track's manager first. A track of kind 4 plays on its parent's manager.
    Track& track = tracks_[index];
    if (track.muted && (track.pause_status & Track::kMuteNoNotes) != 0)
    {
        return -1;
    }

    if (track.channels[slot] != Track::kNone)
    {
        NoteOff(index, slot, 0);
    }

    const uint32_t parent = track.parent;
    uint32_t manager = index;
    uint32_t up = parent;
    while (channels_.Manager(manager).count == 0 || channels_.Manager(manager).lists[ChannelManager::kFree] == kNone)
    {
        if (up == Track::kNone)
        {
            manager = index;
            break;
        }

        manager = up;
        up = tracks_[up].parent;
    }

    // The manager that the note plays on: the track's, or its parent's for a track of kind 4. A channel from another
    // manager goes to the track's free list either way, and counts in the manager that the note plays on.
    uint32_t target = index;
    if (track.flags == 4)
    {
        if (parent == Track::kNone)
        {
            return -1;
        }

        target = parent;
    }

    if (manager != target)
    {
        const uint32_t channel = channels_.ListGet(manager, ChannelManager::kFree);
        if (channel != kNone)
        {
            channels_.Manager(manager).count--;
            channels_.ListAdd(index, ChannelManager::kFree, channel);
            channels_.Manager(target).count++;
            channels_.At(channel).manager = target;
        }
        manager = target;
    }

    const uint16_t bank_program = ReadRegDirect(index, kRegBank);
    const uint32_t bank = banks_.PhysicalBank(bank_program >> 8) & 0xffu;
    const uint32_t program = bank_program & 0xffu;
    const uint32_t channel = channels_.Play(manager, bank, program, key, velocity, time);
    track.channels[slot] = channel;
    if (channel == kNone)
    {
        if (!track.unheard && program < 0xf0 && channels_.CheckInstWave(bank, program, key, velocity) != 0)
        {
            missing_notes_++;
        }
        return -1;
    }

    track.channel_ids[slot] = channels_.At(channel).id;
    channels_.At(channel).unheard = track.unheard;
    channels_.SetPanPower(channel, static_cast<float>(track.registers[kRegArguments + 0]),
                          static_cast<float>(track.registers[kRegArguments + 1]),
                          static_cast<float>(track.registers[kRegArguments + 2]));

    // The track's oscillators on the channel's: a route below 4 names the channel's oscillator. From 4, the track
    // first takes that oscillator's settings and keeps its release. From 8, it takes them all.
    for (std::size_t i = 0; i < 2; i++)
    {
        uint32_t route = track.osc_routes[i];
        if (route == 15 || route == 14)
        {
            continue;
        }

        if (route >= 8)
        {
            route -= 8;
            if (route >= 4)
            {
                unmodelled_++;
            }
            else if (const OscData* data = channels_.At(channel).oscillators[route]; data != nullptr)
            {
                track.oscillators[i] = *data;
            }
        }
        else if (route >= 4)
        {
            route -= 4;
            const OscTable release = track.oscillators[i].release;
            if (const OscData* data = channels_.At(channel).oscillators[route]; data != nullptr)
            {
                track.oscillators[i] = *data;
                track.oscillators[i].release = release;
            }
        }
        if (route < 4)
        {
            channels_.OverwriteOscillator(channel, &track.oscillators[i], route);
        }
    }

    UpdateTrack(index, kUpdateVolume | kUpdatePitch);
    channels_.ResetInitialVolume(channel);

    return 0;
}

bool Tracks::NoteOff(uint32_t index, uint8_t slot, uint16_t release)
{
    // NoteOFF_R: a channel that still plays the track's note stops, or releases over `release` frames.
    Track& track = tracks_[index];
    const uint32_t channel = track.channels[slot];
    if (channel == Track::kNone)
    {
        return false;
    }

    if (channels_.At(channel).id == track.channel_ids[slot])
    {
        if (release == 0)
        {
            channels_.Stop(channel);
        }
        else
        {
            channels_.StopRelease(channel, release);
        }
    }
    track.channels[slot] = Track::kNone;
    track.channel_ids[slot] = 0;

    return true;
}

int32_t Tracks::GateOn(uint32_t index, uint8_t slot, uint8_t key, uint8_t velocity, int32_t time)
{
    // GateON: a tied note goes on in the slot's channel, whichever note that channel now plays.
    const uint32_t channel = tracks_[index].channels[slot];
    if (channel == Track::kNone)
    {
        return -1;
    }

    channels_.Gate(channel, key, velocity, time);

    return 0;
}

bool Tracks::CheckNoteStop(uint32_t index, uint8_t slot)
{
    Track& track = tracks_[index];
    const uint32_t channel = track.channels[slot];
    if (channel == Track::kNone)
    {
        return true;
    }
    if (channels_.At(channel).id != track.channel_ids[slot])
    {
        track.channels[slot] = Track::kNone;
        track.channel_ids[slot] = 0;
        return true;
    }

    return channels_.At(channel).key == 0xff;
}

// jamosc.

OscTable Tracks::SequenceTable(const Track& track, uint32_t offset)
{
    const std::span<const OscPoint> points = track.sequence->Table(offset);
    return {points.data(), points.size()};
}

void Tracks::UpdateOscParam(Track& track, uint32_t param, float value)
{
    // Osc_Update_Param: timed parameters 6 to 11, the two oscillators' width, rate and vertex.
    if (param < 6 || param > 11)
    {
        return;
    }

    OscData& data = track.oscillators[(param - 6) / 3];
    switch ((param - 6) % 3)
    {
    case 0:
        data.width = value;
        break;
    case 1:
        data.rate = value;
        break;
    default:
        data.vertex = value;
        break;
    }
}

void Tracks::SetupSimple(Track& track, uint8_t kind)
{
    switch (kind)
    {
    case 0:
        track.oscillators[1] = kVibratoDef;
        break;
    case 1:
        track.oscillators[0] = kTremoloDef;
        break;
    case 2:
        track.oscillators[1] = kTremoloDef;
        break;
    default:
        break;
    }
}

void Tracks::SetupSimpleEnv(Track& track, uint8_t kind, uint32_t offset)
{
    switch (kind)
    {
    case 0:
        track.oscillators[0] = kEnvelopeDef;
        track.oscillators[0].attack = SequenceTable(track, offset);
        break;

    case 1:
        track.oscillators[0].release = SequenceTable(track, offset);
        break;

    default:
        break;
    }
}

void Tracks::SetupAdsr(Track& track, const std::array<int16_t, 5>& times)
{
    // Osc_Setup_ADSR: the attack's time, the decay's time, the time to the sustain and its level, and the release's
    // time, in the track's tables.
    track.oscillators[0] = kAdsrDef;
    track.adsr_attack = kAdsTable;
    track.adsr_release = kRelTable;
    track.oscillators[0].attack = TableOf(track.adsr_attack);
    track.oscillators[0].release = TableOf(track.adsr_release);
    track.adsr_attack[0].time = times[0];
    track.adsr_attack[1].time = times[1];
    track.adsr_attack[2].time = times[2];
    track.adsr_attack[2].value = times[3];
    track.adsr_release[0].time = times[4];
    for (std::size_t i = 0; i < track.adsr_attack.size(); i++)
    {
        track.attack_table[3 * i] = track.adsr_attack[i].mode;
        track.attack_table[3 * i + 1] = track.adsr_attack[i].time;
        track.attack_table[3 * i + 2] = track.adsr_attack[i].value;
    }
    for (std::size_t i = 0; i < track.adsr_release.size(); i++)
    {
        track.release_table[3 * i] = track.adsr_release[i].mode;
        track.release_table[3 * i + 1] = track.adsr_release[i].time;
        track.release_table[3 * i + 2] = track.adsr_release[i].value;
    }
}

void Tracks::SetupFull(Track& track, uint8_t flags, uint32_t attack, uint32_t release)
{
    // Osc_Setup_Full: bit 4 picks the oscillator. Bit 7 gives it the default envelope's settings and the target in
    // the low 4 bits, bit 6 the attack table at `attack` and bit 5 the release table at `release`. An offset of 0
    // names the sequence's start, as any other offset does.
    OscData& data = track.oscillators[(flags >> 4) & 1];
    if ((flags & 0x80) != 0)
    {
        data = kEnvelopeDef;
        data.target = flags & 0x0f;
        if ((flags & 0x0f) == 1)
        {
            data.vertex = 1.0f;
        }
    }
    if ((flags & 0x40) != 0)
    {
        data.attack = SequenceTable(track, attack);
    }
    if ((flags & 0x20) != 0)
    {
        data.release = SequenceTable(track, release);
    }
}

void Tracks::ClearOverwrite(Track& track)
{
    track.osc_routes = {15, 15};
}

void Tracks::InitEnv(Track& track)
{
    track.oscillators[0] = kEnvelopeDef;
    ClearOverwrite(track);
}

// The parser.

void Tracks::JumpBack(uint32_t index, uint32_t target)
{
    // The cursor has moved past the jump. A jump to the jump itself or before it goes back. The track's levels below
    // the jump are those of the calls and the counted loops that the jump stays inside.
    const Track& track = tracks_[index];
    if (!loop_hook_ || target >= track.cursor)
    {
        return;
    }

    LoopJump jump;
    jump.target = target;
    jump.depth = track.depth;
    std::copy_n(track.stack.begin(), track.depth, jump.stack.begin());
    std::copy_n(track.loop_counts.begin(), track.depth, jump.counts.begin());
    uint32_t& notes = jump_notes_[index][track.depth];
    jump.played = notes_[index] != notes;
    notes = notes_[index];
    looped_[index] = true;
    if (track.depth == 0)
    {
        quiet_[index] = !jump.played;
    }
    loop_hook_(index, jump);
}

bool Tracks::ParseNote(uint32_t index, uint8_t command)
{
    // A note: its key, then a flag byte (bit 7: the key is a register's number, bits 5 and 6: a tie and a sweep from
    // the last note, bits 3 and 4: the bytes of its length, bits 0 to 2: its slot), the velocity, and for slot 0 the
    // gate and the length.
    Track& track = tracks_[index];
    auto key = static_cast<uint8_t>(command + track.final_transpose);
    const uint8_t note_flags = ReadByte(track);
    if ((note_flags & 0x80) != 0)
    {
        key = static_cast<uint8_t>(ExchangeRegisterValue(index, key));
        key = static_cast<uint8_t>(key + track.final_transpose);
    }
    uint8_t sweep_target = 0;
    if (((note_flags >> 5) & 2) != 0)
    {
        sweep_target = key;
        key = track.last_note;
    }
    auto velocity = ReadByte(track);
    if (velocity >= 0x80)
    {
        velocity = static_cast<uint8_t>(ExchangeRegisterValue(index, velocity - 0x80));
    }

    uint8_t slot = note_flags & 7;
    uint8_t gate = 100;
    uint32_t length = 0xffffffff;
    if (slot == 0)
    {
        gate = ReadByte(track);
        if (gate >= 0x80)
        {
            gate = static_cast<uint8_t>(ExchangeRegisterValue(index, gate - 0x80));
        }
        length = 0;
        const uint32_t bytes = (note_flags >> 3) & 3;
        for (uint32_t i = 0; i < bytes; i++)
        {
            length = length << 8 | ReadByte(track);
        }
        if (bytes == 1 && length >= 0x80)
        {
            length = ExchangeRegisterValue(index, static_cast<uint8_t>(length - 0x80));
        }
    }
    else if (((note_flags >> 3) & 3) != 0)
    {
        slot = static_cast<uint8_t>(ExchangeRegisterValue(index, static_cast<uint8_t>(slot - 1)));
        if (slot >= 8)
        {
            return true;
        }
    }

    track.note_flags = (note_flags >> 5) & 3;
    int32_t time = static_cast<int32_t>(length);
    int32_t result = 0;
    if (track.gate != 0)
    {
        if ((track.note_flags & 1) != 0)
        {
            time = -1;
        }
        if (time != -1)
        {
            time = SeqTimeToDspTime(index, time, gate);
        }
        result = track.paused && (track.pause_status & Track::kPauseNoNotes) != 0
                     ? -1
                     : GateOn(index, slot, key, velocity, time);
    }
    else
    {
        if (time != -1)
        {
            time = SeqTimeToDspTime(index, time, gate);
        }
        if ((track.note_flags & 1) != 0)
        {
            time = -1;
        }
        result = track.paused && (track.pause_status & Track::kPauseNoNotes) != 0
                     ? -1
                     : NoteOn(index, slot, key, velocity, time);
    }
    bool played = false;
    if (result != -1)
    {
        track.notes[slot] = key;
        played = true;
        notes_[index]++;
    }

    track.note_played = played ? 1 : 0;
    track.gate_time = gate;
    track.last_velocity = velocity;
    track.note_time = length;
    track.gate = (track.note_flags & 1) != 0 ? 1 : 0;
    if ((track.note_flags & 2) != 0)
    {
        int32_t steps = time;
        if (steps == -1)
        {
            steps = SeqTimeToDspTime(index, static_cast<int32_t>(length), track.gate_time);
        }
        if (track.channels[0] != Track::kNone)
        {
            channels_.SetKeyTarget(track.channels[0], static_cast<uint8_t>(sweep_target + track.final_transpose),
                                   static_cast<uint32_t>(steps));
        }
        key = sweep_target;
    }
    track.last_note = key;

    if (length != 0xffffffff)
    {
        track.wait = static_cast<int32_t>(length);
        if (length == 0)
        {
            track.wait = -1;
        }
        return true;
    }

    return false;
}

bool Tracks::ParseNoteEnd(uint32_t index, uint8_t command)
{
    // A note's end: 0x81 to 0x87 a slot, 0x89 to 0x8f a slot with a release time, 0x80 and 0x88 a wait of a byte or a
    // word. 0xf9 names the slot by a register.
    Track& track = tracks_[index];
    uint32_t wait_bytes = 1;
    uint16_t release = 0;
    uint8_t code = command;
    if (command == 0xf9)
    {
        const uint8_t byte = ReadByte(track);
        const auto slot = static_cast<uint8_t>(ExchangeRegisterValue(index, byte & 7));
        if (slot > 7 || slot == 0)
        {
            if ((byte & 0x80) != 0)
            {
                ReadByte(track);
            }
            return false;
        }
        code = static_cast<uint8_t>(slot + 0x80 + ((byte & 0x80) != 0 ? 8 : 0));
    }
    uint8_t slot = code & 0x0f;
    if (slot == 8)
    {
        wait_bytes = 2;
        slot = 0;
    }
    if (slot > 8)
    {
        slot = static_cast<uint8_t>(slot - 8);
        release = ReadByte(track);
        if (release > 100)
        {
            release = static_cast<uint16_t>((release - 98) * 20);
        }
    }
    if (slot == 0)
    {
        uint32_t wait = 0;
        for (uint32_t i = 0; i < wait_bytes; i++)
        {
            wait = wait << 8 | ReadByte(track);
        }
        track.wait = static_cast<int32_t>(wait);
        if (wait == 0)
        {
            return false;
        }
        return true;
    }
    NoteOff(index, slot, release);
    return false;
}

} // namespace ntdlib::jac
