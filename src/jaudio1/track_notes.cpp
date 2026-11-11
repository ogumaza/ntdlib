// SPDX-License-Identifier: MIT

// The tracks' oscillators and notes: the oscillators' set-up, the notes that the banks start, the gates and the
// jumps back, and the parser's commands of a note on and a note off (see track.cpp).

#include "ntdlib/jaudio1/track.h"

#include <cstddef>
#include <cstdint>

#include "jaudio1/track_internal.h"
#include "ntdlib/jaudio1/calc.h"

namespace ntdlib::jaudio1
{

void Tracks::OscSetupFull(Track& track, uint32_t route, uint32_t attack, uint32_t release)
{
    // JASTrack::oscSetupFull (0x3dd5f0). An offset of 0 names the sequence's start, as any other offset does.
    OscData& data = track.oscillators[(route >> 4) & 1];
    if ((route & 0x80) != 0)
    {
        data = kEnvelopeDef;
        data.target = route & 0xf;
        if ((route & 0xf) == 1)
        {
            data.vertex = 1.0f;
        }
    }
    if ((route & 0x40) != 0)
    {
        data.attack = SequenceTable(track, attack);
    }
    if ((route & 0x20) != 0)
    {
        data.release = SequenceTable(track, release);
    }
}

void Tracks::OscSetupSimpleEnv(Track& track, uint32_t kind, uint32_t offset)
{
    // JASTrack::oscSetupSimpleEnv (0x3dd6b0).
    switch (kind & 0xff)
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

void Tracks::OscSetupSimple(Track& track, uint32_t kind)
{
    // JASTrack::oscSetupSimple (0x3dd704).
    switch (kind & 0xff)
    {
    case 0:
        track.oscillators[1] = kVibratoDef;
        break;
    case 1:
        track.oscillators[0] = kTremoroDef;
        break;
    case 2:
        track.oscillators[1] = kTremoroDef;
        break;
    default:
        break;
    }
}

void Tracks::OverwriteOsc(Track& track, uint32_t channel)
{
    // JASTrack::overwriteOsc (0x3dd4c4): a route other than 0xf gives the channel's oscillator `route & 3` the
    // track's settings. With bit 3, the track first takes the oscillator's settings. With bit 2, it takes them but
    // keeps its release.
    Channel& ch = voices_.ChannelAt(channel);
    for (std::size_t i = 0; i < 2; i++)
    {
        const uint32_t route = track.osc_routes[i];
        if (route == 0xf)
        {
            continue;
        }

        Oscillator& oscillator = ch.oscillators[route & 3];
        OscData& data = track.oscillators[i];
        if ((route & 8) != 0)
        {
            if (oscillator.Data() != nullptr)
            {
                data = *oscillator.Data();
            }
        }
        else if ((route & 4) != 0)
        {
            const OscTable release = data.release;
            if (oscillator.Data() != nullptr)
            {
                data = *oscillator.Data();
            }
            data.release = release;
        }
        oscillator.InitStart(&data, voices_.DacRate());
    }
}

uint32_t Tracks::BankNoteOn(uint32_t bank, uint32_t program, uint32_t key, uint32_t velocity, uint16_t priority,
                            uint32_t owner)
{
    // JASBankMgr::noteOn (0x3d0c90).
    if (program >= 0xf0)
    {
        // noteOnOsc: a note on the DSP's oscillator, waveform `program - 0xf0`.
        const uint32_t channel = voices_.NewChannel(owner);
        if (channel == Channel::kNone)
        {
            return Channel::kNone;
        }

        voices_.SetPanPower(channel, 1.0f, 1.0f, 1.0f);
        Channel& ch = voices_.ChannelAt(channel);
        ch.priority = priority;
        ch.wave_address = program - 0xf0;
        ch.wave_format = Channel::kOsc;
        ch.active_pitch = 16736.0156f / voices_.DacRate();
        ch.modified_pitch = ch.active_pitch;
        ch.modified_pitch = Key2Pitch(static_cast<int32_t>(key & 0xff)) * ch.modified_pitch;
        ch.volume = 1.0f;
        const float v = static_cast<float>(velocity & 0xff) / 127.0f;
        ch.velocity_volume = v * v;
        ch.oscillators[0].InitStart(&kOscEnv, voices_.DacRate());

        return voices_.Play(this, channel) ? channel : Channel::kNone;
    }

    const InstrumentBank* const instruments = banks_[bank & 0xff];
    if (instruments == nullptr)
    {
        missing_bank_notes_++;
        return Channel::kNone;
    }
    if (!instruments->HasProgram(program))
    {
        return Channel::kNone;
    }

    InstParam param;
    if (!instruments->GetInstParam(program, key & 0xff, velocity & 0xff, param, inst_random_))
    {
        return Channel::kNone;
    }

    const WaveBank* const waves = instruments->Waves();
    if (waves == nullptr)
    {
        return Channel::kNone;
    }

    const BankWave* const wave = waves->Find(param.wave_id);
    if (wave == nullptr)
    {
        return Channel::kNone;
    }

    const uint32_t channel = voices_.NewChannel(owner);
    if (channel == Channel::kNone)
    {
        return Channel::kNone;
    }

    voices_.SetPanPower(channel, 1.0f, 1.0f, 1.0f);
    Channel& ch = voices_.ChannelAt(channel);
    ch.priority = priority;
    ch.has_wave = true;
    ch.wave = wave->info;
    ch.wave_address = wave->address;
    ch.wave_format = param.wave_format;
    ch.active_pitch = (wave->info.sample_rate / voices_.DacRate()) * param.pitch;
    ch.modified_pitch = ch.active_pitch;
    if (!param.percussion)
    {
        const int32_t semitones = static_cast<int32_t>(key & 0xff) - wave->info.base_key + 60;
        ch.modified_pitch = Key2Pitch(semitones) * ch.modified_pitch;
    }
    const float v = static_cast<float>(velocity & 0xff) / 127.0f;
    ch.volume = param.volume;
    ch.velocity_volume = (v * v) * param.volume;
    ch.pan_sound = param.pan;
    ch.fxmix_sound = param.fxmix;
    ch.dolby_sound = param.dolby;
    for (uint32_t i = 0; i < param.oscillator_count; i++)
    {
        ch.oscillators[i].InitStart(param.oscillators[i], voices_.DacRate());
    }
    ch.oscillators[0].SetDirectRelease(param.release);

    return voices_.Play(this, channel) ? channel : Channel::kNone;
}

int32_t Tracks::NoteOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity, int32_t time, uint32_t skip)
{
    // JASTrack::noteOn (0x3dd330).
    Track& track = tracks_[index];
    note &= 0xff;
    if (note >= 8)
    {
        unmodelled_++;
        return -1;
    }
    if ((track.muted && (track.pause_status & 0x40) != 0) || ((track.note_mask >> note) & 1) != 0)
    {
        return -1;
    }
    if (track.channels[note] != Track::kNone)
    {
        voices_.Release(track.channels[note], 0);
        track.channels[note] = Track::kNone;
    }

    const uint32_t bank_program = track.registers[kRegBankProgram];
    const uint32_t physical = virtual_banks_[bank_program >> 8];
    const uint32_t channel =
        BankNoteOn(physical, bank_program & 0xff, key, velocity, track.registers[kRegPriority], index);
    if (channel == Channel::kNone)
    {
        return -1;
    }

    notes_[index]++;
    Channel& ch = voices_.ChannelAt(channel);
    ch.update_timer = static_cast<uint32_t>(time);
    track.channel_list.push_back(channel);
    track.channels[note] = channel;
    ch.skip_samples = skip;
    voices_.SetPanPower(channel, static_cast<float>(track.registers[kRegPanPower]),
                        static_cast<float>(track.registers[kRegPanPower + 1]),
                        static_cast<float>(track.registers[kRegPanPower + 2]));
    OverwriteOsc(track, channel);
    if (track.direct_release != 0)
    {
        ch.oscillators[0].SetDirectRelease(track.direct_release);
    }

    return 0;
}

bool Tracks::NoteOff(uint32_t index, uint32_t note, uint16_t release)
{
    // JASTrack::noteOff (0x3dd48c).
    Track& track = tracks_[index];
    if (track.channels[note & 7] == Track::kNone)
    {
        return false;
    }

    voices_.Release(track.channels[note & 7], release);
    track.channels[note & 7] = Track::kNone;

    return true;
}

void Tracks::GateOn(uint32_t index, uint32_t note, uint32_t key, uint32_t velocity, int32_t time)
{
    // JASTrack::gateOn (0x3dd5a0) and JASBankMgr::gateOn (0x3d1040): the playing channel takes the new key and
    // velocity.
    Track& track = tracks_[index];
    note &= 0xff;
    if (note >= 8)
    {
        unmodelled_++;
        return;
    }
    const uint32_t channel = track.channels[note];
    if (channel == Track::kNone)
    {
        return;
    }

    Channel& ch = voices_.ChannelAt(channel);
    int32_t semitones = static_cast<int32_t>(key & 0xff);
    if (ch.wave_format != Channel::kOsc)
    {
        semitones = semitones - ch.wave.base_key + 60;
    }
    ch.modified_pitch = Key2Pitch(semitones) * ch.active_pitch;
    const float v = static_cast<float>(velocity & 0xff) / 127.0f;
    ch.velocity_volume = (v * v) * ch.volume;
    ch.update_timer = static_cast<uint32_t>(time);
}

void Tracks::JumpBack(uint32_t index, uint32_t target)
{
    // The cursor has moved past the jump. A jump to the jump itself or before it goes back.
    const Track& track = tracks_[index];
    if (!loop_hook_ || track.depth != 0 || target >= track.cursor)
    {
        return;
    }

    const bool played = notes_[index] != loop_notes_[index];
    loop_notes_[index] = notes_[index];
    looped_[index] = true;
    quiet_[index] = !played;
    loop_hook_(index, target, played);
}

int32_t Tracks::CmdNoteOn(uint32_t index, uint32_t key)
{
    // JASSeqParser::cmdNoteOn (0x3e0de4). A note id of 0 times the note by its gate and length, and the track waits
    // for it. Notes 1 to 7 last until a note-off.
    Track& track = tracks_[index];
    const uint32_t flags = ReadByte(track);
    if ((flags & 0x80) != 0)
    {
        key = ExchangeRegisterValue(index, key);
    }
    const uint32_t original = key + static_cast<uint32_t>(Transpose(track));
    uint32_t play_key = original;
    if ((flags & 0x40) != 0)
    {
        play_key = track.latest_key;
    }

    uint32_t velocity = ReadByte(track);
    uint32_t gate_flags = (flags >> 5) & 3;
    if ((velocity & 0x80) != 0)
    {
        velocity = ExchangeRegisterValue(index, velocity & 0x7f);
    }

    uint32_t note = flags & 7;
    const uint32_t count = (flags >> 3) & 3;
    uint32_t skip = 0;
    uint32_t percent = 100;
    int32_t time = -1;
    if (note != 0)
    {
        if (count != 0)
        {
            note = ExchangeRegisterValue(index, note - 1);
        }
        if ((gate_flags & 1) != 0)
        {
            skip = ExchangeRegisterValue(index, ReadByte(track));
            gate_flags ^= 1;
        }
    }
    else
    {
        percent = ReadByte(track);
        if ((percent & 0x80) != 0)
        {
            percent = ExchangeRegisterValue(index, percent & 0x7f);
        }
        uint32_t length = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            length = length << 8 | ReadByte(track);
        }
        if (count == 1 && (length & 0x80) != 0)
        {
            length = ExchangeRegisterValue(index, length & 0x7f);
        }
        time = static_cast<int32_t>(length);
    }

