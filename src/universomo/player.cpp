// SPDX-License-Identifier: MIT

// Universomo's sequencer (see player.h). The addresses are those of the routines in Beat City's USA image.

#include "ntdlib/universomo/player.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/universomo/formats.h"

namespace ntdlib::universomo
{
namespace
{

// The vibrato's table at 0x020D56D8: half a sine wave in 32 steps.
constexpr std::array<int, 32> kVibratoTable = {0,   25,  50,  74,  98,  120, 142, 162, 181, 197, 213,
                                               225, 237, 244, 251, 253, 255, 253, 251, 244, 237, 225,
                                               213, 197, 181, 162, 142, 120, 98,  74,  50,  25};

// The largest channel volume.
constexpr int kMaxVolume = 0xFFF;

// No effect in a slot. The others are formats.h's Effect.
constexpr int kNoEffect = 0;

// A note delay's parameter less this is the delay in ticks.
constexpr int kDelayBase = 0xD0;

int ClampVolume(int volume)
{
    return std::clamp(volume, 0, kMaxVolume);
}

} // namespace

HardwareVolume HardwareVolumeOf(int volume, int song_volume)
{
    const int x = std::clamp((volume * song_volume) >> 8, 0, kMaxVolume);
    if (x < 256)
    {
        return {static_cast<uint8_t>(x >> 1), 3};
    }
    if (x < 1024)
    {
        return {static_cast<uint8_t>(x >> 3), 2};
    }
    if (x < 2048)
    {
        return {static_cast<uint8_t>(x >> 4), 1};
    }

    return {static_cast<uint8_t>(x >> 5), 0};
}

double LevelOf(uint8_t volume, uint8_t divider)
{
    constexpr std::array<double, 4> kDividers = {1.0, 2.0, 4.0, 16.0};

    return volume / 128.0 / kDividers[divider & 3];
}

Player::Player(const Song& song, const SampleBank& bank, bool loop, int song_volume, int first_channel)
    : song_(song), bank_(bank), loop_(loop), song_volume_(song_volume), first_channel_(first_channel)
{
    if (song.orders.empty())
    {
        throw xsflib::FormatError("the song has no orders");
    }
    if (song.track_count > kMaxTracks || first_channel < 0 ||
        static_cast<uint32_t>(first_channel) + song.track_count > kMaxTracks)
    {
        throw xsflib::FormatError("the song has more tracks than the sound unit has channels");
    }
    for (const Pattern& pattern : song.patterns)
    {
        if (pattern.tracks.size() != song.track_count)
        {
            throw xsflib::FormatError("a pattern's tracks don't match the song's");
        }
    }
    for (const uint8_t order : song.orders)
    {
        if (order >= song.patterns.size())
        {
            throw xsflib::FormatError("an order names a missing pattern");
        }
    }
    for (const Instrument& instrument : song.instruments)
    {
        if (instrument.sample >= bank.samples.size())
        {
            throw xsflib::FormatError("an instrument names a missing sample (" + std::to_string(instrument.sample) +
                                      ")");
        }
    }

    speed_ = song.speed;
    channels_.resize(song.track_count);
    EnterPattern();
}

const std::vector<Command>& Player::Commands() const
{
    return commands_;
}

bool Player::Playing() const
{
    return playing_;
}

uint32_t Player::Order() const
{
    return order_;
}

uint32_t Player::Row() const
{
    return row_;
}

uint32_t Player::Tick() const
{
    return tick_;
}

uint32_t Player::Speed() const
{
    return speed_;
}

uint32_t Player::Wraps() const
{
    return wraps_;
}

uint64_t Player::Updates() const
{
    return updates_;
}

const Pattern& Player::CurrentPattern() const
{
    return song_.patterns[song_.orders[order_]];
}

const Instrument& Player::InstrumentAt(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= song_.instruments.size())
    {
        throw xsflib::FormatError("an event names a missing instrument (" + std::to_string(index + 1) + ")");
    }

    return song_.instruments[static_cast<std::size_t>(index)];
}

uint8_t Player::ChannelOf(std::size_t track) const
{
    return static_cast<uint8_t>(first_channel_ + static_cast<int>(track));
}

