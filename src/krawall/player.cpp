// SPDX-License-Identifier: MIT

#include "ntdlib/krawall/player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/krawall/build.h"
#include "ntdlib/krawall/driver.h"
#include "player_shared.h"

namespace ntdlib::krawall
{

using ntdlib::agb::Hex;
using ntdlib::agb::WarnOnce;
namespace
{

// The first 13 bytes of kramPlayExt()'s code. In IWRAM, the code follows the mixer's master volumes, quality and ramp.
// The bytes are the same in every build from 1.4 to 2005-04-21.
constexpr uint8_t kPlayExtStart[] = {0xF0, 0x47, 0x2D, 0xE9, 0x02, 0x50, 0xA0, 0xE1, 0xFF, 0x40, 0x05, 0xE2, 0x2C};

// An effect's entry in the player's table of effects.
struct EffectEntry
{
    bool between = false; // whether the effect runs on the ticks between rows as well as on the row's tick
    int second = 0;       // the effect whose part for the ticks between rows runs after it, or 0
};

constexpr EffectEntry EntryOf(int effect)
{
    switch (effect)
    {
    case kVolumeSlideS3m:
    case kVolumeSlideXm:
    case kPortaDownXm:
    case kPortaDownS3m:
    case kPortaUpXm:
    case kPortaUpS3m:
    case kPortaNote:
    case kVibrato:
    case kTremor:
    case kArpeggio:
    case kChannelVolumeSlide:
    case kPanSlide:
    case kRetrig:
    case kTremolo:
    case kFineVibrato:
    case kGlobalVolumeSlide:
    case kPanbrello:
    case kNoteCut:
    case kNoteDelay:
        return {.between = true};
    case kVolumeSlideVibrato:
    case kVolumeSlideVibratoXm:
        return {.between = true, .second = kVibrato};
    case kVolumeSlidePorta:
    case kVolumeSlidePortaXm:
        return {.between = true, .second = kPortaNote};
    default:
        return {};
    }
}

// Returns whether a volume column's effect runs on the ticks between rows.
constexpr bool ColumnBetween(int effect)
{
    switch (effect)
    {
    case kColumnVolumeSlideDown:
    case kColumnVolumeSlideUp:
    case kColumnVibrato:
    case kColumnPanSlideLeft:
    case kColumnPanSlideRight:
    case kColumnPortaNote:
        return true;
    default:
        return false;
    }
}

// The slowest tempo the sound timer takes: 16 times 24 ticks a minute.
constexpr int kMinTimerBpm = 16 * 24;

// The samples that the timer's step divides: 15 seconds of the mix.
constexpr int32_t kTimerSamples = 15 * 16384;

// Returns `v` shifted right by the bottom byte of `amount`, as an ARM register shift does.
uint32_t ShiftRight(uint32_t v, uint32_t amount)
{
    amount &= 0xFF;
    return amount >= 32 ? 0 : v >> amount;
}

} // namespace

int PlayMode(const ModuleSong& song)
{
    if (song.mode >= 0)
    {
        return song.mode;
    }
    if (song.jingle)
    {
        return kModeJingle;
    }

    return song.song >= 0 ? kModeLoop | kModeSong : kModeLoop;
}

uint32_t AmigaPeriod(const Rom& rom, const DriverInfo& info, uint32_t note, int fine_tune)
{
    const uint32_t factor = rom.U16(info.fine_tunes + 2 * uint32_t(32 + (fine_tune >> 2)));
    return (rom.U16(info.periods + 2 * note) * factor) >> 15;
}

int LinearPeriod(uint32_t note, int fine_tune)
{
    return (fine_tune >> 1) + int(note << 6) + 0x600;
}

Player::Player(const Rom& rom, const DriverInfo& info, uint32_t module, int mode, int song) : rom_(rom), info_(info)
{
    owners_.fill(-1);
    if (!ReadModule(rom, info.build, module, module_))
    {
        Warn("the module at " + Hex(module) + " can't be read");
        return;
    }
    if (module_.channels > kPlayerChannels)
    {
        Warn("the module has " + std::to_string(module_.channels) + " channels, more than the player's " +
             std::to_string(kPlayerChannels));
        return;
    }

    // The mixer as the game's call to kramQualityMode() leaves it. Up to 2003/09/01, flag 16 turns the ramp at a stop
    // on. From 2004/09/17 on, the ramp is on, and flag 32 turns it off.
    if (Since2004())
    {
        hq_ramp_ = (info.quality & 0x20) ? uint8_t(0) : uint8_t(1);
    }
    else
    {
        hq_ramp_ = (info.quality & 0x10) ? uint8_t(1) : uint8_t(0);
    }
    hq_mode_ = (info.quality & 0xF) > 2 ? uint8_t(0) : uint8_t(info.quality & 0xF);
    if (info.build < Build::kV1_4)
    {
        // The builds before 1.4 have no ramping, and take the whole argument as the mode.
        hq_ramp_ = 0;
        hq_mode_ = info.quality > 2 ? uint8_t(0) : info.quality;
    }

    // The music volume, from 0 in a game that fades the music in.
    music_volume_ = info.music_fade_in ? 0 : int32_t(info.music_volume);

    // krapPlay(), with the player stopped: a jingle then plays as a module of its own. The first tick plays the first
    // row, at the module's volume and tempo.
    if (mode & kModeJingle)
    {
        mode &= ~kModeJingle;
    }
    mode_ = uint32_t(mode);
    speed_ = tick_ = module_.speed;
    volume_ = 64;
    volume_slide_ = 0;
    SetGlobalVolume();
    tempo_ = module_.tempo;
    SetTimerBpm(tempo_ * 24);

    // The song's first order, and the order that the module goes back to after its last.
    if (song > 63)
    {
        song = 0;
    }
    order_ = module_.song_starts[size_t(song)];
    song_start_ = order_;
    if (!(mode_ & kModeSong))
    {
        song_start_ = module_.restart;
    }
    row_ = 0;
    FindRow();

    goto_row_ = goto_order_ = -1;
    loop_row_ = loop_count_ = loop_active_ = 0;
    instrument_based_ = module_.instruments ? uint8_t(1) : uint8_t(0);
    fast_slides_ = module_.fast_slides ? uint8_t(1) : uint8_t(0);
    vibrato_offset_ = instrument_based_ ? uint8_t(32) : uint8_t(0);

    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        c.vibrato_pos = vibrato_offset_;
        c.vibrato_reset = 1;
        c.vibrato_wave = info.sine;
        c.tremolo_reset = 1;
        c.tremolo_wave = info.sine;
        c.panbrello_reset = 0;
        c.panbrello_wave = info.sine;
        c.panning = module_.channel_pan[size_t(i)];
        c.envelope_volume = 64;
        c.envelope_pan = 0;
        c.channel_volume = 64;
    }

