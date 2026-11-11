// SPDX-License-Identifier: MIT

// The sequencer of WCT 2004 and of the older Rave Master, Captain Tsubasa and Eternal Duelist revisions: its tracks'
// reset, its commands and its per-frame work.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/konami/sequencer.h"

#include "sequencer_internal.h"

namespace ntdlib::konami
{

Wct2004Sequencer::Wct2004Sequencer(const Rom& rom, const DriverInfo& info, int song, Request request)
    : TrackSequencer(rom, info, song),
      revision_(info.revision),
      eternal_duelist_(info.revision == Revision::kEternalDuelist),
      ed_layout_(EternalDuelistLayout(info.revision)),
      song_volume_on_(info.revision == Revision::kMinnaNoOujisama),
      request_(std::move(request))
{
    if (!rom.Contains(header_.base))
    {
        return;
    }

    // Each of the song's tracks starts in the state that a loop resets it to: active and restartable by a song loop,
    // panned to the centre and with no attack or decay. The echo buses start with no voices and no feedback.
    StartTracks();
    ResetTracks();

    valid_ = true;
}

void Wct2004Sequencer::ResetTracks()
{
    // Tracks that can restart run again from their first frame and without vibrato. A loop keeps their attack and decay
    // rates. WCT 2004 and Hunter X Hunter also keep their pan, and Rave Master centres it again.
    const bool keep_pan = Wct2004Layout(revision_) || revision_ == Revision::kHunterXHunter;
    for (Track& t : tracks_)
    {
        t.flags = RestartFlags(t.flags) & kLoopKeptFlags;
        t.pos = 0;
        t.ret = 0;
        t.b2 = 0;
        t.vol = 0;
        t.vib_phase = 0;
        t.vib_depth = 0;
        t.attack = 0;
        t.decay = 0;
        t.pan = keep_pan ? t.pan : 0xFF;
    }

    // Eternal Duelist and Captain Tsubasa set each PSG track's NR51 bits for both sides of its channel, and each sample
    // track's pan to 33.
    if (ed_layout_)
    {
        for (int t = 0; t < kTracks; t++)
        {
            tracks_[t].pan = IsPsgTrack(t) ? uint8_t(0x11 << t) : 0x33;
        }
    }

    // The square channels restart at 50% duty. Minna no Ouji-sama's song volume goes back to full.
    tracks_[0].b2 = 0x80;
    tracks_[1].b2 = 0x80;
    song_volume_ = kFullSongVolume;
}

Sequencer::Result Wct2004Sequencer::StepTrack(int track)
{
    const Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    // The NR51 of Eternal Duelist and Captain Tsubasa takes the pan of every PSG track, running or not.
    if (ed_layout_)
    {
        o.pan = t.pan;
    }

    if (!(t.flags & kTrackOn))
    {
        return Result::kContinue;
    }

    o.active = true;
    o.pan = t.pan;
    o.pan_start = t.pan;

    return RunTrack(track, revision_);
}

void Wct2004Sequencer::FirstFrame(Track& t, TrackOutput& o)
{
    t.attack = 0;
    t.decay = 0;
    Rest(t, o);
}

void Wct2004Sequencer::CountInCall(Track& t, uint32_t& pos)
{
    CountCallCommand(t, pos, eternal_duelist_);
}

Wct2004Sequencer::Next Wct2004Sequencer::RunCommand(int track, const Command& c, uint32_t& pos)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];

    switch (c.op)
    {
    case Op::kJump:
        if (c.value != 0)
        {
            return Next::kRestart;
        }

        // The driver silences every track on the next frame, and the model ends the song here.
        stopped_ = true;
        Silence(t, o);
        return Next::kStop;

    case Op::kEndTrack:
        t.flags &= kTrackRestarts;
        if (!o.Any())
        {
            Silence(t, o);
        }

        return Next::kStop;

    case Op::kCall:
        // The call plays commands from the track's data, from its start in the song table, with the delay before each.
        // 9F plays `count` commands. Like 00-8F, the other opcodes also set the duty byte or the wave. That counts as
        // the call's first command.
        t.saved_start = t.start;
        t.ret = uint16_t(pos);
        t.start = header_.offsets[track];
        pos = uint32_t(c.value);
        if (!CallSetsFirst(c.opcode, revision_))
        {
            t.count = uint8_t(c.count);
            return Next::kReadDelayUncounted;
        }

        t.count = uint8_t(c.count + 1);
        t.b2 = uint8_t(DutyOrWave(c.opcode, revision_).value);
        o.b2 = t.b2;
        if (DutyOrWave(c.opcode, revision_).op == Op::kWave)
        {
            stage_commands_.push_back({Op::kWave, t.b2, t.vol});
        }
        break;

    case Op::kEchoRoute:
        RouteEcho(echo_, c.value >> 4, c.value & 0x0F);
        stage_commands_.push_back({c.op, c.value, 0});
        break;

    case Op::kEchoDelay:
        echo_[size_t(c.value >> 7)].delay = c.value & 0x7F;
        stage_commands_.push_back({c.op, c.value, 0});
        break;

    case Op::kEchoFeedback:
        echo_[0].feedback = c.value & 0x7F;
        stage_commands_.push_back({c.op, c.value, 0});
        break;

    case Op::kLoopPoint:
        t.start = uint16_t(t.start + pos);
        pos = 0;
        PassLoopPoint(track);
        break;

    case Op::kPitchBend:
        // The bend changes the pitch of the note that's playing, and isn't kept. Eternal Duelist marks the change with
        // a flag of 1, in place of the others.
        o.key = t.b2;
        o.pitch = int16_t(t.pitch + c.value);
        o.flags = eternal_duelist_ ? kOutEternalDuelistPitch : o.flags | kOutPitch;
        break;

    case Op::kVibrato:
        t.vib_depth = uint8_t(c.value >> 1);
        t.flags |= kTrackVibrato;
        break;

    case Op::kPanLevels:
    case Op::kPsgPan:
        t.pan = uint8_t(c.value);
        o.pan = t.pan;
        break;

    case Op::kPairVolumes:
        // Put the high nibble in the next track's output record to set its voice's volume. That track has already run
        // this frame. After the last track, this would overwrite the driver's variables.
        t.vol = uint8_t(c.value & 15);
        if (HasNextTrack(track, c))
        {
            out_[size_t(track + 1)].vol = uint8_t(c.value >> 4);
            out_[size_t(track + 1)].trig = 1;
        }

        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kAttack:
        t.attack_rate = uint8_t(c.value);
        break;

    case Op::kDecay:
        t.decay_rate = uint8_t(c.value);
        break;

    case Op::kInstrument:
        // F6 xx up to 63 picks entry xx of the table of samples that the game's request gives the driver. A request
        // gives one only in the live stage of BMAJ. Each later note of the track then plays the entry's sample (see
        // kNote). F6 with a higher value changes the playing note's sample at once. No song with a table uses that,
        // and the model leaves it out.
        if (c.value <= 0x63)
        {
            t.sample_entry = int8_t(c.value);
        }

        break;

    case Op::kNop:
        break;

    case Op::kSongVolume:
        // FB xx sets the volume of the whole song (see PerFrame). It takes effect in the track's frame and in those of
        // the tracks after it.
        song_volume_ = c.value;
        break;

    case Op::kVolumeScale:
        // FA xx with xx other than 0 scales the track's later volumes by the game's setting, and FA 00 stops that. A
        // song request sets the setting to FF. That scales nothing. Only a game that sets it lower after the request
        // hears FA (see DriverInfo::live_songs). A loop keeps the scale.
        t.scale = c.value != 0 ? request_.setting : 0xFF;
        break;

    case Op::kRest:
        t.attack = 0;
        t.decay = 0;
        Rest(t, o);
        break;

    case Op::kPsgNote:
        // The noise track's notes are pitches too, and the driver reads its noise table at them. Eternal Duelist only
        // retriggers the channel for a new volume, and otherwise changes the pitch of the note that's playing. FA's
        // scale applies to the volume, even where it gives 0.
        t.pitch = int16_t(c.note << 5);
        o.pitch = t.pitch;
        o.flags = kOutPsgNote;
        if (eternal_duelist_ && c.vol == t.vol)
        {
            break;
        }

        t.attack = t.attack_rate ? 0xFF : t.attack;
        t.decay = t.decay_rate ? 0xFF : t.decay;
        t.vol = uint8_t(ScaledVolume(t.scale, c.vol));
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kVolume:
        // A volume that FA's scale takes to 0 does nothing.
        if (Scales(t.scale) && ScaledVolume(t.scale, c.vol) == 0)
        {
            break;
        }

        o.pitch = t.pitch;
        o.flags = kOutPsgNote;
        t.vol = uint8_t(ScaledVolume(t.scale, c.vol));
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kNote:
        // A note whose volume FA's scale takes to 0 doesn't play. The note that's playing goes on.
        if (Scales(t.scale) && ScaledVolume(t.scale, c.vol) == 0)
        {
            break;
        }

        t.vol = uint8_t(ScaledVolume(t.scale, c.vol));
        o.key = uint16_t(c.sample);
        t.pitch = int16_t(c.semitone * 32);

        // An entry of F6's table replaces the note's sample, and can set its pitch to 0.
        if (const uint16_t entry = TableSample(request_.samples, t.sample_entry); entry != 0)
        {
            o.key = uint16_t(entry & ~kEntryAtRate);
            t.pitch = (entry & kEntryAtRate) ? int16_t(0) : t.pitch;
        }

        t.b2 = uint8_t(o.key);
        o.flags = kOutNoteOn;
        o.pitch = t.pitch;
        o.b2 = t.b2;
        o.vol = t.vol;
        break;

    case Op::kDuty:
    case Op::kWave:
        // A wave also goes into the wave channel's RAM at once, at the track's volume.
        t.b2 = uint8_t(c.value);
        o.b2 = t.b2;
        if (c.op == Op::kWave)
        {
            stage_commands_.push_back({c.op, c.value, t.vol});
        }
        break;

    default:
        {
            // A command past the end of the ROM decodes as an unknown one, and the others belong to the Ultimate
            // Masters and Dungeon Dice Monsters revisions.
            char buf[80];
            std::snprintf(buf, sizeof buf, "unsupported opcode %02X; track stopped", c.opcode);
            Warn(track, c.addr, rom_.Contains(c.addr) ? buf : "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Next::kStop;
        }
    }

    return Next::kReadDelay;
}

void Wct2004Sequencer::PerFrame(int track)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    if (t.flags & kTrackVibrato)
    {
        const int vib = StepVibrato(t);

        // Eternal Duelist's flag of 1 replaces the flags the commands set, a note's start included.
        o.pitch = int16_t(t.pitch + vib);
        o.b2 = t.b2;
        o.key = t.b2;
        o.flags = eternal_duelist_ ? kOutEternalDuelistPitch : o.flags | kOutPitch;
    }

    StepEnvelope(t, o);

    // Minna no Ouji-sama scales the volume by FB's song volume. While the song volume isn't full, an output without a
    // retrigger gets one, at the note's pitch without vibrato. The driver does that whenever the song volume differs
    // from its value at the song's start or loop. That value is always full.
    if (song_volume_on_)
    {
        o.vol = uint8_t((o.vol * song_volume_) >> 4);
        if (o.trig == 0 && song_volume_ != kFullSongVolume)
        {
            o.pitch = t.pitch;
            o.b2 = t.b2;
            o.trig = 1;
        }
    }
}

} // namespace ntdlib::konami
