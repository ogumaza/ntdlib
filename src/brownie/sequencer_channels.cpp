// SPDX-License-Identifier: MIT

// The channels of the Brownie Brown sequencer: the reading of their commands, their notes on the PSG, sample and FIFO
// voices, and the steps of their envelopes and ramps.

#include <cstddef>
#include <cstdint>
#include <string>

#include "sequencer_internal.h"

namespace ntdlib::brownie
{

// Reads the channel's data up to its next note, rest or hold, or a command that ends its frame.
void Sequencer::ReadChannel(int c)
{
    Channel& ch = channels_[size_t(c)];
    uint32_t at = ch.position;
    for (int n = 0; n < kMaxCommands; n++)
    {
        Visit(c, at);
        reading_ = at;
        if (keep_reads_)
        {
            reads_.push_back({uint8_t(c), frame_, at});
        }

        const uint8_t b = rom_.U8(at++);
        if (b < 0x80)
        {
            ch.position = at;
            if (KindOf(info_.revision, c) == ChannelKind::kNoise)
            {
                // A noise note's byte gives NR43, without the transpose.
                AddEvent(c, Event::kNote, b);
                PsgNote(c, NoiseNr43(b));
                return;
            }

            const int key = ((ch.transpose & 0x80) ? b - (0x100 - ch.transpose) : b + ch.transpose) & 0x7F;
            AddEvent(c, Event::kNote, uint8_t(key));
            if (c >= kFirstSampleChannel)
            {
                events_.back().samples = ch.samples;
                if (vacation_)
                {
                    FifoNote(c, key);
                }
                else
                {
                    SampleNote(c, key);
                }
            }
            else
            {
                PsgNote(c, uint32_t(rom_.U16(info_.frequency_table + 2 * uint32_t(key))) + ch.detune);
            }
            return;
        }
        if (b < 0xE0)
        {
            // The Magical Vacation revision has no length sets. There, LengthSet() gives the group of 16 lengths, one
            // for each tempo, that the command reads from.
            ch.length = NoteLength(rom_, info_, b, ch.length_set);
            length_groups_[size_t(c)] = uint8_t((b & 0x7F) >> 4);
            continue;
        }

        if (!RunCommand(c, b, at))
        {
            return;
        }
    }

    Warn("channel " + std::to_string(c) + " reads commands without reaching a note, so it was ended");
    ch.flags = uint8_t(ch.flags & ~kFlagOn);
    AddEvent(c, Event::kEnd);
}

// Runs command `command` of channel `c`, whose next byte is at `at`. Returns false if the command ends the channel's
// reading for the frame.
bool Sequencer::RunCommand(int c, uint8_t command, uint32_t& at)
{
    // In the Magical Vacation revision, E2-E9 are F2-F9 again, and EF is FF. A command that does nothing has no
    // argument either.
    command = CommandOf(info_.revision, command);
    if (IsNoOp(info_.revision, command))
    {
        return true;
    }

    Channel& ch = channels_[size_t(c)];
    switch (command)
    {
    case 0xE0:
        {
            // The release: the envelope goes on from the release envelope at the next frame.
            const uint8_t index = rom_.U8(at++);
            ch.step = rom_.U32(info_.envelope_table + 4 * uint32_t(index));
            ch.step_frames = 1;
            ch.position = at;
            ch.wait = ch.length;
            AddEvent(c, Event::kRelease);
            return false;
        }

    case 0xE1:
        {
            // A sample set, or in the Magical Vacation revision a sample: its start, end and loop.
            const uint32_t entry = rom_.U32(info_.sample_table + 4 * uint32_t(rom_.U8(at++)));
            if (!vacation_)
            {
                ch.samples = entry;
                return true;
            }

            ch.SetSampleStart(rom_.U32(entry));
            ch.sample_end = rom_.U32(entry + 4);
            ch.SetSampleLoop(rom_.U32(entry + 8));
            return true;
        }

    case 0xE2:
        {
            // The mixer's mode: 0 mixes voices 0 and 1 only, and clears the flags of the other voices' channels and of
            // the 14th record.
            const uint8_t mode = rom_.U8(at++);
            Set8(kMixerMode, mode);
            for (int k = kFirstSampleChannel + kMode0Voices; mode == 0 && k < kRecordCount; k++)
            {
                channels_[size_t(k)].flags = 0;
            }
            return true;
        }

    case 0xE3:
        ch.position = at;
        Resume();
        return false;

    case 0xE8:
    case 0xE9:
        {
            // A sound effect gives the music channel's PSG channel back, or takes it.
            if (c < kFirstEffectChannel)
            {
                Warn("channel " + std::to_string(c) + " has command " + HexByte(command) +
                     ", which changes memory before the channels");
                return true;
            }

            Channel& music = channels_[size_t(c - kFirstEffectChannel)];
            music.flags =
                command == 0xE8 ? uint8_t(music.flags & (kFlagOn | kFlagPaused)) : uint8_t(music.flags | kFlagEffect);
            return true;
        }

    case 0xEA:
        {
            // A sample channel's pan: the right side's level in the low nibble and the left's in the high, each filled
            // out with 0xF unless it's 0.
            const uint8_t pan = rom_.U8(at++);
            const uint32_t right = ((pan & 0x0Fu) << 4) + 0x0F;
            const uint32_t left = (pan & 0xF0u) + 0x0F;
            ch.SetRightLevel(uint8_t((right & 0xF0) ? right : 0));
            ch.SetLeftLevel(uint8_t((left & 0xF0) ? left : 0));
            return true;
        }

    case 0xEC:
        {
            // A sample channel's rest silences its voice, or in the Magical Vacation revision stops the timer of the
            // FIFO of the channel's number & 1, unless a sound effect has the channel's PSG channel or FIFO.
            ch.position = at;
            ch.step_frames = 0;
            ch.wait = ch.length;
            if (vacation_)
            {
                if (!(ch.flags & kFlagEffect))
                {
                    Write(c & 1 ? kIoTimer1Control : kIoTimer0Control, 2, 0);
                    StopEnhanced(c & 1);
                }
                AddEvent(c, Event::kRest);
                return false;
            }

            r5_ = uint32_t(c & 7);
            if ((c & 7) < kVoiceCount)
            {
                voices_[size_t(c & 7)] = Voice();
                StopEnhanced(c & 7);
            }
            else
            {
                Warn("channel " + std::to_string(c) + " has command EC, which changes the mixer's buffers");
            }
            AddEvent(c, Event::kRest);
            return false;
        }

    case 0xED:
        {
            // A PSG channel's rest turns its DAC off, and writes the routine's r5 to the frequency setting.
            ch.position = at;
            ch.step_frames = 0;
            ch.wait = ch.length;
            ch.control = uint16_t(ch.control & 0xFF);
            if (!(ch.flags & kFlagEffect))
            {
                Write(ControlRegister(c), 2, 0);
                r5_ ^= 0x8000;
                Write(FrequencyRegister(c), 2, r5_);
            }
            AddEvent(c, Event::kRest);
            return false;
        }

    case 0xEE:
        ch.length_set = rom_.U8(at++);
        return true;

    case 0xF0:
        ch.envelope = rom_.U32(info_.envelope_table + 4 * uint32_t(rom_.U8(at++)));
        return true;

    case 0xF1:
        ch.duty = rom_.U8(at++);
        return true;

    case 0xF2:
        ch.volume = rom_.U8(at++);
        return true;

    case 0xF3:
        ch.transpose = rom_.U8(at++);
        return true;

    case 0xF4:
        ch.loop_count = rom_.U8(at++);
        ch.loop_start = at;
        return true;

    case 0xF5:
        {
            const int count = ch.loop_count - 1;
            ch.loop_count = uint8_t(count);
            if (count != 0)
            {
                at = ch.loop_start;
            }
            return true;
        }

    case 0xF6:
        {
            const uint32_t target = rom_.U32(ch.subroutines + 4 * uint32_t(rom_.U8(at++)));
            ch.return_to = at;
            at = target;
            return true;
        }

    case 0xF7:
        at = ch.return_to;
        return true;

    case 0xF8:
        {
            // A jump back to a place the channel has read before is its loop.
            at = rom_.U32(ch.subroutines + 4 * uint32_t(rom_.U8(at++)));
            const auto it = visited_[size_t(c)].find(at);
            if (it != visited_[size_t(c)].end())
            {
                AddEvent(c, Event::kLoop);
                events_.back().first = it->second;
            }
            return true;
        }

    case 0xF9:
        ch.detune = rom_.U8(at++);
        return true;

    case 0xFA:
        ch.sweep = rom_.U8(at++);
        return true;

    case 0xFB:
        ch.sweep = 0;
        return true;

    case 0xFC:
        ch.wave = rom_.U32(info_.wave_table + 4 * uint32_t(rom_.U8(at++)));
        if (!(ch.flags & kFlagEffect))
        {
            LoadWave(ch.wave);
            r5_ = 0;
        }
        return true;

    case 0xFD:
        {
            // A PSG channel's pan: its bits of NR51 for the right side in the low nibble and the left in the high,
            // which the driver shifts to the channel's bits.
            uint32_t pan = rom_.U8(at++);
            uint32_t mask = 0x11;
            for (int k = 0; k < (c & 3); k++)
            {
                pan <<= 1;
                mask <<= 1;
            }

            mask ^= 0xFF;
            ch.pan_on = uint8_t(pan);
            ch.pan_off = uint8_t(mask);
            if (!(ch.flags & kFlagEffect))
            {
                mask = (mask << 8) + 0xFF;
                const uint32_t value = (Get16(SoundcntLCopy()) & mask) | (pan << 8);
                Set16(SoundcntLCopy(), value);
                Write(kIoSoundcntL, 2, value);
            }
            r5_ = mask;
            return true;
        }

    case 0xFE:
        ch.position = at;
        ch.wait = ch.length;
        AddEvent(c, Event::kHold);
        return false;

    case 0xFF:
        AddEvent(c, Event::kEnd);
        EndChannel(c);
        return false;

    default:
        // The commands that do nothing have returned above.
        return true;
    }
}

// Starts a PSG note: the frequency setting, the envelope's first step, and the registers, which start the channel.
void Sequencer::PsgNote(int c, uint32_t frequency)
{
    Channel& ch = channels_[size_t(c)];
    ch.frequency = uint16_t(frequency);
    ch.wait = ch.length;
    ch.step_frames = rom_.U8(ch.envelope);
    const uint8_t level = rom_.U8(ch.envelope + 1);
    ch.step = ch.envelope + 2;

    const uint32_t envelope = level < ch.volume ? kSilentNrx2 : uint32_t(level - ch.volume);
    const uint32_t control = ch.duty + (envelope << 8);
    ch.control = uint16_t(control);
    if (!(ch.flags & kFlagEffect))
    {
        if (c % kFirstEffectChannel == 0)
        {
            Write(kIoNr10, 2, ch.sweep);
        }
        Write(ControlRegister(c), 2, control);

        // The Magical Vacation revision flips bit 15 of the frequency setting where the other sets it, and writes it
        // once rather than twice.
        frequency = vacation_ ? frequency ^ 0x8000 : frequency | 0x8000;
        Write(FrequencyRegister(c), 2, frequency);
        if (!vacation_)
        {
            Write(FrequencyRegister(c), 2, frequency);
        }
    }

    r5_ = frequency;
}

// Starts a sample note: the semitone's sample from the channel's set, played at the key's octave, the envelope's first
// step, and the FIFOs if they've stopped.
void Sequencer::SampleNote(int c, int key)
{
    Channel& ch = channels_[size_t(c)];
    key = SampleKey(key);
    const int octave = SampleOctave(key);
    static constexpr uint8_t kModes[4] = {4, 2, 1, 0};
    Voice& v = voices_[size_t(c & 7)];
    v.mode = kModes[octave];
    v.mode2 = v.mode;
    const uint32_t entry = rom_.U32(ch.samples + 4 * uint32_t(key - kOctaveKeys * octave));
    v.point = rom_.U32(entry);
    v.end = rom_.U32(entry + 4);
    v.loop = rom_.U32(entry + 8);
    StartEnhanced(c & 7, v.point, v.end, v.loop);

    // The envelope's first step sets the level, and the voice's sides take it with the master level and the pan.
    ch.wait = ch.length;
    TakeSampleStep(c, ch.envelope);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    const uint32_t master = Get8(kMaster);
    SetNoteSides(v, ch, master);

    // The routine's r5 is left with the volume times the master level plus 1. A note that starts the FIFOs' DMA leaves
    // IE there instead.
    r5_ = ch.EnvelopeVolume() * (master + 1);
    if (!(dma_control_ & kDmaWords))
    {
        Write(kIoDma1Control, 2, 0);
        Write(kIoDma2Control, 2, 0);
        interrupts_ = uint16_t(interrupts_ | kMixerInterrupts);
        r5_ = interrupts_;
        Write(kIoDma1Control, 2, kDmaOn);
        Write(kIoDma2Control, 2, kDmaOn);
    }
    Write(kIoTimer0Control, 2, kTimerOn);
}

// A sample note in the Magical Vacation revision: the timer reload value for the key and the detune, which is signed
// and in 12ths of a semitone, and the envelope's first step. Unless a sound effect has the FIFO, the note stops the
// FIFO's timer, gives the FIFO the sample's first 4 points as they are, sets the FIFO's sample from the channel's, and
// starts the timer again at the new rate, with the FIFO's DMA, whose requests interrupt.
void Sequencer::FifoNote(int c, int key)
{
    Channel& ch = channels_[size_t(c)];
    const int index = key * kRateSteps + int(int8_t(ch.detune));
    ch.SetTimerReload(rom_.U16(info_.rate_table + 2 * uint32_t(index)));
    ch.wait = ch.length;
    TakeFifoStep(c, ch.envelope);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    if (ch.flags & kFlagEffect)
    {
        return;
    }

    const int x = c & 1;
    const uint32_t timer = x ? kIoTimer1Control : kIoTimer0Control;
    Write(timer, 2, 0);
    fifos_[size_t(x)].owner = c;
    Write(kIoFifoA + 4 * uint32_t(x), 2, rom_.U16(ch.SampleStart()));
    Write(kIoFifoA + 4 * uint32_t(x) + 2, 2, rom_.U16(ch.SampleStart() + 2));
    FifoVoice& v = fifo_voices_[size_t(x)];
    v.point = ch.SampleStart() + 4;
    v.end = ch.sample_end;
    v.loop = ch.SampleLoop();
    v.volume = ch.EnvelopeVolume();
    StartEnhanced(x, ch.SampleStart(), v.end, v.loop);
    interrupts_ = uint16_t(interrupts_ | (kFifoAInterrupt << x));
    Write(x ? kIoTimer1Reload : kIoTimer0Reload, 2, ch.TimerReload());
    Write(x ? kIoDma2Control : kIoDma1Control, 2, kFifoDmaOn);
    Write(timer, 2, kTimerOn);

    r5_ = interrupts_;
}

// A PSG envelope's next step: its frames and NRx2, less the channel's volume, which starts the channel again. The
// Magical Vacation revision keeps the new NRx1 and NRx2 only while the channel has its PSG channel.
void Sequencer::PsgEnvelopeStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.step_frames = rom_.U8(ch.step);
    ch.level = rom_.U8(ch.step + 1);
    ch.step += 2;

