// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "xsflib/bytes.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/krawall/build.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"
#include "player_shared.h"

namespace ntdlib::krawall
{

namespace
{

// Slides `level` between rows: down by the low digit of `slide`, or else up by the high one, within 0 and 64. Returns
// false if `slide` is 0.
bool SlideLevel(int8_t& level, uint8_t slide)
{
    if (slide & 0xF)
    {
        level = std::max<int8_t>(int8_t(level - (slide & 0xF)), 0);
        return true;
    }
    if (slide >> 4)
    {
        level = std::min<int8_t>(int8_t(level + (slide >> 4)), 64);
        return true;
    }

    return false;
}

} // namespace

void Player::Effect(Channel& c, int effect, bool on_row)
{
    // The builds before 1.2.2.17 have no channel volume. Their table has no routines for its effects.
    if (info_.build < Build::kV1_2_2_17 && (effect == kChannelVolume || effect == kChannelVolumeSlide))
    {
        return;
    }

    const uint8_t op = c.effect_op;
    switch (effect)
    {
    case kSpeed:
        if (op)
        {
            speed_ = op;
        }
        break;

    case kBpm:
    case kSpeedBpm:
        if (op > 0x1F)
        {
            tempo_ = op;
            SetTimerBpm(op * 24);
        }
        else if (effect == kSpeedBpm)
        {
            speed_ = op;
        }
        break;

    case kPatternJump:
        if (info_.build < Build::kV1_2_2_15)
        {
            OldPatternJump(op);
            break;
        }
        goto_order_ = int8_t(op);
        break;

    case kPatternBreak:
        {
            if (info_.build < Build::kV1_2_2_15)
            {
                OldPatternBreak(op);
                break;
            }

            // Up to 2003/09/01, the row is a signed byte, and a break past row 127 counts as none.
            const int row = (op >> 4) * 10 + (op & 0xF);
            goto_row_ = info_.build < Build::k2004_09_17 ? int16_t(int8_t(uint8_t(row))) : int16_t(row);
            break;
        }

    case kVolumeSlideS3m:
    case kVolumeSlideVibrato:
    case kVolumeSlidePorta:
        VolumeSlideS3m(c, on_row);
        break;

    case kVolumeSlideXm:
    case kVolumeSlideVibratoXm:
    case kVolumeSlidePortaXm:
        VolumeSlideXm(c, on_row);
        break;

    case kVolumeSlideDownFine:
    case kVolumeSlideUpFine:
        if (op)
        {
            c.fine_volume_slide = op;
        }
        if (effect == kVolumeSlideDownFine)
        {
            VolumeDown(c, c.fine_volume_slide);
        }
        else
        {
            VolumeUp(c, c.fine_volume_slide);
        }
        break;

    case kPortaDownXm:
    case kPortaUpXm:
        if (on_row)
        {
            if (op)
            {
                c.porta = op;
            }
            break;
        }

        // Down subtracts, and a period that reaches 0 stops the note.
        if (effect == kPortaUpXm)
        {
            RaisePeriod(c, c.porta << 2);
        }
        else
        {
            LowerPeriod(c, c.porta << 2);
        }
        break;

    case kPortaDownS3m:
    case kPortaUpS3m:
        PortaS3m(c, on_row, effect == kPortaUpS3m);
        break;

    case kPortaDownFine:
    case kPortaDownExtraFine:
    case kPortaUpFine:
    case kPortaUpExtraFine:
        {
            const bool fine = effect == kPortaDownFine || effect == kPortaUpFine;
            uint8_t& step = fine ? c.fine_porta : c.extra_fine_porta;
            if (op)
            {
                step = op;
            }
            const int by = fine ? step << 2 : step;
            if (effect == kPortaUpFine || effect == kPortaUpExtraFine)
            {
                RaisePeriod(c, by);
            }
            else
            {
                LowerPeriod(c, by);
            }
            break;
        }

    case kVolume:
        c.volume = int8_t(op);
        SetChannelVolume(c);
        break;

    case kPortaNote:
        PortaNote(c, on_row);
        break;

    case kVibrato:
        Vibrato(c, on_row);
        break;

    case kTremor:
        Tremor(c, on_row);
        break;

    case kArpeggio:
        Arpeggio(c, on_row);
        break;

    case kChannelVolume:
        c.channel_volume = int8_t(op);
        if (c.channel_volume > 64)
        {
            c.channel_volume = 64;
        }
        SetPlayedVolume(c);
        break;

    case kChannelVolumeSlide:
        if (on_row)
        {
            if (op)
            {
                c.channel_volume_slide = op;
            }
        }
        else if (SlideLevel(c.channel_volume, c.channel_volume_slide))
        {
            SetPlayedVolume(c);
        }
        break;

    case kOffset:
        if (op)
        {
            if (c.play_note)
            {
                c.start_point = op;
            }
            else
            {
                SetPos(c.handle, uint32_t(op) << 8);
            }
        }
        break;

    case kPanSlide:
        PanSlide(c, on_row);
        break;

    case kRetrig:
        Retrig(c, on_row);
        break;

    case kTremolo:
        Tremolo(c, on_row);
        break;

    case kFineVibrato:
        Vibrato(c, on_row);
        if (on_row && (op & 0xF))
        {
            c.vibrato_depth >>= 2;
        }
        break;

    case kGlobalVolume:
        volume_ = int8_t(std::min<uint8_t>(op, 64));
        SetGlobalVolume();
        break;

    case kGlobalVolumeSlide:
        if (on_row)
        {
            if (op)
            {
                volume_slide_ = op;
            }
        }
        else if (SlideLevel(volume_, volume_slide_))
        {
            SetGlobalVolume();
        }
        break;

    case kPan:
        c.panning = op > 0x80 ? int8_t(0) : int8_t(op - 0x40);
        SetChannelPan(c);
        break;

    case kPanbrello:
        Panbrello(c, on_row);
        break;

    case kGlissando:
        c.glissando = op != 0;
        break;

    case kVibratoWave:
    case kTremoloWave:
    case kPanbrelloWave:
        {
            const uint32_t tables[4] = {info_.sine, info_.ramp, info_.square, info_.random};
            uint32_t& table = effect == kVibratoWave   ? c.vibrato_wave
                              : effect == kTremoloWave ? c.tremolo_wave
                                                       : c.panbrello_wave;
            uint8_t& reset = effect == kVibratoWave   ? c.vibrato_reset
                             : effect == kTremoloWave ? c.tremolo_reset
                                                      : c.panbrello_reset;
            table = tables[op & 3];
            reset = op <= 3;
            break;
        }

    case kPatternLoop:
        PatternLoop(c);
        break;

    case kNoteCut:
        if (on_row)
        {
            if (op == 0)
            {
                c.play_note = 0;
            }
        }
        else if (op == tick_ && Since2004())
        {
            // From 2004/09/17 on, the cut sets the volume to 0 and leaves the note playing.
            c.volume = c.played_volume = 0;
            SetVol(c.handle, 0);
        }
        else if (op == tick_)
        {
            StopChannel(c);
        }
        break;

    case kNoteDelay:
        if (on_row)
        {
            if (op)
            {
                c.play_note = 0;
            }
        }
        else if (op == tick_)
        {
            PlayNote(c);
        }
        break;

    case kMark:
    default:
        if (effect >= kEffectCount)
        {
            Warn("a pattern has effect " + std::to_string(effect) + ", which the player doesn't have");
        }
        break;
    }
}

void Player::VolumeColumnEffect(Channel& c, bool on_row)
{
    // A column from 0x51 to 0x5F in the builds before 1.4 names an entry past the table of the column's effects. The
    // model leaves the column out.
    if ((c.column_effect >> 4) > kColumnPortaNote)
    {
        Warn("a volume column from 0x51 to 0x5F names no effect in this build of Krawall and is left out");
        return;
    }

    const int x = c.column_effect & 0xF;
    switch (c.column_effect >> 4)
    {
    case kColumnVolumeSlideDown:
        if (!on_row)
        {
            VolumeDown(c, x);
        }
        break;

    case kColumnVolumeSlideUp:
        if (!on_row)
        {
            VolumeUp(c, x);
        }
        break;

    case kColumnFineVolumeDown:
        VolumeDown(c, x);
        break;

    case kColumnFineVolumeUp:
        VolumeUp(c, x);
        break;

    case kColumnVibratoSpeed:
        if (c.column_effect)
        {
            c.vibrato_speed = uint8_t(x);
        }
        break;

    case kColumnVibrato:
        if (!on_row)
        {
            VibratoTick(c);
        }
        else if (c.column_effect)
        {
            c.vibrato_depth = uint8_t(x << 2);
        }
        break;

    case kColumnPan:
        c.panning = c.column_effect == 7 ? int8_t(0) : int8_t(((x * 0x88900) >> 16) - 64);
        SetChannelPan(c);
        break;

    case kColumnPanSlideLeft:
        if (!on_row)
        {
            PanLeft(c, x);
        }
        break;

    case kColumnPanSlideRight:
        if (!on_row)
        {
            PanRight(c, x);
        }
        break;

    case kColumnPortaNote:
    default:
        // The slide to a note sets its step on the row; between rows, this build runs the effect's part for the row
        // rather than the slide.
        if (on_row)
        {
            if (c.effect_op)
            {
                c.porta_step = uint16_t(x << 4);
            }
        }
        else
        {
            PortaNote(c, true);
        }
        break;
    }
}

void Player::VolumeDown(Channel& c, int by)
{
    c.volume = std::max<int8_t>(int8_t(c.volume - by), 0);
    SetChannelVolume(c);
}

void Player::VolumeUp(Channel& c, int by)
{
    c.volume = std::min<int8_t>(int8_t(c.volume + by), 64);
    SetChannelVolume(c);
}

void Player::PanLeft(Channel& c, int by)
{
    c.panning = std::max<int8_t>(int8_t(c.panning - by), -64);
    SetChannelPan(c);
}

void Player::PanRight(Channel& c, int by)
{
    c.panning = std::min<int8_t>(int8_t(c.panning + by), 64);
    SetChannelPan(c);
}

void Player::VolumeSlideXm(Channel& c, bool on_row)
{
    if (on_row)
    {
        if (c.effect_op)
        {
            c.volume_slide = c.effect_op;
        }
        if (c.effect == kVolumeSlideVibratoXm && c.restore_note && c.last_effect == kVibrato)
        {
            c.restore_note = 0;
        }
        return;
    }

    SlideVolume(c);
}

void Player::SlideVolume(Channel& c)
{
    if (SlideLevel(c.volume, c.volume_slide))
    {
        SetChannelVolume(c);
    }
}

void Player::RaisePeriod(Channel& c, int by)
{
    c.period = int16_t(c.period + by);
    SetChannelFreq(c);
}

void Player::LowerPeriod(Channel& c, int by)
{
    c.period = int16_t(c.period - by);
    if (c.period > 0)
    {
        SetChannelFreq(c);
    }
    else
    {
        StopChannel(c);
    }
}

void Player::PortaNote(Channel& c, bool on_row)
{
    if (on_row)
    {
        if (c.effect_op)
        {
            c.porta_step = c.effect_op;
        }
        return;
    }

    const auto period_of = [&](int note)
    {
        return int(rom_.U16(info_.periods + 2 * uint32_t(int16_t(note))));
    };

    // The period slides towards the goal and stops there. With glissando, the note plays the nearest note of the table
    // on the slide's side.
    const int16_t step = int16_t(c.porta_step << 2);
    if (c.period > c.porta_goal)
    {
        c.period = int16_t(c.period - step);
        if (c.period < c.porta_goal)
        {
            c.period = c.porta_goal;
        }
        if (c.glissando)
        {
            if (c.period < period_of(c.gliss_note))
            {
                do
                {
                    c.gliss_note++;
                } while (c.period < period_of(c.gliss_note));
            }
            if (c.period > period_of(c.gliss_note))
            {
                c.gliss_note--;
            }
        }
    }
    else if (c.period < c.porta_goal)
    {
        c.period = int16_t(c.period + step);
        if (c.period > c.porta_goal)
        {
            c.period = c.porta_goal;
        }
        if (c.glissando)
        {
            if (c.period > period_of(c.gliss_note))
            {
                do
                {
                    c.gliss_note--;
                } while (c.period > period_of(c.gliss_note));
            }
            if (c.period < period_of(c.gliss_note))
            {
                c.gliss_note++;
            }
        }
    }
    else
    {
        return;
    }

    if (!c.glissando)
    {
        SetChannelFreq(c);
        return;
    }
    const uint16_t period = rom_.U16(info_.periods + 2 * uint32_t(c.gliss_note));
    c.played_period = int16_t(period);
    SetFreq(c.handle, FrequencyOf(period));
}

void Player::Vibrato(Channel& c, bool on_row)
{
    if (!on_row)
    {
        VibratoTick(c);
        return;
    }

    if (c.play_note && c.vibrato_reset)
    {
        c.vibrato_pos = vibrato_offset_;
    }
    if (c.effect_op & 0xF)
    {
        c.vibrato_depth = uint8_t((c.effect_op & 0xF) << 2);
    }
    if (c.effect_op & 0xF0)
    {
        c.vibrato_speed = uint8_t(c.effect_op >> 4);
    }
    if (c.restore_note && c.last_effect == kVolumeSlideVibrato)
    {
        c.restore_note = 0;
    }
}

void Player::VibratoTick(Channel& c)
{
    const uint16_t period =
        uint16_t(uint16_t(c.period) + ((Wave(c.vibrato_wave, c.vibrato_pos) * c.vibrato_depth) >> 7));
    c.played_period = int16_t(period);
    SetFreq(c.handle, FrequencyOf(period));
    c.vibrato_pos = uint8_t((c.vibrato_pos + c.vibrato_speed) & 0x3F);
    c.restore_note = 1;
}

void Player::Tremor(Channel& c, bool on_row)
{
    const uint8_t op = c.effect_op;
    if (on_row && op)
    {
        c.tremor_on = uint8_t((op >> 4) + 1);
        c.tremor_cycle = uint8_t(c.tremor_on + (op & 0xF) + 1);
    }
    if (c.tremor_count == c.tremor_on && Since2004())
    {
        c.played_volume = 0;
        SetPlayedVolume(c);
        c.restore_note = 1;
    }
    else if (c.tremor_count == c.tremor_on)
    {
        SetVol(c.handle, 0);
        c.restore_note = 1;
    }
    else if (c.tremor_count == c.tremor_cycle)
    {
        c.tremor_count = 0;
        SetChannelVolume(c);
        c.restore_note = 0;
    }
    c.tremor_count++;
}

void Player::Tremolo(Channel& c, bool on_row)
{
    const uint8_t op = c.effect_op;
    if (on_row)
    {
        if (c.play_note && c.tremolo_reset)
        {
            c.tremolo_pos = 0;
        }
        if (op & 0xF)
        {
            c.tremolo_depth = uint8_t(op & 0xF);
        }
        if (op & 0xF0)
        {
            c.tremolo_speed = uint8_t(op >> 4);
        }
    }
    else
    {
        // From 1.2.2.17 to 2003/09/01, the volume shifts down by 6 bits rather than 12. The mixer gets the bottom byte
        // of a volume 64 times too loud. The builds before 1.2.2.17 have no channel volume, and 6 bits is right there.
        // Later builds keep the volume as the played one.
        const int delta = int8_t((Wave(c.tremolo_wave, c.tremolo_pos) * c.tremolo_depth) >> 6);
        const int volume = std::clamp<int>(int8_t(uint8_t(delta + c.volume)), 0, 64);
        if (Since2004())
        {
            c.played_volume = int8_t(volume);
            SetPlayedVolume(c);
        }
        else
        {
            const int channel_volume = info_.build < Build::kV1_2_2_17 ? 1 : c.channel_volume;
            SetVol(c.handle, uint32_t(volume * channel_volume * c.envelope_volume) >> 6);
        }
        c.tremolo_pos = uint8_t((c.tremolo_pos + c.tremolo_speed) & 0x3F);
        c.restore_note = 1;
    }
}

void Player::Panbrello(Channel& c, bool on_row)
{
    const uint8_t op = c.effect_op;
    if (on_row)
    {
        if (c.play_note && c.panbrello_reset)
        {
            c.panbrello_pos = 0;
        }
        if (op & 0xF)
        {
            c.panbrello_depth = uint8_t(op & 0xF);
        }
        if (op & 0xF0)
        {
            c.panbrello_speed = uint8_t(op >> 4);
        }
    }
    else
    {
        const int delta = int8_t((Wave(c.panbrello_wave, c.panbrello_pos >> 2) * c.panbrello_depth) >> 6);
        const int panning = int8_t(uint8_t(delta + c.panning));
        c.panbrello_pos = uint8_t(c.panbrello_pos + c.panbrello_speed);
        c.played_panning = int8_t(panning);
        SetPan(c.handle, std::clamp(panning + c.envelope_pan, -64, 64));
        c.restore_note = 1;
    }
}

void Player::PanSlide(Channel& c, bool on_row)
{
    // On the row, a fine slide (xF or Fx) slides once by twice its step. The fine slide right marks the volume slide as
    // fine rather than the pan slide, as this build does.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.pan_slide = c.effect_op;
        }

