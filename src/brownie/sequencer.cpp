// SPDX-License-Identifier: MIT

#include "ntdlib/brownie/sequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/song.h"
#include "ntdlib/wave/sinc.h"

#include "sequencer_internal.h"

namespace ntdlib::brownie
{

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int song)
    : rom_(rom), info_(info), layout_(LayoutOf(info.revision)), vacation_(info.revision == Revision::kMagicalVacation)
{
    if (vacation_)
    {
        // The Magical Vacation revision's init empties the FIFOs, sets the mix, sets SOUNDCNT_L before it turns the
        // sound off and on, which clears it again, clears the PSG channels' registers, writes 0x8000 to 0x0400007A
        // rather than to NR44, and stops the timers. The channels and the queue start clear.
        Write(kIoSoundcntH, 2, 0xC80E);
        Write(kIoSoundcntH, 2, 0x730E);
        Write(kIoSoundcntL, 2, 0x77);
        Set16(kVacationSoundcntLCopy, 0x77);
        Write(kIoNr52, 2, 0);
        Write(kIoNr52, 2, 0x8F);
        for (const uint32_t address :
             {kIoControlRegisters[0], kIoControlRegisters[1], kIoControlRegisters[3], kIoControlRegisters[2], kIoNr10})
        {
            Write(address, 2, 0);
        }
        for (const uint32_t address :
             {kIoFrequencyRegisters[0], kIoFrequencyRegisters[1], kIoFrequencyRegisters[2], 0x0400007Au})
        {
            Write(address, 2, 0x8000);
        }
        Write(kIoTimer0Control, 2, 0);
        Write(kIoTimer1Control, 2, 0);
        Set32(kVacationMaster, 0xFF);
    }
    else
    {
        // The init resets the sound, the channels, the queue, the voices and the master level, and stops the FIFOs.
        Write(kIoNr52, 2, 0);
        Write(kIoNr52, 2, 0x8F);
        Write(kIoTimer0Control, 2, 0);
        Write(kIoSoundcntH, 2, 0x8802);
        Write(kIoSoundcntH, 2, 0x210E);
        Write(kIoSoundcntL, 2, 0x77);
        Set16(kSoundcntLCopy, 0x77);
        Set32(kMaster, 0xFF);
        Write(kIoDma1Control, 2, 0);
        Write(kIoDma2Control, 2, 0);
    }

    mix_cycles_ = uint64_t(kMixPoints) * info.point_cycles;
    std::vector<SongChannel> channels;
    valid_ = mix_cycles_ > 0 && ReadSong(rom, info, song, channels);
    if (valid_)
    {
        Request(song);
    }
}

std::array<uint8_t, 0x38> Sequencer::RecordBytes(const Channel& c)
{
    std::array<uint8_t, 0x38> b = {};
    b[0x00] = c.flags;
    b[0x01] = c.wait;
    b[0x02] = c.step_frames;
    b[0x03] = c.loop_count;
    xsflib::StoreLe32(&b[0x04], c.position);
    xsflib::StoreLe32(&b[0x08], c.loop_start);
    xsflib::StoreLe32(&b[0x0C], c.return_to);
    xsflib::StoreLe32(&b[0x10], c.subroutines);
    xsflib::StoreLe32(&b[0x14], c.samples);
    xsflib::StoreLe32(&b[0x18], c.sample_end);
    xsflib::StoreLe32(&b[0x1C], c.wave);
    xsflib::StoreLe16(&b[0x20], c.frequency);
    xsflib::StoreLe16(&b[0x22], c.control);
    b[0x24] = c.duty;
    b[0x25] = c.sweep;
    b[0x26] = c.volume;
    b[0x27] = c.detune;
    b[0x28] = c.level;
    b[0x29] = c.length;
    b[0x2A] = c.pan_on;
    b[0x2B] = c.pan_off;
    b[0x2C] = c.transpose;
    b[0x2D] = c.ramp;
    b[0x2E] = c.ramp_frames;
    b[0x2F] = c.length_set;
    xsflib::StoreLe32(&b[0x30], c.envelope);
    xsflib::StoreLe32(&b[0x34], c.step);

    return b;
}

std::array<uint8_t, 0x10> Sequencer::VoiceBytes(const Voice& v)
{
    std::array<uint8_t, 0x10> b = {};
    b[0] = v.mode;
    b[1] = v.mode2;
    b[2] = v.right;
    b[3] = v.left;
    xsflib::StoreLe32(&b[4], v.point);
    xsflib::StoreLe32(&b[8], v.end);
    xsflib::StoreLe32(&b[12], v.loop);

    return b;
}

