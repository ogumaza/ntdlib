// SPDX-License-Identifier: MIT

// The Konami output stage's note starts, PSG stores and each revision's stage: Ultimate's, WCT's, the ring's and the
// FIFO revision's. output.cpp and this file build as one translation unit.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/wave/sinc.h"
#include "output_impl.h"

namespace ntdlib::konami
{

void Output::Impl::NoteStart(OutputVoice& v, const TrackOutput& r, int right, int left)
{
    v.flags = 0;
    const uint32_t key = r.key;
    uint32_t table = info.sample_table;
    if (stage == Stage::kUltimate)
    {
        if (key & 0xA000)
        {
            table = key & 0x2000 ? 0 : second_table;
        }

        v.key = uint16_t(key & ~0x1000u);
    }
    else
    {
        if (key & 0x8000)
        {
            table = second_table;
        }

        v.key = uint16_t(key & 0xCFFFu);
    }

    const uint32_t header = rom.U32(table + (key & 0xFFF) * 4);
    v.run = 0;
    if (clears_held)
    {
        v.held = 0;
    }

    v.volume = r.vol;
    const uint32_t word = Pitched(r.pitch, rom.U32(header));
    v.step = uint16_t(word);
    v.fraction = uint16_t(word >> 16);
    v.start = header + 12;
    int32_t length = rom.S32(header + 4);
    uint8_t flags = kVoiceOn;
    if (length < 0)
    {
        length &= 0x00FFFFFF;
        flags |= kVoiceCoded;
        if (rules.coded == Coded::kAdpcm)
        {
            v.adpcm_index = 0;
            v.adpcm_value = 0;
        }
        else if (rules.coded == Coded::kPacked)
        {
            // The table of pairs comes first.
            v.start = header + 12 + 64;
        }
    }

    v.position = v.start;
    v.count = length;
    if (key & 0x4000)
    {
        v.position = v.start + uint32_t(rom.S32(header + 4)) - 1;
        flags |= kVoiceReverse;
    }
    else if (rom.S32(header + 8) >= 0)
    {
        flags |= kVoiceLooped;
    }

    v.right = uint8_t(right);
    v.left = uint8_t(left);
    v.mix_path = 0;
    v.flags = flags;
    if (enhanced)
    {
        const bool reverse = flags & kVoiceReverse;
        StartEnhanced(int(&v - voices.data()), v.start, uint32_t(length), rom.S32(header + 8),
                      !reverse && (flags & kVoiceCoded), reverse);
    }
}

// Stores the registers of the PSG channels whose tracks (0 to 3) have output. With `wave_on_trigger`, the wave
// channel loads its wave only on a trigger.
void Output::Impl::StorePsg(const std::array<TrackOutput, kTracks>& rec, bool wave_on_trigger)
{
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    const auto frequency = [this](int pitch)
    {
        return rom.U16(info.psg_freq_table + uint32_t(pitch * 2));
    };

    if (any(rec[0]))
    {
        const TrackOutput& r = rec[0];
        const uint32_t f = frequency(r.pitch);
        if (r.trig)
        {
            Store(kSound1CntH, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound1CntX, 2, f);
        }
        else
        {
            Store(kSound1CntX, 2, f & 0x7FF);
        }
    }

    if (any(rec[1]))
    {
        const TrackOutput& r = rec[1];
        uint32_t f;
        if (r.pitch >= 0 && (r.pitch & 0x4000))
        {
            f = (uint32_t(uint16_t(r.pitch)) & ~0x4000u) | 0x8000;
        }
        else
        {
            f = frequency(r.pitch);
        }

        if (r.trig)
        {
            Store(kSound2CntL, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound2CntH, 2, f);
        }
        else
        {
            Store(kSound2CntH, 2, f & 0x7FF);
        }
    }

    if (any(rec[2]))
    {
        const TrackOutput& r = rec[2];
        const uint32_t f = frequency(r.pitch) & 0x7FF;
        if (r.vol == 0)
        {
            Store(kSound3CntH, 2, 0);
        }
        else if (r.trig || !wave_on_trigger)
        {
            WaveLoad(r.b2, r.vol);
            Store(kSound3CntH, 2, 0x2000);
        }

        Store(kSound3CntX, 2, f);
    }

    if (any(rec[3]))
    {
        const TrackOutput& r = rec[3];
        Store(kSound4CntL, 2, uint32_t(r.vol) << 12);
        if ((r.trig & 2) || (r.flags & 2))
        {
            Store(kSound4CntH, 2, uint16_t(r.pitch));
        }
        else
        {
            Store(kSound4CntH, 2, rom.U16(info.noise_table + uint32_t(r.pitch * 2)));
        }
    }
}

void Output::Impl::StageUltimate()
{
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    uint32_t mix = nr51 << 4;
    for (int c = 0; c < 4; c++)
    {
        const TrackOutput& r = records[size_t(c)];
        mix >>= 1;
        if (any(r) && r.vol != 0)
        {
            mix |= 0x88;
            if (r.pan_l < r.pan_r)
            {
                mix &= ~0x80u;
            }
            if (r.pan_l > r.pan_r)
            {
                mix &= ~0x08u;
            }
        }
    }

    if (!wave_started)
    {
        if (records[2].trig != 0 && records[2].vol != 0)
        {
            wave_started = true;
        }
        else
        {
            mix &= ~0x44u;
        }
    }

    if (nr51 != mix)
    {
        nr51 = mix;
        Store(kNr51, 1, mix);
    }

    StorePsg(records, true);

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = records[size_t(n + 4)];
        if (!any(r))
        {
            continue;
        }

        const uint32_t levels = info.volume_table + uint32_t(r.vol) * 64;
        const int right = rom.U8(levels + r.pan_r);
        const int left = rom.U8(levels + r.pan_l);
        if (r.flags & kOutStop)
        {
            v.flags = 0;
            v.volume = 0;
        }
        else if (r.flags != 0)
        {
            if (r.flags & kOutNoteOn)
            {
                NoteStart(v, r, right, left);
            }

            if (r.flags & kOutPitch)
            {
                const uint32_t table = r.key & 0x8000 ? second_table : info.sample_table;
                const uint32_t header = rom.U32(table + (r.key & 0xFFFu) * 4);
                v.step = uint16_t(Pitched(r.pitch, rom.U32(header)));
            }
        }

        if (r.trig != 0 || (pan_sets_levels && (r.flags & kOutPan)))
        {
            v.volume = r.vol;
            v.right = uint8_t(right);
            v.left = uint8_t(left);
        }
    }
}