    track.note_flags = static_cast<uint8_t>(gate_flags);
    const int32_t length = time;
    int32_t dsp_time = -1;
    const bool blocked = track.paused && (track.pause_status & 0x10) != 0;
    if (track.gate != 0)
    {
        if (length != -1 && (gate_flags & 1) == 0)
        {
            dsp_time = SeqTimeToDspTime(track, length, percent);
        }
        if (!blocked)
        {
            GateOn(index, note, play_key & 0xff, velocity & 0xff, dsp_time);
        }
    }
    else
    {
        if (length != -1)
        {
            dsp_time = SeqTimeToDspTime(track, length, percent);
        }
        if ((track.note_flags & 1) != 0)
        {
            dsp_time = -1;
        }
        if (!blocked)
        {
            NoteOn(index, note, play_key & 0xff, velocity & 0xff, dsp_time, skip);
        }
    }

    const uint32_t note_flags = track.note_flags;
    track.note_time = length;
    track.gate = static_cast<uint8_t>(note_flags & 1);
    uint32_t latest = play_key;
    if ((note_flags & 2) != 0)
    {
        if (dsp_time == -1)
        {
            dsp_time = SeqTimeToDspTime(track, length, percent);
        }
        if (track.channels[0] != Track::kNone)
        {
            voices_.SetKeySweepTarget(track.channels[0], original & 0xff, static_cast<uint32_t>(dsp_time));
        }
        latest = original;
    }
    track.latest_key = static_cast<uint8_t>(latest);
    if (length == -1)
    {
        return 0;
    }

    track.wait = length != 0 ? length : -1;

    return 1;
}

void Tracks::CmdNoteOff(uint32_t index, uint32_t command)
{
    // cmdNoteOff: 0xf9 takes the note id from a register.
    Track& track = tracks_[index];
    uint32_t flags = command;
    if (command == 0xf9)
    {
        const uint32_t byte = ReadByte(track);
        flags = ExchangeRegisterValue(index, byte & 7) ^ 0xffffff80u;
        if ((byte & 0x80) != 0)
        {
            flags |= 8;
        }
    }
    uint32_t release = 0;
    if ((flags & 8) != 0)
    {
        release = ReadByte(track);
        if (release > 100)
        {
            release = (release - 98) * 20;
        }
    }
    NoteOff(index, flags & 7, static_cast<uint16_t>(release));
}

} // namespace ntdlib::jaudio1
