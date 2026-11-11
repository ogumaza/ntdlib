// SPDX-License-Identifier: MIT

// The sequencer of Dungeon Dice Monsters' driver: its tracks' reset, its commands and its per-frame work.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/konami/sequencer.h"

#include "sequencer_internal.h"

namespace ntdlib::konami
{

DungeonDiceSequencer::DungeonDiceSequencer(const Rom& rom, const DriverInfo& info, int song)
    : TrackSequencer(rom, info, song),
      sample_map_(info.sample_map),
      sample_periods_(info.sample_period_table),
      vibrato_entry_(info.vibrato_entry)
{
    if (!rom.Contains(header_.base))
    {
        return;
    }

    // Start all tracks active and restartable on a song loop. That's the state a loop resets them to.
    StartTracks();
    ResetTracks();

    valid_ = true;
}

void DungeonDiceSequencer::ResetTracks()
{
    // Tracks that can restart run again from their first frame and without vibrato or a fade. A loop keeps each track's
    // note and its loop point.
    for (Track& t : tracks_)
    {
        t.flags = RestartFlags(t.flags) & (kTrackOn | kTrackRestarts);
        t.pos = 0;
        t.ret = 0;
        t.pitch = 0;
        t.b2 = 0;
        t.vol = 0;
        t.vib_depth = 0;
        t.vib_count = 0;
    }

    // The square channels restart at 50% duty, the PSG at full volume on both sides, and the wave RAM with wave 0.
    tracks_[0].b2 = 0x80;
    tracks_[1].b2 = 0x80;
    psg_volume_ = 0x77;
    loaded_wave_ = 0;
    wave_loads_++;
}

Sequencer::Result DungeonDiceSequencer::StepTrack(int track)
{
    if (track >= header_.tracks)
    {
        return Result::kContinue;
    }

    if (!(tracks_[track].flags & kTrackOn))
    {
        PerFrame(track);
        return Result::kContinue;
    }

    out_[track].active = true;

    return RunTrack(track, Revision::kDungeonDiceMonsters);
}

void DungeonDiceSequencer::FirstFrame(Track& t, TrackOutput& o)
{
    Play(t, o, 0);
}

void DungeonDiceSequencer::CountInCall(Track& t, uint32_t& pos)
{
    CountCallCommand(t, pos, true);
}

DungeonDiceSequencer::Next DungeonDiceSequencer::RunCommand(int track, const Command& c, uint32_t& pos)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    // Returns the sample that sample map entry `entry` names.
    auto sample_of = [&](int entry)
    {
        const uint32_t a = sample_map_ + uint32_t(entry);
        return sample_map_ && rom_.Contains(a) ? rom_.U8(a) : uint8_t(entry);
    };