void Output::Impl::StageWct()
{
    std::array<TrackOutput, kTracks> rec = records;
    uint32_t mix = LevelsAndPansWct(rec);

    if (wave_gate && !wave_started)
    {
        if (rec[2].trig != 0 && rec[2].vol != 0)
        {
            wave_started = true;
        }
        else
        {
            mix &= ~0x44u;
        }
    }

    if (nr51 != mix)
    {
        nr51 = mix;
        Store(kNr51, 1, mix);
    }

    StorePsg(rec, false);
    UpdateVoicesWct(rec);
}

uint32_t Output::Impl::LevelsAndPansWct(std::array<TrackOutput, kTracks>& rec)
{
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    const auto plain = [](uint8_t pan)
    {
        return pan == 0xFF || pan == 0x0F || pan == 0xF0;
    };

    const auto levels = [this](int volume)
    {
        return info.volume_table + uint32_t(volume) * 16;
    };

    uint32_t mix = 0;
    channel_tracks = {0, 1, 2, 3};
    for (int t = voice_count + 3; t >= 0; t--)
    {
        TrackOutput& r = rec[size_t(t)];
        const uint8_t pan = r.pan;
        if (t > 3)
        {
            const OutputVoice& v = voices[size_t(t - 4)];
            const bool had_output = any(r);
            if (had_output || (levels_each_frame && (v.flags & kVoiceOn)))
            {
                const uint32_t row = levels(r.vol);
                r.pan_r = rom.U8(row + (pan & 15));
                r.pan_l = rom.U8(row + (pan >> 4));
                if (levels_each_frame && !had_output)
                {
                    r.trig = 1;
                }
            }
            continue;
        }

        // A plain pan after one off centre leaves square 2 at volume 0 with square 1's output.
        if (t == 0 && !any(rec[1]) && plain(pan) && !plain(r.pan_start))
        {
            rec[1] = r;
            rec[1].vol = 0;
        }

        mix = (mix << 1) | 0x11;
        switch (pan)
        {
        case 0xF0:
            mix &= ~0x01u;
            break;
        case 0x0F:
            mix &= ~0x10u;
            break;
        case 0xFF:
            break;
        default:
            {
                // The channel plays the right side, and the next channel the left.
                TrackOutput& next = rec[size_t(t + 1)];
                next = r;
                if (t < 3)
                {
                    channel_tracks[size_t(t + 1)] = uint8_t(t);
                }

                const uint32_t row = levels(r.vol);
                r.vol = rom.U8(row + (pan & 15));
                next.vol = rom.U8(row + (pan >> 4));
                mix &= ~0x33u;
                mix |= 0x21;
            }
            break;
        }
    }

    return mix;
}

