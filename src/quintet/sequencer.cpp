// SPDX-License-Identifier: MIT

#include "ntdlib/quintet/sequencer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/song.h"

#include "sequencer_internal.h"

namespace ntdlib::quintet
{

using ntdlib::agb::WarnOnce;

uint32_t SampleCycles(uint32_t rate)
{
    return DivideUnsigned(kTimerClock, rate);
}

uint32_t FifoFrames(uint32_t position, uint32_t rate, uint32_t end)
{
    // Each update adds `rate`, and then stops or loops the FIFO if another `rate` would reach `end` × 60.
    const uint64_t target = uint64_t(end) * 60;
    if (rate == 0 || position + 2 * uint64_t(rate) >= target)
    {
        return 0;
    }

    return uint32_t((target - position + rate - 1) / rate - 2);
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : rom_(rom), info_(info), traits_(TraitsOf(info.revision))
{
    SongHeader header;
    const uint32_t address = SongAddress(info, song);
    if (!address || !ReadSongHeader(rom, address, header))
    {
        return;
    }

    // The game's sound init: the driver's init resets the channels and the PSG, and the routine that sets up the FIFOs
    // stops them. None of that is traced, but it leaves the registers that the driver reads back.
    ResetPsg();
    for (const FifoRegisters& fifo : kFifoRegisters)
    {
        Write(fifo.timer, 4, 0);
        Write(fifo.dma_control, 2, 0);
    }
    for (Channel& ch : channels_)
    {
        ResetChannel(ch);
    }

    writes_.clear();
    effect_ = EffectIndex(info, song) >= 0;
    if (effect_)
    {
        // The routine for sound effects starts each sound effect channel whose data doesn't start with FF. It resets
        // the channel and turns its outputs on in the sound effects' image. The channel then reads the sound effects'
        // bank of samples and noise macros. The music's channels play nothing.
        music_image_ = image_;
        ResetChannel(music_wave_);
        for (int c = 0; c < kChannels; c++)
        {
            const uint32_t data = header.channels[size_t(c)];
            if (rom_.U8(data) == 0xFF)
            {
                continue;
            }

            Channel& ch = channels_[size_t(c)];
            ResetChannel(ch);
            ch.sample_bank = 1;
            ch.macro_bank = 1;
            if (c < kPcmA)
            {
                image_[kNr50] = uint16_t(image_[kNr50] | 0x1100 << c);
            }
            else
            {
                image_[kSoundcntH] = 0x730E;
            }
            ch.position = data;
        }

        valid_ = true;
        return;
    }

    // The play routine resets the channels and the PSG, sets the PSG's master volume, and points each channel at its
    // data.
    for (Channel& ch : channels_)
    {
        ResetChannel(ch);
    }

    ResetPsg();
    image_[kNr50] = uint16_t((image_[kNr50] & 0xFF00) | 0x77);

    for (int c = 0; c < kChannels; c++)
    {
        channels_[size_t(c)].position = header.channels[size_t(c)];
    }

    if (traits_.reseeds)
    {
        Reseed();
    }

    start_writes_ = writes_;
    valid_ = true;
}

const std::vector<RegisterWrite>& Sequencer::Step()
{
    writes_.clear();
    events_.clear();
    hardware_.restarted = 0;

    // The channels read their notes when their countdowns run out, and then apply their effects. When a sound effect's
    // PSG channel ends, the driver writes its level of 0 straight to the channel's envelope register.
    for (int c = 0; c < kChannels; c++)
    {
        Channel& ch = channels_[size_t(c)];
        if (!ch.position)
        {
            continue;
        }

        ch.countdown--;
        if (c == audition_.channel)
        {
            RunAudition(c);
        }
        else if (int16_t(ch.countdown) <= 0)
        {
            ReadNotes(c);
            UpdateImage(c);
            if (effect_ && c < kPcmA && !ch.position)
            {
                const size_t r = size_t(kPsgEnvelope[c]);
                Write(kImageRegisters[r], 2, image_[r]);
            }
        }

        if (c < kPcmA)
        {
            ChannelEffects(c);
        }
        else
        {
            PcmEffects(c);
        }
    }

    Output();
    UpdateFifos();

    // The J revision sets the timers that the frame's starts and bends left until now.
    for (int f = 0; f < 2; f++)
    {
        if (pending_timers_[size_t(f)])
        {
            Write(kFifoRegisters[size_t(f)].timer, 4, pending_timers_[size_t(f)]);
            pending_timers_[size_t(f)] = 0;
        }
    }

    frame_++;

    return writes_;
}

bool Sequencer::Ended() const
{
    for (const Channel& ch : channels_)
    {
        if (ch.position)
        {
            return false;
        }
    }

    return true;
}

void Sequencer::ResetChannel(Channel& ch)
{
    ch = Channel();
    ch.tempo = traits_.start_tempo;
    ch.envelope_up = traits_.start_envelope_up;
}

void Sequencer::ResetPsg()
{
    for (int r = kNr10; r <= kNr43; r++)
    {
        Write(kImageRegisters[r], 2, 0);
    }

    Write(kImageRegisters[kNr50], 2, 0xFF77);
    Write(kImageRegisters[kSoundcntH], 2, 0xFB0E);
    image_.fill(0);
    image_[kNr50] = 0xFF77;
    image_[kSoundcntH] = 0x730E;
    shadow_ = image_;
    Write(kSoundbias, 2, (soundbias_ & 0x3FFF) | uint32_t(traits_.soundbias_resolution) << 14);
}

void Sequencer::ReadNotes(int c)
{
    Channel& ch = channels_[size_t(c)];
    for (int commands = 0;; commands++)
    {
        if (commands > kMaxCommands)
        {
            Warn("channel " + std::to_string(c) + " reads commands without reaching a note, so it was ended");
            ch.position = 0;
            AddEvent(c, Event::kEnd);
            return;
        }
        if (rom_.U8(ch.position) == 0xFF)
        {
            RecordRead(c, ch.position);
            if (!FollowEnd(c))
            {
                return;
            }
        }

        // A note's low nibble is its pitch, or 12 to play again or 13 to rest, and its high nibble picks its length. In
        // the S revision, a note's first byte gives the pitch and its second the note value.
        const uint32_t at = ch.position;
        RecordRead(c, at);
        const uint8_t b = rom_.U8(ch.position++);
        int note = b & 15;
        int value = 0;
        const uint8_t volume = ch.volume;
        if (traits_.byte_notes)
        {
            if (b >= kFirstCommandS)
            {
                ch.position += b == kSkipByteS ? 1 : 0;
                RunCommand(c, CommandOfS(b));
                RecordVolume(c, volume);
                continue;
            }

            note = b <= 11 ? b : b == kPlayAgainS ? 12 : b == kRestS ? 13 : -1;
            value = rom_.U8(ch.position++);
        }
        else
        {
            const int length = b >> 4;
            if (note > 13 || length > 12)
            {
                RunCommand(c, b);
                RecordVolume(c, volume);
                continue;
            }

            value = rom_.U8(info_.length_table + uint32_t(length));
        }

        // A note, a "play again" or a rest: its start, its length with any dot and ties, the effects, the event and the
        // wait for the next note. A note of the S revision that only waits has no event.
        StartNote(c, note);
        uint32_t last = at;
        const uint16_t ticks = NoteTicks(ch, b, value, last);
        if (record_ && stretch_[size_t(c)])
        {
            stretches_[stretch_[size_t(c)] - 1].last = last;
        }
        RestartEffects(ch, note);
        if (note >= 0)
        {
            const Event::Kind kind = note <= 11 ? Event::kNote : note == 12 ? Event::kPlayAgain : Event::kRest;
            AddEvent(c, kind, NoteIndex(ch, note, c >= kPcmA ? kPcmPitchEntries : kFrequencyEntries), at);
        }
        ticks_[size_t(c)] += ticks;
        WaitTicks(ch, ticks);

        // Each wait ends a stretch. The commands of the next stretch run in the frame where the wait ends.
        split_[size_t(c)] = true;
        return;
    }
}

bool Sequencer::FollowEnd(int c)
{
    // At the loop point itself, FF reads nothing more in this frame or any later one. The channel holds its note.
    Channel& ch = channels_[size_t(c)];
    if (ch.position == ch.loop)
    {
        if (!held_[size_t(c)])
        {
            held_[size_t(c)] = true;
            AddEvent(c, Event::kHold);
        }

        return false;
    }

    // Without a loop point, FF ends the channel.
    if (!ch.loop)
    {
        ch.position = 0;
        ch.level = 0;
        ch.envelope_up = 0;
        ch.envelope_step = 0;
        if (c >= kPcmA)
        {
            StopFifo(c - kPcmA);
        }
        AddEvent(c, Event::kEnd);
        return false;
    }

    // Otherwise FF goes back to the loop point. The channel counts its ticks and frames again from there.
    ch.position = ch.loop;
    jumped_[size_t(c)] = true;
    ch.frames = 0;
    ch.ticks = 0;
    if (traits_.reseeds)
    {
        Reseed();
    }
    AddEvent(c, Event::kLoop);

    return true;
}

void Sequencer::StartNote(int c, int note)
{
    Channel& ch = channels_[size_t(c)];
    const bool pcm = c >= kPcmA;
    ch.sounding = 1;
    ch.sliding = 0;
    if (note >= 0 && note <= 11)
    {
        // After D4 the note slides from the pitch of the note before.
        const uint16_t before = ch.pitch;
        ch.pitch = pcm ? PcmPitch(ch, note) : PsgFrequency(ch, note);
        ch.pitch_from = ch.slide_next ? before : ch.pitch;
        if (pcm)
        {
            PcmNoteOn(c);
        }
        if (ch.slide_next)
        {
            ch.slide_time = 0;
            ch.sliding = 1;
        }

        ch.level = ch.volume;
    }
    else if (note == 12)
    {
        ch.level = ch.volume;
        if (pcm)
        {
            ch.pitch = ch.detune;
            ch.pitch_from = ch.detune;
            PcmNoteOn(c);
        }
    }
    else if (note == 13)
    {
        ch.level = 0;
        ch.sounding = 0;
        if (pcm)
        {
            StopFifo(c - kPcmA);
        }
    }

    ch.slide_next = 0;
}

uint16_t Sequencer::NoteTicks(Channel& ch, uint8_t first, int value, uint32_t& last)
{
    // D1 makes the note dotted. D4 followed by a note of the same pitch ties another length to it: in the S revision,
    // a note with the same first byte. The driver adds the lengths up in the channel's countdown. The countdown is a
    // halfword.
    const uint8_t dot = traits_.byte_notes ? kDotS : 0xD1;
    const uint8_t tie = traits_.byte_notes ? kTieS : 0xD4;
    uint16_t ticks = uint16_t(Divide(kWholeNote, value));
    if (rom_.U8(ch.position) == dot)
    {
        last = ch.position++;
        ticks = uint16_t(ticks + Half(int16_t(ticks)));
    }
    while (rom_.U8(ch.position) == tie)
    {
        const uint8_t next = rom_.U8(ch.position + 1);
        int32_t add = 0;
        if (traits_.byte_notes)
        {
            if (next != first)
            {
                break;
            }

            add = Divide(kWholeNote, rom_.U8(ch.position + 2));
            last = ch.position + 1;
            ch.position += 3;
        }
        else
        {
            if ((next & 15) != (first & 15) || (next >> 4) > 12)
            {
                break;
            }

            add = Divide(kWholeNote, rom_.U8(info_.length_table + uint32_t(next >> 4)));
            last = ch.position + 1;
            ch.position += 2;
        }
        if (rom_.U8(ch.position) == dot)
        {
            last = ch.position++;
            add += Half(add);
        }

        ticks = uint16_t(ticks + add);
    }

    return ticks;
}

void Sequencer::RunCommand(int c, uint8_t command)
{
    Channel& ch = channels_[size_t(c)];

    auto arg = [&]()
    {
        return rom_.U8(ch.position++);
    };

    switch (command)
    {
    case 0x0E:
        ch.sweep_shift = arg();
        break;

    case 0x0F:
        ch.sweep_down = 0;
        break;

    case 0x1E:
        ch.sweep_down = 1;
        break;

    case 0x1F:
        ch.sweep_time = arg();
        break;

    case 0x2E:
        ch.length = arg();
        ch.length_on = ch.length != 0x63;
        if (ch.length == 0x63)
        {
            ch.length = 0x3F;
        }
        break;

    case 0x2F:
        ch.duty = arg();
        ch.switch_delay = 0;
        break;

    case 0x3E:
        ch.envelope_step = arg();
        break;

    case 0x3F:
        ch.envelope_up = 1;
        break;

    case 0x4E:
        ch.envelope_up = 0;
        break;

    case 0x4F:
        ch.octave = arg();
        break;

    case 0x5E:
        ch.volume = arg();
        break;

    case 0x5F:
        {
            // Volume up, to at most 3 on the wave channel (4 in the J revision), 1 on a PCM channel and 15 on the
            // others.
            const uint8_t wave = uint8_t(traits_.wave_levels - 1);
            const uint8_t limit = c == kWave ? wave : c >= kPcmA ? 1 : 15;
            if (ch.volume != limit)
            {
                ch.volume++;
            }
            break;
        }

    case 0x6E:
        if (ch.volume != 0)
        {
            ch.volume--;
        }
        break;

    case 0x6F:
        if (ch.octave != 6)
        {
            ch.octave++;
        }
        break;

    case 0x7E:
        if (ch.octave != 0)
        {
            ch.octave--;
        }
        break;

    case 0x7F:
        // A tempo on the first channel sets every channel's.
        ch.tempo = arg();
        ch.frames = 0;
        ch.ticks = 0;
        if (c == 0)
        {
            for (int k = 1; k < kChannels; k++)
            {
                Channel& other = channels_[size_t(k)];
                other.tempo = ch.tempo;
                other.frames = 0;
                other.ticks = 0;
            }
        }
        AddEvent(c, Event::kTempo);
        break;

    case 0x8E:
        ch.noise_shift = arg();
        break;

    case 0x8F:
        ch.noise_width = arg();
        break;

    case 0x9E:
        ch.noise_ratio = arg();
        break;

    case 0x9F:
        ch.wave_size = arg();
        break;

    case 0xAE:
        ch.wave_bank = arg();
        break;

    case 0xAF:
        ch.wave_on = arg();
        break;

    case 0xBE:
        ch.wave_75 = arg();
        break;

    case 0xBF:
        ch.loop = ch.position;
        AddEvent(c, Event::kLoopPoint);
        break;

    case 0xCE:
        image_[kNr50] = uint16_t((image_[kNr50] & 0xFF8F) | arg());
        break;

    case 0xCF:
        image_[kNr50] = uint16_t((image_[kNr50] & 0xFFF8) | arg());
        break;

    case 0xD0:
        {
            // The PSG channels' enables on each side, or a FIFO's: keep the bits of the first byte, then set those of
            // the second.
            const uint8_t keep = arg();
            const uint8_t set = arg();
            uint16_t& reg = image_[c < kPcmA ? kNr50 : kSoundcntH];
            reg = uint16_t((((keep << 8) | 0xFF) & reg) | (set << 8));
            break;
        }

    case 0xD2:
        ch.envelope_on = 1;
        for (uint8_t& e : ch.envelope)
        {
            e = arg();
        }
        break;

    case 0xD3:
        ch.envelope_on = 0;
        break;

    case 0xD4:
        ch.slide_next = 1;
        break;

    case 0xD5:
        ch.detune = uint16_t(int8_t(arg()));
        if (c >= kPcmA)
        {
            ch.detune = uint16_t(int16_t(ch.detune) * 10);
        }
        break;

    case 0xD6:
        if (ch.repeat_depth >= ch.repeat_start.size())
        {
            Warn("channel " + std::to_string(c) + " nests its repeats too deeply");
            break;
        }

        ch.repeat_start[ch.repeat_depth] = ch.position;
        ch.repeat_count[ch.repeat_depth] = 0;
        ch.repeat_depth++;
        break;

    case 0xD7:
        {
            if (ch.repeat_depth == 0)
            {
                Warn("channel " + std::to_string(c) + " ends a repeat it didn't start");
                ch.position++;
                break;
            }

            uint8_t& count = ch.repeat_count[ch.repeat_depth - 1];
            if (count == 0)
            {
                count = rom_.U8(ch.position);
            }

            count--;
            if (count == 0)
            {
                ch.position++;
                ch.repeat_depth--;
            }
            else
            {
                ch.position = ch.repeat_start[ch.repeat_depth - 1];
                jumped_[size_t(c)] = true;
            }
            break;
        }

    case 0xDA:
        LoadWave(ch, arg());
        break;

    case 0xDB:
        SelectMacro(ch, arg());
        break;

    case 0xDC:
    case 0xDD:
        SelectSample(c, arg());
        ch.pcm_loop = 0;
        ch.pcm_end = 0;
        ch.pcm_start = 0;
        break;

    case 0xDE:
        ch.switch_delay = arg();
        ch.switch_value = arg();
        ch.switch_to = arg();
        break;

    case 0xDF:
        ch.lfo_on = 1;
        ch.lfo_delay = arg();
        ch.lfo_period = arg();
        ch.lfo_depth = arg();
        break;

    case 0xE0:
        ch.lfo_on = 0;
        break;

    case 0xE1:
    case 0xEB:
        ch.switch_delay = 0;
        break;

    case 0xE2:
        ch.bend_on = 1;
        for (int8_t& b : ch.bend)
        {
            b = int8_t(arg());
        }
        break;

    case 0xE3:
        ch.bend_on = 0;
        break;

    case 0xE4:
        ch.transpose = int8_t(arg());
        break;

    case 0xE5:
        ch.slide_frames = arg();
        break;

    case 0xE6:
        {
            const uint8_t low = arg();
            ch.detune = uint16_t(low | arg() << 8);
            break;
        }

    case 0xE7:
        ch.pcm_start = arg();
        ch.pcm_end = arg();
        ch.pcm_loop = arg();
        break;

    case 0xE8:
        Write(kSoundbias, 2, uint32_t(arg() << 14) | (soundbias_ & 0x3FFF));
        break;

    case 0xE9:
        ch.transpose2 = int8_t(arg());
        break;

    case 0xEA:
        ch.switch_delay = arg();
        ch.switch_value = 0;
        ch.switch_to = 1;
        break;

    case 0xEC:
        ch.lfo_shape = arg();
        break;

    case 0xED:
        ch.lfo_shape = uint8_t((ch.lfo_shape & 0x7F) | arg());
        break;

    case 0xEE:
        ch.transpose++;
        break;

    case 0xEF:
        ch.transpose--;
        break;

    case 0xF1:
        {
            // Add to the volume, keeping it from 0 to 4 on the wave channel in both revisions, to 1 on a PCM channel
            // and to 15 on the others.
            ch.volume = uint8_t(ch.volume + arg());
            const int8_t v = int8_t(ch.volume);
            const int8_t limit = c == kWave ? 4 : c >= kPcmA ? 1 : 15;
            if (v < 0)
            {
                ch.volume = 0;
            }
            else if (v > limit)
            {
                ch.volume = uint8_t(limit);
            }
            break;
        }

    case 0xF0:
        // In the J revision, the noise channel switches between its banks of noise macros, and the others between their
        // banks of samples.
        if (traits_.banks)
        {
            (c == kNoise ? ch.macro_bank : ch.sample_bank) ^= 1;
        }
        break;

    default:
        // D1, D8, D9 and F2 to FE do nothing as commands, and neither does F0 in the A revision.
        break;
    }
}

void Sequencer::AddEvent(int c, Event::Kind kind, int index, uint32_t address)
{
    Event e;
    e.kind = kind;
    e.channel = uint8_t(c);
    e.index = index;
    e.tick = ticks_[size_t(c)];
    e.frame = frame_;
    e.address = address;
    events_.push_back(e);
}

void Sequencer::Audition(int channel, uint8_t instrument, int index, uint8_t volume)
{
    if (!valid_)
    {
        throw std::logic_error("the song's header wasn't read");
    }
    if (channel < 0 || channel >= kChannels)
    {
        throw std::out_of_range("no channel " + std::to_string(channel));
    }
    if (index < 0 || index >= (channel >= kPcmA ? kPcmPitchEntries : kFrequencyEntries))
    {
        throw std::out_of_range("no pitch " + std::to_string(index));
    }

    // The channel never reads its data: any address keeps it going.
    for (int c = 0; c < kChannels; c++)
    {
        channels_[size_t(c)].position = c == channel ? std::max<uint32_t>(channels_[size_t(c)].position, 1) : 0;
    }
    audition_ = {channel, true, false, instrument, index, volume};
}

void Sequencer::ReleaseAudition()
{
    audition_.release = audition_.channel >= 0;
}

void Sequencer::RunAudition(int c)
{
    // The channel waits for nothing: the countdown never runs out.
    Channel& ch = channels_[size_t(c)];
    ch.countdown = 0x7FFF;
    if (audition_.release)
    {
        ch.loop = 0;
        FollowEnd(c);
        UpdateImage(c);
        audition_ = {};
        return;
    }
    if (!audition_.start)
    {
        return;
    }

    // The commands that select the instrument and the volume, then the note.
    switch (c)
    {
    case kWave:
        LoadWave(ch, audition_.instrument);
        break;

    case kNoise:
        SelectMacro(ch, audition_.instrument);
        break;

    case kPcmA:
    case kPcmB:
        SelectSample(c, audition_.instrument);
        ch.pcm_loop = 0;
        ch.pcm_end = 0;
        ch.pcm_start = 0;
        break;

    default:
        ch.duty = audition_.instrument;
        ch.switch_delay = 0;
        break;
    }
    ch.volume = audition_.volume;
    ch.transpose = 0;
    ch.transpose2 = 0;
    ch.octave = uint8_t(audition_.index / 12);
    const int note = audition_.index % 12;
    StartNote(c, note);
    RestartEffects(ch, note);
    AddEvent(c, Event::kNote, NoteIndex(ch, note, c >= kPcmA ? kPcmPitchEntries : kFrequencyEntries));
    UpdateImage(c);
    audition_.start = false;
}

void Sequencer::RecordRead(int c, uint32_t address)
{
    // The channel reads on from its last command, unless it went back or waited. At its loop point, FF reads again each
    // frame.
    if (!record_)
    {
        return;
    }

    size_t& stretch = stretch_[size_t(c)];
    bool& jumped = jumped_[size_t(c)];
    bool& split = split_[size_t(c)];
    if (stretch && !jumped && !split)
    {
        stretches_[stretch - 1].last = address;
        return;
    }

    TrackStretch s;
    s.channel = uint8_t(c);
    s.frame = frame_;
    s.tick = ticks_[size_t(c)];
    s.first = address;
    s.last = address;
    s.depth = channels_[size_t(c)].repeat_depth;
    s.loop = stretch != 0;
    if (stretch && !jumped)
    {
        s.loop = stretches_[stretch - 1].loop;
        s.split = split;
    }
    stretches_.push_back(s);
    stretch = stretches_.size();
    jumped = false;
    split = false;
    AddEvent(c, Event::kStretch, int(stretch - 1), address);
}

void Sequencer::RecordVolume(int c, uint8_t before)
{
    const uint8_t volume = channels_[size_t(c)].volume;
    if (record_ && volume != before)
    {
        AddEvent(c, Event::kVolume, volume);
    }
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace ntdlib::quintet