        const int left = c.pan_slide >> 4, right = c.pan_slide & 0xF;
        c.pan_slide_once = 0;
        if (left && right == 0xF)
        {
            c.pan_slide_once = 1;
            PanLeft(c, left << 1);
        }
        else if (right && left == 0xF)
        {
            c.volume_slide_once = 1;
            PanRight(c, right << 1);
        }
        return;
    }

    if (c.pan_slide_once)
    {
        return;
    }

    if (c.pan_slide & 0xF)
    {
        PanRight(c, (c.pan_slide & 0xF) << 1);
    }
    else if (c.pan_slide >> 4)
    {
        PanLeft(c, (c.pan_slide >> 4) << 1);
    }
}

void Player::Retrig(Channel& c, bool on_row)
{
    // On the row, a note that the row doesn't start starts again; between rows, the note starts again every so many
    // ticks, with its volume changed as the high digit says.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.retrig = c.effect_op;
        }
        if (!c.play_note && c.retrig)
        {
            SetPos(c.handle, 0);
        }
        return;
    }

    // Up to 2003/09/01, the whole operand divides the ticks. Later builds take its low digit.
    if (!c.retrig || Div(tick_, Since2004() ? c.retrig & 0xF : c.retrig).second != 0)
    {
        return;
    }

    SetPos(c.handle, 0);

    const int x = c.retrig >> 4;
    if (x >= 1 && x <= 5)
    {
        VolumeDown(c, 1 << (x - 1));
    }
    else if (x >= 9 && x <= 13)
    {
        VolumeUp(c, 1 << (x - 9));
    }
    else if (x == 6 || x == 7 || x == 14 || x == 15)
    {
        // Two thirds, a half, three halves and twice the volume.
        const int volume = c.volume;
        const int changed = x == 6    ? (volume * 342) >> 9
                            : x == 7  ? volume >> 1
                            : x == 14 ? (volume * 3) >> 1
                                      : volume << 1;
        c.volume = int8_t(changed);
        if ((x == 6 ? changed : int(c.volume)) > 64)
        {
            c.volume = 64;
        }
        SetChannelVolume(c);
    }
}