    status_ = 2;
    SetTimer(true);
    valid_ = true;
}

Player::Player(const Rom& rom, const DriverInfo& info, const ModuleSong& song)
    : Player(rom, info, song.module, PlayMode(song), std::max(song.song, 0))
{
    if (valid_ && song.music_volume >= 0)
    {
        music_volume_ = song.music_volume;
        SetGlobalVolume();
    }
}

void Player::Warn(const std::string& text)
{
    WarnOnce(warnings_, text);
}

double Player::OutputScale() const
{
    // The mix adds each point times the side's volume, shifted down 3 bits, and its output shifts that down by the
    // master volume's shift and scales it by the master volume's level over 128. The builds before 1.4 take the whole
    // master volume as the level, and shift by 5.
    if (info_.build < Build::kV1_4)
    {
        return double(info_.master_volume) / double(1u << (3 + 5));
    }

    const uint32_t shift = (info_.master_volume >> 16) ? (info_.master_volume >> 16) : 5;
    return double(info_.master_volume & 0xFF) / double(1u << (3 + shift));
}

// ---------------------------------------------------------------------------------------------------------------------
// The mixer

uint32_t Player::Play(uint32_t sample, int sample_index, int sfx, uint32_t handle, uint32_t freq, uint32_t volume,
                      int32_t pan)
{
    // With ramping, the channel's note ramps down first, if it's still the channel's.
    MixChannel* m = &mixer_[handle & 0x1F];
    if (hq_ramp_ && m->handle == handle)
    {
        Stop(handle);
    }

    // A channel whose play is another's is left alone, and the play takes the first free channel instead.
    uint32_t channel = handle & 0xFF;
    if (!Plays(*m, handle))
    {
        channel = 0;
        while (channel < uint32_t(MixChannels()) && mixer_[channel].status != 0)
        {
            channel++;
        }
        if (channel >= uint32_t(MixChannels()))
        {
            return 0;
        }
        m = &mixer_[channel];
    }

    // The modified build's kramPlayExt() turns interpolation off. Its note on then sets it from the instrument.
    const bool modified = info_.build == Build::kModified;
    plays_++;
    m->sfx = uint8_t(sfx & 1);
    if (modified)
    {
        m->hq = 0;
    }
    else
    {
        m->hq = hq_mode_ == 2 ? uint8_t(8) : uint8_t((m->hq_sample & hq_mode_) << 3);
    }
    m->handle = channel | (plays_ << 8);
    SetPanning(*m, pan);
    m->volume = uint8_t(volume);
    m->inc = int32_t(freq << 2);
    SetSides(*m);
    m->hq_sample = modified ? uint8_t(0) : rom_.U8(sample + kSampleInterpolate);
    m->pos = m->start = sample + kSamplePoints;
    m->loop_length = rom_.U32(sample + kSampleLoopLength);
    m->end = SampleEnd(rom_, info_.build, sample);
    m->loop = rom_.U8(sample + kSampleLoop);
    m->status = 1;
    m->loop_from = m->end - m->loop_length;
    m->frac = 0;
    m->moves++;
    m->sample = sample;
    m->sample_index = sample_index;

    return m->handle;
}