    const int envelope = ch.level - ch.volume;
    const uint32_t control = ch.duty + (uint32_t(envelope < 0 ? 0 : envelope) << 8);
    if (!vacation_ || !(ch.flags & kFlagEffect))
    {
        ch.control = uint16_t(control);
    }
    if (!(ch.flags & kFlagEffect))
    {
        Write(ControlRegister(c), 2, control);
        Write(FrequencyRegister(c), 2, ch.frequency | 0x8000u);
    }
}

// A sample envelope's next step. The voice's sides take its level.
void Sequencer::SampleEnvelopeStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    TakeSampleStep(c, ch.step);
    SetVoiceLevels(c);
}

// A sample envelope's next step in the Magical Vacation revision. The FIFO's sample takes the volume with the master
// level, unless a sound effect has the FIFO.
void Sequencer::FifoEnvelopeStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    TakeFifoStep(c, ch.step);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    if (!(ch.flags & kFlagEffect))
    {
        r5_ = ch.EnvelopeVolume() * uint32_t(Get8(kVacationMaster));
        fifo_voices_[size_t(c & 1)].volume = r5_ >> 8;
    }
}

// Takes the sample envelope's step at `at`: its frames, the change in level each frame and the level.
void Sequencer::TakeSampleStep(int c, uint32_t at)
{
    Channel& ch = channels_[size_t(c)];
    ch.step_frames = rom_.U8(at);
    ch.SetRampWait(1);
    ch.ramp_frames = 1;
    ch.ramp = rom_.U8(at + 1);
    ch.level = rom_.U8(at + 2);
    ch.step = at + 3;
}