void Player::EnterPattern()
{
    const Pattern& pattern = CurrentPattern();
    waits_.assign(pattern.tracks.size(), 0);
    for (std::size_t t = 0; t < pattern.tracks.size(); t++)
    {
        waits_[t] = pattern.tracks[t].first_rows;
    }
    next_event_.assign(pattern.tracks.size(), 0);
}

void Player::SendVolume(std::size_t track)
{
    const HardwareVolume hardware = HardwareVolumeOf(channels_[track].volume, song_volume_);
    commands_.push_back({.kind = Command::Kind::kVolume,
                         .channel = ChannelOf(track),
                         .volume = hardware.volume,
                         .divider = hardware.divider});
}

// 0x0202D76C. Effects 7, 8 and 9 and unknown effects clear the slot.
void Player::SetEffect(std::size_t track, int effect, int parameter, int slot)
{
    Channel& c = channels_[track];
    int kept = kNoEffect;
    switch (effect)
    {
    case kPitchSlideUp:
    case kPitchSlideDown:
        {
            parameter = parameter == 0 ? c.parameter : parameter;
            c.parameter = parameter;
            const int base = c.pending ? c.target : c.pitch;
            const int amount = static_cast<int>(speed_) * 16 * parameter / 5;
            c.slide_start = base;
            c.slide_end = effect == kPitchSlideUp ? base + amount : base - amount;
            kept = effect;
            break;
        }

    case kTonePortamento:
        {
            parameter = parameter == 0 ? c.parameter : parameter;
            c.parameter = parameter;
            c.slide_start = c.pitch;
            const int amount = static_cast<int>(speed_) * 16 * parameter / 5;
            c.slide_end = c.target <= c.pitch ? c.pitch - amount : c.pitch + amount;
            if (c.pending)
            {
                c.no_retrigger = true;
            }
            kept = effect;
            break;
        }

    case kVolumeSlide:
    case kFineVolumeSlide:
        c.volume_slide = parameter == 0 ? c.volume_slide : parameter;
        kept = effect;
        break;

    case kVibrato:
        parameter = parameter == 0 ? c.parameter : parameter;
        c.vibrato_speed = (parameter >> 4) & 15;
        c.vibrato_depth = parameter & 15;
        c.parameter = parameter;
        kept = effect;
        break;

    case kSampleOffset:
        if (c.pending)
        {
            c.offset = static_cast<uint32_t>(parameter);
        }
        break;

    case kNoteDelay:
        if (c.pending)
        {
            c.delay = std::max(parameter - kDelayBase, 0);
        }
        break;

    case kSpeed:
        speed_ = static_cast<uint32_t>(parameter);
        break;

    case kVolume:
    case kArpeggio:
        c.parameter = parameter;
        kept = effect;
        break;

    default:
        break;
    }

    c.effects[static_cast<std::size_t>(slot)] = kept;
}

// 0x0202D63C: the volume column's effects, in slot 1.
void Player::VolumeColumn(std::size_t track, int value)
{
    if (value >= 0x70 && value <= 0x7F)
    {
        SetEffect(track, kVolumeSlide, ((value - 0x70) << 4) & 0xFF, 1);
    }
    if (value >= 0x60 && value <= 0x6F)
    {
        SetEffect(track, kVolumeSlide, (value - 0x60) & 15, 1);
    }
    if (value >= 0x90 && value <= 0x9F)
    {
        SetEffect(track, kFineVolumeSlide, ((value - 0x90) << 4) & 0xFF, 1);
    }
    if (value >= 0x80 && value <= 0x8F)
    {
        SetEffect(track, kFineVolumeSlide, (value - 0x80) & 15, 1);
    }
    if (value >= 0xF0)
    {
        SetEffect(track, kTonePortamento, (value - 0xF0) & 15, 1);
    }
}

// 0x0202D4CC.
void Player::RunEvent(std::size_t track, const Event& event)
{
    Channel& c = channels_[track];
    if (event.instrument != 0)
    {
        c.instrument = event.instrument - 1;
    }

    const Instrument& instrument = InstrumentAt(c.instrument);
    if ((event.note & kEffectFlag) != 0)
    {
        SetEffect(track, event.note & 0x7F, event.value, 0);
    }
    else if (event.note == kNoteCut)
    {
        c.volume = 0;
        SendVolume(track);
    }
    else if (event.note != 0)
    {
        c.pending = true;
        c.no_retrigger = false;
        c.event_volume = event.value;
        c.event_instrument = event.instrument;
        c.delay = 0;
        c.target = (instrument.relative_note + event.note - 48) * 64 + (instrument.finetune >> 1);
    }
    else
    {
        if (event.value >= 0x10 && event.value < 0x50)
        {
            c.volume = (event.value - 0x10) << 6;
        }
        else
        {
            VolumeColumn(track, event.value);
            c.volume = std::min(instrument.volume << 6, kMaxVolume);
        }
        SendVolume(track);
    }
}

