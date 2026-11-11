// SPDX-License-Identifier: MIT

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

#include "sequencer_internal.h"

namespace ntdlib::konami
{

std::unique_ptr<Sequencer> Sequencer::Create(const Rom& rom, const DriverInfo& info, int song, const Request& request)
{
    switch (info.revision)
    {
    case Revision::kUltimateMasters:
    case Revision::kLiveStart:
    case Revision::kPrinceOfTennis:
        return std::make_unique<UltimateMastersSequencer>(rom, info, song, request);
    case Revision::kDungeonDiceMonsters:
        return std::make_unique<DungeonDiceSequencer>(rom, info, song);
    default:
        return std::make_unique<Wct2004Sequencer>(rom, info, song, request);
    }
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song) : rom_(rom)
{
    loop_frame_.fill(-1);

    // A song whose entry isn't in the ROM keeps an empty header, so it plays nothing.
    ReadSongHeader(rom, info.song_table, song, info.revision, header_);
}

bool Sequencer::AnyTrackActive() const
{
    for (int t = 0; t < kTracks; t++)
    {
        if (TrackActive(t))
        {
            return true;
        }
    }

    return false;
}

int Sequencer::LoopStartFrame() const
{
    if (loop_track_ < 0 || loop_frame_[size_t(loop_track_)] < 0)
    {
        return 0;
    }

    const int looping = loop_frame_[size_t(loop_track_)];
    int start = looping;
    for (int frame : loop_frame_)
    {
        if (frame >= 0 && frame >= looping - kLoopPointWindow)
        {
            start = std::min(start, frame);
        }
    }

    return start;
}

bool Sequencer::LoopPointDelayed() const
{
    for (int t = 0; t < kTracks; t++)
    {
        if (loop_frame_[size_t(t)] >= 0 && loop_delay_[size_t(t)] > 0)
        {
            return true;
        }
    }

    return false;
}

void Sequencer::Warn(int track, uint32_t addr, const std::string& what)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "frame %u, track %d, 0x%08X: ", frame_, track, addr);
    warnings_.push_back(buf + what);
}

bool Sequencer::HasNextTrack(int track, const Command& c)
{
    if (track + 1 < header_.tracks)
    {
        return true;
    }

    char buf[80];
    std::snprintf(buf, sizeof buf, "%02X on the last track writes into the driver's variables; ignored", c.opcode);
    Warn(track, c.addr, buf);

    return false;
}

void Sequencer::PassLoopPoint(int track, int delay)
{
    // Each loop point moves the track's start, so a loop takes the track back to the last one it passed. Those it
    // passes once the song has looped don't change where the loop starts.
    if (loops_ == 0)
    {
        loop_frame_[size_t(track)] = int(frame_);
        loop_delay_[size_t(track)] = delay;
    }
}

void Sequencer::EndForGood(int track)
{
    if (loops_ == 0)
    {
        loop_frame_[size_t(track)] = -1;
        loop_delay_[size_t(track)] = 0;
    }
}

void Sequencer::LogRead(int track, const Command& command)
{
    if (read_log_)
    {
        read_log_->push_back({frame_, track, command});
    }
}

const std::array<TrackOutput, kTracks>& Sequencer::Step()
{
    looped_last_frame_ = false;
    stage_commands_.clear();
    if (!valid_ || stopped_)
    {
        out_.fill(TrackOutput());
        frame_++;
        return out_;
    }

    // Tracks run from 15 down to 0. A loop command resets every track and the whole frame is run again from track 15,
    // as the driver does.
    bool hung = false;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        out_.fill(TrackOutput());
        bool restarted = false;
        for (int t = kTracks - 1; t >= 0; t--)
        {
            if (StepTrack(t) == Result::kRestart)
            {
                // Looping again in the rerun means the loop takes no time, and the driver would never finish the frame.
                // The song stops there, the frame plays nothing, and the loop doesn't count.
                if (attempt == 1)
                {
                    Warn(t, header_.base + TrackStart(t),
                         "song loops without waiting a frame, which would hang the driver; song stopped");
                    hung = true;
                    stopped_ = true;
                    looped_last_frame_ = false;
                    loops_--;
                    loop_track_ = loops_ > 0 ? loop_track_ : -1;
                    break;
                }

                if (loop_track_ < 0)
                {
                    loop_track_ = t;
                }

                ResetTracks();
                stage_commands_.push_back({Op::kJump, 1, 0});
                loops_++;
                looped_last_frame_ = true;
                restarted = true;
                LogRead(-1, Command());
                break;
            }
        }
        if (!restarted)
        {
            break;
        }
    }

    // Clear output from a hung frame. Doing this where the loop is detected triggers a false array-bounds warning in
    // GCC 13 at -O3.
    if (hung)
    {
        out_.fill(TrackOutput());
    }

    for (int t = 0; t < kTracks; t++)
    {
        if (out_[t].active)
        {
            out_[t].note_pitch = NotePitch(t);
        }
    }

    frame_++;

    return out_;
}