void Output::Impl::UpdateVoicesWct(const std::array<TrackOutput, kTracks>& rec)
{
    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = rec[size_t(n + 4)];
        if (r.flags & kOutStop)
        {
            v.flags = 0;
            v.volume = 0;
        }
        else if (r.flags != 0)
        {
            if (r.flags & kOutNoteOn)
            {
                NoteStart(v, r, r.pan_r, r.pan_l);
            }

            if (r.flags & kOutPitch)
            {
                const uint32_t table = r.key & 0x8000 ? second_table : info.sample_table;
                const uint32_t header = rom.U32(table + (r.key & 0xFFFu) * 4);
                v.step = uint16_t(Pitched(r.pitch, rom.U32(header)));
            }
        }

        if (r.trig != 0)
        {
            v.volume = r.vol;
            v.right = r.pan_r;
            v.left = r.pan_l;
        }
    }
}

void Output::Impl::NoteStartRing(OutputVoice& v, const TrackOutput& r)
{
    const uint32_t key = r.key & 0xCFFFu;
    const uint32_t entry = key & 0x8000 ? second_table + (key & 0x3FFF) * 4 : info.sample_table + key * 4;
    const uint32_t header = rom.U32(entry);
    const uint32_t word = Pitched(r.pitch, rom.U32(header));
    v.step = uint16_t(word);
    v.fraction = uint16_t(word >> 16);
    v.count = rom.S32(header + 4);
    v.position = header + 12;
    v.key = uint16_t(key & 0x8FFFu);
    v.volume = r.vol;
    v.flags = rom.S32(header + 8) >= 0 ? kVoiceOn | kVoiceLooped : kVoiceOn;
    if (enhanced)
    {
        StartEnhanced(int(&v - voices.data()), v.position, uint32_t(v.count), rom.S32(header + 8), false, false);
    }
}

void Output::Impl::StageRing()
{
    const std::array<TrackOutput, kTracks>& rec = records;
    Store(kNr51, 1, nr51);
    if (nr51_twice)
    {
        Store(kNr51, 1, nr51);
    }

    StorePsg(rec, false);

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = rec[size_t(n + 4)];
        if (r.flags != 0)
        {
            bool pitch = false;
            if (pitch_on_any_flag)
            {
                if (r.flags & kOutNoteOn)
                {
                    NoteStartRing(v, r);
                }
                else if (r.flags & kOutStop)
                {
                    v.flags = 0;
                }
                else
                {
                    pitch = true;
                }
            }
            else if (r.flags & kOutStop)
            {
                v.flags = 0;
            }
            else
            {
                if (r.flags & kOutNoteOn)
                {
                    NoteStartRing(v, r);
                }

                pitch = r.flags & kOutPitch;
            }

            if (pitch)
            {
                const uint32_t entry =
                    r.key & 0x8000 ? second_table + (r.key & 0x3FFFu) * 4 : info.sample_table + uint32_t(r.key) * 4;
                v.step = uint16_t(Pitched(r.pitch, rom.U32(rom.U32(entry))));
            }
        }

        if (r.trig != 0)
        {
            v.volume = r.vol;
        }
    }
}

void Output::Impl::RunStage()
{
    switch (stage)
    {
    case Stage::kUltimate:
        StageUltimate();
        break;
    case Stage::kWct:
        StageWct();
        break;
    case Stage::kRing:
        StageRing();
        break;
    case Stage::kFifo:
        StageFifo();
        break;
    }
}