void Player::Arpeggio(Channel& c, bool on_row)
{
    if (on_row)
    {
        if (c.effect_op)
        {
            c.arpeggio = c.effect_op;
        }
        if (!c.play_note)
        {
            SetChannelFreq(c);
        }
        c.arpeggio_step = 0;
        return;
    }

    // Each tick plays the note, the note plus the high digit and the note plus the low digit in turn, with linear
    // frequencies in every module.
    if (!c.sample)
    {
        return;
    }

    c.arpeggio_step++;
    if (c.arpeggio_step > 2)
    {
        c.arpeggio_step = 0;
    }
    int16_t note = c.note;
    if (c.arpeggio_step == 1)
    {
        note = int16_t(uint16_t(c.note) + (c.arpeggio >> 4));
    }
    else if (c.arpeggio_step == 2)
    {
        note = int16_t(uint16_t(c.note) + (c.arpeggio & 0xF));
    }

    const uint32_t capped = std::min<uint32_t>(uint16_t(note), 0x77);
    const uint16_t period = uint16_t(LinearPeriod(capped, rom_.S8(c.sample + kSampleFineTune)));
    c.played_period = int16_t(period);
    SetFreq(c.handle, LinearFreq(period));
    c.restore_note = 1;
}

void Player::PatternLoop(Channel& c)
{
    // An operand of 0 marks the row that the loop goes back to, and another goes back there that many times.
    if (!c.effect_op)
    {
        loop_row_ = int8_t(row_);
        return;
    }

    if (!loop_active_)
    {
        loop_count_ = int8_t(c.effect_op);
        loop_active_ = 1;
    }
    loop_count_--;
    if (loop_count_ == -1)
    {
        loop_active_ = 0;
    }
    else
    {
        goto_row_ = loop_row_;
    }
}

} // namespace ntdlib::krawall
