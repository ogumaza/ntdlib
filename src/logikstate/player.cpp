// SPDX-License-Identifier: MIT

#include "ntdlib/logikstate/player.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "player_parts.h"

namespace ntdlib::logikstate
{

using parts::Put;
using parts::RecordBytes;
using parts::VibratoWave;

namespace
{

// The effects that the player runs.
constexpr uint16_t kArpeggio = 0x0;
constexpr uint16_t kSlideUp = 0x1;
constexpr uint16_t kSlideDown = 0x2;
constexpr uint16_t kSlideToNote = 0x3;
constexpr uint16_t kVibrato = 0x4;
constexpr uint16_t kSlideToNoteAndVolume = 0x5;
constexpr uint16_t kVibratoAndVolume = 0x6;
constexpr uint16_t kVolumeSlide = 0xA;
constexpr uint16_t kJump = 0xB;
constexpr uint16_t kVolume = 0xC;
constexpr uint16_t kBreak = 0xD;
constexpr uint16_t kExtended = 0xE;
constexpr uint16_t kSpeed = 0xF;

// The extended effect that the player runs, in the operand's high 4 bits, and the two values of the pattern loop's
// marker that its code tests besides kNoLoopRow.
constexpr uint8_t kPatternLoop = 0x60;
constexpr uint16_t kLoopRowHeld = 0xFFFE;
constexpr uint16_t kLoopRowSpent = 0xFFFD;

// The largest operand that sets the speed. A larger one sets the tempo.
constexpr uint8_t kLargestSpeed = 0x1F;

// Divides as the driver's two division routines do: both return 0 for a divisor of 0.
uint32_t Divide(uint32_t value, uint32_t by)
{
    return by ? value / by : 0;
}

int32_t DivideSigned(int32_t value, int32_t by)
{
    return by ? int32_t(int64_t(value) / by) : 0;
}

} // namespace

Player::Player(const Rom& rom, const Song& song, int32_t music_volume, Build build)
    : rom_(rom), song_(song), music_shortfall_(std::clamp(kFullVolume - music_volume, 0, kFullVolume)), build_(build)
{
    if (!ReadModule(rom, song.module, module_) || song.order < 0 || uint32_t(song.order) >= module_.order_count)
    {
        warnings_.push_back("the module at " + agb::Hex(song.module) + " can't be read");
        return;
    }

    // The play routine: the load, then the start at the song's order.
    points_ = SamplePoints(rom, module_, song);
    order_ = uint16_t(song.order);
    row_samples_ = Divide(module_.rate * kStartSpeed, tick_rate_);
    tick_samples_ = Divide(module_.rate, tick_rate_);
    valid_ = true;
    playing_ = true;
}

bool Player::Valid() const
{
    return valid_;
}

const Module& Player::Header() const
{
    return module_;
}

void Player::OnStep(std::function<void(bool)> callback)
{
    on_step_ = std::move(callback);
}

void Player::Frame()
{
    if (!playing_)
    {
        return;
    }

    // The driver's loop: a row and a tick where they fall due, then the samples up to the next of them. Each pass
    // counts as one sample more than it mixes. A frame with several passes can therefore end a few samples short. The
    // driver leaves those samples silent, and its counts of the samples up to the next row and tick stay.
    const uint32_t size = FrameSamples(module_.rate);
    uint32_t done = 0;
    for (int32_t left = int32_t(size); left > 0; left--)
    {
        elapsed_ = frames_ * size + done;
        if (row_left_ == 0)
        {
            Row();
            row_left_ = row_samples_;
            if (on_step_)
            {
                on_step_(true);
            }
        }
        if (tick_left_ == 0)
        {
            if (row_left_ != row_samples_)
            {
                Tick();
                if (on_step_)
                {
                    on_step_(false);
                }
            }
            tick_left_ = tick_samples_;
        }
        if (row_left_ == 0 || tick_left_ == 0)
        {
            Warn("a row or a tick has no length, which the driver can't play");
            playing_ = false;
            break;
        }

        const uint32_t run = std::min({row_left_, tick_left_, size - done});
        for (int c = 0; c < kMixChannels; c++)
        {
            if (Sounding(channels_[size_t(c)], c))
            {
                Advance(channels_[size_t(c)], run);
            }
        }
        done += run;
        left -= int32_t(run);
        row_left_ -= run;
        tick_left_ -= run;
    }

    frames_++;
    elapsed_ = frames_ * size;
}

uint64_t Player::Elapsed() const
{
    return elapsed_;
}

int Player::RowOrder() const
{
    return row_order_;
}

int Player::RowNumber() const
{
    return row_number_;
}

uint32_t Player::RowSamples() const
{
    return row_samples_;
}

uint32_t Player::Speed() const
{
    return speed_;
}

uint32_t Player::Tempo() const
{
    return tempo_;
}

uint16_t Player::LoopRow() const
{
    return loop_row_;
}

std::vector<uint16_t> Player::LoopState() const
{
    if (build_ == Build::kShortRecords)
    {
        return {loop_row_};
    }

    std::vector<uint16_t> state;
    for (const Channel& ch : channels_)
    {
        state.push_back(ch.loop_row);
        state.push_back(ch.loop_count);
    }

    return state;
}

std::vector<uint32_t> Player::RepeatState() const
{
    std::vector<uint32_t> state = {speed_, tempo_};
    for (uint16_t value : LoopState())
    {
        state.push_back(value);
    }

    return state;
}

const std::array<Channel, kMixChannels>& Player::Channels() const
{
    return channels_;
}

int32_t Player::MixVolume(const Channel& channel, int number) const
{
    const int music = build_ == Build::kShortRecords ? kMusicChannels : int(module_.channels);
    return std::max(channel.volume - (number < music ? music_shortfall_ : 0), 0);
}

bool Player::Sounding(const Channel& channel, int number) const
{
    return channel.points != 0 && channel.length > kStoppedLength &&
           (build_ == Build::kLongRecords || MixVolume(channel, number) != 0);
}

bool Player::Playing() const
{
    return playing_;
}

std::vector<uint8_t> Player::Globals() const
{
    std::vector<uint8_t> out;
    Put(out, order_, 2);
    Put(out, place_, 2);
    Put(out, speed_, 4);
    Put(out, tempo_, 4);
    Put(out, tick_rate_, 4);
    Put(out, row_samples_, 4);
    Put(out, row_left_, 4);
    Put(out, tick_samples_, 4);
    Put(out, tick_left_, 4);
    Put(out, tick_, 2);
    Put(out, arpeggio_tick_, 2);
    Put(out, broke_, 1);
    Put(out, loop_row_, 2);

    return out;
}

std::vector<uint8_t> Player::ChannelRecord(int channel) const
{
    const Channel& ch = channels_[size_t(channel)];
    std::vector<uint8_t> out = RecordBytes(ch);
    if (build_ == Build::kLongRecords)
    {
        Put(out, ch.loop_row, 2);
        Put(out, ch.loop_count, 2);
    }

    return out;
}

const std::vector<std::string>& Player::Warnings() const
{
    return warnings_;
}

void Player::Row()
{
    // The row's cells, or an empty row past the pattern's rows.
    const uint32_t pattern = rom_.U8(module_.address + kModuleOrderList + order_);
    if (pattern >= module_.pattern_count)
    {
        Warn("an order past the module's last names a pattern that the module doesn't have");
        playing_ = false;
        return;
    }

    const uint32_t cells = module_.patterns[pattern] + uint32_t(place_) * kCellSize;
    const bool empty = place_ >= module_.rows[pattern] * module_.channels;
    row_order_ = order_;
    row_number_ = int(place_ / module_.channels);

    place_ = uint16_t(place_ + module_.channels);
    tick_ = 0;
    arpeggio_tick_ = 0;
    broke_ = 0;
    for (uint32_t c = 0; c < module_.channels; c++)
    {
        ReadRowCell(channels_[c], cells + c * kCellSize, empty);
    }

    // The order's end, after the 64th row or a jump.
    if (place_ == module_.channels * uint32_t(kPatternRows))
    {
        order_++;
        if (order_ >= module_.order_count)
        {
            order_ = 0;
        }
        place_ = 0;
    }
}

void Player::ReadRowCell(Channel& ch, uint32_t address, bool empty)
{
    Cell cell = empty ? Cell() : ReadCell(rom_, address);

    // A slide to a note keeps the note as the slide's target, and uses the volume of the cell's sample.
    if ((cell.effect & 0xF) == kSlideToNote)
    {
        if (cell.pitch != kNoPitch)
        {
            ch.slide_target = int16_t(cell.pitch);
        }
        cell.pitch = kNoPitch;
        if (cell.sample != 0)
        {
            ch.volume = module_.samples[cell.sample - 1u].volume;
        }
        cell.sample = 0;
    }

    ch.effect = -1;
    ch.vibrato = 0;
    if (cell.pitch != kNoPitch)
    {
        // A note starts the channel's sample, or the cell's, from its first point. The step counts a slide that an
        // earlier row left, and the slide is cleared afterwards.
        if (cell.sample != 0)
        {
            ch.sample = uint16_t(cell.sample - 1);
            ch.volume = module_.samples[ch.sample].volume;
        }
        SetSample(ch, ch.sample, true);
        ch.pitch = cell.pitch;
        ch.position = 0;
        ch.step = Step(ch);
        if (cell.effect != kSlideToNote)
        {
            ch.slide = 0;
        }
    }
    else if (cell.sample != 0)
    {
        // A sample without a note: the channel uses its points and its volume, and keeps its place, length and loop.
        ch.sample = uint16_t(cell.sample - 1);
        SetSample(ch, ch.sample, false);
        ch.volume = module_.samples[ch.sample].volume;
    }

    ch.arpeggio_1 = 0;
    ch.arpeggio_2 = 0;
    RowEffect(ch, cell.effect, cell.operand);
}

void Player::RowEffect(Channel& ch, uint16_t effect, uint8_t operand)
{
    switch (effect)
    {
    case kArpeggio:
        ch.effect = effect;
        ch.arpeggio_1 = uint16_t((operand >> 4) * kSemitonePitches);
        ch.arpeggio_2 = uint16_t((operand & 0xF) * kSemitonePitches);
        break;

    case kSlideUp:
        ch.effect = effect;
        ch.slide_step = int16_t(-int32_t(operand));
        break;

    case kSlideDown:
        ch.effect = effect;
        ch.slide_step = int16_t(operand);
        break;

    case kSlideToNote:
        // The slide's direction: a higher pitch has a lower period.
        ch.effect = effect;
        if (operand != 0)
        {
            ch.slide_speed = operand;
        }
        if (ch.slide_target == int16_t(ch.pitch))
        {
            ch.slide_step = 0;
        }
        else
        {
            ch.slide_step = int16_t(ch.slide_target > int32_t(ch.pitch) ? -int32_t(ch.slide_speed) : ch.slide_speed);
        }
        break;

    case kVibrato:
        ch.effect = effect;
        if (operand != 0)
        {
            ch.vibrato_speed = operand >> 4;
            ch.vibrato_depth = operand & 0xF;
        }
        break;

    case kSlideToNoteAndVolume:
    case kVibratoAndVolume:
    case kVolumeSlide:
        ch.effect = effect;
        ch.volume_up = operand >> 4;
        ch.volume_down = operand & 0xF;
        break;

    case kJump:
        // The row is the order's last, and the order after it is the operand's.
        order_ = uint16_t(operand - 1);
        place_ = uint16_t(module_.channels * uint32_t(kPatternRows));
        break;

    case kVolume:
        ch.volume = operand;
        break;

    case kBreak:
        // The first break of a row moves to the next order. The operand times 4 is the next row's first cell.
        if (!broke_)
        {
            place_ = uint16_t(operand * 4);
            order_++;
            if (order_ >= module_.order_count)
            {
                order_ = 0;
            }
            broke_ = 1;
        }
        break;

    case kExtended:
        if ((operand & 0xF0) == kPatternLoop && build_ == Build::kShortRecords)
        {
            PatternLoop(operand & 0xF);
        }
        else if ((operand & 0xF0) == kPatternLoop)
        {
            ChannelLoop(ch, operand & 0xF);
        }
        break;

    case kSpeed:
        SetSpeed(operand);
        break;

    default:
        break;
    }
}

void Player::ChannelLoop(Channel& ch, int count)
{
    // A count of 0 marks the row. Another count starts the channel's passes, or counts one off, and goes back to the
    // marked row while passes are left.
    if (count == 0)
    {
        ch.loop_row = uint16_t(place_ - module_.channels);
        return;
    }

    ch.loop_count = ch.loop_count == 0 ? uint16_t(count) : uint16_t(ch.loop_count - 1);
    if (ch.loop_count != 0)
    {
        place_ = ch.loop_row;
    }
}

void Player::PatternLoop(int count)
{
    // The driver's pattern loop never goes back to its marker. A count of 0 sets the marker to the row. Another count
    // clears a marker, or goes back to the pattern's first row when none is set.
    if (count == 0)
    {
        loop_row_ = loop_row_ == kLoopRowHeld ? kLoopRowSpent : uint16_t(place_ - module_.channels);
    }
    else if (loop_row_ != kNoLoopRow)
    {
        loop_row_ = kNoLoopRow;
    }
    else
    {
        place_ = 0;
    }
}

void Player::SetSpeed(uint8_t operand)
{
    if (operand <= kLargestSpeed)
    {
        speed_ = operand;
    }
    else
    {
        tempo_ = operand;
    }

    // The tempo sets the ticks a second, in whole ticks, and a row lasts the speed's count of them. A tick of effects
    // keeps the length that the module started with.
    tick_rate_ = uint32_t(DivideSigned(int32_t((tempo_ * 50) << 16), 125) >> 16);
    row_samples_ = Divide(speed_ * module_.rate, tick_rate_);
}

void Player::Tick()
{
    for (int c = 0; c < kEffectChannels; c++)
    {
        Channel& ch = channels_[size_t(c)];
        switch (ch.effect)
        {
        case kArpeggio:
            Arpeggio(ch);
            break;

        case kSlideUp:
        case kSlideDown:
            ch.slide = int16_t(ch.slide + ch.slide_step);
            break;

        case kSlideToNote:
            SlideToNote(ch);
            break;

        case kVibrato:
            Vibrato(ch);
            break;

        case kSlideToNoteAndVolume:
            SlideToNote(ch);
            VolumeSlide(ch);
            break;

        case kVibratoAndVolume:
            Vibrato(ch);
            VolumeSlide(ch);
            break;

        case kVolumeSlide:
            VolumeSlide(ch);
            break;

        default:
            break;
        }

        // The step follows the slide and the vibrato, except in an arpeggio.
        if (ch.arpeggio_1 + ch.arpeggio_2 == 0)
        {
            ch.step = Step(ch);
        }
    }

    tick_++;
    arpeggio_tick_++;
}

void Player::Arpeggio(Channel& ch)
{
    if (arpeggio_tick_ == 0 || ch.arpeggio_1 + ch.arpeggio_2 == 0)
    {
        return;
    }

    // The ticks after a row's first play the first interval, then the second twice, and so on.
    if (arpeggio_tick_ == 4)
    {
        arpeggio_tick_ = 1;
    }
    const int phase = arpeggio_tick_ & 3;
    const uint32_t interval = phase == 0 ? 0 : phase == 1 ? ch.arpeggio_1 : ch.arpeggio_2;
    ch.step = PeriodStep(uint32_t(ch.fine_tune) + ch.pitch + interval, ch.vibrato);
}

void Player::SlideToNote(Channel& ch)
{
    // The driver compares steps where periods were meant: the pitch's step plus the slide, against the target's step.
    // A slide down has passed its target at once, and so has a slide up. The note reaches the target's pitch at the
    // first tick.
    ch.slide = int16_t(ch.slide + ch.slide_step);
    const uint32_t from = rom_.U32(module_.steps + uint32_t(ch.pitch) * 4) + uint32_t(int32_t(ch.slide));
    const uint32_t target = rom_.U32(module_.steps + uint32_t(int32_t(ch.slide_target)) * 4);
    if (ch.slide_step > 0 ? from >= target : from <= target)
    {
        ch.pitch = uint16_t(ch.slide_target);
        ch.slide_step = 0;
        ch.slide = 0;
    }
}

void Player::Vibrato(Channel& ch)
{
    if (tick_ == 0)
    {
        ch.vibrato_position = 0;
    }

    const uint16_t place = ch.vibrato_position;
    const int32_t wave = VibratoWave(place);
    ch.vibrato = int16_t((wave * int32_t(ch.vibrato_depth)) >> 7);
    ch.vibrato_position = uint16_t((place + ch.vibrato_speed) & 0x3F);
}

void Player::VolumeSlide(Channel& ch)
{
    if (tick_ == 0 || uint16_t(ch.volume_up + ch.volume_down) == 0)
    {
        return;
    }

    ch.volume = ch.volume_up != 0 ? ch.volume + ch.volume_up : ch.volume - ch.volume_down;
    ch.volume = std::clamp(ch.volume, 0, kFullVolume);
}

uint32_t Player::Step(const Channel& ch) const
{
    if (ch.slide == 0 && ch.vibrato == 0 && ch.fine_tune == 0)
    {
        return rom_.U32(module_.steps + uint32_t(ch.pitch) * 4);
    }

    return PeriodStep(uint32_t(ch.fine_tune) + ch.pitch, ch.slide + ch.vibrato);
}

uint32_t Player::PeriodStep(uint32_t pitch, int32_t offset) const
{
    // The sample's rate in hertz, from the period, and then the step for the mixer's rate. Both divisions round down.
    const int32_t period = int32_t(rom_.U16(module_.address + kModulePeriods + pitch * 2)) + offset;
    const int32_t rate = DivideSigned(kPeriodClock, period * 2);

    return Divide(uint32_t(rate) << 12, module_.rate);
}

void Player::Advance(Channel& ch, uint32_t samples)
{
    // The mixer reads a point, then steps. At the end, the points move on to the loop's start, the loop's length
    // becomes the length, and the place is masked. A sample without a loop then has no length, and the mixer leaves the
    // channel alone from its next call.
    while (samples > 0)
    {
        const uint32_t end = uint32_t(ch.length) << 12;
        if (ch.position >= end)
        {
            ch.points += ch.loop_start;
            ch.loop_start = 0;
            ch.length = ch.loop_length;
            ch.position = (ch.position & kEndPlaceMask) + ch.step;
            samples--;
            continue;
        }

        const uint32_t until = ch.step ? (end - ch.position + ch.step - 1) / ch.step : samples;
        const uint32_t run = std::min(samples, until);
        ch.position += run * ch.step;
        samples -= run;
    }
}

void Player::SetSample(Channel& ch, uint16_t sample, bool whole)
{
    ch.points = sample < kSamples ? points_[sample] : 0;
    if (whole && sample < kSamples)
    {
        const Sample& header = module_.samples[sample];
        ch.length = header.length;
        ch.loop_start = header.loop_start;
        ch.loop_length = header.loop_length;
        ch.fine_tune = header.fine_tune;
    }

    ch.starts++;
    ch.first_point = ch.points;
    ch.whole_length = ch.length;
    ch.whole_loop_start = ch.loop_start;
    ch.whole_loop_length = ch.loop_length;
}

void Player::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

} // namespace ntdlib::logikstate