std::array<uint8_t, 0x10> Sequencer::VoiceRecord(int v) const
{
    if (!vacation_)
    {
        return VoiceBytes(voices_[size_t(v)]);
    }

    const FifoVoice& f = fifo_voices_[size_t(v)];
    std::array<uint8_t, 0x10> b = {};
    xsflib::StoreLe32(&b[0], f.point);
    xsflib::StoreLe32(&b[4], f.end);
    xsflib::StoreLe32(&b[8], f.loop);
    xsflib::StoreLe32(&b[12], f.volume);

    return b;
}

const std::vector<RegisterWrite>& Sequencer::Step()
{
    writes_.clear();
    events_.clear();
    reads_.clear();
    fifo_bytes_[0].clear();
    fifo_bytes_[1].clear();
    timer_changes_.clear();
    returned_ = false;
    restarted_ = 0;
    r5_ = 0;
    if (metering_)
    {
        voice_peaks_ = {};
    }

    // The per-frame routine: the channels, then the fades and a request, unless E3 has ended it. Only the game's fade
    // out routine starts a fade in the Magical Vacation revision, which has no E3, so the model has no fades for it.
    now_ = uint64_t(frame_) * kFrameCycles;
    RunChannels();
    if (!returned_)
    {
        if (!vacation_)
        {
            Fade();
        }
        Requests();
    }

    if (vacation_)
    {
        fifos_before_ = fifos_;
        fifo_voices_before_ = fifo_voices_;
        MixEnhanced();
        RunFifos();
        frame_++;

        return writes_;
    }

    // The FIFOs play while their DMA and Timer 0 run, and the mixer makes 16 more points of each whenever they've
    // played the last 16, counting from the frame they started in.
    voices_before_mix_ = voices_;
    if ((dma_control_ & 0x8000) && (timer_control_ & kTimerOn))
    {
        if (mix_start_ < 0)
        {
            mix_start_ = int64_t(frame_);
            mixed_ = 0;
        }

        const uint64_t due = (uint64_t(frame_) - uint64_t(mix_start_) + 1) * kFrameCycles / mix_cycles_;
        for (; mixed_ < due; mixed_++)
        {
            Mix();
        }
    }
    else
    {
        mix_start_ = -1;
    }

    MixEnhanced();
    frame_++;

    return writes_;
}

bool Sequencer::Playing() const
{
    for (int c = 0; c < layout_.channels; c++)
    {
        if (channels_[size_t(c)].flags & kFlagOn)
        {
            return true;
        }
    }

    return vacation_ ? fifos_[0].running || fifos_[1].running : mix_start_ >= 0;
}

// The per-frame routine's loop over the channels: each one counts down its wait, and reads its data when the wait runs
// out; otherwise it counts down to its envelope's next step and, on a sample channel, to its level's next step.
void Sequencer::RunChannels()
{
    for (int c = 0; c < layout_.channels && !returned_; c++)
    {
        Channel& ch = channels_[size_t(c)];
        if (!(ch.flags & kFlagOn))
        {
            continue;
        }

        const uint8_t wait = uint8_t(ch.wait - 1);
        if (wait == 0)
        {
            ReadChannel(c);
            continue;
        }

        ch.wait = wait;
        if (ch.step_frames != 0)
        {
            const uint8_t frames = uint8_t(ch.step_frames - 1);
            if (frames == 0)
            {
                if (c < kFirstSampleChannel)
                {
                    PsgEnvelopeStep(c);
                }
                else if (vacation_)
                {
                    FifoEnvelopeStep(c);
                }
                else
                {
                    SampleEnvelopeStep(c);
                }
                continue;
            }

            ch.step_frames = frames;
        }

        if (c >= kFirstSampleChannel && ch.RampWait() != 0)
        {
            const uint8_t frames = uint8_t(ch.RampWait() - 1);
            if (frames == 0)
            {
                if (vacation_)
                {
                    FifoRampStep(c);
                }
                else
                {
                    RampStep(c);
                }
                continue;
            }

            ch.SetRampWait(frames);
        }
    }
}