    switch (c.op)
    {
    case Op::kJump:
        if (c.value != 0)
        {
            return Next::kRestart;
        }

        // The driver silences every track on the next frame, and the model ends the song here. The track ends first.
        stopped_ = true;
        [[fallthrough]];

    case Op::kEndTrack:
        t.flags &= kTrackRestarts;
        if (!o.Any())
        {
            t.vol = 0;
            o.b2 = 0;
            o.vol = 0;
            o.pitch = t.pitch;
            o.flags = kOutDungeonDiceNote;
            o.trig = 1;
        }

        return Next::kStop;

    case Op::kRest:
        Play(t, o, 0);
        break;

    case Op::kRelease:
        // A release is a rest during vibrato. Otherwise it fades the track out, and gives a square channel an envelope
        // that takes it down a step every 1/64 second.
        if (t.flags & kTrackVibrato)
        {
            Play(t, o, 0);
            break;
        }

        t.flags |= kTrackFade;
        t.fade = 4;
        t.fade_from = t.vol;
        o.key = 0x100;
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kFade:
        {
            Track* u = c.opcode & 1 ? NextTrack(track, c) : &t;
            if (u)
            {
                u->flags |= kTrackFade;
                u->fade = 4;
                u->fade_from = u->vol;
            }
            break;
        }

    case Op::kLoopPoint:
        // The loop starts with FA's two bytes, a delay and a command, which the track skips until the song loops.
        t.start = uint16_t(t.start + pos - 2);
        pos = 2;
        PassLoopPoint(track, rom_.U8(header_.base + t.start));
        break;

    case Op::kCall:
        // The call plays commands from the track's data, from its start in the song table, with the delay before each.
        // F5 starts with the command in its last byte, which counts as the call's first, and the delay after the call
        // is read from that byte too.
        t.saved_start = t.start;
        t.ret = uint16_t(pos);
        t.start = header_.offsets[track];
        t.count = uint8_t(c.count);
        pos = uint32_t(c.value);
        if (!CallSetsFirst(c.opcode, Revision::kDungeonDiceMonsters))
        {
            return Next::kReadDelayUncounted;
        }

        t.count++;
        RunDutyOrWave(track, rom_.U8(c.addr + 4));
        break;

    case Op::kPitchBend:
        t.pitch = int16_t(t.note * 16 + c.value);
        Play(t, o, t.vol);
        break;

    case Op::kSampleBend:
        {
            // The bend is from the note's pitch, or from note 24, a sample's rate, after a sample map note. A note that
            // starts this frame starts at the pitch, and otherwise the track's pitch becomes the period, which the
            // output stage hands to the voice's FIFO at once.
            Track* u = c.opcode & 1 ? NextTrack(track, c) : &t;
            if (!u)
            {
                break;
            }

            TrackOutput& uo = out_[size_t(u - tracks_.data())];
            const int pitch = (u->note ? u->note : 24) * 16 + 1 + c.value;
            if (uo.flags & kOutDungeonDiceNote)
            {
                u->pitch = int16_t(pitch);
            }
            else
            {
                u->pitch = int16_t(TableEntry(rom_, sample_periods_, pitch).value_or(0));
                uo.flags = kOutDungeonDicePeriod;
            }

            uo.pitch = u->pitch;
            break;
        }

    case Op::kPsgVolume:
        psg_volume_ = uint8_t(c.value);
        break;

    case Op::kPsgNote:
        t.note = uint8_t(c.note);
        t.pitch = int16_t(c.note < kDungeonDiceNoiseNote ? c.note << 4 : c.note + 15 * kDungeonDiceNoiseNote);
        t.flags &= uint8_t(~kTrackFade);
        Play(t, o, c.vol);
        break;

    case Op::kSampleAtNote:
        t.note = uint8_t(c.note);
        t.pitch = int16_t((c.note << 4) + 1);
        t.flags &= uint8_t(~(kTrackVibrato | kTrackFade));
        Play(t, o, c.vol);
        break;

    case Op::kNote:
        t.b2 = sample_of(c.sample);
        t.note = 0;
        t.pitch = 0;
        t.flags &= uint8_t(~(kTrackVibrato | kTrackFade));
        Play(t, o, c.vol);
        break;

    case Op::kNextNote:
        {
            // The next track has had its turn this frame, so the note goes straight into its record.
            Track* u = NextTrack(track, c);
            if (u)
            {
                TrackOutput& uo = out_[size_t(track + 1)];
                u->vol = uint8_t(c.vol);
                u->b2 = sample_of(c.sample);
                u->note = 0;
                u->pitch = 0;
                u->flags &= uint8_t(~(kTrackVibrato | kTrackFade));
                uo.vol = u->vol;
                uo.b2 = u->b2;
                uo.pitch = 0;
                uo.flags = kOutDungeonDiceNote;
                uo.trig = 1;
            }
            break;
        }

    case Op::kVolume:
        Play(t, o, c.vol);
        break;

    case Op::kSetVolume:
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kVibrato:
        t.flags &= uint8_t(~(kTrackVibrato | kTrackFade));
        t.vib_depth = uint8_t(c.value);
        if (c.value)
        {
            t.flags |= kTrackVibrato;
        }
        break;

    case Op::kDuty:
    case Op::kWave:
        RunDutyOrWave(track, c.opcode);
        break;

    case Op::kNop:
        break;

    default:
        {
            char buf[80];
            std::snprintf(buf, sizeof buf, "unsupported opcode %02X; track stopped", c.opcode);
            Warn(track, c.addr, rom_.Contains(c.addr) ? buf : "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Next::kStop;
        }
    }

    return Next::kReadDelay;
}

void DungeonDiceSequencer::RunDutyOrWave(int track, uint8_t op)
{
    // A wave goes into the wave RAM at once.
    const Command set = DutyOrWave(op, Revision::kDungeonDiceMonsters);
    if (set.op == Op::kWave)
    {
        loaded_wave_ = set.value;
        wave_loads_++;
        stage_commands_.push_back({Op::kWave, set.value, 0});
    }

    tracks_[size_t(track)].b2 = uint8_t(set.value);
    out_[size_t(track)].b2 = uint8_t(set.value);
}

DungeonDiceSequencer::Track* DungeonDiceSequencer::NextTrack(int track, const Command& c)
{
    return HasNextTrack(track, c) ? &tracks_[size_t(track + 1)] : nullptr;
}

void DungeonDiceSequencer::PerFrame(int track)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    const uint8_t flags = t.flags;

    // Each frame of a fade subtracts a quarter of the starting volume. At the end, output duty byte 0 and volume 0.
    if (flags & kTrackFade)
    {
        o.pitch = t.pitch;
        t.fade--;
        if (t.fade == 0)
        {
            t.flags &= uint8_t(~kTrackFade);
            o.b2 = 0;
            o.trig = 1;
        }

        t.vol = uint8_t((t.fade * t.fade_from) >> 2);
    }

    // Vibrato outputs a pitch every 4th frame, alternating normal and lowered pitch. The vibrato section of the
    // frequency table has 4 entries per whole note.
    if (flags & kTrackVibrato)
    {
        int pitch = t.pitch;
        if ((pitch & 15) == 0)
        {
            pitch = pitch / 4 + vibrato_entry_;
        }

        t.vib_count++;
        if (t.vol != 0 && (t.vib_count & 3) == 0)
        {
            if (t.vib_count & 4)
            {
                pitch -= t.vib_depth;
            }

            o.pitch = int16_t(pitch);
            o.b2 = t.b2;
            o.flags = kOutDungeonDiceNote;
        }
    }

    o.vol = t.vol;
}

} // namespace ntdlib::konami
