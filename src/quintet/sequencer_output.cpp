// SPDX-License-Identifier: MIT

// The output of the quintet sequencer: the image of the PSG registers, the writes to the hardware, and the FIFOs that
// play the PCM channels.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sequencer_internal.h"

namespace ntdlib::quintet
{

void Sequencer::UpdateImage(int c)
{
    const Channel& ch = channels_[size_t(c)];
    const uint8_t duty = ch.switch_count ? ch.switch_value : ch.duty;
    const uint16_t frequency = uint16_t(ch.length_on << 14 | ch.pitch_from | kRestart);
    switch (c)
    {
    case kSquare1:
    case kSquare2:
        {
            const int step = ch.sounding || !traits_.rest_clears_step ? ch.envelope_step : 0;
            if (c == kSquare1)
            {
                image_[kNr10] = uint16_t(ch.sweep_shift | ch.sweep_down << 3 | ch.sweep_time << 4);
            }

            image_[size_t(kPsgEnvelope[c])] =
                uint16_t(duty << 6 | ch.length | step << 8 | ch.envelope_up << 11 | ch.level << 12);
            image_[size_t(kPsgControl[c])] = frequency;
            break;
        }

    case kWave:
        {
            // A level above the revision's wave levels leaves the volume code that came before.
            if (ch.level < traits_.wave_levels)
            {
                wave_code_ = traits_.wave_codes[ch.level];
            }
            else
            {
                Warn("the wave channel plays at volume " + std::to_string(ch.level) +
                     ", which the driver doesn't handle");
            }

            const uint8_t bank = ch.switch_count ? ch.switch_value : ch.wave_bank;
            image_[kNr30] = uint16_t(ch.wave_size << 5 | bank << 6 | ch.wave_on << 7);
            image_[kNr31] = uint16_t(ch.length << 2 | wave_code_ << 13 | ch.wave_75 << 15);
            image_[kNr33] = frequency;
            break;
        }

    case kNoise:
        image_[kNr41] = uint16_t(ch.length | ch.envelope_step << 8 | ch.envelope_up << 11 | ch.level << 12);
        image_[kNr43] =
            uint16_t(ch.noise_ratio | ch.noise_width << 3 | ch.noise_shift << 4 | ch.length_on << 14 | kRestart);
        break;

    case kPcmA:
        image_[kSoundcntH] = uint16_t((image_[kSoundcntH] & 0xFFFB) | ch.level << 2);
        break;

    default:
        image_[kSoundcntH] = uint16_t((image_[kSoundcntH] & 0xFFF7) | ch.level << 3);
        break;
    }
}

void Sequencer::Output()
{
    // Each PSG channel's registers are written when the channel restarts, and otherwise when they change. Square 1
    // writes its frequency register three times.
    struct Psg
    {
        int first;
        int count;
        uint16_t enable;
    };

    static constexpr Psg kPsgs[4] = {{kNr10, 3, 0x1100}, {kNr21, 2, 0x2200}, {kNr30, 3, 0x4400}, {kNr41, 2, 0x8800}};

    // Each hardware channel plays the sound effect's image while the sound effect's channel plays, and otherwise the
    // music's.
    std::array<uint16_t, 12>& music = effect_ ? music_image_ : image_;
    auto image_of = [&](int c) -> std::array<uint16_t, 12>&
    {
        return effect_ && channels_[size_t(c)].position ? image_ : music;
    };

    uint16_t nr50 = uint16_t(music[kNr50] & 0xFF);
    for (int p = 0; p < 4; p++)
    {
        const Psg& psg = kPsgs[p];
        std::array<uint16_t, 12>& image = image_of(p);
        if (p == kWave && &image == &music)
        {
            // The music's waves go back into the wave RAM if something else has loaded others.
            Channel& wave = effect_ ? music_wave_ : channels_[kWave];
            if (loaded_waves_[0] != wave.wave)
            {
                LoadWave(wave, wave.wave);
            }
            if (loaded_waves_[1] != wave.wave_high)
            {
                LoadWave(wave, wave.wave_high);
            }
        }

        const int last = psg.first + psg.count - 1;
        if (image[size_t(last)] & kRestart)
        {
            for (int r = psg.first; r <= last; r++)
            {
                Write(kImageRegisters[r], 2, image[size_t(r)]);
            }
            if (p == kSquare1)
            {
                Write(kImageRegisters[last], 2, image[size_t(last)]);
                Write(kImageRegisters[last], 2, image[size_t(last)]);
            }

            image[size_t(last)] &= 0x7FFF;
            for (int r = psg.first; r <= last; r++)
            {
                shadow_[size_t(r)] = image[size_t(r)];
            }
        }
        else
        {
            for (int r = psg.first; r <= last; r++)
            {
                if (shadow_[size_t(r)] != image[size_t(r)])
                {
                    Write(kImageRegisters[r], 2, image[size_t(r)]);
                    shadow_[size_t(r)] = image[size_t(r)];
                }
            }
        }

        nr50 = uint16_t(nr50 | (image[kNr50] & psg.enable));
    }

    if (shadow_[kNr50] != nr50)
    {
        Write(kImageRegisters[kNr50], 2, nr50);
        shadow_[kNr50] = nr50;
    }

    // The FIFOs' volumes and enables. The master volume that the game sets would halve or mute them at 4 or less, but
    // the play routine sets it to 7.
    uint16_t soundcnt_h = uint16_t(shadow_[kSoundcntH] & 0xCC03);
    soundcnt_h = uint16_t(soundcnt_h | (image_of(kPcmA)[kSoundcntH] & 0x0304) | (image_of(kPcmB)[kSoundcntH] & 0x3008));
    if (shadow_[kSoundcntH] != soundcnt_h)
    {
        Write(kImageRegisters[kSoundcntH], 2, soundcnt_h);
        shadow_[kSoundcntH] = soundcnt_h;
    }
}

void Sequencer::UpdateFifos()
{
    // The driver works out how far each FIFO's sample has played from its rate, and stops or loops it a frame before
    // the end.
    for (int f = 0; f < 2; f++)
    {
        Fifo& fifo = fifos_[size_t(f)];
        if (fifo.position == ~0u)
        {
            continue;
        }

        fifo.position += fifo.rate;
        if (DivideUnsigned(fifo.position + fifo.rate, 60) >= fifo.end)
        {
            StopFifo(f);
            if (fifo.loop)
            {
                fifo.position = fifo.loop_position + fifo.rate;
                StartFifo(fifo.loop, f);
            }
        }
    }
}

uint16_t Sequencer::PsgFrequency(const Channel& ch, int note) const
{
    const int32_t n = NoteIndex(ch, note, kFrequencyEntries);
    int32_t f = int16_t(rom_.U16(info_.frequency_table + uint32_t(n) * 2)) + int16_t(ch.detune);
    f = f < 0 ? 0 : f > 0x7FF ? 0x7FF : f;

    return uint16_t(f);
}

uint16_t Sequencer::PcmPitch(const Channel& ch, int note) const
{
    const int32_t n = NoteIndex(ch, note, kPcmPitchEntries);

    return uint16_t(rom_.U16(info_.pcm_pitch_table + uint32_t(n) * 2) + ch.detune);
}

void Sequencer::LoadWave(Channel& ch, uint8_t wave)
{
    if (!info_.waves)
    {
        return;
    }

    if (wave & 0x80)
    {
        ch.wave_high = wave;
    }
    else
    {
        ch.wave = wave;
    }

    // The wave RAM's writes go to the bank that isn't playing, so the driver selects the other bank, writes the wave
    // and puts NR30 back as it read it, which leaves the channel off.
    const uint8_t nr30 = uint8_t(nr30_ & 0x7F);
    uint8_t select = 0;
    if (wave & 0x80)
    {
        select = nr30 & 0xBF;
        loaded_waves_[1] = wave;
    }
    else
    {
        select = nr30 | 0x40;
        loaded_waves_[0] = wave;
    }

    Write(kImageRegisters[kNr30], 1, select);
    const uint32_t data = info_.waves + (wave & 0x7Fu) * 16 + 2;
    for (uint32_t i = 0; i < 16; i++)
    {
        Write(kWaveRam + i, 1, rom_.U8(data + i));
    }

    Write(kImageRegisters[kNr30], 1, nr30);
}

void Sequencer::SelectMacro(Channel& ch, uint8_t macro)
{
    // The A revision leaves the macro alone without a table, and the J revision clears it.
    const uint32_t table = info_.macros[ch.macro_bank & 1];
    if (!table)
    {
        ch.macro_start = traits_.clears_missing_macro ? 0 : ch.macro_start;
        return;
    }

    ch.macro_start = macro ? table + rom_.U16(table + 2 * uint32_t(macro) + 2) : 0;
}

void Sequencer::SelectSample(int c, uint8_t sample)
{
    // The loaded sample is remembered by its number alone, so a bank switch takes a sample of another number to show.
    const int f = c - kPcmA;
    LoadSample(f, SampleAddress(channels_[size_t(c)].sample_bank & 1, sample));
    channels_[size_t(c)].wave = sample;
    loaded_samples_[size_t(f)] = sample;
}

uint32_t Sequencer::SampleAddress(int bank, uint8_t sample)
{
    const std::vector<uint32_t>& list = info_.sample_addresses[size_t(bank)];
    if (sample < list.size())
    {
        return list[sample];
    }

    Warn("a PCM channel plays sample " + std::to_string(sample) + " of bank " + std::to_string(bank) +
         ", which isn't in the game's sample file");

    return 0;
}

void Sequencer::PcmNoteOn(int c)
{
    const Channel& ch = channels_[size_t(c)];
    const int f = c - kPcmA;
    if (loaded_samples_[size_t(f)] != ch.wave)
    {
        SelectSample(c, ch.wave);
    }

    PlaySample(ch.pitch_from, ch.pcm_start, ch.pcm_end, ch.pcm_loop, f);
}

void Sequencer::LoadSample(int f, uint32_t sample)
{
    StopFifo(f);
    Fifo& fifo = fifos_[size_t(f)];
    if (!sample)
    {
        fifo.sample = 0;
        fifo.next_start = 0;
        return;
    }

    // A sample's header holds its size, header included, and its rate. The driver plays 64 bytes fewer, less 0.6%: 256
    // bytes fewer in the S revision.
    const uint32_t size = rom_.U32(sample);
    const uint32_t length = DivideUnsigned((size - traits_.sample_trim) * 994, 1000);
    fifo.end = length;
    fifo.length = length;
    fifo.base_rate = rom_.U16(sample + 4);
    fifo.sample = sample + 6;
    fifo.next_start = sample + 6;
}

void Sequencer::PlaySample(uint16_t pitch, int start, int end, int loop, int f)
{
    StopFifo(f);
    if (!fifos_[size_t(f)].next_start)
    {
        return;
    }

    SetRate(pitch, f);
    SetRange(start, end, loop, f);
    StartFifo(fifos_[size_t(f)].next_start, f);
}

void Sequencer::SetRate(uint16_t pitch, int f)
{
    Fifo& fifo = fifos_[size_t(f)];
    if (!fifo.base_rate)
    {
        return;
    }

    fifo.rate = PlayRate(fifo.base_rate, pitch);
    fifo.end = fifo.length;
}

void Sequencer::SetRange(int start, int end, int loop, int f)
{
    // E7 plays the part of the sample from `start`% to `end`%, then loops back to `loop`% unless that's the end. The S
    // revision loops back there too.
    Fifo& fifo = fifos_[size_t(f)];
    if (start == 0 && end == 0 && loop == 0)
    {
        fifo.loop = 0;
        fifo.position = 0;
        return;
    }

    const uint32_t length = fifo.end;
    const uint32_t from = DivideUnsigned(length * uint32_t(start), 100);
    const uint32_t to = DivideUnsigned(length * uint32_t(end), 100);
    const uint32_t back = DivideUnsigned(length * uint32_t(loop), 100);
    fifo.position = from * 60;
    fifo.loop_position = back * 60;
    fifo.end = to;
    fifo.next_start = fifo.sample + from;
    fifo.loop = loop != end || traits_.range_end_loops ? fifo.sample + back : 0;
}

void Sequencer::StartFifo(uint32_t data, int f)
{
    // Reset the FIFO, fill it with the first 32 bytes, and point its DMA at the rest. The J revision clears the DMA's
    // count and control together.
    const FifoRegisters& registers = kFifoRegisters[size_t(f)];
    Write(kImageRegisters[kSoundcntH], 2, soundcnt_h_ | registers.reset);
    Write(registers.dma_source, 4, data + 32);
    Write(registers.dma_dest, 4, registers.fifo);
    Write(registers.dma_count, traits_.dma_clear_size, 0);
    Write(registers.dma_control, 2, 0xB640);
    SetTimer(f, TimerSetting(fifos_[size_t(f)].rate));
}

void Sequencer::StopFifo(int f)
{
    // DC or DD on a PSG channel gives a FIFO number below 0. Any number but 0 stops FIFO B.
    const size_t fifo = f == 0 ? 0 : 1;
    Write(kFifoRegisters[fifo].timer, 4, 0);
    Write(kFifoRegisters[fifo].dma_control, 2, dma_control_[fifo] & 0x7FFF);
    fifos_[fifo].position = ~0u;
    pending_timers_[fifo] = 0;
}

void Sequencer::BendFifo(uint16_t pitch, int f)
{
    Fifo& fifo = fifos_[size_t(f)];
    fifo.rate = PlayRate(fifo.base_rate, pitch);
    SetTimer(f, TimerSetting(fifo.rate));
}

void Sequencer::SetTimer(int f, uint32_t value)
{
    // The J revision writes the timer at the end of the frame.
    if (traits_.timers_at_frame_end)
    {
        pending_timers_[size_t(f)] = value;
    }
    else
    {
        Write(kFifoRegisters[size_t(f)].timer, 4, value);
    }
}

void Sequencer::Write(uint32_t address, int size, uint32_t value)
{
    value &= size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
    writes_.push_back({address, uint8_t(size), value});

    // Remember what the registers that the driver reads back would return.
    if (address == kImageRegisters[kSoundcntH])
    {
        soundcnt_h_ = uint16_t(value & kSoundcntHRead);
    }
    else if (address == kSoundbias)
    {
        soundbias_ = uint16_t(value);
    }
    else if (address == kImageRegisters[kNr30])
    {
        nr30_ = uint8_t(value & kNr30Read);
    }
    else if (address == kFifoRegisters[0].dma_control || address == kFifoRegisters[1].dma_control)
    {
        dma_control_[address == kFifoRegisters[0].dma_control ? 0 : 1] = uint16_t(value);
    }

    // Keep the hardware's state for the conversion: the sound registers, the channels the writes restart, and the wave
    // RAM, whose writes go to the bank that isn't playing.
    for (size_t r = 0; r < hardware_.registers.size(); r++)
    {
        if (kImageRegisters[r] != address)
        {
            continue;
        }

        uint16_t& reg = hardware_.registers[r];
        reg = size == 1 ? uint16_t((reg & 0xFF00) | value) : uint16_t(value);
        for (int p = 0; p < 4; p++)
        {
            if (r == size_t(kPsgControl[p]) && (value & kRestart))
            {
                hardware_.restarted |= uint8_t(1 << p);
            }
        }
    }
    if (address >= kWaveRam && address < kWaveRam + 16)
    {
        const size_t bank = ((hardware_.registers[kNr30] >> 6) & 1) ^ 1;
        hardware_.wave_ram[bank][address - kWaveRam] = uint8_t(value);
    }
}

} // namespace ntdlib::quintet