// FF: the channel ends. A PSG channel falls silent, unless a sound effect has it; a sound effect gives its PSG channel
// back to the music, which plays on as it was; and a sample channel's voice falls silent, which stops the FIFOs if
// voices 0 and 1 seem to be silent. In the Magical Vacation revision, a sample channel stops its FIFO's timer, unless a
// sound effect has the FIFO, and a sound effect's sample channel gives the FIFO back to the music's, whose sample stays
// stopped until its next note.
void Sequencer::EndChannel(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.flags = uint8_t(ch.flags & ~kFlagOn);
    if (c >= kFirstSampleChannel && vacation_)
    {
        if (c >= kFirstEffectSampleChannel)
        {
            Channel& music = channels_[size_t(c - (kFirstEffectSampleChannel - kFirstSampleChannel))];
            music.flags = uint8_t(music.flags & ~kFlagEffect);
        }
        else if (ch.flags & kFlagEffect)
        {
            return;
        }

        Write(c & 1 ? kIoTimer1Control : kIoTimer0Control, 2, 0);
        StopEnhanced(c & 1);
        return;
    }
    if (c >= kFirstSampleChannel)
    {
        voices_[size_t(c & 7)] = Voice();
        StopEnhanced(c & 7);

        // The driver tests only the low byte of the two voices' next points.
        if (((voices_[0].point | voices_[1].point) & 0xFF) == 0)
        {
            Write(kIoTimer0Control, 2, 0);
            Write(kIoDma1Control, 2, 0);
            Write(kIoDma2Control, 2, 0);
        }
        r5_ = voices_[1].point;
        return;
    }

    if (c >= kFirstEffectChannel)
    {
        Channel& music = channels_[size_t(c - kFirstEffectChannel)];
        music.flags = uint8_t(music.flags & ~kFlagEffect);
        if (music.flags & kFlagOn)
        {
            if ((c & 3) == kWaveChannel)
            {
                LoadWave(music.wave);
            }

            const uint32_t mask = (uint32_t(music.pan_off) << 8) + 0xFF;
            const uint32_t value = (Get16(SoundcntLCopy()) & mask) | (uint32_t(music.pan_on) << 8);
            Set16(SoundcntLCopy(), value);
            Write(kIoSoundcntL, 2, value);
            r5_ = vacation_ ? mask : music.control;
            Write(ControlRegister(c), 2, music.control);
            Write(FrequencyRegister(c), 2, music.frequency ^ 0x8000u);
            if (c == kFirstEffectChannel)
            {
                Write(kIoNr10, 2, music.sweep);
            }
            return;
        }
    }
    else if (ch.flags & kFlagEffect)
    {
        return;
    }

    Write(ControlRegister(c), 2, 0);
    Write(FrequencyRegister(c), 2, 0x8000);
}

// Copies a wave's 16 bytes to the wave RAM, with the wave channel switched to the other bank while it does.
void Sequencer::LoadWave(uint32_t wave)
{
    Write(kIoNr30, 2, 0x40);
    for (uint32_t i = 0; i < 16; i += 2)
    {
        Write(kIoWaveRam + i, 2, rom_.U16(wave + i));
    }
    Write(kIoNr30, 2, 0x80);
}

// E3: the sound comes back after a fade out, fading in over 64 frames, and the driver queues sound 0x36, through a
// different wrap of the queue's index, and ends its routine for the frame.
void Sequencer::Resume()
{
    Set8(kMaster, 0);
    Set16(kFadeInFrames, 0x40);
    Set8(kFadeStep, 4);
    Set16(kFadeInTimer, 0x100);
    Set16(kFadeInPeriod, 0x100);
    Write(kIoSoundcntL, 2, 0);
    for (Channel& ch : channels_)
    {
        ch.flags = (ch.flags & (kFlagOn | kFlagPaused)) ? kFlagOn : 0;
    }
    Write(kIoTimer0Control, 2, kTimerOn);

    const uint8_t at = Get8(kQueueWrite);
    Set16(kQueue + at, kResumeSound);
    Set8(kQueueWrite, uint8_t((at + 2) & 7));
    returned_ = true;
}