bool Player::Plays(const MixChannel& m, uint32_t handle) const
{
    return (handle & 0xFF) < uint32_t(MixChannels()) && m.handle == handle;
}

bool Player::Stop(uint32_t handle)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    // Without ramping a stop is at once. With it, a channel that still plays loud enough ramps down from its point.
    if (!hq_ramp_ || m.status != 1 || m.volume <= 5)
    {
        m.status = 0;
        return true;
    }

    const uint8_t point = rom_.U8(m.pos);
    m.sfx = 16;
    m.loop_from = uint32_t(uint16_t(point << 4)) | (uint32_t(uint16_t(0x80 - point)) << 16);
    m.mix = 7;
    m.inc = 0;
    m.hq = 0;
    m.handle = 0;
    m.pos = m.start;

    return true;
}

bool Player::SetFreq(uint32_t handle, uint32_t freq)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    m.inc = m.inc >= 0 ? int32_t(freq << 2) : int32_t(0u - (freq << 2));

    return true;
}

bool Player::SetVol(uint32_t handle, uint32_t volume)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    m.volume = uint8_t(volume);
    SetSides(m);

    return true;
}

bool Player::SetPan(uint32_t handle, int32_t pan)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    SetPanning(m, pan);
    SetSides(m);

    return true;
}

bool Player::SetPos(uint32_t handle, uint32_t pos)
{
    MixChannel& m = mixer_[handle & 0x1F];
    if (!Plays(m, handle))
    {
        return false;
    }

    if (m.start + pos < m.end)
    {
        m.pos = m.start + pos;
        m.frac = 0;
        m.moves++;
    }

    return true;
}

void Player::SetSides(MixChannel& m) const
{
    // A ramping channel keeps its count of samples to ramp (4 to 16) where the mixer keeps the choice of master volume.
    // The two master volumes are followed by the quality and the ramp, then by the code of kramPlayExt(). The mixer
    // reads the master volume of such a channel from those bytes.
    uint32_t master = 0;
    if (m.sfx < master_.size())
    {
        master = master_[m.sfx];
    }
    else if (m.sfx == 2)
    {
        master = hq_mode_;
    }
    else if (m.sfx == 3)
    {
        master = hq_ramp_;
    }
    else if (size_t(m.sfx - 4) < std::size(kPlayExtStart))
    {
        master = kPlayExtStart[m.sfx - 4];
    }

    const uint32_t level = master * m.volume;
    m.left = uint8_t((level * uint32_t(64 - m.pan)) >> 13);
    m.right = uint8_t((level * uint32_t(64 + m.pan)) >> 13);
}

void Player::SetPanning(MixChannel& m, int32_t pan) const
{
    // In mono, every channel mixes to the left buffer alone, at the level of a pan to the left. From 2005-04-21 on, and
    // in the unsigned and modified builds, it mixes at the level of the centre. A channel in the centre then has a
    // mixing routine of its own.
    const bool centre = info_.build >= Build::k2005_04_21;
    if (!info_.stereo)
    {
        m.mix = 1;
        m.pan = centre ? int8_t(0) : int8_t(-64);
        return;
    }

    if (pan == 0)
    {
        m.mix = centre ? uint8_t(4) : uint8_t(0);
    }
    else
    {
        m.mix = uint8_t(pan > 0 ? (pan >> 6) + 2 : (-pan) >> 6);
    }
    m.pan = int8_t(pan);
}

void Player::ResetChannels()
{
    // Up to 2003/09/01, every channel that plays takes the new master volume. From 2004/09/17 on, only the music's
    // channels do, and from 2004/11/14 on, they set their mixing routines again too.
    for (MixChannel& m : mixer_)
    {
        if (m.status == 0 || (Since2004() && m.sfx != 0))
        {
            continue;
        }

        if (info_.build >= Build::k2004_11_14)
        {
            SetPanning(m, m.pan);
        }
        SetSides(m);
    }
}

void Player::SetMusicVolume(int32_t volume)
{
    master_[0] = uint32_t(volume) > 255 ? uint8_t(255) : uint8_t(volume);
    ResetChannels();
}

void Player::SetTimerBpm(int bpm)
{
    bpm = std::max(bpm, kMinTimerBpm);
    timer_speed_ = timer_count_ = timer_backup_ = uint16_t((kTimerSamples / bpm) << 2);
}

void Player::SetTimer(bool on)
{
    timer_on_ = on;

    // The builds before 1.4 keep the timer's count as it is.
    if (info_.build < Build::kV1_4)
    {
        return;
    }

    if (on)
    {
        timer_count_ = timer_backup_;
    }
    else
    {
        timer_backup_ = timer_count_;
    }
}

