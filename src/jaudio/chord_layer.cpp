// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/chord_layer.h"

#include <string>
#include <utility>

#include "xsflib/binary.h"

namespace ntdlib::jaudio
{

namespace
{

constexpr uint32_t kAltoSax = 0x50042;
constexpr uint32_t kTrumpet = 0x50045;
constexpr uint32_t kElectricPianoT = 0x50063;
constexpr uint32_t kElectricPiano = 0x50062;
constexpr uint8_t kFirstKeyPort = 0xb;

// The start step of a record: the high nibble of byte 1 of its fourth word (0x8024c4c8).
int StartStep(const ChordDatabase::Record& record)
{
    return static_cast<int>((record.fourth >> 20) & 0xf);
}

const ChordDatabase::Record& RecordOf(const ChordDatabase& database, uint32_t record)
{
    if (record >= database.records.size() || database.records[record].entry >= database.entries.size())
    {
        throw xsflib::FormatError("chord record " + std::to_string(record) + " isn't in the chord database");
    }

    return database.records[record];
}

} // namespace

uint32_t ChordEffectAtStage(uint32_t sound, uint32_t stage)
{
    if (sound != kAltoSax && sound != kTrumpet)
    {
        return sound;
    }

    if (stage == 0x2c)
    {
        return kElectricPianoT;
    }

    if (stage == 0x35 || stage == 0x41)
    {
        return kElectricPiano;
    }

    return sound;
}

ChordLayer::ChordLayer(std::span<const uint8_t> data, ChordFile chords, uint32_t stage,
                       std::span<const uint32_t> records)
    : data_(data), database_(ReadChordDatabase(data)), chords_(std::move(chords))
{
    const xsflib::Reader r(data, true);
    for (const uint32_t record : records)
    {
        const ChordDatabase::Record& rec = RecordOf(database_, record);
        effects_.push_back({.record = record,
                            .player = ChordPlayer(data, database_.entries[rec.entry], StartStep(rec), 1.0f),
                            .sound = ChordEffectAtStage(rec.sound, stage),
                            .shared = rec.third});
        if (rec.third != 0 && !shared_keys_.contains(rec.third))
        {
            shared_keys_[rec.third] = r.U8(rec.third);
        }
    }
}

ChordMoment ChordLayer::MomentAt(std::optional<uint16_t> position, const Effect& effect)
{
    // 0x8024ace4 looks the position up in order 0 of the chord file. +0xce0 keeps the chord that a lookup last found.
    ChordMoment moment;
    const std::optional<uint8_t> index = position ? chords_.ChordAt(0, *position) : std::nullopt;
    if (index)
    {
        moment.chord = &chords_.chords[*index];
        last_root_ = moment.chord->root;
    }

    moment.fallback_key = last_root_;
    if (effect.shared != 0)
    {
        moment.shared_key = shared_keys_[effect.shared];
    }

    // 0x8024e048: the alto sax and the trumpet fit their keys to the chord, unless the last beat of wait 0 was strong
    // and the position hasn't moved since the effect's last step.
    const bool kept = rhythm_.WaitZeroStrong() && effect.last_step_position == position;
    moment.fit_to_shared = (effect.sound == kAltoSax || effect.sound == kTrumpet) && !kept;
    return moment;
}

void ChordLayer::Apply(std::size_t index, std::optional<uint16_t> position, ChordMoment& moment,
                       const std::vector<ChordStep>& steps, std::vector<ChordLayerSound>& sounds)
{
    Effect& effect = effects_[index];
    if (effect.shared != 0 && moment.shared_key)
    {
        shared_keys_[effect.shared] = *moment.shared_key;
    }

    for (const ChordStep& step : steps)
    {
        effect.last_step_position = position;
        if (step.action == ChordStep::Action::kKeep)
        {
            continue;
        }

        ChordLayerSound sound{.effect = index, .sound = effect.sound, .level = step.level};
        if (step.action == ChordStep::Action::kStop)
        {
            sound.action = ChordLayerSound::Action::kStop;
        }
        else
        {
            // 0x8024da30: a key of 0 or more goes to port 0xb + k of the effect (0x8024658c).
            for (std::size_t k = 0; k < step.keys.size(); ++k)
            {
                if (step.keys[k] >= 0 && kFirstKeyPort + k < 16)
                {
                    sound.ports[static_cast<uint8_t>(kFirstKeyPort + k)] = static_cast<uint16_t>(step.keys[k]);
                }
            }
        }

        sounds.push_back(std::move(sound));
    }

    // A player that waits for timer 3 waits in timer 0's list unless timer 3 has fired (0x802496e4).
    const std::optional<int> wait = effect.player.Waiting();
    effect.list = wait ? (*wait == 3 && !rhythm_.TimerThreeRunning() ? 0 : *wait) : -1;
}

std::vector<ChordLayerSound> ChordLayer::Frame(std::optional<uint16_t> position)
{
    std::vector<ChordLayerSound> sounds;
    if (position)
    {
        last_position_ = position;
    }

    // The objects' asks. startChordSe refuses a record until the chords are in.
    for (std::size_t i = 0; i < effects_.size(); ++i)
    {
        Effect& effect = effects_[i];
        if (effect.started || !last_position_)
        {
            continue;
        }

        effect.started = true;
        ChordMoment moment = MomentAt(last_position_, effect);
        const std::vector<ChordStep> steps = effect.player.Start(moment, random_);
        Apply(i, last_position_, moment, steps, sounds);
    }

    // The rhythm's frame process: each beat plays the next step of the players in its list.
    for (const ChordBeat& beat : rhythm_.Frame())
    {
        for (std::size_t i = 0; i < effects_.size(); ++i)
        {
            Effect& effect = effects_[i];
            if (!effect.started || effect.list != beat.wait)
            {
                continue;
            }

            ChordMoment moment = MomentAt(last_position_, effect);
            const std::vector<ChordStep> steps = effect.player.Event(beat.accent, moment, random_);
            Apply(i, last_position_, moment, steps, sounds);
        }
    }

    return sounds;
}

} // namespace ntdlib::jaudio