// The fade in, or else the fade out, takes its next step when its timer runs out. Only the game's fade routine sets
// kFadeOutFrames. Neither the model nor brownie::GameDriver runs that routine. The model never fades out.
void Sequencer::Fade()
{
    if (Get8(kFadeInFrames) != 0)
    {
        const uint8_t frames = uint8_t(Get8(kFadeInFrames) - 1);
        Set8(kFadeInFrames, frames);
        if (frames != 0)
        {
            FadeStep(kFadeInTimer, kFadeInPeriod, true);
            return;
        }

        // The fade in's end sets NR50 to full, and writes 0xFF to address 0, where it does nothing.
        const uint32_t value = (Get16(kSoundcntLCopy) & 0xFF00) | 0x77;
        Set16(kSoundcntLCopy, value);
        Write(kIoSoundcntL, 2, value);
        return;
    }

    if (Get16(kFadeOutFrames) != 0)
    {
        const uint16_t frames = uint16_t(Get16(kFadeOutFrames) - 1);
        Set16(kFadeOutFrames, frames);
        if (frames != 0)
        {
            FadeStep(kFadeOutTimer, kFadeOutPeriod, false);
        }
        else
        {
            StopAll();
        }
    }
}

// A step of a fade: the master level goes up or down by the step, and so do NR50 and the sample channels' voices.
void Sequencer::FadeStep(int timer, int period, bool up)
{
    const uint32_t count = uint32_t(Get16(timer)) - 0x100;
    if (count & 0xFF00)
    {
        Set16(timer, count);
        return;
    }

    Set16(timer, count + Get16(period));
    int master = Get8(kMaster);
    const int step = Get8(kFadeStep);
    master = up ? (master + step > 0xFF ? 0xFF : master + step) : (master < step ? 0 : master - step);
    Set8(kMaster, uint32_t(master));

    const uint32_t nr50 = uint32_t(master >> 5) * 0x11;
    const uint32_t value = (Get16(kSoundcntLCopy) & 0xFF00) | nr50;
    Set16(kSoundcntLCopy, value);
    Write(kIoSoundcntL, 2, value);
    for (int v = 0; v < kVoiceCount; v++)
    {
        const Channel& ch = channels_[size_t(kFirstSampleChannel + v)];
        if (ch.flags & kFlagOn)
        {
            SetNoteSides(voices_[size_t(v)], ch, uint32_t(master));
        }
    }
}

// The end of a fade out: the sound and the FIFOs stop, and each channel in use is marked to come back with E3.
void Sequencer::StopAll()
{
    Set8(kMaster, 0);
    Write(kIoNr52, 2, 0);
    Write(kIoNr52, 2, 0x8F);
    Write(kIoTimer0Control, 2, 0);
    Write(kIoSoundcntH, 2, 0x8802);
    Write(kIoSoundcntH, 2, 0x210E);
    Write(kIoSoundcntL, 2, 0x77);
    Set16(kSoundcntLCopy, 0x77);

    // The driver clears the queue's indexes and first four requests with the last record's new flags.
    uint32_t flags = 0;
    for (Channel& ch : channels_)
    {
        flags = (ch.flags & (kFlagOn | kFlagPaused)) ? kFlagPaused : 0;
        ch.flags = uint8_t(flags);
    }
    Set16(kQueueRead, flags);
    Set32(kQueue, flags);
    Set32(kQueue + 4, flags);
    Set32(kMaster, 0xFF);
}

// Starts the next request in the queue, if there is one. The Magical Vacation revision's queue holds 4 requests, and it
// starts the next one if the queue's offsets differ, whatever it is.
void Sequencer::Requests()
{
    const uint8_t read = Get8(kQueueRead);
    if (vacation_)
    {
        if (read == Get8(kQueueWrite))
        {
            return;
        }

        const uint16_t sound = Get16(kQueue + read);
        Set16(kQueue + read, 0);
        Set8(kQueueRead, uint8_t((read + 2) & layout_.queue_mask));
        StartSound(sound);
        return;
    }

    const uint16_t sound = Get16(kQueue + read);
    if (sound == 0)
    {
        if (read != Get8(kQueueWrite))
        {
            Set32(kQueueRead, 0);
        }
        return;
    }

    Set16(kQueue + read, 0);
    Set8(kQueueRead, uint8_t((read + 2) & layout_.queue_mask));
    if (sound == kNoiseSound1 || sound == kNoiseSound2)
    {
        // These wait for the noise channel to be silent, or to have a few frames left of its note.
        const uint32_t nr41 = ControlRegister(kNoiseChannel) - kIoNr10;
        const uint32_t value = nr41 + 1 < registers_.size() ? xsflib::LoadLe16(&registers_[nr41]) : 0;
        const Channel& noise = channels_[kNoiseChannel];
        if ((value & 0xF000) || ((noise.flags & ~kFlagPaused) && noise.wait < 3))
        {
            return;
        }
    }

    StartSound(sound);
}