void Player::FadeIn()
{
    if (info_.music_fade_in && status_ != 0)
    {
        music_volume_ = std::min(music_volume_ + int32_t(info_.music_fade_in), int32_t(info_.music_volume));
        SetGlobalVolume();
    }
}

void Player::SkipFrame()
{
    output_[0].clear();
    output_[1].clear();
    enhanced_[0].clear();
    enhanced_[1].clear();
    peaks_ = {};
    FadeIn();
}

void Player::Frame()
{
    if (mix_output_)
    {
        output_[0].clear();
        output_[1].clear();
    }
    enhanced_[0].clear();
    enhanced_[1].clear();
    peaks_ = {};
    FadeIn();

    if (info_.build < Build::kV1_4)
    {
        OldFrame();
        return;
    }

    // The worker mixes up to each tick that falls in the frame, runs it, and mixes the rest.
    uint32_t amount = uint32_t(info_.frame_samples);
    while (amount)
    {
        if (timer_count_ > amount || !timer_on_)
        {
            Mix(amount);
            timer_count_ = uint16_t(timer_count_ - amount);
            break;
        }

        const uint32_t count = timer_count_;
        Mix(count);
        amount -= count;
        Timer();
        if (on_tick_)
        {
            on_tick_(elapsed_);
        }
        timer_count_ = timer_speed_;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The player

void Player::Timer()
{
    // An audition runs the instrument's effects alone.
    ticks_++;
    if (audition_)
    {
        if (instrument_based_)
        {
            RunEnvelopes();
        }
        return;
    }

    tick_++;
    if (tick_ >= speed_)
    {
        tick_ = 0;
        PlayRow();
    }
    else
    {
        PlayTickBetweenRows();
    }

    if (instrument_based_)
    {
        RunEnvelopes();
    }
}

void Player::FindRow()
{
    // The order's pattern, from the module's table of patterns.
    pattern_ = rom_.U32(module_.address + ModulePatterns(info_.build) + 4 * uint32_t(module_.orders[order_]));
    data_ = PatternData(info_.build, pattern_);
    if (!rom_.Contains(pattern_, data_ - pattern_))
    {
        Warn("order " + std::to_string(order_) + " names a pattern outside the ROM, so the module stops there");
        StopModule();
        return;
    }
    if (row_ == 0)
    {
        return;
    }

    // The index gives every 4th row's data, and the rows between are skipped.
    data_ += rom_.U16(pattern_ + 2 * ((uint32_t(row_) >> 2) & 0xF));
    for (int left = row_ & 3; left > 0;)
    {
        const uint8_t flags = rom_.U8(data_++);
        if (!flags)
        {
            left--;
            continue;
        }

        ReadEntry(rom_, info_.build, flags, data_);
    }
}

void Player::NextRow()
{
    if (info_.build < Build::kV1_2_2_15)
    {
        OldNextRow();
        return;
    }
    if (info_.build < Build::k2003_09_01)
    {
        NextRowPlayer();
        return;
    }

    // A jump goes to its order's first row, through the order's end.
    const bool byte_rows = info_.build < Build::k2004_09_17;
    if (goto_order_ >= 0)
    {
        order_ = uint8_t(goto_order_ - 1);
        row_ = byte_rows ? 0xFE : 0x2000;
        goto_order_ = -1;
    }

    row_ = byte_rows ? uint8_t(row_ + 1) : uint16_t(row_ + 1);
    if (row_ < PatternRows(rom_, info_.build, pattern_) && goto_row_ < 0)
    {
        return;
    }

    // The next order, past the markers between songs (+++).
    order_++;
    bool marker = false;
    while (module_.orders[order_] == kOrderSkip && order_ < module_.order_count)
    {
        order_++;
        marker = true;
    }

    // Past the last order, or at a marker in song mode, the module goes back to its restart or its song's start.
    if (order_ >= module_.order_count || (marker && (mode_ & kModeSong)))
    {
        order_ = uint8_t(song_start_);
        if (!(mode_ & kModeLoop))
        {
            StopModule();
            return;
        }
    }
    row_ = 0;
    FindRow();

    // A break goes to its row of the next order, or the first row if the pattern is shorter.
    if (goto_row_ >= 0)
    {
        row_ = uint32_t(goto_row_) >= PatternRows(rom_, info_.build, pattern_) ? uint16_t(0) : uint16_t(goto_row_);
        FindRow();
        goto_row_ = -1;
    }
}

void Player::PlayRow()
{
    row_order_ = order_;
    row_number_ = row_;
    row_data_ = data_;
    row_loop_state_ =
        uint32_t(uint8_t(loop_row_)) | (uint32_t(uint8_t(loop_count_)) << 8) | (uint32_t(uint8_t(loop_active_)) << 16);

    while (status_ != 0)
    {
        const uint32_t at = data_;
        const uint8_t flags = rom_.U8(data_++);
        if (!flags)
        {
            break;
        }

        const RowEntry entry = ReadEntry(rom_, info_.build, flags, data_);
        channels_[size_t(entry.channel)].entry = at;
        PlayEntry(entry);
    }
    row_end_ = data_;

    EndRow();
    if (status_ != 0)
    {
        NextRow();
    }
}

void Player::PlayEntry(const RowEntry& entry)
{
    // The channel's note and instrument, volume and effect, whichever the entry gives.
    Channel& c = channels_[size_t(entry.channel)];
    c.in_row = 1;
    c.last_effect = c.effect;
    c.effect = entry.effect;

    bool sample_set = false;
    const bool instrument_volume = EntryInstrument(c, entry, sample_set);
    const bool set_volume = EntryVolume(c, entry) || instrument_volume;
    EntryNote(c, entry, sample_set);

    // The effect's part for the row, then the note or the volume.
    c.effect_op = entry.effect_op;
    if (entry.effect)
    {
        Effect(c, entry.effect, true);
    }
    if (c.play_note)
    {
        PlayNote(c);
    }
    else if (set_volume)
    {
        SetChannelVolume(c);
    }
}

bool Player::EntryInstrument(Channel& c, const RowEntry& entry, bool& sample_set)
{
    // An instrument sets the sample and its volume, and in a module with instruments, its pan too.
    if (!entry.instrument)
    {
        return false;
    }

    const uint32_t index = uint16_t(entry.instrument - 1);
    if (!instrument_based_)
    {
        c.sample = rom_.U32(info_.samples + 4 * index);
        c.sample_index = int(index);
        c.volume = c.played_volume = int8_t(rom_.U8(c.sample + kSampleVolume));
        return true;
    }
    if (!entry.note)
    {
        return false;
    }

    c.instrument = module_.instruments_at ? module_.instruments_at + kInstrumentSize * index
                                          : rom_.U32(info_.instruments + 4 * index);
    const uint32_t sample_index = rom_.U16(c.instrument + 2 * uint32_t(entry.note) - 2);
    c.sample = rom_.U32(info_.samples + 4 * sample_index);
    c.sample_index = int(sample_index);
    if (info_.build >= Build::k2003_09_01)
    {
        c.panning = c.played_panning = int8_t(rom_.U8(c.sample + kSamplePan));
    }
    c.volume = c.played_volume = int8_t(rom_.U8(c.sample + kSampleVolume));
    sample_set = true;

    return true;
}

bool Player::EntryVolume(Channel& c, const RowEntry& entry)
{
    // The volume column: a volume, or from 0x61 on, an effect. The builds before 1.4 take a volume up to 0x50, and
    // an effect from 0x51.
    if (!entry.volume)
    {
        return false;
    }
    if (entry.volume > (info_.build >= Build::kV1_4 ? 0x60 : 0x50))
    {
        c.column_effect = uint8_t(entry.volume - 0x60);
        VolumeColumnEffect(c, true);
        return false;
    }

    c.volume = int8_t(entry.volume - 0x10);
    if (c.volume > 64)
    {
        c.volume = 64;
    }
    c.played_volume = c.volume;

    // The builds before 1.2.2.14 keep the column's last effect. It goes on between the rows.
    if (info_.build >= Build::kV1_2_2_14)
    {
        c.column_effect = 0;
    }

    return true;
}

void Player::EntryNote(Channel& c, const RowEntry& entry, bool sample_set)
{
    // A note, unless the channel has no sample: a note off releases the note, a note with a slide to it sets the
    // slide's goal, and any other starts a note.
    c.play_note = 0;
    if (!entry.note || !c.sample)
    {
        return;
    }
    if (entry.note == kNoteOff)
    {
        NoteOff(c);
        return;
    }

    const uint8_t n = uint8_t(entry.note - 1 + rom_.U8(c.sample + kSampleRelativeNote));
    if (entry.effect == kPortaNote || entry.effect == kVolumeSlidePorta)
    {
        c.porta_goal = int16_t(PeriodOf(n, c.sample));
        return;
    }

    if (instrument_based_)
    {
        if (!sample_set)
        {
            const uint32_t sample_index = rom_.U16(c.instrument + 2 * uint32_t(n));
            c.sample = rom_.U32(info_.samples + 4 * sample_index);
            c.sample_index = int(sample_index);
        }

        // The builds before 2003/09/01 take the pan of the note's sample when the note starts.
        if (info_.build < Build::k2003_09_01)
        {
            c.panning = int8_t(rom_.U8(c.sample + kSamplePan));
        }

        // Up to 2003/09/01, the row starts the envelopes. Later builds start them with the note.
        if (!Since2004())
        {
            RestartEnvelopes(c);
        }
    }

    c.gliss_note = c.note = n;
    c.period = c.played_period = c.porta_goal = int16_t(PeriodOf(n, c.sample));
    c.tremor_count = 0;
    c.start_point = 0;
    c.play_note = 1;
}

void Player::Audition(uint16_t instrument, uint8_t note)
{
    audition_ = true;
    RowEntry entry;
    entry.note = note;
    entry.instrument = instrument;
    PlayEntry(entry);
    channels_[0].in_row = 0;
}

void Player::ReleaseAudition()
{
    RowEntry entry;
    entry.note = kNoteOff;
    PlayEntry(entry);
    channels_[0].in_row = 0;
}

void Player::EndRow()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!c.in_row)
        {
            c.effect_op = 0;
            c.last_effect = c.effect;
            c.effect = 0;
            c.column_effect = 0;
        }
        else
        {
            c.in_row = 0;
        }

        if (c.restore_note && c.effect != c.last_effect)
        {
            SetChannelFreq(c);
            SetChannelVolume(c);
            SetChannelPan(c);
            c.restore_note = 0;
        }
    }
}