UltimateMastersSequencer::UltimateMastersSequencer(const Rom& rom, const DriverInfo& info, int song, Request request)
    : TrackSequencer(rom, info, song),
      revision_(info.revision),
      later_(LaterUltimateMasters(info.revision)),
      key_from_duty_(info.revision == Revision::kPrinceOfTennis),
      bend_kept_(info.bend_kept && !later_),
      request_(std::move(request))
{
    if (!rom.Contains(header_.base))
    {
        return;
    }

    // Every track starts active and restartable by a song loop. That's the state a loop resets it to.
    StartTracks();
    ResetTracks();

    valid_ = true;
}

void UltimateMastersSequencer::ResetTracks()
{
    // Tracks that can restart run again. Every track starts over from its first frame and without vibrato.
    for (Track& t : tracks_)
    {
        t.flags = RestartFlags(t.flags) & kLoopKeptFlags;
        t.pos = 0;
        t.b2 = 0;
        t.vol = 0;
        t.vib_phase = 0;
        t.vib_depth = 0;
        t.bend = 0;
        t.ret = 0;
        t.attack = 0;
        t.decay = 0;
    }

    // The square channels restart at 50% duty.
    tracks_[0].b2 = 0x80;
    tracks_[1].b2 = 0x80;
}

Sequencer::Result UltimateMastersSequencer::StepTrack(int track)
{
    const Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    if (!(t.flags & kTrackOn))
    {
        return Result::kContinue;
    }

    o.active = true;
    o.pan_r = t.pan_r;
    o.pan_l = t.pan_l;

    return RunTrack(track, revision_);
}

void UltimateMastersSequencer::FirstFrame(Track& t, TrackOutput& o)
{
    Rest(t, o);
}

void UltimateMastersSequencer::CountInCall(Track& t, uint32_t& pos)
{
    CountCallCommand(t, pos, false);
}