// 0x0202D174: the events of each track on the row. Both effect slots clear first.
void Player::RunRow()
{
    const Pattern& pattern = CurrentPattern();
    for (std::size_t t = 0; t < pattern.tracks.size(); t++)
    {
        SetEffect(t, kNoEffect, 0, 0);
        SetEffect(t, kNoEffect, 0, 1);
        const std::vector<Event>& events = pattern.tracks[t].events;
        if (events.empty() || waits_[t] != 0 || next_event_[t] >= events.size())
        {
            continue;
        }

        // Read events until one waits for a later row.
        std::size_t i = next_event_[t];
        uint8_t wait = 0;
        for (; i < events.size(); i++)
        {
            RunEvent(t, events[i]);
            next_event_[t]++;
            wait = events[i].rows;
            if (wait != 0)
            {
                break;
            }
        }
        waits_[t] = i >= events.size() ? 0xFF : wait;
    }
}

// 0x0202D2D0: starts a pending note.
void Player::StartNote(std::size_t track)
{
    Channel& c = channels_[track];
    const bool keep = c.event_instrument == 0;
    if (!keep)
    {
        c.instrument = c.event_instrument - 1;
    }

    const Instrument& instrument = InstrumentAt(c.instrument);
    const Sample& sample = bank_.samples[instrument.sample];
    int volume = 0;
    if (c.event_volume >= 0x10 && c.event_volume < 0x50)
    {
        volume = (c.event_volume - 0x10) << 6;
    }
    else
    {
        VolumeColumn(track, c.event_volume);
        volume = c.effects[0] == kVolume ? c.parameter << 6 : keep ? c.volume : instrument.volume << 6;
        volume = ClampVolume(volume);
    }
    c.volume = volume;
    const HardwareVolume hardware = HardwareVolumeOf(volume, song_volume_);

    if (c.no_retrigger)
    {
        // Tone portamento: the sample plays on, and only the volume is set.
        c.no_retrigger = false;
        commands_.push_back({.kind = Command::Kind::kVolume,
                             .channel = ChannelOf(track),
                             .volume = hardware.volume,
                             .divider = hardware.divider});
        c.pending = false;
        return;
    }

    c.pitch = c.target;
    const auto size = static_cast<uint32_t>(sample.data.size());
    if (c.offset > 0)
    {
        c.offset = sample.looped ? 0 : c.offset << (sample.bits == 8 ? 8 : 9);
        if (size <= c.offset)
        {
            c.offset = 0;
        }
    }

    const uint8_t channel = ChannelOf(track);
    Command setup{.kind = Command::Kind::kSetup,
                  .channel = channel,
                  .format = static_cast<uint8_t>(sample.bits == 8 ? 0 : 1),
                  .volume = hardware.volume,
                  .divider = hardware.divider,
                  .timer = ntdlib::ntr::snd::Timer(kBaseTimer, c.target),
                  .pan = (channel & 1) != 0 ? kOddPan : kEvenPan,
                  .sample = instrument.sample,
                  .offset = c.offset};
    if (sample.looped)
    {
        setup.loop_mode = 1;
        setup.loop_start = sample.loop_start >> 2;
        setup.length = sample.loop_length >> 2;
    }
    else
    {
        setup.loop_mode = 2;
        setup.length = (size - c.offset) >> 2;
    }
    commands_.push_back(setup);
    commands_.push_back({.kind = Command::Kind::kStart, .channel = channel});
    c.offset = 0;
    c.pending = false;
}