void Player::NoteOff(Channel& c)
{
    if (c.volume_env_state)
    {
        c.fading = 1;
        c.volume_env_state = c.volume_env_state == 2 ? uint8_t(3) : uint8_t(4);

        // The builds before 2003/09/01 release a pan envelope that's off too. That envelope then runs.
        if (c.pan_env_state || info_.build < Build::k2003_09_01)
        {
            c.pan_env_state = c.pan_env_state == 2 ? uint8_t(3) : uint8_t(4);
        }
    }
    else
    {
        StopChannel(c);
    }
}

void Player::RestartEnvelopes(Channel& c)
{
    c.envelopes_on = 0;
    c.instrument_vibrato_pos = 0;
    if (rom_.U8(c.instrument + kInstrumentVolumeEnvelope + kEnvelopeFlags) & kEnvelopeOn)
    {
        c.volume_env_state = 3;
        c.volume_env_tick = c.volume_env_point = 0xFF;
        c.volume_env_target = 0;
        if (info_.build < Build::kV1_2_2_14)
        {
            c.volume_env_tick = 0;
        }
        c.fade = 0x7FFF;
        c.fading = 0;
        c.envelopes_on = 1;
    }
    else
    {
        c.envelope_volume = 64;
        c.volume_env_state = 0;
    }
    if (rom_.U8(c.instrument + kInstrumentPanEnvelope + kEnvelopeFlags) & kEnvelopeOn)
    {
        c.pan_env_state = 3;
        c.pan_env_tick = c.pan_env_point = 0xFF;
        c.pan_env_target = 0;
        if (info_.build < Build::kV1_2_2_14)
        {
            c.pan_env_tick = 0;
        }
        c.envelopes_on = 1;
    }
    else
    {
        c.envelope_pan = 0;
        c.pan_env_state = 0;
    }

    // From 2004/09/17 on, an instrument's vibrato runs without envelopes too.
    if (Since2004() && rom_.U8(c.instrument + kInstrumentVibratoRate))
    {
        c.envelopes_on = 1;
    }
}