// Starts each channel that a sound's entry lists, with its data, its subroutines and the channel's pan, and gives each
// sound effect's PSG channel to the sound effect.
void Sequencer::StartSound(int sound)
{
    uint32_t at = SongAddress(rom_, info_, sound);
    for (int n = 0; n < kMaxSoundChannels; n++, at += 12)
    {
        const uint32_t c = rom_.U32(at);
        if (c == 0xFF)
        {
            return;
        }
        if (c >= uint32_t(layout_.records))
        {
            Warn("sound " + std::to_string(sound) + " lists channel " + std::to_string(c) + ", which doesn't exist");
            return;
        }

        Channel& ch = channels_[c];
        ch.flags = uint8_t(ch.flags | kFlagOn);
        ch.position = rom_.U32(at + 4);
        ch.subroutines = rom_.U32(at + 8);
        ch.duty = ch.sweep = ch.volume = ch.detune = 0;
        ch.level = ch.length = ch.pan_on = ch.pan_off = 0;
        ch.transpose = ch.ramp = ch.ramp_frames = ch.length_set = 0;
        ch.wait = 1;
        ch.pan_on = rom_.U8(info_.pan_table + c);
        ch.pan_off = rom_.U8(info_.pan_table + c + 12);
        if (c >= uint32_t(kFirstEffectChannel) && c < uint32_t(kFirstSampleChannel))
        {
            Channel& music = channels_[c - kFirstEffectChannel];
            music.flags = uint8_t(music.flags | kFlagEffect);
        }
        else if (vacation_ && c >= uint32_t(kFirstEffectSampleChannel))
        {
            Channel& music = channels_[c - (kFirstEffectSampleChannel - kFirstSampleChannel)];
            music.flags = uint8_t(music.flags | kFlagEffect);
        }
    }

    Warn("sound " + std::to_string(sound) + " lists too many channels");
}

// The DMA 1 or DMA 2 interrupt in the Magical Vacation revision, which gives FIFO x the next 4 points of its sample,
// scaled by its volume. At the end of the sample, it goes on from the loop, or at the end of one that doesn't loop, it
// stops the timer and gives the FIFO nothing more.
void Sequencer::Interrupt(int x)
{
    FifoVoice& v = fifo_voices_[size_t(x)];
    const uint32_t word = LoadWord(v.point);
    v.point += 4;
    if (v.point == v.end)
    {
        if (v.end == v.loop)
        {
            Write(x ? kIoTimer1Control : kIoTimer0Control, 2, 0);
            return;
        }
        v.point = v.loop;
    }

    // The routine scales each pair of points by (volume + 1) / 256. FIFO A's routine reads the volume's low byte, and
    // FIFO B's its whole word. A positive point rounds to nearest, half up. A negative point's size rounds the same
    // way, and comes out negative. That negative carries into the pair's second point, which comes out a step lower.
    const uint32_t scale = (x == 0 ? (v.volume & 0xFF) : v.volume) + 1;
    const auto pair = [scale](uint32_t points)
    {
        const uint32_t low = points & 0xFF;
        uint32_t first = (low & 0x80 ? (0u - low) & 0xFF : low) * scale;
        first += first & 0x80 ? 0x100 : 0;
        first = low & 0x80 ? 0u - first : first;

        const uint32_t high = points & 0xFF00;
        uint32_t second = (high & 0x8000 ? (0u - high) & 0xFF00 : high) * scale;
        second += second & 0x8000 ? 0x10000 : 0;
        second = high & 0x8000 ? 0u - second : second;
        return ((second >> 8) & 0xFF00) + (first >> 8);
    };

    const uint32_t fifo = kIoFifoA + 4 * uint32_t(x);
    Write(fifo, 2, pair(word));
    Write(fifo + 2, 2, pair(word >> 16));
}

void Sequencer::Request(int sound)
{
    const uint8_t at = Get8(kQueueWrite);
    Set16(kQueue + at, uint32_t(sound));
    Set8(kQueueWrite, uint8_t((at + 2) & layout_.queue_mask));
}

uint8_t Sequencer::Get8(int offset) const
{
    return offset >= 0 && size_t(offset) < globals_.size() ? globals_[size_t(offset)] : 0;
}

uint16_t Sequencer::Get16(int offset) const
{
    return uint16_t(Get8(offset) | (Get8(offset + 1) << 8));
}

void Sequencer::Set8(int offset, uint32_t value)
{
    if (offset >= 0 && size_t(offset) < globals_.size())
    {
        globals_[size_t(offset)] = uint8_t(value);
    }
    else
    {
        Warn("the driver writes past its variables");
    }
}

