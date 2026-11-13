// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/chord_player.h"

#include "xsflib/binary.h"

namespace ntdlib::jaudio
{

namespace
{

constexpr uint8_t kHold = 0x08;
constexpr int kNow = 4;

// The levels that stop the effect, and that leave it playing, in place of a start.
constexpr uint8_t kStopLevel = 1;
constexpr uint8_t kKeepLevel = 2;

} // namespace

ChordPlayer::ChordPlayer(std::span<const uint8_t> data, const ChordDatabase::Entry& entry, int start, float level)
    : data_(data), steps_(entry.steps), level_(level), step_(start + 1)
{
    const xsflib::Reader r(data, true);
    const uint8_t lowest = r.U8(entry.offset + 2);
    const uint8_t highest = r.U8(entry.offset + 3);
    for (const ChordDatabase::Part& part : entry.parts)
    {
        parts_.emplace_back(data, part, lowest, highest);
        parts_.back().Start(start);
    }

    keys_.assign(parts_.size(), 0);
}

uint8_t ChordPlayer::Flow(int step) const
{
    return xsflib::Reader(data_, true).U8(steps_ + 1 + 2 * static_cast<std::size_t>(step));
}

uint8_t ChordPlayer::Level(int step) const
{
    return xsflib::Reader(data_, true).U8(steps_ + 2 + 2 * static_cast<std::size_t>(step));
}

std::vector<ChordStep> ChordPlayer::Start(ChordMoment& moment, ChordRandom& random)
{
    std::vector<ChordStep> steps;
    const int wait = Flow(0) & 7;
    if (wait < kNow)
    {
        waiting_ = wait;
    }
    else if (wait == kNow)
    {
        steps.push_back(Play(1.0f, moment, random));
    }

    return steps;
}

std::vector<ChordStep> ChordPlayer::Event(float accent, ChordMoment& moment, ChordRandom& random)
{
    std::vector<ChordStep> steps;
    if (!playing_ || !waiting_)
    {
        return steps;
    }

    waiting_.reset();
    steps.push_back(Play(accent, moment, random));
    Wait(Flow(step_), moment, random, steps);
    return steps;
}

std::vector<ChordStep> ChordPlayer::Jump(int step, ChordMoment& moment, ChordRandom& random)
{
    std::vector<ChordStep> steps;
    if (!playing_)
    {
        return steps;
    }

    waiting_.reset();
    step_ = step + 1;
    for (ChordPart& part : parts_)
    {
        part.Start(step);
    }

    const int wait = Flow(step) & 7;
    if (wait < kNow)
    {
        waiting_ = wait;
    }
    else if (wait == kNow)
    {
        steps.push_back(Play(1.0f, moment, random));
    }

    return steps;
}

ChordStep ChordPlayer::Play(float accent, ChordMoment& moment, ChordRandom& random)
{
    // 0x8024da30. A level of 0 plays at full level without the accent. Bit 7 leaves the accent out too.
    const uint8_t level = Level(step_);
    float scale = 1.0f;
    if (level != 0)
    {
        scale = static_cast<float>(level & 0x7f) / 127.0f;
        if ((level & 0x80) == 0)
        {
            scale *= accent;
        }
    }

    ChordStep step{.step = step_, .level = scale * level_};
    if (level == kStopLevel || level == kKeepLevel)
    {
        step.action = level == kStopLevel ? ChordStep::Action::kStop : ChordStep::Action::kKeep;
        return step;
    }

    step.keys = StepChordParts(parts_, moment, random, keys_);
    return step;
}

void ChordPlayer::Wait(uint8_t flow, ChordMoment& moment, ChordRandom& random, std::vector<ChordStep>& steps)
{
    // 0x8024dc2c.
    if ((flow & kHold) != 0)
    {
        return;
    }

    const int wait = flow & 7;
    if (wait < kNow)
    {
        waiting_ = wait;
    }
    else if (wait == kNow)
    {
        // The step plays once more at once, before the move to the next. The player then holds.
        steps.push_back(Play(1.0f, moment, random));
    }
    else
    {
        playing_ = false;
        return;
    }

    const int repeat = flow >> 4;
    step_ = repeat != 0 ? repeat + 1 : step_ + 1;
}

void ChordRhythm::Note(uint8_t key, uint8_t velocity)
{
    // 0x802485d4 keeps the last velocity of each key until the frame.
    if (key == kAccentKey)
    {
        accent_mark_ = velocity;
        return;
    }

    for (std::size_t wait = 0; wait < kWaitKeys.size(); ++wait)
    {
        if (key == kWaitKeys[wait])
        {
            wait_marks_[wait] = velocity;
        }
    }
}

std::vector<ChordBeat> ChordRhythm::Frame()
{
    constexpr uint8_t kStrong = 100;
    constexpr float kWeakAccent = 0.7f;

    if (accent_mark_ && *accent_mark_ != 0)
    {
        accent_ = *accent_mark_ < kStrong ? 1.0f : 0.9f;
    }

    accent_mark_.reset();

    std::vector<ChordBeat> beats;
    for (const int wait : {3, 0, 1, 2})
    {
        std::optional<uint8_t>& mark = wait_marks_[static_cast<std::size_t>(wait)];
        if (mark && *mark != 0)
        {
            beats.push_back({.wait = wait, .accent = accent_});
            if (wait == 3)
            {
                timer_three_ = true;
            }

            if (wait == 0)
            {
                wait_zero_strong_ = *mark >= kStrong;
                accent_ = kWeakAccent;
            }
        }

        mark.reset();
    }

    return beats;
}

} // namespace ntdlib::jaudio