void Player::PlayTickBetweenRows()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (c.column_effect && ColumnBetween(c.column_effect >> 4))
        {
            VolumeColumnEffect(c, false);
        }

        if (!c.effect || c.effect >= kEffectCount)
        {
            continue;
        }

        const EffectEntry entry = EntryOf(c.effect);
        if (entry.between)
        {
            Effect(c, c.effect, false);
        }
        if (entry.second)
        {
            Effect(c, entry.second, false);
        }
    }
}

void Player::RunEnvelopes()
{
    for (int i = 0; i < module_.channels; i++)
    {
        Channel& c = channels_[size_t(i)];
        if (!c.envelopes_on)
        {
            continue;
        }

        // A note that an envelope stops goes no further.
        const bool old = info_.build < Build::kV1_2_2_14;
        if (c.volume_env_state && !(old ? OldRunVolumeEnvelope(c) : RunVolumeEnvelope(c)))
        {
            continue;
        }
        if (c.pan_env_state > 2 && !(old ? OldRunPanEnvelope(c) : RunPanEnvelope(c)))
        {
            continue;
        }
        RunInstrumentVibrato(c);
    }
}

bool Player::RunVolumeEnvelope(Channel& c)
{
    // The volume envelope and the fade after the release.
    const uint32_t ins = c.instrument;
    bool set_volume = false;
    if (c.fading)
    {
        set_volume = true;
        c.fade = int16_t(c.fade - rom_.U16(ins + kInstrumentFade));
        if (c.fade <= 0)
        {
            StopChannel(c);
            return false;
        }
    }

    if (c.volume_env_state > 2)
    {
        set_volume = true;
        const uint32_t env = ins + kInstrumentVolumeEnvelope;
        c.volume_env_level = uint16_t(c.volume_env_level + rom_.U16(env + 4 * uint32_t(c.volume_env_point) + 2));
        c.volume_env_tick++;
        if (c.volume_env_tick == c.volume_env_target)
        {
            // The next point: a sustain point holds until the release, and the last one loops or stops.
            c.volume_env_point++;
            if ((rom_.U8(env + kEnvelopeFlags) & kEnvelopeSustains) &&
                c.volume_env_point == rom_.U8(env + kEnvelopeSustain) && c.volume_env_state != 4)
            {
                c.volume_env_state = 2;
            }
            if (c.volume_env_point == rom_.U8(env + kEnvelopeLast))
            {
                if (rom_.U8(env + kEnvelopeFlags) & kEnvelopeLoops)
                {
                    c.volume_env_point = rom_.U8(env + kEnvelopeLoopStart);
                    c.volume_env_tick = uint8_t(rom_.U16(env + 4 * uint32_t(c.volume_env_point)));
                }
                else
                {
                    c.volume_env_state = 1;
                    if ((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) == 0)
                    {
                        StopChannel(c);
                        return false;
                    }
                }
            }
            c.volume_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.volume_env_point)) >> 9) << 8);
            c.volume_env_target = uint8_t(rom_.U8(env + 4 * uint32_t(c.volume_env_point) + 4) + 1);
        }
    }

    if (set_volume)
    {
        c.envelope_volume = int8_t((int32_t(c.volume_env_level) * c.fade) >> 23);
        if (!SetPlayedVolume(c))
        {
            StopChannel(c);
            return false;
        }
    }

    return true;
}