// 0x0202D95C: a tick of a channel's effects, then the commands for what changed.
void Player::StepEffects(std::size_t track)
{
    Channel& c = channels_[track];
    const int old_volume = c.volume;
    const int old_pitch = c.pitch;
    for (std::size_t slot = 0; slot < 2; slot++)
    {
        const int effect = c.effects[slot];
        if (effect == kPitchSlideUp || effect == kPitchSlideDown || effect == kTonePortamento)
        {
            // Linear from the start to the end over the row, tick 0 included.
            int64_t pitch = c.slide_end;
            if (tick_ + 1 < speed_)
            {
                const int64_t f = std::clamp<int64_t>(int64_t{256 / static_cast<int>(speed_)} * (tick_ + 1), 0, 255);
                pitch = (int64_t{c.slide_end} * f + int64_t{c.slide_start} * (255 - f)) >> 8;
            }
            if (effect == kTonePortamento)
            {
                const bool up = c.slide_end > c.slide_start;
                if ((up && pitch > c.target) || (!up && pitch < c.target))
                {
                    pitch = c.target;
                }
            }
            c.pitch = static_cast<int>(pitch);
        }
        else if (effect == kVolumeSlide || effect == kFineVolumeSlide)
        {
            const int step = c.volume_slide > 15 ? c.volume_slide >> 4 : -c.volume_slide;
            c.volume = ClampVolume(c.volume + step * 64);
            if (effect == kFineVolumeSlide)
            {
                // The code clears slot 0's parameter in place of its effect: in slot 0 the slide repeats.
                if (slot == 0)
                {
                    c.parameter = 0;
                }
                else
                {
                    c.effects[1] = kNoEffect;
                }
            }
        }
        else if (effect == kVibrato)
        {
            // Half a sine for each sign of the position. The wave jumps at each wrap.
            int offset =
                (kVibratoTable[static_cast<std::size_t>((c.vibrato_position + 16) & 31)] * c.vibrato_depth) >> 6;
            if (c.vibrato_position < 0)
            {
                offset = -offset;
            }
            c.pitch = c.target + offset;
            c.vibrato_position += c.vibrato_speed;
            if (c.vibrato_position > 31)
            {
                c.vibrato_position -= 64;
            }
        }
        else if (effect == kVolume)
        {
            c.volume = ClampVolume(c.parameter << 6);
        }
        else if (effect == kArpeggio)
        {
            const uint32_t step = tick_ % 3;
            const int semitones = step == 1 ? (c.parameter >> 4) & 15 : step == 2 ? c.parameter & 15 : 0;
            c.pitch = c.target + semitones * 64;
        }
    }

    if (c.volume != old_volume)
    {
        SendVolume(track);
    }
    if (c.pitch != old_pitch)
    {
        commands_.push_back({.kind = Command::Kind::kTimer,
                             .channel = ChannelOf(track),
                             .timer = ntdlib::ntr::snd::Timer(kBaseTimer, c.pitch)});
    }
}

// 0x0202CE98, for one song.
const std::vector<Command>& Player::Update()
{
    commands_.clear();
    if (!playing_)
    {
        return commands_;
    }

    updates_++;

    // A new row: its events, then the count of rows, orders and the loop.
    if (tick_ >= speed_ || first_)
    {
        tick_ = 0;
        first_ = false;
        RunRow();

        const Pattern& pattern = CurrentPattern();
        for (std::size_t t = 0; t < pattern.tracks.size(); t++)
        {
            if (!pattern.tracks[t].events.empty())
            {
                waits_[t] = static_cast<uint8_t>(waits_[t] - 1);
            }
        }
        row_++;
        if (row_ >= pattern.rows)
        {
            row_ = 0;
            order_++;
            if (order_ >= song_.orders.size())
            {
                if (!loop_)
                {
                    // 0x0202CCA8: the stop sets every channel's volume to 0. The channels' timers run on.
                    playing_ = false;
                    for (std::size_t t = 0; t < channels_.size(); t++)
                    {
                        commands_.push_back({.kind = Command::Kind::kVolume, .channel = ChannelOf(t)});
                    }
                    return commands_;
                }
                order_ = song_.restart < song_.orders.size() ? song_.restart : 0;
                wraps_++;
            }
            EnterPattern();
        }
    }

    // The pending notes start in track order, after a delay.
    for (std::size_t t = 0; t < channels_.size(); t++)
    {
        Channel& c = channels_[t];
        if (c.pending && c.delay <= 0)
        {
            StartNote(t);
        }
        else if (c.pending)
        {
            c.delay--;
        }
    }

    for (std::size_t t = 0; t < channels_.size(); t++)
    {
        StepEffects(t);
    }
    tick_++;

    return commands_;
}

} // namespace ntdlib::universomo
