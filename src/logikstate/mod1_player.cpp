// SPDX-License-Identifier: MIT

// The addresses in the comments are those of Pinball Challenge Deluxe, the European release (game code APLP).

#include "ntdlib/logikstate/mod1_player.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/player.h"
#include "player_parts.h"

namespace ntdlib::logikstate
{

using parts::Put;
using parts::RecordBytes;
using parts::VibratoWave;

namespace
{

// The effects that the player runs. Effects 5, 6, 7, 8, 9 and E do nothing in this build: the row routine's table
// sends them to its exit.
constexpr uint16_t kArpeggio = 0x0;
constexpr uint16_t kSlideUp = 0x1;
constexpr uint16_t kSlideDown = 0x2;
constexpr uint16_t kSlideToNote = 0x3;
constexpr uint16_t kVibrato = 0x4;
constexpr uint16_t kVolumeSlide = 0xA;
constexpr uint16_t kJump = 0xB;
constexpr uint16_t kVolume = 0xC;
constexpr uint16_t kBreak = 0xD;
constexpr uint16_t kSpeed = 0xF;

// The largest operand that sets a row's length from the rate. A larger one sets it from the operand alone.
constexpr uint8_t kLargestSpeed = 0x1F;

// The speed that the start routine sets for a row, in ticks of a 50th of a second.
constexpr uint32_t kMod1StartSpeed = 6;
constexpr uint32_t kTicksASecond = 50;

// The place at which a pattern ends: 64 rows of 4 cells.
constexpr uint16_t kPatternEnd = 0x100;

// The order of the range that stands for none.
constexpr uint16_t kNoOrder = 0xFFFF;

// The scale of slides 1 and 2: the halfword at 0x020006EC, whose initialised data nothing changes.
constexpr int32_t kSlideScale = 4;

// The last index of the table of steps that the driver reads as it is, and the index that it reads for any above it
// (0x08004E2E). The word at that index is past the table, in the module's patterns.
constexpr int32_t kLastStepIndex = 0x128;
constexpr int32_t kPastStepIndex = 0x240;

// The arpeggio's counter goes 0, 1, 2, 1, 2 and so on (0x08004E82).
constexpr uint16_t kArpeggioWrap = 3;

// The mixer skips a channel whose length is kStoppedLength exactly (0x08004BA4).
static_assert(kStoppedLength == 2);

} // namespace

Mod1Player::Mod1Player(const Rom& rom, const Song& song, int32_t music_volume)
    : rom_(rom), song_(song), music_shortfall_(std::clamp(kFullVolume - music_volume, 0, kFullVolume))
{
    if (!ReadModule(rom, song.module, module_) || module_.format != Format::kGbaMod1 || song.order < 0 ||
        uint32_t(song.order) >= module_.order_count)
    {
        warnings_.push_back("the module at " + agb::Hex(song.module) + " can't be read");
        return;
    }

    // The load (0x08004380), then the start (0x08004428) at the song's order with its range.
    points_ = SamplePoints(rom, module_, song);
    order_ = uint16_t(song.order);
    first_order_ = uint16_t(song.first_order);
    last_order_ = uint16_t(song.last_order);
    row_samples_ = module_.rate * kMod1StartSpeed / kTicksASecond;
    tick_samples_ = module_.rate / kTicksASecond;
    valid_ = true;
    playing_ = true;
}

bool Mod1Player::Valid() const
{
    return valid_;
}

const Module& Mod1Player::Header() const
{
    return module_;
}

void Mod1Player::OnStep(std::function<void(bool)> callback)
{
    on_step_ = std::move(callback);
}

void Mod1Player::Frame()
{
    if (!playing_)
    {
        return;
    }

    // Past the module's last order, the frame routine clears the music's channels at each frame (0x080048A0), and the
    // row reader goes on. The conversion's count of starts goes on. A new note after the clear
    // then counts as one.
    if (order_ >= module_.order_count)
    {
        for (uint32_t c = 0; c < module_.channels; c++)
        {
            const uint32_t starts = channels_[c].starts;
            channels_[c] = Channel();
            channels_[c].starts = starts + 1;
        }
    }

    Mix();
    frames_++;
    elapsed_ = frames_ * kMod1FrameSamples;
}

void Mod1Player::Mix()
{
    // 0x08004ABC: a row and a tick where they fall due, then the samples up to the next of them. Each pass counts as
    // one sample more than it mixes. A tick that falls due with a row is left out.
    uint32_t done = 0;
    for (int32_t left = int32_t(kMod1FrameSamples); left > 0 && playing_; left--)
    {
        elapsed_ = frames_ * kMod1FrameSamples + done;
        if (row_left_ == 0)
        {
            Row();
            row_left_ = row_samples_;
            if (on_step_ && playing_)
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

        const uint32_t run = std::min({row_left_, tick_left_, kMod1FrameSamples - done});
        for (int c = 0; c < kMod1MixChannels; c++)
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
}

uint64_t Mod1Player::Elapsed() const
{
    return elapsed_;
}

int Mod1Player::RowOrder() const
{
    return row_order_;
}

int Mod1Player::RowNumber() const
{
    return row_number_;
}

uint32_t Mod1Player::RowSamples() const
{
    return row_samples_;
}

bool Mod1Player::RowJumped() const
{
    return row_jumped_;
}

std::vector<uint32_t> Mod1Player::RepeatState() const
{
    return {row_samples_};
}

const std::array<Channel, kMod1MixChannels>& Mod1Player::Channels() const
{
    return channels_;
}

int32_t Mod1Player::MixVolume(const Channel& channel, int number) const
{
    // Channels 0 to 3 use the music's volume, and channels 4 and 5 the sound effects' (0x08004BAA).
    return std::max(channel.volume - (number < int(module_.channels) ? music_shortfall_ : 0), 0);
}

bool Mod1Player::Sounding(const Channel& channel, int number) const
{
    return channel.points != 0 && channel.length != kStoppedLength && MixVolume(channel, number) != 0;
}

bool Mod1Player::Playing() const
{
    return playing_;
}

std::vector<uint8_t> Mod1Player::Globals() const
{
    std::vector<uint8_t> out;
    Put(out, order_, 2);
    Put(out, place_, 2);
    Put(out, row_samples_, 4);
    Put(out, row_left_, 4);
    Put(out, tick_samples_, 4);
    Put(out, tick_left_, 4);
    Put(out, tick_, 2);
    Put(out, arpeggio_tick_, 2);
    Put(out, broke_, 1);
    Put(out, first_order_, 2);
    Put(out, last_order_, 2);

    return out;
}

std::vector<uint8_t> Mod1Player::ChannelRecord(int channel) const
{
    return RecordBytes(channels_[size_t(channel)]);
}

const std::vector<std::string>& Mod1Player::Warnings() const
{
    return warnings_;
}

void Mod1Player::Row()
{
    // 0x08004910. The order indexes the order list as it is: past the module's orders, it reads what follows.
    const uint32_t pattern = rom_.U8(module_.address + kMod1OrderList + order_);
    if (pattern >= module_.pattern_count)
    {
        Warn("an order names a pattern that the module doesn't have");
        playing_ = false;
        return;
    }

    // The rows past the pattern's count play as empty rows.
    const bool empty = place_ >= module_.rows[pattern] * module_.channels;
    const uint32_t cells = module_.patterns[pattern] + uint32_t(place_) * kCellSize;
    row_order_ = order_;
    row_number_ = int(place_ / module_.channels);
    row_jumped_ = false;

    place_ = uint16_t(place_ + module_.channels);
    tick_ = 0;
    arpeggio_tick_ = 0;
    broke_ = 0;
    for (uint32_t c = 0; c < module_.channels; c++)
    {
        ReadRowCell(channels_[c], empty ? 0 : cells + c * kCellSize);
    }

    // The pattern's end (0x080049C0). Without a range, the order moves on. With one, an order above the range's last
    // goes to its first, and another stays: the driver compares the order as an unsigned halfword with the last order
    // as a signed one. The pattern then plays again until a jump or a break moves the order.
    if (place_ == kPatternEnd)
    {
        if (last_order_ == kNoOrder)
        {
            order_++;
        }
        else if (int32_t(order_) > int32_t(int16_t(last_order_)))
        {
            order_ = first_order_;
        }
        place_ = 0;
    }
}

void Mod1Player::ReadRowCell(Channel& ch, uint32_t address)
{
    Cell cell = address ? ReadCell(rom_, address) : Cell();

    // A slide to a note keeps the note as the slide's target, and uses the volume of the cell's sample. The cell
    // reader tests the effect's low 4 bits alone.
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
        const Sample& header = module_.samples[ch.sample];
        ch.length = header.length;
        ch.loop_start = header.loop_start;
        ch.loop_length = header.loop_length;
        ch.fine_tune = header.fine_tune;
        ch.pitch = cell.pitch;
        ch.position = 0;
        StartSample(ch, ch.sample);
        ch.step = Step(ch);
        if (cell.effect != kSlideToNote)
        {
            ch.slide = 0;
        }
    }
    else if (cell.sample != 0)
    {
        // A sample without a note: the channel uses the sample's points and volume, and starts from its place 0 with
        // the length and loop that it had (0x08005002).
        ch.sample = uint16_t(cell.sample - 1);
        ch.volume = module_.samples[ch.sample].volume;
        ch.position = 0;
        StartSample(ch, ch.sample);
    }

    // The arpeggio's fields stay from row to row.
    RowEffect(ch, cell.effect, cell.operand);
}

void Mod1Player::RowEffect(Channel& ch, uint16_t effect, uint8_t operand)
{
    const uint16_t x = operand >> 4;
    const uint16_t y = operand & 0xF;
    switch (effect)
    {
    case kArpeggio:
        // The row keeps the two intervals as they are, in pitches, and only when one isn't 0 (0x08005032).
        if (x + y != 0)
        {
            ch.effect = effect;
            ch.arpeggio_1 = x;
            ch.arpeggio_2 = y;
        }
        break;

    case kSlideUp:
        // 0x08005038 and 0x0800504C: the slide adds to the step. A slide up therefore adds.
        ch.effect = effect;
        ch.slide_step = int16_t(operand * kSlideScale);
        break;

    case kSlideDown:
        ch.effect = effect;
        ch.slide_step = int16_t(-operand * kSlideScale);
        break;

    case kSlideToNote:
        {
            // 0x08005064: the speed is the operand times the game's scale, kept when it isn't 0.
            ch.effect = effect;
            const uint16_t speed = uint16_t(operand * song_.note_slide_scale);
            if (speed != 0)
            {
                ch.slide_speed = speed;
            }
            if (ch.slide_target == int32_t(ch.pitch))
            {
                ch.slide_step = 0;
            }
            else
            {
                ch.slide_step =
                    int16_t(ch.slide_target > int32_t(ch.pitch) ? ch.slide_speed : -int32_t(ch.slide_speed));
            }
            break;
        }

    case kVibrato:
        ch.effect = effect;
        if (x != 0 || y != 0)
        {
            ch.vibrato_speed = x;
            ch.vibrato_depth = y;
        }
        break;

    case kVolumeSlide:
        ch.effect = effect;
        ch.volume_up = x;
        ch.volume_down = y;
        break;

    case kJump:
        // 0x08005104: the row is the order's last, and the order is the operand less 1. The pattern's end then moves
        // it on, or applies the range.
        order_ = uint16_t(operand - 1);
        place_ = kPatternEnd;
        row_jumped_ = true;
        break;

    case kVolume:
        ch.volume = operand;
        break;

    case kBreak:
        // 0x08005140: the first break of a row moves to the next order, with no wrap, at the cell of 4 times the
        // operand.
        if (!broke_)
        {
            place_ = uint16_t(operand * 4);
            order_++;
            broke_ = 1;
        }
        break;

    case kSpeed:
        // A row lasts the rate times the operand over 50 (0x0800516A). A larger operand makes a row of the operand
        // times 2 over 5 samples (0x08005184): the ticks a second of that tempo, taken as a length.
        row_samples_ = operand <= kLargestSpeed ? module_.rate * operand / kTicksASecond : uint32_t(operand * 2 / 5);
        break;

    default:
        break;
    }
}

void Mod1Player::Tick()
{
    // 0x08004CE0. A volume slide whose operand has both digits set leaves the pointer to the channel where it is
    // (0x08004E04): the loop's other passes come back to that channel and skip it again. The channels after it get
    // no tick.
    uint32_t c = 0;
    for (uint32_t pass = 0; pass < module_.channels; pass++)
    {
        Channel& ch = channels_[c];
        bool holds = false;
        switch (ch.effect)
        {
        case kArpeggio:
            // The step that the arpeggio works out (0x08004D2E) is replaced below. The arpeggio plays nothing.
            break;

        case kSlideUp:
        case kSlideDown:
            ch.slide = int16_t(ch.slide + ch.slide_step);
            break;

        case kSlideToNote:
            {
                // 0x08004D52: the step at the pitch plus the slide against the step at the target, without the fine
                // tune. A step above 0 has reached the target when the sum isn't below the target's step, and a step of
                // 0 or below when the sum isn't above it.
                ch.slide = int16_t(ch.slide + ch.slide_step);
                const uint32_t now = TableStep(ch.pitch) + uint32_t(int32_t(ch.slide));
                const uint32_t goal = TableStep(ch.slide_target);
                if (ch.slide_step > 0 ? now >= goal : now <= goal)
                {
                    ch.pitch = uint16_t(ch.slide_target);
                    ch.slide_step = 0;
                    ch.slide = 0;
                }
                break;
            }

        case kVibrato:
            // 0x08004DD2: the wave times the depth over 256. The position starts again at a row's first tick.
            if (tick_ == 0)
            {
                ch.vibrato_position = 0;
            }
            ch.vibrato = int16_t((VibratoWave(ch.vibrato_position) * int32_t(ch.vibrato_depth)) >> 8);
            ch.vibrato_position = uint16_t((ch.vibrato_position + ch.vibrato_speed) & 0x3F);
            break;

        case kVolumeSlide:
            if (tick_ != 0 && (ch.volume_up & ch.volume_down) != 0)
            {
                holds = true;
            }
            else if (tick_ != 0)
            {
                ch.volume = ch.volume_up != 0 ? ch.volume + ch.volume_up : ch.volume - ch.volume_down;
                ch.volume = std::clamp(ch.volume, 0, kFullVolume);
            }
            break;

        default:
            break;
        }

        if (!holds)
        {
            ch.step = Step(ch);
            c++;
        }
    }

    tick_++;
    arpeggio_tick_++;
    if (arpeggio_tick_ == kArpeggioWrap)
    {
        arpeggio_tick_ = 1;
    }
}

uint32_t Mod1Player::Step(const Channel& ch) const
{
    // 0x08004E2E and 0x08004F86: half of the vibrato, rounded towards 0, moves the pitch. An index below 0 becomes 0.
    const int32_t vibrato = ch.vibrato / 2;
    int32_t index = int32_t(ch.fine_tune) + int32_t(ch.pitch) + vibrato;
    index = index < 0 ? 0 : index > kLastStepIndex ? kPastStepIndex : index;

    return TableStep(index) + uint32_t(int32_t(ch.slide));
}

uint32_t Mod1Player::TableStep(int32_t index) const
{
    return rom_.U32(module_.steps + uint32_t(index) * 4);
}

void Mod1Player::Advance(Channel& ch, uint32_t samples)
{
    // The mixer (0x080CE184, copied to RAM) reads a point, then steps. The place and the step are 32-bit words that
    // wrap. A step that a slide brings below 0 makes the place fall, and the sample plays backwards. A place below 0
    // wraps to a point past the sample's end. At the end, the points move on to the loop's start, the loop's length
    // becomes the length, and the place is masked. A sample whose loop's length is 0 then has the length 0 and stays
    // in the mixer, at its end at each sample.
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

        if (ch.step == 0 || int32_t(ch.step) < 0)
        {
            // The samples up to the one that brings the place below 0.
            const uint64_t back = ch.step ? uint64_t(0x100000000) - ch.step : 0;
            const uint64_t run = back ? std::min<uint64_t>(samples, ch.position / back + 1) : samples;
            ch.position = uint32_t(ch.position - run * back);
            samples -= uint32_t(run);
            continue;
        }

        const uint64_t until = (uint64_t(end) - ch.position + ch.step - 1) / ch.step;
        const uint32_t run = uint32_t(std::min<uint64_t>(samples, until));
        ch.position += run * ch.step;
        samples -= run;
    }
}

void Mod1Player::StartSample(Channel& ch, uint16_t sample)
{
    ch.points = points_[sample];
    ch.starts++;
    ch.first_point = ch.points;
    ch.whole_length = ch.length;
    ch.whole_loop_start = ch.loop_start;
    ch.whole_loop_length = ch.loop_length;
}

void Mod1Player::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

} // namespace ntdlib::logikstate