bool Player::RunPanEnvelope(Channel& c)
{
    const uint32_t env = c.instrument + kInstrumentPanEnvelope;
    c.pan_env_level = uint16_t(c.pan_env_level + rom_.U16(env + 4 * uint32_t(c.pan_env_point) + 2));
    c.pan_env_tick++;
    if (c.pan_env_tick == c.pan_env_target)
    {
        c.pan_env_point++;
        if ((rom_.U8(env + kEnvelopeFlags) & kEnvelopeSustains) && c.pan_env_point == rom_.U8(env + kEnvelopeSustain) &&
            c.pan_env_state != 4)
        {
            c.pan_env_state = 2;
        }
        if (c.pan_env_point == rom_.U8(env + kEnvelopeLast))
        {
            if (rom_.U8(env + kEnvelopeFlags) & kEnvelopeLoops)
            {
                c.pan_env_point = rom_.U8(env + kEnvelopeLoopStart);
                c.pan_env_tick = uint8_t(rom_.U8(env + 4 * uint32_t(c.pan_env_point)) + 1);
            }
            else
            {
                c.pan_env_state = 1;
            }
        }
        c.pan_env_level = uint16_t((rom_.U16(env + 4 * uint32_t(c.pan_env_point)) >> 9) << 8);
        c.pan_env_target = rom_.U8(env + 4 * uint32_t(c.pan_env_point) + 4);
    }

    c.envelope_pan = int8_t(((c.pan_env_level >> 8) - 32) << 1);
    if (!SetPlayedPan(c))
    {
        StopChannel(c);
        return false;
    }

    return true;
}

void Player::RunInstrumentVibrato(Channel& c)
{
    // The instrument's vibrato, on the pitch that the effects leave.
    const uint32_t ins = c.instrument;
    const uint8_t rate = rom_.U8(ins + kInstrumentVibratoRate);
    if (rate)
    {
        const int32_t wave = Wave(c.vibrato_wave, uint32_t(c.instrument_vibrato_pos) >> 2);
        const uint16_t period =
            uint16_t(uint16_t(c.played_period) + ((wave * rom_.U8(ins + kInstrumentVibratoDepth)) >> 8));
        SetFreq(c.handle, FrequencyOf(period));
        c.instrument_vibrato_pos = uint8_t(c.instrument_vibrato_pos + rate);
    }
}

void Player::PlayNote(Channel& c)
{
    c.restore_note = 0;
    if (!c.sample)
    {
        Warn("a note plays on a channel that has no sample yet, so it's left out");
        return;
    }

    // From 2004/09/17 on, a note in a module with instruments starts the envelopes.
    if (Since2004() && instrument_based_)
    {
        RestartEnvelopes(c);
    }

    const uint32_t freq = FrequencyOf(c.period);
    c.handle =
        Play(c.sample, c.sample_index, 0, c.handle, freq, uint32_t((c.volume * c.channel_volume) >> 6), c.panning);
    if (c.handle)
    {
        owners_[c.handle & 0x1F] = int8_t(&c - channels_.data());
    }
    if (info_.build == Build::kModified && c.handle)
    {
        MixChannel& m = mixer_[c.handle & 0xFF];
        m.hq_sample = rom_.U8(c.instrument + kInstrumentInterpolate);
        m.hq = hq_mode_ == 2 ? uint8_t(8) : uint8_t((m.hq_sample & hq_mode_) << 3);
    }
    if (c.start_point)
    {
        SetPos(c.handle, uint32_t(c.start_point) << 8);
    }
}

