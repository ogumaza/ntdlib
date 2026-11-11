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
#include "sequencer_shared.h"

namespace ntdlib::mp2k
{
using commands::Kind;
using ntdlib::agb::Rom;
using ntdlib::agb::WarnOnce;

namespace
{

// The player's priority whose notes take a priority of 255 in a later version of Camelot's sequencer.
constexpr int kCamelotTopPlayer = 120;

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

constexpr uint32_t kRegNr50 = 0x04000080;

constexpr uint32_t kRegSoundCntH = 0x04000082;

constexpr uint32_t kRegSoundCntX = 0x04000084;

constexpr uint32_t kRegSoundBias = 0x04000088;

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
                          RunningState{track.running, track.key, track.velocity}, NoteArguments(info_));
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
