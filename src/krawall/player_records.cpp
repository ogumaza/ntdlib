// SPDX-License-Identifier: MIT

// The records of the Krawall player: the player's, a player channel's and a mixer channel's, laid out as the game's
// build keeps them, and the mixer's other variables.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/krawall/build.h"
#include "ntdlib/krawall/player.h"

namespace ntdlib::krawall
{

std::vector<uint8_t> Player::PlayerRecord() const
{
    // From 2004/09/17 on, the row and the row of a break are halfwords. They move the bytes between them.
    std::vector<uint8_t> r(0x34);
    xsflib::StoreLe32(&r[0x00], mode_);
    xsflib::StoreLe32(&r[0x04], status_);
    xsflib::StoreLe32(&r[0x08], valid_ ? module_.address : 0);
    xsflib::StoreLe16(&r[0x0C], tempo_);
    if (info_.build < Build::k2004_09_17)
    {
        r[0x0E] = speed_;
        r[0x0F] = tick_;
        r[0x10] = uint8_t(row_);
        r[0x11] = order_;
        r[0x18] = uint8_t(goto_order_);
        r[0x19] = uint8_t(goto_row_);
    }
    else
    {
        xsflib::StoreLe16(&r[0x0E], row_);
        r[0x10] = speed_;
        r[0x11] = tick_;
        r[0x12] = order_;
        r[0x13] = uint8_t(goto_order_);
        xsflib::StoreLe16(&r[0x18], uint16_t(goto_row_));
    }
    xsflib::StoreLe32(&r[0x14], data_);
    r[0x1A] = uint8_t(loop_row_);
    r[0x1B] = uint8_t(loop_count_);
    r[0x1C] = uint8_t(loop_active_);
    r[0x1D] = uint8_t(volume_);
    r[0x1E] = volume_slide_;
    r[0x1F] = vibrato_offset_;
    r[0x20] = instrument_based_;
    r[0x21] = fast_slides_;
    xsflib::StoreLe32(&r[0x2C], song_start_);
    xsflib::StoreLe32(&r[0x30], pattern_);

    return r;
}

std::vector<uint8_t> Player::ChannelRecord(const Channel& c) const
{
    // From 2004/09/17 on, the played volume and pan follow the start point. They move the effect and the periods by 2
    // bytes, and with the alignment of the words, the rest of the record by 4.
    const bool later = info_.build >= Build::k2004_09_17;
    std::vector<uint8_t> r(later ? 0x64 : 0x60);
    xsflib::StoreLe32(&r[0x00], c.handle);
    r[0x04] = uint8_t(c.volume);
    r[0x05] = uint8_t(c.channel_volume);
    r[0x06] = uint8_t(c.panning);
    r[0x07] = c.start_point;
    if (later)
    {
        r[0x08] = uint8_t(c.played_volume);
        r[0x09] = uint8_t(c.played_panning);
    }

    uint8_t* const periods = r.data() + (later ? 2 : 0);
    periods[0x08] = c.effect;
    xsflib::StoreLe16(&periods[0x0A], uint16_t(c.note));
    xsflib::StoreLe16(&periods[0x0C], uint16_t(c.period));
    xsflib::StoreLe16(&periods[0x0E], uint16_t(c.played_period));

    uint8_t* const rest = r.data() + (later ? 4 : 0);
    xsflib::StoreLe32(&rest[0x10], c.sample);
    xsflib::StoreLe32(&rest[0x14], c.instrument);
    rest[0x18] = c.last_effect;
    rest[0x19] = c.effect_op;
    xsflib::StoreLe16(&rest[0x1A], c.porta_step);
    xsflib::StoreLe16(&rest[0x1C], uint16_t(c.porta_goal));
    xsflib::StoreLe16(&rest[0x1E], uint16_t(c.gliss_note));
    rest[0x20] = c.vibrato_pos;
    rest[0x21] = c.vibrato_speed;
    rest[0x22] = c.vibrato_depth;
    rest[0x23] = c.vibrato_reset;
    xsflib::StoreLe32(&rest[0x24], c.vibrato_wave);
    rest[0x28] = c.tremolo_pos;
    rest[0x29] = c.tremolo_speed;
    rest[0x2A] = c.tremolo_depth;
    rest[0x2B] = c.tremolo_reset;
    xsflib::StoreLe32(&rest[0x2C], c.tremolo_wave);
    rest[0x30] = c.panbrello_pos;
    rest[0x31] = c.panbrello_speed;
    rest[0x32] = c.panbrello_depth;
    rest[0x33] = c.panbrello_reset;
    xsflib::StoreLe32(&rest[0x34], c.panbrello_wave);
    const uint8_t bytes[] = {c.glissando,
                             c.tremor_count,
                             c.tremor_on,
                             c.tremor_cycle,
                             c.volume_slide,
                             c.fine_volume_slide,
                             c.channel_volume_slide,
                             c.volume_slide_once,
                             c.pan_slide,
                             c.pan_slide_once,
                             c.arpeggio,
                             c.arpeggio_step,
                             c.porta,
                             c.fine_porta,
                             c.extra_fine_porta,
                             c.retrig,
                             c.restore_note,
                             c.play_note,
                             c.in_row,
                             c.envelopes_on,
                             c.volume_env_state};
    std::copy(std::begin(bytes), std::end(bytes), rest + 0x38);
    rest[0x4D] = uint8_t(c.envelope_volume);
    xsflib::StoreLe16(&rest[0x4E], c.volume_env_level);
    rest[0x50] = c.volume_env_point;
    rest[0x51] = c.volume_env_tick;
    rest[0x52] = c.volume_env_target;
    rest[0x53] = c.fading;
    xsflib::StoreLe16(&rest[0x54], uint16_t(c.fade));
    rest[0x56] = c.pan_env_state;
    rest[0x57] = uint8_t(c.envelope_pan);
    xsflib::StoreLe16(&rest[0x58], c.pan_env_level);
    rest[0x5A] = c.pan_env_point;
    rest[0x5B] = c.pan_env_tick;
    rest[0x5C] = c.pan_env_target;
    rest[0x5D] = c.column_effect;
    rest[0x5E] = c.instrument_vibrato_pos;

    return info_.build < Build::kV1_2_2_17 ? OldChannelRecord(r) : r;
}

std::vector<uint8_t> Player::MixRecord(const MixChannel& m) const
{
    if (info_.build < Build::kV1_4)
    {
        return OldMixRecord(m);
    }

    std::vector<uint8_t> r(0x2C);
    r[0x00] = m.volume;
    r[0x01] = uint8_t(m.pan);
    r[0x02] = m.status;
    r[0x03] = m.loop;
    xsflib::StoreLe32(&r[0x04], m.start);
    xsflib::StoreLe32(&r[0x08], m.pos);
    xsflib::StoreLe32(&r[0x0C], m.end);
    xsflib::StoreLe32(&r[0x10], m.loop_length);
    xsflib::StoreLe32(&r[0x14], uint32_t(m.inc));
    xsflib::StoreLe32(&r[0x18], m.handle);
    xsflib::StoreLe16(&r[0x1C], m.frac);
    r[0x1E] = m.left;
    r[0x1F] = m.right;
    r[0x20] = m.hq;
    r[0x21] = m.mix;
    r[0x22] = m.hq_sample;
    r[0x24] = m.sfx;
    xsflib::StoreLe32(&r[0x28], m.loop_from);

    return r;
}

std::vector<uint8_t> Player::Globals() const
{
    std::vector<uint8_t> r(16);
    r[0] = master_[0];
    r[1] = master_[1];
    r[2] = hq_mode_;
    r[3] = hq_ramp_;
    xsflib::StoreLe16(&r[4], timer_speed_);
    xsflib::StoreLe16(&r[6], timer_count_);
    xsflib::StoreLe32(&r[8], uint32_t(music_volume_));
    xsflib::StoreLe32(&r[12], plays_);

    return r;
}

} // namespace ntdlib::krawall