// Takes the sample envelope's step at `at` in the Magical Vacation revision: its frames, the frames between its changes
// in level and the change in the high and low nibbles of a byte, and the level.
void Sequencer::TakeFifoStep(int c, uint32_t at)
{
    Channel& ch = channels_[size_t(c)];
    ch.step_frames = rom_.U8(at);
    const uint8_t ramp = rom_.U8(at + 1);
    ch.ramp_frames = uint8_t(ramp >> 4);
    ch.SetRampWait(ch.ramp_frames);
    ch.ramp = uint8_t(ramp & 0x0F);
    ch.level = rom_.U8(at + 2);
    ch.step = at + 3;
}

// A sample channel's change in level, which stops at 0 and 255.
void Sequencer::RampStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.SetRampWait(ch.ramp_frames);
    if (ch.ramp == 0)
    {
        return;
    }

    const int change = ch.ramp & 0x7F;
    int level = (ch.ramp & 0x80) ? ch.level - change : ch.level + change;
    level = level < 0 ? 0 : level > 255 ? 255 : level;
    ch.level = uint8_t(level);
    SetVoiceLevels(c);
}

// A sample channel's change in level in the Magical Vacation revision, by its low 3 bits, down with bit 3 set, which
// stops at 0 and 255. The FIFO's sample takes the volume with the master level plus 1, where the envelope's step uses
// the master level itself.
void Sequencer::FifoRampStep(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.SetRampWait(ch.ramp_frames);
    if ((ch.ramp & 0x0F) == 0)
    {
        return;
    }

    const int change = ch.ramp & 7;
    int level = (ch.ramp & 8) ? ch.level - change : ch.level + change;
    level = level < 0 ? 0 : level > 255 ? 255 : level;
    ch.level = uint8_t(level);
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    if (!(ch.flags & kFlagEffect))
    {
        r5_ = ch.EnvelopeVolume() * (Get8(kVacationMaster) + 1u);
        fifo_voices_[size_t(c & 1)].volume = r5_ >> 8;
    }
}

// Sets a sample channel's volume with its envelope's level, and its voice's sides from that, the master level and the
// pan. Each step rounds down.
void Sequencer::SetVoiceLevels(int c)
{
    Channel& ch = channels_[size_t(c)];
    ch.SetEnvelopeVolume(VolumeAtLevel(ch));
    const uint32_t scaled = (ch.EnvelopeVolume() * (Get8(kMaster) + 1u)) >> 8;
    Voice& v = voices_[size_t(c & 7)];
    v.right = uint8_t(((ch.RightLevel() + 1u) * scaled) >> 8);
    v.left = uint8_t(((ch.LeftLevel() + 1u) * scaled) >> 8);
    r5_ = v.left;
}

} // namespace ntdlib::brownie
