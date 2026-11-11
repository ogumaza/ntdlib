// SPDX-License-Identifier: MIT

// Definitions that the parts of Konami's sequencer share (sequencer.cpp, sequencer_wct2004.cpp and
// sequencer_dungeon_dice.cpp): the tracks' helpers and the loop of a track's commands. The three files build as one
// translation unit. The helpers therefore keep internal linkage. Only those files include this header.

#pragma once

#include "ntdlib/konami/sequencer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"

namespace ntdlib::konami
{

namespace
{

// The number of commands a track can run without a delay before it's stopped.
constexpr int kMaxCommandsPerFrame = 4096;

// The Ultimate Masters and WCT 2004 revisions scale every delay by this tempo: frames = (delay * tempo + frac) >> 8.
// Here frac is the low byte of the previous result. The tempo starts at 0x100, and no command changes it. So frac stays
// 0. The Rave Master, Eternal Duelist and Dungeon Dice Monsters revisions don't scale their delays. RunTrack() scales
// theirs by kTempo too. At 0x100 that leaves them as they are.
constexpr uint32_t kTempo = 0x100;

// The flags that a song loop keeps in the Ultimate Masters and WCT 2004 revisions. Besides kTrackOn and kTrackRestarts,
// they're bits 0x10 and 0x08. Nothing sets those.
constexpr uint8_t kLoopKeptFlags = kTrackOn | kTrackRestarts | 0x10 | 0x08;

// The first quarter of the driver's vibrato waveform, trunc(4096 * sin(2 * pi * i / 256)) for i = 0 to 64; the rest
// follows by symmetry. The values are written out because a table computed with std::sin would only reach 4096 if sin()
// returned exactly 1 at the peak, which isn't guaranteed on every platform.
constexpr int16_t kQuarterSine[65] = {0,    100,  200,  301,  401,  501,  601,  700,  799,  897,  995,  1092, 1189,
                                      1284, 1379, 1474, 1567, 1659, 1751, 1841, 1930, 2018, 2105, 2191, 2275, 2358,
                                      2439, 2519, 2598, 2675, 2750, 2824, 2896, 2966, 3034, 3101, 3166, 3229, 3289,
                                      3348, 3405, 3460, 3513, 3563, 3612, 3658, 3702, 3744, 3784, 3821, 3856, 3889,
                                      3919, 3947, 3973, 3996, 4017, 4035, 4051, 4065, 4076, 4084, 4091, 4094, 4096};

// Returns the driver's vibrato waveform at `phase`, which runs through one cycle in 256 steps.
int VibratoSine(uint8_t phase)
{
    const int i = phase & 0x7F;
    const int value = kQuarterSine[i <= 64 ? i : 128 - i];
    return (phase & 0x80) ? -value : value;
}

// Routes DirectSound voice `voice` to echo bus `bus` - 1, or takes it off buses 0 and 1 if `bus` is 0, as F9 does in
// the Ultimate Masters and WCT 2004 revisions.
void RouteEcho(std::array<EchoBus, 3>& echo, int voice, int bus)
{
    if (voice > 11)
    {
        return;
    }

    if (bus == 0)
    {
        echo[0].voices &= uint16_t(~(1u << voice));
        echo[1].voices &= uint16_t(~(1u << voice));
    }
    else if (bus <= 3)
    {
        echo[size_t(bus - 1)].voices |= uint16_t(1u << voice);
    }
}

// Returns a track's flags as a song loop starts the track again: a track that can restart runs again, from its first
// frame. Each revision then keeps only some of the flags.
uint8_t RestartFlags(uint8_t flags)
{
    uint8_t f = flags & uint8_t(~kTrackStarted);
    if (f & kTrackRestarts)
    {
        f |= kTrackOn;
    }

    return f;
}

// Returns true if WCT 2004's FA scale `scale` scales a track's volumes. A scale from 80 up scales nothing.
bool Scales(uint8_t scale)
{
    return scale < 0x80;
}

// Returns volume `vol` (0-15) of a WCT 2004 note or volume command with FA scale `scale` applied: `vol` × `scale` / 16,
// rounded down.
int ScaledVolume(uint8_t scale, int vol)
{
    return Scales(scale) ? (vol * scale) >> 4 : vol;
}

// Returns entry `entry` of the table of samples that a request gave F6, or 0 for none. The driver plays no sample from
// entry 0, or from an entry that holds 0.
uint16_t TableSample(const std::vector<uint16_t>& samples, int entry)
{
    return entry > 0 && size_t(entry) < samples.size() ? samples[size_t(entry)] : 0;
}

// The bits of an entry of F6's table of samples. Bit 15 of the sample number selects the second sample table. Only the
// live stages' songs reach it.
constexpr uint16_t kEntryAtRate = 0x4000;     // the note plays the sample at pitch 0, its rate
constexpr uint16_t kEntryFullVolume = 0x1000; // Live Start: the note plays at volume 7F

// Moves the vibrato of track `t` on by a frame in every revision but Dungeon Dice Monsters, and returns the vibrato's
// pitch offset in 1/32 semitones. At depth 0, the vibrato turns off and goes back to the start of its cycle.
template <typename Track>
int StepVibrato(Track& t)
{
    if (t.vib_depth == 0)
    {
        t.flags &= uint8_t(~kTrackVibrato);
        t.vib_phase = 0;
        return 0;
    }

    t.vib_phase = uint8_t(t.vib_phase + 0x18);
    return (VibratoSine(t.vib_phase) * t.vib_depth) >> 12;
}

// Silences the channel of track `t` through its output `o`. A stop or the end of a track does that in every revision
// but Dungeon Dice Monsters.
template <typename Track>
void Silence(Track& t, TrackOutput& o)
{
    t.vol = 0;
    o.b2 = 0;
    o.vol = 0;
    o.pitch = t.pitch;
    o.flags = kOutStop;
    o.trig = kTrigStop;
}

// Outputs a rest on the channel of track `t` through its output `o`, in every revision but Dungeon Dice Monsters. The
// first frame of a track starts with one too.
template <typename Track>
void Rest(Track& t, TrackOutput& o)
{
    o.pitch = t.pitch;
    o.flags = kOutStop;
    t.vol = 0;
    o.b2 = t.b2;
    o.trig = 1;
}

// Outputs the pitch of Dungeon Dice Monsters track `t` at volume `vol` through its output `o`, for notes and rests. A
// rest plays at volume 0. The first frame of a track starts with one.
void Play(DungeonDiceTrack& t, TrackOutput& o, int vol)
{
    o.pitch = t.pitch;
    o.flags = kOutDungeonDiceNote;
    t.vol = uint8_t(vol);
    o.b2 = t.b2;
    o.trig = 1;
}

// Moves the attack or decay of track `t`'s PSG note on by a frame, and outputs the track's volume through its output
// `o`, in the WCT 2004, Rave Master and Eternal Duelist revisions and the later Ultimate Masters builds. The attack
// takes the note's volume from 1/16 of it up to 1.06 times it, and the decay then takes it back down to 0.06 times it.
// When the level changes, the channel is retriggered at it.
template <typename Track>
void StepEnvelope(Track& t, TrackOutput& o)
{
    int level = t.vol;
    int before = level;
    if (t.attack)
    {
        const int from = t.attack;
        t.attack = uint8_t(std::max(0, from - t.attack_rate));
        before = ((0x10F - from) * t.vol) >> 8;
        level = ((0x10F - t.attack) * t.vol) >> 8;
    }
    else if (t.decay)
    {
        const int from = t.decay;
        t.decay = uint8_t(std::max(0, from - t.decay_rate));
        before = ((from + 0xF) * t.vol) >> 8;
        level = ((t.decay + 0xF) * t.vol) >> 8;
    }

    if (level != before)
    {
        if (o.flags == 0)
        {
            o.pitch = t.pitch;
        }

        o.b2 = t.b2;
        o.trig = 1;
    }

    o.vol = uint8_t(level);
}

// Counts a command that a call of track `t` plays. After the call's last command, the track's start and its read
// position `pos` go back to those before the call. With `keep_return`, the track keeps the return position and counts
// on. Every 256 commands it then jumps back there again.
template <typename Track>
void CountCallCommand(Track& t, uint32_t& pos, bool keep_return)
{
    if (t.ret == 0)
    {
        return;
    }

    t.count--;
    if (t.count == 0)
    {
        t.start = t.saved_start;
        pos = t.ret;
        t.ret = keep_return ? t.ret : 0;
    }
}

} // namespace

template <typename TrackState>
Sequencer::Result TrackSequencer<TrackState>::RunTrack(int track, Revision revision)
{
    Track& t = tracks_[track];
    uint32_t pos;
    bool read_command;
    if (!(t.flags & kTrackStarted))
    {
        // First frame of the track: silence the channel, then read the initial delay.
        t.flags |= kTrackStarted;
        pos = 0;
        FirstFrame(t, out_[track]);
        read_command = false;
    }
    else
    {
        t.delay = uint16_t(t.delay - 1);
        if (t.delay != 0)
        {
            PerFrame(track);
            return Result::kContinue;
        }

        pos = t.pos;
        read_command = true;
    }

    for (int guard = 0;; guard++)
    {
        if (guard > kMaxCommandsPerFrame)
        {
            Warn(track, header_.base + t.start + pos, "too many commands without a delay; track stopped");
            t.flags = 0;
            return Result::kContinue;
        }

        if (read_command)
        {
            const Command c = DecodeCommand(rom_, header_.base + t.start + pos, track, revision);
            LogRead(track, c);
            pos += c.length;
            const Next next = RunCommand(track, c, pos);
            if (next == Next::kStop || next == Next::kRestart)
            {
                return next == Next::kRestart ? Result::kRestart : Result::kContinue;
            }
            if (next == Next::kReadDelay)
            {
                CountInCall(t, pos);
            }
        }

        read_command = true;

        const uint32_t daddr = header_.base + t.start + pos;
        if (!rom_.Contains(daddr, 1))
        {
            Warn(track, daddr, "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Result::kContinue;
        }

        uint32_t delay;
        const uint32_t delay_bytes = ReadDelay(rom_, daddr, revision, delay);
        pos += delay_bytes;
        if (LogsReads())
        {
            Command read;
            read.op = Op::kDelay;
            read.addr = daddr;
            read.length = delay_bytes;
            read.delay = delay;
            LogRead(track, read);
        }
        t.pos = uint16_t(pos);
        pos = t.pos;
        t.delay = uint16_t((kTempo * delay) >> 8);
        if (delay == 0)
        {
            continue;
        }

        PerFrame(track);
        return Result::kContinue;
    }
}

} // namespace ntdlib::konami