void Player::StopModule()
{
    if (status_ == 0)
    {
        return;
    }

    status_ = 0;
    SetTimer(false);
    for (int i = 0; i < module_.channels; i++)
    {
        StopChannel(channels_[size_t(i)]);
    }
}

void Player::SetGlobalVolume()
{
    SetMusicVolume((int32_t(module_.global_volume) * volume_ * music_volume_) >> 13);
}

bool Player::SetChannelVolume(Channel& c)
{
    c.played_volume = c.volume;
    return SetVol(c.handle, uint32_t((c.channel_volume * c.volume * c.envelope_volume) >> 12));
}

bool Player::SetPlayedVolume(Channel& c)
{
    if (!Since2004())
    {
        return SetChannelVolume(c);
    }

    return SetVol(c.handle, uint32_t((c.channel_volume * c.played_volume * c.envelope_volume) >> 12));
}

bool Player::SetChannelPan(Channel& c)
{
    c.played_panning = c.panning;
    const int pan = int8_t(uint8_t(c.panning + c.envelope_pan));
    return SetPan(c.handle, std::clamp(pan, -64, 64));
}

bool Player::SetPlayedPan(Channel& c)
{
    if (!Since2004())
    {
        return SetChannelPan(c);
    }

    return SetPan(c.handle, std::clamp(c.played_panning + c.envelope_pan, -64, 64));
}

bool Player::Since2004() const
{
    return info_.build >= Build::k2004_09_17;
}

void Player::SetChannelFreq(Channel& c)
{
    c.played_period = c.period;
    SetFreq(c.handle, FrequencyOf(c.period));
}

void Player::StopChannel(Channel& c)
{
    c.envelopes_on = 0;
    Stop(c.handle);
}

uint16_t Player::PeriodOf(uint32_t note, uint32_t sample) const
{
    const int fine_tune = rom_.S8(sample + kSampleFineTune);
    if (module_.linear)
    {
        return uint16_t(LinearPeriod(note, fine_tune));
    }

    return uint16_t(AmigaPeriod(rom_, info_, note, fine_tune));
}

uint32_t Player::FrequencyOf(int32_t period) const
{
    if (module_.linear)
    {
        return LinearFreq(uint32_t(period));
    }

    return uint32_t(Div(kAmigaClock, period).first);
}

uint32_t Player::LinearFreq(uint32_t period) const
{
    // The octave above the table's, and the step within it, of 768 to the octave.
    const uint32_t octave = ((period >> 6) * 0xAAAB) >> 19;
    const uint32_t step = period - octave * 768;
    return ShiftRight(rom_.U32(info_.linear + 4 * step), 23 - octave);
}

int16_t Player::Wave(uint32_t table, uint32_t index) const
{
    return int16_t(rom_.U16(table + 2 * index));
}

// ---------------------------------------------------------------------------------------------------------------------
// The effects

void Player::VolumeSlideS3m(Channel& c, bool on_row)
{
    // On the row, a fine slide (xF or Fx) slides once, and a normal one, in a module with fast slides, too.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.volume_slide = c.effect_op;
        }

        const int up = c.volume_slide >> 4, down = c.volume_slide & 0xF;
        c.volume_slide_once = 0;
        if (up && down == 0xF)
        {
            c.volume_slide_once = 1;
            VolumeUp(c, up);
        }
        else if (down && up == 0xF)
        {
            c.volume_slide_once = 1;
            VolumeDown(c, down);
        }

        if (c.effect == kVolumeSlideVibrato && c.restore_note && c.last_effect == kVibrato)
        {
            c.restore_note = 0;
        }
        if (!fast_slides_)
        {
            return;
        }
    }

    if (!c.volume_slide_once)
    {
        SlideVolume(c);
    }
}

void Player::PortaS3m(Channel& c, bool on_row, bool up)
{
    // On the row, Ex and Fx steps slide once, extra fine and fine; between rows, other steps slide by 4 times
    // themselves. Up lowers the period, and a period that reaches 0 stops the note.
    if (on_row)
    {
        if (c.effect_op)
        {
            c.porta = c.effect_op;
        }
        if (c.porta < 0xE0)
        {
            return;
        }
        if ((c.porta & 0xF0) == 0xF0)
        {
            c.period = int16_t(c.period + (up ? -1 : 1) * ((c.porta & 0xF) << 2));
        }
        else if ((c.porta & 0xF0) == 0xE0)
        {
            c.period = int16_t(c.period + (up ? -1 : 1) * (c.porta & 0xF));
        }
        if (up && c.period <= 0)
        {
            StopChannel(c);
        }
        SetChannelFreq(c);
        return;
    }

    // A step of 0, or of 0xE0 or more, doesn't slide between rows.
    if (uint8_t(c.porta - 1) > 0xDE)
    {
        return;
    }

    if (up)
    {
        LowerPeriod(c, c.porta << 2);
    }
    else
    {
        RaisePeriod(c, c.porta << 2);
    }
}

} // namespace ntdlib::krawall