UltimateMastersSequencer::Next UltimateMastersSequencer::RunCommand(int track, const Command& c, uint32_t& pos)
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

        stopped_ = true;
        Silence(t, o);
        return Next::kStop;

    case Op::kEndTrack:
    case Op::kEndTrackHard:
        t.flags &= kTrackRestarts;
        if (c.op == Op::kEndTrackHard)
        {
            t.flags = 0;
            EndForGood(track);
        }

        if (!o.Any())
        {
            Silence(t, o);
        }

        return Next::kStop;

    case Op::kCall:
        // The later builds play calls the way WCT 2004 does. Like 00-8F, 90-9E set the duty byte first.
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
        t.b2 = DutyByteForNibble(c.opcode);
        o.b2 = t.b2;
        break;

    case Op::kEchoRoute:
        RouteEcho(echo_, c.value >> 4, c.value & 0x0F);
        stage_commands_.push_back({c.op, c.value, 0});
        break;

    case Op::kEchoDelay:
        if (c.value > 0x7F)
        {
            Warn(track, c.addr, "echo delay above 0x7F (unverified behaviour)");
        }

        echo_[0].delay = c.value;
        stage_commands_.push_back({c.op, c.value, 0});
        break;

    case Op::kEchoFeedback:
        if (c.value > 0x7F)
        {
            Warn(track, c.addr, "echo feedback above 0x7F (unverified behaviour)");
        }

        echo_[0].feedback = c.value;
        stage_commands_.push_back({c.op, c.value, 0});
        break;

    case Op::kLoopPoint:
        t.start = uint16_t(t.start + pos);
        pos = 0;
        PassLoopPoint(track);
        break;

    case Op::kPitchBend:
        {
            // The bend changes the pitch of the note that's playing. Some builds of the driver also keep it, and add it
            // to the notes after it and to vibrato.
            const int16_t bend = int16_t(c.value);
            o.key = PitchKey(t);
            o.pitch = int16_t(t.pitch + bend);
            o.flags |= kOutPitch;
            if (bend_kept_)
            {
                t.bend = bend;
            }
            break;
        }

    case Op::kVibrato:
        t.vib_depth = uint8_t(c.value >> 1);
        t.flags |= kTrackVibrato;
        break;

    case Op::kPan:
        if (c.value <= 0x3F)
        {
            t.pan_r = uint8_t(c.value);
            t.pan_l = 0x3F;
        }
        else
        {
            t.pan_r = 0x3F;
            t.pan_l = uint8_t(0x7F - c.value);
        }

        o.pan_r = t.pan_r;
        o.pan_l = t.pan_l;
        o.flags |= kOutPan;
        break;

    case Op::kAttack:
        t.attack_rate = uint8_t(c.value);
        break;

    case Op::kDecay:
        t.decay_rate = uint8_t(c.value);
        break;

    case Op::kInstrument:
        // F6 xx up to 63 picks entry xx of the table of samples that the game's request gives the driver. A request
        // gives one only in the live stage of B3MJ. Each later note of the track then plays the entry's sample (see
        // kNote). F6 with a higher value changes the playing note's sample at once. No song with a table uses that,
        // and the model leaves it out.
        if (c.value <= 0x63)
        {
            t.sample_entry = int8_t(c.value);
        }

        break;

    case Op::kHoldOutput:
        // FA xx with xx other than 0 holds the track back if the game's setting is 0 at that moment. FA 00 lets it go.
        // A song request sets the setting to FF, and only a game that sets it to 0 after the request hears FA (see
        // DriverInfo::live_songs).
        t.flags &= uint8_t(~kTrackHeld);
        if (c.value != 0 && request_.setting == 0)
        {
            t.flags |= kTrackHeld;
        }

        break;

    case Op::kRest:
        t.attack = 0;
        t.decay = 0;
        Rest(t, o);
        break;

    case Op::kPsgNote:
        if (track <= 2)
        {
            t.pitch = int16_t(c.note << 5);
            o.pitch = int16_t(t.bend + t.pitch);
        }
        else
        {
            t.pitch = int16_t(c.note);
            o.pitch = t.pitch;
        }

        o.flags = kOutPsgNote;
        t.attack = t.attack_rate ? 0xFF : t.attack;
        t.decay = t.decay_rate ? 0xFF : t.decay;
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kVolume:
        o.pitch = t.pitch;
        o.flags = kOutPsgNote;
        t.vol = uint8_t(c.vol);
        o.b2 = t.b2;
        o.trig = 1;
        break;

    case Op::kNote:
        {
            t.vol = uint8_t(c.vol);
            t.key = uint16_t(c.sample);
            t.pitch = int16_t(c.semitone * 32);

            // An entry of F6's table replaces the note's sample, and can set its pitch and volume.
            if (const uint16_t entry = TableSample(request_.samples, t.sample_entry); entry != 0)
            {
                t.key = uint16_t(entry & ~(kEntryAtRate | kEntryFullVolume));
                t.pitch = (entry & kEntryAtRate) ? int16_t(0) : t.pitch;
                t.vol = (entry & kEntryFullVolume) ? uint8_t(0x7F) : t.vol;
            }

            t.b2 = uint8_t(t.key);
            o.key = t.key;
            o.flags = kOutNoteOn;
            o.pitch = int16_t(t.bend + t.pitch);
            o.b2 = t.b2;
            o.vol = t.vol;
            break;
        }

    case Op::kDuty:
        o.b2 = uint8_t(c.value);
        t.b2 = uint8_t(c.value);
        break;

    default:
        {
            // Opcodes FB and FC, and 90-9F, F4-F6, FA and notes of sample numbers above EF outside the later builds,
            // decode as unknown ones, and so do commands past the end of the ROM. The other ops belong to the older
            // revisions.
            char buf[80];
            if (c.opcode >= 0xA0 && c.opcode < 0xF0 && c.sample > 0xEF)
            {
                std::snprintf(buf, sizeof buf, "note of sample %02X: samples above EF are unsupported; track stopped",
                              c.sample);
            }
            else
            {
                std::snprintf(buf, sizeof buf, "unsupported opcode %02X; track stopped", c.opcode);
            }
            Warn(track, c.addr, rom_.Contains(c.addr) ? buf : "track runs past the end of the ROM; track stopped");
            t.flags = 0;
            return Next::kStop;
        }
    }

    return Next::kReadDelay;
}

void UltimateMastersSequencer::PerFrame(int track)
{
    Track& t = tracks_[track];
    TrackOutput& o = out_[track];
    if (t.flags & kTrackVibrato)
    {
        const int vib = StepVibrato(t);
        o.pitch = int16_t(t.bend + t.pitch + vib);
        o.b2 = t.b2;
        o.key = PitchKey(t);
        o.flags |= kOutPitch;
    }

    StepEnvelope(t, o);

    // A held track neither starts nor changes a note: its output record's retrigger byte and flags are cleared.
    if (t.flags & kTrackHeld)
    {
        o.trig = 0;
        o.flags = 0;
    }
}

} // namespace ntdlib::konami