void Sequencer::Set16(int offset, uint32_t value)
{
    Set8(offset, value);
    Set8(offset + 1, value >> 8);
}

void Sequencer::Set32(int offset, uint32_t value)
{
    Set16(offset, value);
    Set16(offset + 2, value >> 16);
}

void Sequencer::Write(uint32_t address, int size, uint32_t value)
{
    value &= size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (address >= kIoNr10 && address + uint32_t(size) <= kIoSoundEnd)
    {
        writes_.push_back({address, uint8_t(size), value});
        for (int i = 0; i < size; i++)
        {
            registers_[address - kIoNr10 + uint32_t(i)] = uint8_t(value >> (8 * i));
        }

        // A write to NRx3 and NRx4 with bit 15 set starts a PSG channel again, and SOUNDCNT_H's bits 11 and 15 empty
        // FIFO A and FIFO B.
        for (int c = 0; c < 4; c++)
        {
            if (address == kIoFrequencyRegisters[c] && size == 2 && (value & 0x8000))
            {
                restarted_ = uint8_t(restarted_ | (1 << c));
            }
        }
        if (address == kIoSoundcntH && size == 2)
        {
            if (value & 0x0800)
            {
                fifos_[0].count = 0;
            }
            if (value & 0x8000)
            {
                fifos_[1].count = 0;
            }
        }
    }
    else if (address >= kIoFifoA && address < kIoFifoA + 8)
    {
        const int x = int((address - kIoFifoA) / 4);
        Fifo& f = fifos_[size_t(x)];
        for (int i = 0; i < size && f.count < kFifoBytes; i++)
        {
            f.count++;
            KeepFifoByte(x, uint8_t(value >> (8 * i)));
        }
    }
    else if (address == kIoTimer0Control || address == kIoTimer1Control)
    {
        // A timer that starts runs from its reload value, so that its first overflow comes a period later.
        const int x = address == kIoTimer1Control ? 1 : 0;
        Fifo& f = fifos_[size_t(x)];
        if (x == 0)
        {
            timer_control_ = uint16_t(value);
        }
        const uint64_t frame_start = uint64_t(frame_) * kFrameCycles;
        if ((value & kTimerOn) && !f.running)
        {
            f.running = true;
            f.next = now_ + Period(x);
            const uint32_t period = vacation_ ? uint32_t(Period(x)) : info_.point_cycles;
            timer_changes_.push_back({uint32_t(now_ - frame_start), x, period, period});
        }
        else if (!(value & kTimerOn))
        {
            if (f.running)
            {
                timer_changes_.push_back({uint32_t(now_ - frame_start), x, 0, 0});
            }
            f.running = false;
        }
    }
    else if (address == kIoTimer0Reload || address == kIoTimer1Reload)
    {
        fifos_[address == kIoTimer1Reload ? 1 : 0].reload = uint16_t(value);
    }
    else if (address == kIoDma1Control || address == kIoDma2Control)
    {
        if (address == kIoDma1Control)
        {
            dma_control_ = uint16_t(value);
        }
        fifos_[address == kIoDma2Control ? 1 : 0].dma = (value & 0x8000) != 0;
    }
}

void Sequencer::KeepFifoByte(int x, uint8_t value)
{
    if (keep_bytes_)
    {
        const int owner = fifos_[size_t(x)].owner;
        const bool muted = owner >= 0 && (muted_channels_ >> owner & 1);
        fifo_bytes_[size_t(x)].push_back(muted ? int8_t(0) : int8_t(value));
        if (metering_ && !muted && owner >= 0 && !enhanced_)
        {
            MeterPart(owner, x, std::fabs(float(int8_t(value))));
        }
    }
}

uint32_t Sequencer::ControlRegister(int c) const
{
    return rom_.U32(info_.control_registers + 4 * uint32_t(c));
}

uint32_t Sequencer::FrequencyRegister(int c) const
{
    return rom_.U32(info_.frequency_registers + 4 * uint32_t(c));
}

void Sequencer::Visit(int c, uint32_t address)
{
    visited_[size_t(c)].emplace(address, frame_);
}

void Sequencer::AddEvent(int c, Event::Kind kind, uint8_t key)
{
    Event e;
    e.kind = kind;
    e.channel = uint8_t(c);
    e.key = key;
    e.frame = frame_;
    e.command = reading_;
    events_.push_back(e);
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace ntdlib::brownie