void Output::Impl::StageFifo()
{
    const std::array<TrackOutput, kTracks>& rec = records;
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    const auto frequency = [this](int pitch)
    {
        return rom.U16(info.psg_freq_table + uint32_t(pitch * 2));
    };

    if (any(rec[0]))
    {
        const TrackOutput& r = rec[0];
        Store(kSound1CntH, 2, uint32_t(r.vol) << 12 | r.b2 | r.key);
        Store(kSound1CntX, 2, frequency(r.pitch));
    }

    if (any(rec[1]))
    {
        const TrackOutput& r = rec[1];
        Store(kSound2CntL, 2, uint32_t(r.vol) << 12 | r.b2 | r.key);
        Store(kSound2CntH, 2, frequency(r.pitch));
    }

    Store(kNr50, 1, sequencer->PsgVolume());
    if (any(rec[2]))
    {
        const TrackOutput& r = rec[2];
        Store(kSound3CntH, 2, rom.U16(info.wave_volume_table + uint32_t(r.vol) * 2) | r.b2);
        const uint32_t f = frequency(r.pitch);
        Store(kSound3CntX, 2, wave_keeps_restart ? f : f & 0x7FFF);
    }

    if (any(rec[3]))
    {
        const TrackOutput& r = rec[3];
        Store(kSound4CntL, 2, uint32_t(r.vol) << 12);
        Store(kSound4CntH, 2, frequency(r.pitch));
    }

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = rec[size_t(n + 4)];
        v.volume = r.vol;
        if (r.flags == 0)
        {
            continue;
        }

        if (r.flags & kOutDungeonDicePeriod)
        {
            fifos[size_t(n / 2)].goal = uint16_t(r.pitch);
        }
        else if (r.vol == 0)
        {
            v.flags = 0;
            v.position = 0;
        }
        else
        {
            v.key = r.b2;
            v.period = rom.U16(info.sample_period_table + uint32_t(r.pitch * 2));
            v.flags |= kFifoVoicePending;
        }
    }
}

void Output::Impl::StartNotesFifo()
{
    std::array<uint16_t, 4> starts{};
    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        if (!(v.flags & kFifoVoicePending))
        {
            continue;
        }

        v.flags = 0;
        const uint32_t entry = info.sample_table + uint32_t(v.key) * 8;
        const uint32_t word = rom.U32(entry + 4);
        uint32_t period = v.period;
        if (period == 0)
        {
            period = word & 0xFFFF;
            v.flags = kFifoVoiceSampleRate;
        }

        v.count = int32_t(word >> 20);
        v.position = rom.U32(entry);
        starts[size_t(n)] = uint16_t(0 - period);
        if (enhanced)
        {
            StartEnhanced(n, v.position, uint32_t(v.count) * kBlock, -1, false, false);
        }
    }

    for (int f = 0; f < 2; f++)
    {
        OutputVoice& first = voices[size_t(f * 2)];
        OutputVoice& second = voices[size_t(f * 2 + 1)];
        uint16_t first_start = starts[size_t(f * 2)];
        uint16_t second_start = starts[size_t(f * 2 + 1)];
        uint32_t reload = 0;
        if (first_start != 0)
        {
            if (!(second.flags & kFifoVoiceSampleRate))
            {
                second.flags = 0;
                second_start = 0;
            }

            first.flags |= kVoiceOn;
            reload = first_start;
        }

        if (second_start != 0)
        {
            if ((first.flags & kFifoVoiceSampleRate) && !(second.flags & kFifoVoiceSampleRate))
            {
                second.flags = 0;
            }
            else
            {
                second.flags |= kVoiceOn;
                reload = second_start;
            }
        }

        if (reload == 0)
        {
            continue;
        }

        Fifo& fifo = fifos[size_t(f)];
        if (!fifo.running)
        {
            // The DMA and the timer stop, and the FIFO starts again with 4 bytes of 0. FIFO B also takes timer 1.
            timers[size_t(f)] = 0;
            Store(kSoundCntHHigh, 1, sound_control_high | (f == 0 ? 0x08 : 0xF3));
            sound_control_high = (sound_control_high | (f == 0 ? 0x08 : 0xF3)) & 0x77;
            fifo.restarted = true;
            fifo.running = true;
        }

        fifo.dma_on = true;
        fifo.goal = uint16_t(0 - reload);
        fifo.period = fifo.goal;
        timers[size_t(f)] = reload | kTimerOn << 16;
    }
}

} // namespace ntdlib::konami
