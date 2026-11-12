// SPDX-License-Identifier: MIT

#include "ntdlib/logikstate/mod30_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/mod30.h"

namespace ntdlib::logikstate
{

namespace
{

// The first half of the vibrato's wave: ProTracker's sine table. The second half is the first, negated.
constexpr std::array<int32_t, 32> kSine = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                           224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                           212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// The Amiga's clock that the drivers divide by twice a period.
constexpr int32_t kPeriodClock = 0x6C3E1D;

// The highest linear period, and the periods of an octave.
constexpr int32_t kLinearTop = 7680;
constexpr int32_t kOctave = 768;

// The linear period of note 1 at no relative note and no fine tune, and the period of a semitone.
constexpr int32_t kLinearBase = 7616;
constexpr int kSemitoneShift = 6;

// A position's fraction: 4096ths of a point.
constexpr int kFractionBits = 12;

// The fade's full value, and what the frame routine adds to it each frame.
constexpr uint32_t kFullFade = 0x40;
constexpr uint32_t kFadeStep = 3;

// A channel's volume at most, and the length up to which the mixer leaves a channel alone.
constexpr int32_t kFullVolume = 64;
constexpr uint32_t kStoppedLength = 2;

// The channels of the first builds' ticks, and the record whose volume column those ticks apply.
constexpr int kTickChannels = 4;

// The largest operand of effect F that sets the speed. A larger one sets the tempo.
constexpr uint8_t kLargestSpeed = 0x1F;

// The effects.
constexpr uint8_t kArpeggio = 0x0;
constexpr uint8_t kSlideUp = 0x1;
constexpr uint8_t kSlideDown = 0x2;
constexpr uint8_t kSlideToNote = 0x3;
constexpr uint8_t kVibrato = 0x4;
constexpr uint8_t kSlideToNoteAndVolume = 0x5;
constexpr uint8_t kVibratoAndVolume = 0x6;
constexpr uint8_t kVolumeSlide = 0xA;
constexpr uint8_t kJump = 0xB;
constexpr uint8_t kVolume = 0xC;
constexpr uint8_t kBreak = 0xD;
constexpr uint8_t kExtended = 0xE;
constexpr uint8_t kSpeed = 0xF;
constexpr uint8_t kPatternLoop = 0x60;

// The linear slides' fine and extra-fine forms, in the operand's high 4 bits.
constexpr uint32_t kFine = 0xF;
constexpr uint32_t kExtraFine = 0xE;

// The most passes of a frame loop. A loop that makes no progress stops there.
constexpr int kMaxPasses = 100000;

// Divides as the BIOS does: signed, truncating towards 0. A divisor of 0 gives 0.
int32_t Divide(int32_t value, int32_t by)
{
    return by ? int32_t(int64_t(value) / by) : 0;
}

int32_t Remainder(int32_t value, int32_t by)
{
    return by ? int32_t(int64_t(value) % by) : value;
}

// Shifts as ARM's register-specified LSR does: by the low byte of `amount`, and to 0 from 32 on.
uint32_t ShiftRight(uint32_t value, uint32_t amount)
{
    amount &= 0xFF;
    return amount >= 32 ? 0 : value >> amount;
}

// The fields of a channel's record, and where each layout keeps them.
enum class Field
{
    kPoints,
    kPosition,
    kStep,
    kLength,
    kLoopStart,
    kLoopLength,
    kVolume,
    kRelativeNote,
    kSample,
    kFineTune,
    kNote,
    kEffect,
    kVolumeUp,
    kVolumeDown,
    kArpeggio1,
    kArpeggio2,
    kVibrato,
    kVibratoDepth,
    kVibratoSpeed,
    kVibratoPosition,
    kSlide,
    kSlideSpeed,
    kSlideStep,
    kTarget,
    kLoopRow,
    kLoopCount,
    kPacked,
    kLinearSlide,
    kLinearVibrato,
    kLinearSlideOperand,
    kVolumeColumn,
    kLinearPeriod,
    kLinearTarget,
};

struct FieldPlace
{
    Field field;
    uint32_t offset;
    uint32_t size;
};

// The fields from the volume on, after the six words that every layout starts with.
constexpr FieldPlace kWords[] = {
    {Field::kPoints, 0x00, 4},    {Field::kPosition, 0x04, 4},   {Field::kStep, 0x08, 4},   {Field::kLength, 0x0C, 4},
    {Field::kLoopStart, 0x10, 4}, {Field::kLoopLength, 0x14, 4}, {Field::kVolume, 0x18, 4},
};

constexpr FieldPlace kLayoutA[] = {
    {Field::kRelativeNote, 0x30, 2},    {Field::kSample, 0x34, 2},
    {Field::kFineTune, 0x38, 2},        {Field::kNote, 0x3A, 2},
    {Field::kEffect, 0x3C, 4},          {Field::kVolumeUp, 0x40, 2},
    {Field::kVolumeDown, 0x42, 2},      {Field::kArpeggio1, 0x44, 2},
    {Field::kArpeggio2, 0x46, 2},       {Field::kVibrato, 0x48, 2},
    {Field::kVibratoDepth, 0x4A, 2},    {Field::kVibratoSpeed, 0x4C, 2},
    {Field::kVibratoPosition, 0x4E, 2}, {Field::kSlide, 0x50, 2},
    {Field::kSlideSpeed, 0x52, 2},      {Field::kSlideStep, 0x54, 2},
    {Field::kTarget, 0x56, 2},          {Field::kLoopRow, 0x68, 2},
    {Field::kLoopCount, 0x6A, 2},       {Field::kPacked, 0x6C, 2},
    {Field::kLinearSlide, 0x70, 4},     {Field::kLinearSlideOperand, 0x74, 4},
    {Field::kVolumeColumn, 0x78, 4},
};

constexpr FieldPlace kLayoutB[] = {
    {Field::kRelativeNote, 0x30, 2},
    {Field::kSample, 0x34, 2},
    {Field::kFineTune, 0x38, 2},
    {Field::kNote, 0x3A, 2},
    {Field::kEffect, 0x3C, 4},
    {Field::kVolumeUp, 0x40, 2},
    {Field::kVolumeDown, 0x42, 2},
    {Field::kArpeggio1, 0x44, 2},
    {Field::kArpeggio2, 0x46, 2},
    {Field::kVibrato, 0x48, 2},
    {Field::kVibratoDepth, 0x4A, 2},
    {Field::kVibratoSpeed, 0x4C, 2},
    {Field::kVibratoPosition, 0x4E, 2},
    {Field::kSlide, 0x52, 2},
    {Field::kSlideSpeed, 0x54, 2},
    {Field::kSlideStep, 0x56, 2},
    {Field::kTarget, 0x58, 2},
    {Field::kLoopRow, 0x6C, 2},
    {Field::kLoopCount, 0x6E, 2},
    {Field::kPacked, 0x70, 2},
    {Field::kLinearSlide, 0x74, 4},
    {Field::kLinearVibrato, 0x78, 4},
    {Field::kLinearSlideOperand, 0x7C, 4},
    {Field::kVolumeColumn, 0x80, 4},
    {Field::kLinearPeriod, 0x88, 4},
    {Field::kLinearTarget, 0x8C, 4},
};

constexpr FieldPlace kLayoutC[] = {
    {Field::kRelativeNote, 0x38, 2},
    {Field::kSample, 0x3C, 2},
    {Field::kFineTune, 0x40, 2},
    {Field::kNote, 0x42, 2},
    {Field::kEffect, 0x44, 4},
    {Field::kVolumeUp, 0x48, 2},
    {Field::kVolumeDown, 0x4A, 2},
    {Field::kArpeggio1, 0x4C, 2},
    {Field::kArpeggio2, 0x4E, 2},
    {Field::kVibrato, 0x50, 2},
    {Field::kVibratoDepth, 0x52, 2},
    {Field::kVibratoSpeed, 0x54, 2},
    {Field::kVibratoPosition, 0x56, 2},
    {Field::kSlide, 0x5A, 2},
    {Field::kSlideSpeed, 0x5C, 2},
    {Field::kSlideStep, 0x5E, 2},
    {Field::kTarget, 0x60, 2},
    {Field::kLoopRow, 0x74, 2},
    {Field::kLoopCount, 0x76, 2},
    {Field::kPacked, 0x78, 2},
    {Field::kLinearSlide, 0x7C, 4},
    {Field::kLinearVibrato, 0x80, 4},
    {Field::kLinearSlideOperand, 0x84, 4},
    {Field::kVolumeColumn, 0x88, 4},
    {Field::kLinearTarget, 0x90, 4},
};

constexpr FieldPlace kLayoutE[] = {
    {Field::kRelativeNote, 0x3C, 2},
    {Field::kSample, 0x40, 2},
    {Field::kFineTune, 0x44, 2},
    {Field::kNote, 0x46, 2},
    {Field::kEffect, 0x48, 4},
    {Field::kVolumeUp, 0x4C, 2},
    {Field::kVolumeDown, 0x4E, 2},
    {Field::kArpeggio1, 0x50, 2},
    {Field::kArpeggio2, 0x52, 2},
    {Field::kVibrato, 0x54, 2},
    {Field::kVibratoDepth, 0x56, 2},
    {Field::kVibratoSpeed, 0x58, 2},
    {Field::kVibratoPosition, 0x5A, 2},
    {Field::kSlide, 0x5E, 2},
    {Field::kSlideSpeed, 0x60, 2},
    {Field::kSlideStep, 0x62, 2},
    {Field::kTarget, 0x64, 2},
    {Field::kLoopRow, 0x78, 2},
    {Field::kLoopCount, 0x7A, 2},
    {Field::kPacked, 0x7C, 2},
    {Field::kLinearSlide, 0x80, 4},
    {Field::kLinearVibrato, 0x84, 4},
    {Field::kLinearSlideOperand, 0x88, 4},
    {Field::kVolumeColumn, 0x8C, 4},
    {Field::kLinearTarget, 0x94, 4},
};

uint32_t RecordSize(Mod30RecordLayout layout)
{
    switch (layout)
    {
    case Mod30RecordLayout::kA:
        return 0x80;

    case Mod30RecordLayout::kB:
        return 0x90;

    case Mod30RecordLayout::kC:
        return 0x94;

    case Mod30RecordLayout::kE:
    default:
        return 0x98;
    }
}

uint32_t FieldValue(const Mod30Channel& ch, Field field)
{
    switch (field)
    {
    case Field::kPoints:
        return ch.points;
    case Field::kPosition:
        return ch.position;
    case Field::kStep:
        return ch.step;
    case Field::kLength:
        return ch.length;
    case Field::kLoopStart:
        return ch.loop_start;
    case Field::kLoopLength:
        return ch.loop_length;
    case Field::kVolume:
        return uint32_t(ch.volume);
    case Field::kRelativeNote:
        return uint16_t(ch.relative_note);
    case Field::kSample:
        return ch.sample;
    case Field::kFineTune:
        return uint16_t(ch.fine_tune);
    case Field::kNote:
        return ch.note;
    case Field::kEffect:
        return uint32_t(ch.effect);
    case Field::kVolumeUp:
        return ch.volume_up;
    case Field::kVolumeDown:
        return ch.volume_down;
    case Field::kArpeggio1:
        return ch.arpeggio_1;
    case Field::kArpeggio2:
        return ch.arpeggio_2;
    case Field::kVibrato:
        return uint16_t(ch.vibrato);
    case Field::kVibratoDepth:
        return ch.vibrato_depth;
    case Field::kVibratoSpeed:
        return ch.vibrato_speed;
    case Field::kVibratoPosition:
        return ch.vibrato_position;
    case Field::kSlide:
        return uint16_t(ch.slide);
    case Field::kSlideSpeed:
        return ch.slide_speed;
    case Field::kSlideStep:
        return uint16_t(ch.slide_step);
    case Field::kTarget:
        return uint16_t(ch.target);
    case Field::kLoopRow:
        return ch.loop_row;
    case Field::kLoopCount:
        return ch.loop_count;
    case Field::kPacked:
        return ch.packed;
    case Field::kLinearSlide:
        return uint32_t(ch.linear_slide);
    case Field::kLinearVibrato:
        return uint32_t(ch.linear_vibrato);
    case Field::kLinearSlideOperand:
        return ch.linear_slide_operand;
    case Field::kVolumeColumn:
        return ch.volume_column;
    case Field::kLinearPeriod:
        return ch.linear_period;
    case Field::kLinearTarget:
    default:
        return ch.linear_target;
    }
}

// Writes `value` into `out` at `offset` in `size` bytes, the lowest first.
void Put(std::vector<uint8_t>& out, uint32_t offset, uint32_t value, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
    {
        out[offset + i] = uint8_t(value >> (8 * i));
    }
}

void Append(std::vector<uint8_t>& out, uint32_t value, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
    {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

// The builds of the eight groups.
constexpr Mod30Build kBuildA = {
    .group = Mod30Group::kA,
    .record_layout = Mod30RecordLayout::kA,
    .records = 10,
    .channel_cap = 0,
    .mix_cap = 8,
    .loop_flag = false,
    .play_request = false,
    .frame_loop = Mod30FrameLoop::kShort,
    .tick_with_row = false,
    .speed_sets_tick = false,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kOnce,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kAmigaOnly,
    .slide_to_note = Mod30LinearForm::kAmigaOnly,
    .vibrato_nibbles = false,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildB = {
    .group = Mod30Group::kB,
    .record_layout = Mod30RecordLayout::kB,
    .records = 8,
    .channel_cap = 0,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = false,
    .frame_loop = Mod30FrameLoop::kShort,
    .tick_with_row = false,
    .speed_sets_tick = false,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kTwice,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kOwnPeriod,
    .slide_to_note = Mod30LinearForm::kOwnPeriod,
    .vibrato_nibbles = false,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildC = {
    .group = Mod30Group::kC,
    .record_layout = Mod30RecordLayout::kC,
    .records = 8,
    .channel_cap = 0,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = false,
    .frame_loop = Mod30FrameLoop::kShort,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kOnceClamp,
    .plain_slide_scale = 1,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kNoteField,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildCPrime = {
    .group = Mod30Group::kCPrime,
    .record_layout = Mod30RecordLayout::kC,
    .records = 8,
    .channel_cap = 0,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = true,
    .frame_loop = Mod30FrameLoop::kShortList,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = true,
    .start_clears_records = true,
    .tick_all_channels = false,
    .tick_column = Mod30TickColumn::kRecordFour,
    .volume_slide = Mod30VolumeSlide::kOnceClamp,
    .plain_slide_scale = 1,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kNoteField,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = false,
    .linear_arpeggio = false,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildD = {
    .group = Mod30Group::kD,
    .record_layout = Mod30RecordLayout::kC,
    .records = 12,
    .channel_cap = 12,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = true,
    .frame_loop = Mod30FrameLoop::kFull,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = false,
    .start_clears_records = false,
    .tick_all_channels = true,
    .tick_column = Mod30TickColumn::kAfterModule,
    .volume_slide = Mod30VolumeSlide::kOnceAlways,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kSlideOffset,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = true,
    .linear_arpeggio = true,
    .scaled_zero_waits = false,
    .mode1_volume_zero = false,
};

constexpr Mod30Build kBuildE = {
    .group = Mod30Group::kE2,
    .record_layout = Mod30RecordLayout::kE,
    .records = 12,
    .channel_cap = 12,
    .mix_cap = 0,
    .loop_flag = true,
    .play_request = true,
    .frame_loop = Mod30FrameLoop::kFull,
    .tick_with_row = true,
    .speed_sets_tick = true,
    .short_lengths = false,
    .start_clears_records = false,
    .tick_all_channels = true,
    .tick_column = Mod30TickColumn::kEvery,
    .volume_slide = Mod30VolumeSlide::kOnceAlways,
    .plain_slide_scale = 4,
    .vibrato = Mod30LinearForm::kNoteField,
    .slide_to_note = Mod30LinearForm::kSlideOffset,
    .vibrato_nibbles = true,
    .keep_slide_on_3 = true,
    .linear_arpeggio = true,
    .scaled_zero_waits = true,
    .mode1_volume_zero = true,
};

constexpr Mod30Build WithGroup(Mod30Build build, Mod30Group group, int records)
{
    build.group = group;
    build.records = records;
    build.channel_cap = uint32_t(records);
    return build;
}

constexpr Mod30Build kBuildE1 = WithGroup(kBuildE, Mod30Group::kE1, 12);
constexpr Mod30Build kBuildE3 = WithGroup(kBuildE, Mod30Group::kE3, 8);

} // namespace

const Mod30Build& BuildOf(Mod30Group group)
{
    switch (group)
    {
    case Mod30Group::kA:
        return kBuildA;
    case Mod30Group::kB:
        return kBuildB;
    case Mod30Group::kC:
        return kBuildC;
    case Mod30Group::kCPrime:
        return kBuildCPrime;
    case Mod30Group::kD:
        return kBuildD;
    case Mod30Group::kE1:
        return kBuildE1;
    case Mod30Group::kE3:
        return kBuildE3;
    case Mod30Group::kE2:
    default:
        return kBuildE;
    }
}

Mod30Player::Mod30Player(const Rom& rom, uint32_t module, const Mod30Settings& settings)
    : rom_(rom), settings_(settings), build_(settings_.build)
{
    if (!ReadMod30Module(rom, module, module_) || !ReadMod30Bank(rom, settings.bank, settings.header_size, bank_) ||
        settings.rate_index < 0 || settings.rate_index >= int(kMod30Rates.size()))
    {
        warnings_.push_back("the module at " + agb::Hex(module) + " can't be read");
        return;
    }

    // The init: the rate and the volumes. The play routine of the first builds loads and starts the module at once.
    rate_ = kMod30Rates[size_t(settings.rate_index)];
    frame_samples_ = kMod30FrameSamples[size_t(settings.rate_index)];
    valid_ = true;
    channels_ = LoadedChannels();
    if (!build_.play_request)
    {
        Start();
    }
}

bool Mod30Player::Valid() const
{
    return valid_;
}

const Mod30Module& Mod30Player::Header() const
{
    return module_;
}

const Mod30Bank& Mod30Player::Bank() const
{
    return bank_;
}

void Mod30Player::OnStep(std::function<void(bool)> callback)
{
    on_step_ = std::move(callback);
}

uint64_t Mod30Player::Elapsed() const
{
    return elapsed_;
}

bool Mod30Player::Playing() const
{
    return valid_ && (playing_ != 0 || !started_);
}

int Mod30Player::RowOrder() const
{
    return row_order_;
}

int Mod30Player::RowNumber() const
{
    return row_number_;
}

uint32_t Mod30Player::Speed() const
{
    return speed_;
}

uint32_t Mod30Player::Tempo() const
{
    return tempo_;
}

uint32_t Mod30Player::RowSamples() const
{
    return uint32_t(row_samples_);
}

uint32_t Mod30Player::TickSamples() const
{
    return uint32_t(tick_samples_);
}

uint32_t Mod30Player::Rate() const
{
    return rate_;
}

int Mod30Player::Channels() const
{
    return channels_;
}

const std::array<Mod30Channel, kMod30Records>& Mod30Player::Records() const
{
    return records_;
}

int32_t Mod30Player::MixVolume(const Mod30Channel& channel) const
{
    int32_t v = (channel.volume * settings_.music) >> 6;
    v = (v * settings_.master) >> 6;
    v = (v * int32_t(fade_)) >> 6;

    return std::max(v, 0);
}

bool Mod30Player::Sounding(const Mod30Channel& channel, int number) const
{
    const int mixed =
        build_.mix_cap != 0 && settings_.mixer_mode == 0 ? std::min(build_.mix_cap, channels_) : channels_;
    if (number >= mixed || channel.points == 0 || channel.length <= kStoppedLength)
    {
        return false;
    }

    return !(settings_.mixer_mode == 0 && build_.scaled_zero_waits && MixVolume(channel) == 0);
}

double Mod30Player::RootRate(int sample) const
{
    Mod30Channel ch;
    ch.relative_note = bank_.samples[size_t(sample)].relative_note;
    ch.fine_tune = bank_.samples[size_t(sample)].fine_tune;
    if (module_.amiga != 0)
    {
        const int32_t period = Period(uint32_t(ch.fine_tune + kMod30RootNote));
        return period > 0 ? double(kPeriodClock) / (2.0 * period) : 0;
    }

    // The frequency that the driver works the step out from, before it divides by the mixer's rate.
    const int32_t y = kLinearTop - int32_t(LinearPeriod(ch, kMod30RootNote));
    const int32_t octaves = Divide(y, kOctave);
    const int32_t place = std::clamp(Remainder(y, kOctave), 0, kOctave - 1);
    const double entry = rom_.U16(settings_.linear + uint32_t(place * 2)) * 4.0;

    return std::ldexp(entry, octaves - 7);
}

std::vector<uint32_t> Mod30Player::RepeatState() const
{
    std::vector<uint32_t> state = {speed_, tempo_};
    for (int c = 0; c < channels_; c++)
    {
        const Mod30Channel& ch = records_[size_t(c)];
        state.push_back(uint32_t(ch.loop_row) << 16 | ch.loop_count);
        for (const Stream& s : decoders_[size_t(c)].streams)
        {
            state.push_back(s.address);
            state.push_back(uint32_t(s.count));
        }
    }

    return state;
}

const std::vector<std::string>& Mod30Player::Warnings() const
{
    return warnings_;
}

std::vector<uint8_t> Mod30Player::Globals() const
{
    std::vector<uint8_t> out;
    Append(out, order_, 4);
    Append(out, row_, 4);
    Append(out, speed_, 4);
    Append(out, tempo_, 4);
    Append(out, hz_, 4);
    Append(out, uint32_t(row_samples_), 4);
    Append(out, uint32_t(row_left_), 4);
    Append(out, uint32_t(tick_samples_), 4);
    Append(out, uint32_t(tick_left_), 4);
    Append(out, tick_, 2);
    Append(out, arpeggio_tick_, 2);
    Append(out, broke_, 1);
    Append(out, playing_, 2);
    Append(out, fade_, 4);

    return out;
}

std::vector<uint8_t> Mod30Player::Record(int channel) const
{
    const Mod30Channel& ch = records_[size_t(channel)];
    std::vector<uint8_t> out(RecordSize(build_.record_layout), 0);
    for (const FieldPlace& f : kWords)
    {
        Put(out, f.offset, FieldValue(ch, f.field), f.size);
    }

    const auto put_all = [&](const auto& places)
    {
        for (const FieldPlace& f : places)
        {
            Put(out, f.offset, FieldValue(ch, f.field), f.size);
        }
    };
    switch (build_.record_layout)
    {
    case Mod30RecordLayout::kA:
        put_all(kLayoutA);
        break;
    case Mod30RecordLayout::kB:
        put_all(kLayoutB);
        break;
    case Mod30RecordLayout::kC:
        put_all(kLayoutC);
        break;
    case Mod30RecordLayout::kE:
        put_all(kLayoutE);
        break;
    }

    return out;
}

int Mod30Player::LoadedChannels() const
{
    if (build_.channel_cap != 0 && module_.channels > build_.channel_cap)
    {
        return int(build_.channel_cap);
    }

    return int(module_.channels);
}

void Mod30Player::Start()
{
    // The load: the module's speed, tempo and channels, the latter up to the build's cap. The sound effects take the
    // records after the module's channels.
    speed_ = module_.speed;
    tempo_ = module_.tempo;
    channels_ = LoadedChannels();
    const int cap = build_.channel_cap != 0 ? int(build_.channel_cap) : build_.records;
    end_channel_ = channels_ + std::min(cap - int(module_.channels), settings_.sfx_channels);

    // The start at order 0.
    fade_ = kFullFade;
    hz_ = uint32_t(Divide(int32_t((tempo_ * 50) << 16), 125) >> 16);
    row_samples_ = Divide(int32_t(rate_ * speed_), int32_t(hz_));
    tick_samples_ = Divide(int32_t(rate_), int32_t(hz_));
    order_ = 0;
    row_ = 0;
    row_left_ = 0;
    tick_left_ = 0;
    for (int c = 0; c < build_.records; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (build_.start_clears_records)
        {
            ch = Mod30Channel();
        }
        else
        {
            ch.volume = 0;
            ch.length = 0;
            ch.loop_length = 0;
        }
    }
    broke_ = 0;
    tick_ = 0;
    playing_ = 1;
    started_ = true;
}

void Mod30Player::Frame()
{
    if (!valid_)
    {
        return;
    }

    // The fade rises each frame. A play request then starts its module.
    fade_ = std::min(fade_ + kFadeStep, kFullFade);
    if (!started_)
    {
        Start();
    }

    MixFrame();
    frames_++;
    elapsed_ = std::max(elapsed_, frames_ * frame_samples_);
}

void Mod30Player::MixFrame()
{
    const int32_t size = int32_t(frame_samples_);
    const bool full = build_.frame_loop == Mod30FrameLoop::kFull;
    const bool mode1 = settings_.mixer_mode == 1;
    if (build_.frame_loop == Mod30FrameLoop::kShort && playing_ == 0)
    {
        return;
    }

    // The first builds count each pass as one sample more than it mixes, and the frame can end a few samples short.
    int32_t done = 0;
    int32_t left = size;
    for (int pass = 0; pass < kMaxPasses; pass++)
    {
        if (full ? done >= size : left <= 0)
        {
            return;
        }

        int32_t n = size;
        if (playing_ != 0 || build_.frame_loop == Mod30FrameLoop::kShort)
        {
            n = RowAndTick(done);
        }

        // The mix of the pass. Mixer mode 1 ends a pass early where a channel's sample ends.
        int listed = 0;
        if (mode1)
        {
            n = ListChannels(end_channel_, n, listed);
        }
        else
        {
            const int count = build_.mix_cap != 0 ? std::min(build_.mix_cap, channels_) : channels_;
            MixChannels(0, count, uint32_t(std::max(n, 0)));
        }
        if (full)
        {
            if (done + n > size)
            {
                n = size - done;
            }
            if (playing_ != 0)
            {
                n = std::min({n, row_left_, tick_left_});
            }
        }
        if (mode1 && listed != 0)
        {
            AdvanceListed(end_channel_, n);
        }

        done += n;
        row_left_ -= n;
        tick_left_ -= n;
        left -= n + (full ? 0 : 1);
        if (n <= 0 && full)
        {
            Warn("a pass of the mixer has no samples, which the driver can't play");
            valid_ = false;
            return;
        }
    }

    Warn("a frame doesn't end, which the driver can't play");
    valid_ = false;
}

int32_t Mod30Player::RowAndTick(int32_t done)
{
    const int32_t size = int32_t(frame_samples_);
    elapsed_ = frames_ * frame_samples_ + uint64_t(done);
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
        if (build_.tick_with_row || row_left_ != row_samples_)
        {
            Tick();
            if (on_step_)
            {
                on_step_(false);
            }
        }
        tick_left_ = tick_samples_;
    }

    int32_t n = std::min(row_left_, tick_left_);
    if (build_.frame_loop != Mod30FrameLoop::kFull && n > size)
    {
        n = size;
    }
    if (done + n > size)
    {
        n = size - done;
    }

    return n;
}

void Mod30Player::MixChannels(int first, int end, uint32_t samples)
{
    for (int c = first; c < end; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (ch.points == 0 || ch.length <= kStoppedLength)
        {
            continue;
        }
        if (build_.scaled_zero_waits && MixVolume(ch) == 0)
        {
            continue;
        }

        Advance(ch, samples);
    }
}

int32_t Mod30Player::ListChannels(int end, int32_t samples, int& listed)
{
    // A pass ends where a channel's sample ends. Group C' lists every channel, and ends the pass at the sample's end
    // itself. The later builds end it a point before.
    const bool listed_all = build_.frame_loop == Mod30FrameLoop::kShortList;
    for (int c = 0; c < end; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (ch.points == 0 && !listed_all)
        {
            continue;
        }
        listed++;
        if (build_.mode1_volume_zero && ch.length <= 1)
        {
            ch.volume = 0;
        }
        if (ch.length != 0 && ch.volume != 0)
        {
            const uint32_t e = ch.position + ch.step * uint32_t(samples);
            const uint32_t limit = listed_all ? ch.length : ch.length - 0x1000;
            if (e > limit)
            {
                samples -= Divide(int32_t(e - ch.length), int32_t(ch.step));
            }
        }
    }

    return samples;
}

void Mod30Player::AdvanceListed(int end, int32_t samples)
{
    const bool listed_all = build_.frame_loop == Mod30FrameLoop::kShortList;
    for (int c = 0; c < end; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        if (!listed_all && ch.length == 0 && ch.volume == 0)
        {
            continue;
        }

        ch.position += ch.step * uint32_t(samples);
        if (ch.position >= ch.length)
        {
            ch.points += ch.loop_start;
            ch.position -= ch.length;
            ch.length = ch.loop_length;
            ch.loop_start = 0;
            ch.looped = true;
        }
    }
}

void Mod30Player::Advance(Mod30Channel& ch, uint32_t samples)
{
    // Each sample: at the end, the points move on to the loop, the loop becomes the length, and the place keeps its
    // overshoot. A sample without a loop stops there. Then the mixer reads a point and steps.
    while (samples > 0 && ch.length != 0)
    {
        if (int32_t(ch.position) >= int32_t(ch.length))
        {
            ch.points += ch.loop_start;
            ch.loop_start = 0;
            ch.position -= ch.length;
            ch.length = ch.loop_length;
            ch.looped = true;
            if (ch.length == 0)
            {
                return;
            }
            ch.position += ch.step;
            samples--;
            continue;
        }

        const int64_t room = int64_t(int32_t(ch.length)) - int32_t(ch.position);
        const uint64_t until = ch.step ? uint64_t((room + ch.step - 1) / ch.step) : samples;
        const uint32_t run = uint32_t(std::min<uint64_t>(samples, until));
        ch.position += run * ch.step;
        samples -= run;
    }
}

void Mod30Player::Row()
{
    if (playing_ == 0)
    {
        return;
    }

    tick_ = 0;
    arpeggio_tick_ = 0;
    broke_ = 0;
    row_order_ = int(order_);
    row_number_ = int(row_);
    uint32_t block = 0;
    for (int c = 0; c < channels_; c++)
    {
        Decode(c, block);
    }
    row_++;
    for (int c = 0; c < channels_; c++)
    {
        PlayCell(records_[size_t(c)], c);
    }

    // The order's end, after its last row or a jump.
    if (row_ == module_.rows)
    {
        order_++;
        if (order_ >= module_.order_count)
        {
            order_ = 0;
            if (!build_.loop_flag || !settings_.loop)
            {
                playing_ = 0;
            }
        }
        row_ = 0;
    }
}

uint32_t Mod30Player::PatternBlock(uint32_t order) const
{
    const Mod30HeaderLayout& layout = HeaderLayout(module_.layout);
    const uint32_t pattern = rom_.U8(module_.address + layout.orders + order);
    if (module_.pattern_offsets)
    {
        return module_.address + layout.patterns + rom_.U32(module_.address + layout.pattern_table + 4 * pattern);
    }

    return module_.patterns[pattern];
}

uint32_t Mod30Player::RepeatOrders() const
{
    const uint32_t count = module_.order_count;
    if (!build_.loop_flag || !settings_.loop)
    {
        return count;
    }

    // The shortest period of the orders' first blocks, counted round from the last order to the first.
    for (uint32_t period = 1; period < count; period++)
    {
        if (count % period != 0)
        {
            continue;
        }
        bool repeats = true;
        for (uint32_t o = period; o < count && repeats; o++)
        {
            repeats = PatternBlock(o) == PatternBlock(o - period);
        }
        if (repeats)
        {
            return period;
        }
    }

    return count;
}

void Mod30Player::Decode(int c, uint32_t& block)
{
    Decoder& d = decoders_[size_t(c)];
    const auto next = [this](Stream& s, bool wide)
    {
        if (s.count <= 0)
        {
            s.count = rom_.U8(s.address);
            s.value = wide ? uint32_t(rom_.U8(s.address + 1) << 8 | rom_.U8(s.address + 2)) : rom_.U8(s.address + 1);
            s.address += wide ? 3 : 2;
        }
        s.count--;

        return s.value;
    };

    // Row 0 starts each channel's streams in its block of the order's pattern.
    if (row_ == 0)
    {
        if (block == 0)
        {
            block = PatternBlock(order_);
        }
        const uint32_t data = block + kMod30BlockHeader;
        d.streams[0] = {data, 0, 0};
        d.streams[1] = {data + rom_.U32(block + kMod30BlockStreamB), 0, 0};
        d.streams[2] = {data + rom_.U32(block + kMod30BlockStreamC), 0, 0};
        block = data + rom_.U32(block + kMod30BlockSize);
    }

    const uint32_t a = next(d.streams[0], true);
    d.note = uint16_t(a >> kMod30NoteShift);
    d.sample = uint8_t(a & kMod30CellSample);
    d.volume = uint8_t(next(d.streams[1], false));
    const uint32_t e = next(d.streams[2], true);
    d.effect = uint8_t(e >> 8);
    d.operand = uint8_t(e);
}

void Mod30Player::PlayCell(Mod30Channel& ch, int c)
{
    const Decoder& d = decoders_[size_t(c)];
    uint16_t note = d.note;
    uint8_t sample = d.sample;
    const uint8_t effect = d.effect;
    ch.volume_column = d.volume;

    // A slide to a note keeps the note as its target, and takes the volume of the cell's sample.
    if ((effect & 0xF) == kSlideToNote)
    {
        if (note != kMod30NoNote)
        {
            ch.target = int16_t(note);
        }
        note = kMod30NoNote;
        if (sample != 0)
        {
            ch.volume = bank_.samples[sample - 1u].volume;
        }
        sample = 0;
    }

    ch.effect = -1;
    ch.vibrato = 0;
    if (note != kMod30NoNote)
    {
        // A note starts the channel's sample, or the cell's, from its first point.
        if (sample != 0)
        {
            ch.sample = uint16_t(sample - 1);
            ch.volume = bank_.samples[ch.sample].volume;
        }
        const Mod30Sample& h = bank_.samples[ch.sample];
        const auto scaled = [this](uint32_t points)
        {
            return build_.short_lengths && points != 0 ? (points << kFractionBits) - 1 : points << kFractionBits;
        };
        ch.points = bank_.points[ch.sample];
        ch.length = scaled(h.length);
        ch.loop_start = h.loop_start;
        ch.loop_length = scaled(h.loop_length);
        ch.fine_tune = h.fine_tune;
        ch.relative_note = h.relative_note;
        ch.note = note;
        ch.position = 0;
        ch.packed = 0;
        ch.starts++;
        ch.first_point = ch.points;
        ch.whole_length = h.length;
        ch.whole_loop_start = h.loop_start;
        ch.whole_loop_length = h.loop_length;
        ch.looped = false;
        if (!build_.keep_slide_on_3 || effect != kSlideToNote)
        {
            ch.linear_slide = 0;
        }
        ch.linear_vibrato = 0;
        ch.linear_slide_operand = 0;
        if (module_.amiga != 0)
        {
            ch.step = AmigaStep(ch);
        }
        else if (build_.vibrato == Mod30LinearForm::kAmigaOnly)
        {
            ch.step = LinearStep(int32_t(LinearPeriod(ch, ch.note)) + ch.linear_slide);
        }
        else if (build_.vibrato == Mod30LinearForm::kOwnPeriod)
        {
            ch.linear_period = LinearPeriod(ch, ch.note);
            Recompute(ch);
        }
        else
        {
            ch.note = uint16_t(LinearPeriod(ch, ch.note));
            Recompute(ch);
            ch.linear_target = 0;
        }
        if (effect != kSlideToNote)
        {
            ch.slide = 0;
        }
    }
    else if (sample != 0)
    {
        // A sample without a note: the channel takes its points and its volume, and keeps its place and lengths.
        ch.sample = uint16_t(sample - 1);
        ch.points = bank_.points[ch.sample];
        ch.volume = bank_.samples[ch.sample].volume;
        ch.packed = 0;
        ch.starts++;
        ch.first_point = ch.points;
    }

    ch.arpeggio_1 = 0;
    ch.arpeggio_2 = 0;
    if (effect <= 0xF)
    {
        RowEffect(ch, effect, d.operand);
    }
    VolumeColumn(ch);
}

void Mod30Player::RowEffect(Mod30Channel& ch, uint8_t effect, uint8_t operand)
{
    const bool linear = module_.amiga == 0;
    const uint16_t x = uint16_t(operand >> 4);
    const uint16_t y = uint16_t(operand & 0xF);
    switch (effect)
    {
    case kArpeggio:
        ch.effect = effect;
        ch.arpeggio_1 = uint16_t(x * 8);
        ch.arpeggio_2 = uint16_t(y * 8);
        break;

    case kSlideUp:
    case kSlideDown:
        ch.effect = effect;
        if (!linear)
        {
            ch.slide_step = int16_t(effect == kSlideUp ? -int32_t(operand) : int32_t(operand));
            break;
        }
        LinearSlide(ch, effect == kSlideUp, operand);
        Recompute(ch);
        break;

    case kSlideToNote:
        ch.effect = effect;
        if (operand != 0)
        {
            ch.slide_speed = operand;
        }
        if (!linear || build_.slide_to_note == Mod30LinearForm::kAmigaOnly)
        {
            ch.slide_step = ch.target == int16_t(ch.note)  ? 0
                            : ch.target > int32_t(ch.note) ? int16_t(-int32_t(ch.slide_speed))
                                                           : int16_t(ch.slide_speed);
        }
        else
        {
            ch.linear_target = LinearPeriod(ch, uint16_t(ch.target));
        }
        break;

    case kVibrato:
        ch.effect = effect;
        if (!build_.vibrato_nibbles)
        {
            if (operand != 0)
            {
                ch.vibrato_speed = x;
                ch.vibrato_depth = y;
            }
            break;
        }
        if (x != 0)
        {
            ch.vibrato_speed = x;
        }
        if (y != 0)
        {
            ch.vibrato_depth = y;
        }
        if (operand != 0)
        {
            ch.vibrato_position = 0;
        }
        break;

    case kSlideToNoteAndVolume:
    case kVibratoAndVolume:
    case kVolumeSlide:
        ch.effect = effect;
        ch.volume_up = x;
        ch.volume_down = y;
        break;

    case kJump:
        // The row is the order's last, and the order after it is the operand's.
        order_ = uint32_t(operand) - 1;
        row_ = module_.rows;
        break;

    case kVolume:
        ch.volume = operand;
        break;

    case kBreak:
        // The first break of a row moves to the next order. The streams don't seek: the next row reads on in them
        // unless the operand is 0.
        if (!broke_)
        {
            row_ = operand;
            order_++;
            if (order_ >= module_.order_count)
            {
                order_ = 0;
            }
            broke_ = 1;
        }
        break;

    case kExtended:
        if ((operand & 0xF0) == kPatternLoop)
        {
            if (y == 0)
            {
                ch.loop_row = uint16_t(row_ - 1);
            }
            else
            {
                ch.loop_count = ch.loop_count != 0 ? uint16_t(ch.loop_count - 1) : y;
                if (ch.loop_count != 0)
                {
                    row_ = ch.loop_row;
                }
            }
        }
        break;

    case kSpeed:
        SetSpeed(operand);
        break;

    default:
        break;
    }
}

void Mod30Player::SetSpeed(uint8_t operand)
{
    if (operand <= kLargestSpeed)
    {
        speed_ = operand;
    }
    else
    {
        tempo_ = operand;
    }
    hz_ = uint32_t(Divide(int32_t((tempo_ * 50) << 16), 125) >> 16);
    row_samples_ = Divide(int32_t(rate_ * speed_), int32_t(hz_));
    if (build_.speed_sets_tick)
    {
        tick_samples_ = Divide(int32_t(rate_), int32_t(hz_));
    }
}

void Mod30Player::Tick()
{
    if (playing_ == 0)
    {
        return;
    }

    const int count = build_.tick_all_channels ? channels_ : kTickChannels;
    for (int c = 0; c < count; c++)
    {
        Mod30Channel& ch = records_[size_t(c)];
        TickEffect(ch);
        if (ch.arpeggio_1 + ch.arpeggio_2 == 0 && module_.amiga != 0)
        {
            ch.step = AmigaStep(ch);
        }
        if (build_.tick_column == Mod30TickColumn::kEvery)
        {
            VolumeColumn(ch);
        }
    }
    tick_++;
    arpeggio_tick_++;

    if (build_.tick_column == Mod30TickColumn::kRecordFour)
    {
        VolumeColumn(records_[kTickChannels]);
    }
    else if (build_.tick_column == Mod30TickColumn::kAfterModule && channels_ < build_.records)
    {
        VolumeColumn(records_[size_t(channels_)]);
    }
}

void Mod30Player::TickEffect(Mod30Channel& ch)
{
    switch (ch.effect)
    {
    case kArpeggio:
        // The arpeggio adds no interval.
        if (arpeggio_tick_ == 0 || ch.arpeggio_1 + ch.arpeggio_2 == 0)
        {
            break;
        }
        if (arpeggio_tick_ == 4)
        {
            arpeggio_tick_ = 1;
        }
        if (build_.linear_arpeggio && module_.amiga == 0)
        {
            ch.step = TableStep(ch.note);
        }
        else
        {
            ch.step = PeriodStep(int32_t(Period(uint32_t(ch.fine_tune + ch.note))) + ch.slide + ch.vibrato);
        }
        break;

    case kSlideUp:
    case kSlideDown:
        if (module_.amiga != 0)
        {
            ch.slide = int16_t(ch.slide + ch.slide_step);
            break;
        }
        LinearSlide(ch, ch.effect == kSlideUp, 0);
        Recompute(ch);
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
}

void Mod30Player::LinearSlide(Mod30Channel& ch, bool up, uint32_t operand)
{
    if (operand != 0)
    {
        ch.linear_slide_operand = operand;
    }
    else
    {
        operand = ch.linear_slide_operand;
    }

    // The fine and extra-fine forms act at the row's first tick, the plain form at the others.
    const uint32_t high = (operand >> 4) & 0xFF;
    const uint32_t low = operand & 0xF;
    int32_t move = 0;
    if (high == kFine || high == kExtraFine)
    {
        if (tick_ != 0)
        {
            return;
        }
        move = int32_t(high == kFine ? low * 4 : low);
    }
    else
    {
        if (tick_ == 0)
        {
            return;
        }
        move = int32_t(operand & 0xFFFF) * build_.plain_slide_scale;
    }
    ch.linear_slide += up ? -move : move;
}

void Mod30Player::SlideToNote(Mod30Channel& ch)
{
    const bool linear = module_.amiga == 0;
    if (build_.slide_to_note == Mod30LinearForm::kAmigaOnly ||
        (!linear && build_.slide_to_note == Mod30LinearForm::kOwnPeriod))
    {
        // The Amiga form compares steps where periods were meant.
        ch.slide = int16_t(ch.slide + ch.slide_step);
        const uint32_t from = TableStep(ch.note) + uint32_t(int32_t(ch.slide));
        const uint32_t to = TableStep(uint32_t(int32_t(ch.target)));
        if (ch.slide_step > 0 ? from >= to : from <= to)
        {
            ch.note = uint16_t(ch.target);
            ch.slide_step = 0;
            ch.slide = 0;
        }
        return;
    }

    const int32_t speed = int32_t(ch.slide_speed) * 4;
    if (build_.slide_to_note == Mod30LinearForm::kOwnPeriod)
    {
        // The linear period moves towards the target, without an end.
        const int32_t d = int32_t(ch.linear_period) - int32_t(ch.linear_target);
        if (d == 0 || speed > std::abs(d))
        {
            ch.linear_period = ch.linear_target;
        }
        else
        {
            ch.linear_period = uint32_t(int32_t(ch.linear_period) + (d > 0 ? -speed : speed));
        }
        Recompute(ch);
        return;
    }

    if (ch.linear_target == 0)
    {
        return;
    }
    if (!linear)
    {
        ch.slide = int16_t(ch.slide + ch.slide_step);
        const uint32_t from = TableStep(ch.note) + uint32_t(int32_t(ch.slide));
        const uint32_t to = TableStep(uint32_t(int32_t(ch.target)));
        if (ch.slide_step > 0 ? from >= to : from <= to)
        {
            ch.note = uint16_t(ch.target);
            ch.slide_step = 0;
            ch.slide = 0;
        }
        return;
    }

    const int32_t target = int32_t(ch.linear_target);
    if (build_.slide_to_note == Mod30LinearForm::kNoteField)
    {
        // The period itself moves, and the slide ends at the target.
        const int32_t d = int32_t(ch.note) - target;
        if (d == 0 || int32_t(int16_t(ch.slide_speed)) * 4 > std::abs(d))
        {
            ch.note = uint16_t(target);
            ch.linear_target = 0;
        }
        else if (d > 0)
        {
            ch.note = uint16_t(ch.note - speed);
            if (int32_t(ch.note) <= target)
            {
                ch.note = uint16_t(target);
                ch.linear_target = 0;
            }
        }
        else
        {
            ch.note = uint16_t(ch.note + speed);
            if (int32_t(ch.note) >= target)
            {
                ch.note = uint16_t(target);
                ch.linear_target = 0;
            }
        }
        Recompute(ch);
        return;
    }

    // The linear slide's offset moves, and the slide ends at the target.
    const int32_t d = int32_t(ch.note) + ch.linear_slide - target;
    if (d == 0 || int32_t(int16_t(ch.slide_speed)) * 4 > std::abs(d))
    {
        ch.linear_slide = target - int32_t(ch.note);
        ch.linear_target = 0;
    }
    else if (d > 0)
    {
        ch.linear_slide -= speed;
        if (int32_t(ch.note) + ch.linear_slide <= target)
        {
            ch.linear_slide = target - int32_t(ch.note);
            ch.linear_target = 0;
        }
    }
    else
    {
        ch.linear_slide += speed;
        if (int32_t(ch.note) + ch.linear_slide >= target)
        {
            ch.linear_slide = target - int32_t(ch.note);
            ch.linear_target = 0;
        }
    }
    Recompute(ch);
}

void Mod30Player::Vibrato(Mod30Channel& ch)
{
    const bool linear = module_.amiga == 0;
    if (build_.vibrato == Mod30LinearForm::kAmigaOnly || build_.vibrato == Mod30LinearForm::kOwnPeriod)
    {
        // The first builds restart the vibrato at a row's first tick.
        if (tick_ == 0)
        {
            ch.vibrato_position = 0;
        }
        const uint16_t p = ch.vibrato_position;
        if (linear && build_.vibrato == Mod30LinearForm::kOwnPeriod)
        {
            const int32_t v = (kSine[(p >> 1) & 0x1F] * int32_t(ch.vibrato_depth)) >> 6;
            ch.linear_vibrato = v;
            ch.vibrato_position = uint16_t((p + ch.vibrato_speed) & 0x3F);
            Recompute(ch);
            return;
        }
        const int32_t wave = p < 32 ? kSine[p & 31] : -kSine[p & 31];
        ch.vibrato = int16_t((wave * int32_t(ch.vibrato_depth)) >> 7);
        ch.vibrato_position = uint16_t((p + ch.vibrato_speed) & 0x3F);
        return;
    }

    const uint16_t p = ch.vibrato_position;
    if (!linear)
    {
        ch.vibrato = int16_t((int32_t(ch.vibrato_depth) * kSine[p & 31]) >> 7);
        ch.vibrato_position = uint16_t((p + ch.vibrato_speed) & 0x3F);
        return;
    }
    const int32_t v = (kSine[(p >> 2) & 0x1F] * int32_t(ch.vibrato_depth)) >> 5;
    ch.linear_vibrato = p > 0x1F ? -v : v;
    ch.vibrato_position = uint16_t((p + ((ch.vibrato_speed + 4) >> 1)) & 0x3F);
    Recompute(ch);
}

void Mod30Player::VolumeSlide(Mod30Channel& ch)
{
    switch (build_.volume_slide)
    {
    case Mod30VolumeSlide::kOnce:
    case Mod30VolumeSlide::kTwice:
        {
            if (tick_ == 0 || uint16_t(ch.volume_up + ch.volume_down) == 0)
            {
                return;
            }
            const int32_t scale = build_.volume_slide == Mod30VolumeSlide::kTwice ? 2 : 1;
            ch.volume = ch.volume_up != 0 ? ch.volume + scale * ch.volume_up : ch.volume - scale * ch.volume_down;
            break;
        }

    case Mod30VolumeSlide::kOnceClamp:
        if (tick_ == 0)
        {
            return;
        }
        ch.volume = ch.volume_up != 0 ? ch.volume + ch.volume_up : ch.volume - ch.volume_down;
        break;

    case Mod30VolumeSlide::kOnceAlways:
        if (tick_ != 0)
        {
            ch.volume = ch.volume_up != 0 ? ch.volume + ch.volume_up : ch.volume - ch.volume_down;
        }
        break;
    }
    ch.volume = std::clamp(ch.volume, 0, kFullVolume);
}

void Mod30Player::VolumeColumn(Mod30Channel& ch)
{
    if (ch.volume_column != 0)
    {
        ch.volume = int32_t(ch.volume_column) - 1;
    }
}

uint32_t Mod30Player::LinearPeriod(const Mod30Channel& ch, uint32_t note) const
{
    const uint32_t semitones = uint32_t(ch.relative_note - 1 + int32_t(uint16_t(note)) - 1) & 0xFFFF;

    return uint32_t(kLinearBase - int32_t(semitones << kSemitoneShift) - (ch.fine_tune >> 1)) & 0xFFFF;
}

uint32_t Mod30Player::LinearStep(int32_t period) const
{
    const int32_t y = kLinearTop - period;
    const int32_t octaves = Divide(y, kOctave);
    const int32_t place =
        build_.vibrato == Mod30LinearForm::kAmigaOnly ? int32_t(uint32_t(y) % kOctave) : Remainder(y, kOctave);
    const uint32_t entry = rom_.U16(settings_.linear + uint32_t(place * 2));
    const uint32_t frequency = ShiftRight(entry << 2, uint32_t(7 - octaves));

    return uint32_t(Divide(int32_t(frequency << kFractionBits), int32_t(rate_)));
}

void Mod30Player::Recompute(Mod30Channel& ch)
{
    switch (build_.vibrato)
    {
    case Mod30LinearForm::kAmigaOnly:
        ch.step = LinearStep(int32_t(LinearPeriod(ch, ch.note)) + ch.linear_slide);
        break;

    case Mod30LinearForm::kOwnPeriod:
        ch.step = LinearStep(int32_t(ch.linear_period) + ch.linear_slide + ch.linear_vibrato);
        break;

    default:
        ch.step = LinearStep(int32_t(ch.note) + ch.linear_slide + ch.linear_vibrato);
        break;
    }
}

uint32_t Mod30Player::PeriodStep(int32_t period) const
{
    int32_t twice = period * 2;
    if (twice == 0)
    {
        twice = 1;
    }

    return uint32_t(Divide(Divide(kPeriodClock, twice) << kFractionBits, int32_t(rate_)));
}

uint32_t Mod30Player::AmigaStep(const Mod30Channel& ch) const
{
    if (ch.slide == 0 && ch.vibrato == 0 && ch.fine_tune == 0)
    {
        return TableStep(ch.note);
    }

    return PeriodStep(int32_t(Period(uint32_t(ch.fine_tune + ch.note))) + ch.slide + ch.vibrato);
}

uint16_t Mod30Player::Period(uint32_t pitch) const
{
    return rom_.U16(settings_.periods + pitch * 2);
}

uint32_t Mod30Player::TableStep(uint32_t pitch) const
{
    return rom_.U32(settings_.steps + pitch * 4);
}

void Mod30Player::Warn(const std::string& text)
{
    if (std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
    {
        warnings_.push_back(text);
    }
}

} // namespace ntdlib::logikstate
