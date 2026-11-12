// SPDX-License-Identifier: MIT

// A model of Logik State's GBA sound driver: the player of a module's rows and effects, and the mixer's count of each
// channel's place in its sample. The model keeps the driver's variables and steps them as the driver's code does. It
// mixes no sound.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"

namespace ntdlib::logikstate
{

// The Amiga's clock that the driver divides by twice a period for a sample's rate in hertz.
constexpr int32_t kPeriodClock = 0x6C3E1D;

// The channels whose effects the player runs between rows. A module's other channels play their rows alone.
constexpr int kEffectChannels = 4;

// The speed and the tempo that a module starts with: the ticks of a row, and the beats a minute at 4 rows a beat and
// 6 ticks a row.
constexpr uint32_t kStartSpeed = 6;
constexpr uint32_t kStartTempo = 125;

// The row of the pattern loop's marker when no pattern loop is set.
constexpr uint16_t kNoLoopRow = 0xFFFF;

// The length that marks a channel as stopped. The mixer plays a channel only above it.
constexpr uint16_t kStoppedLength = 2;

// The mask that the mixer applies to a channel's place at its sample's end. The mixer's code was written to keep the
// place's fraction, with the mask 0xFFF. The driver runs a copy of the code in RAM, and the copy leaves the mask
// behind: it reads the word 8 bytes before the copy, where the games checked keep 0. The loop therefore starts from
// its first point exactly.
constexpr uint32_t kEndPlaceMask = 0;

// A channel of the mixer: the driver's record, at the record's offsets, and what the conversion follows of a note.
struct Channel
{
    uint32_t points = 0;           // 0x00: the address of the point that the position counts from, or 0
    uint32_t position = 0;         // 0x04: in 4096ths of a point
    uint32_t step = 0;             // 0x08: what a sample adds to the position
    uint16_t length = 0;           // 0x0C: the points up to the end, and after it the loop's
    uint16_t loop_start = 0;       // 0x0E: what the end adds to `points`, once
    uint16_t loop_length = 0;      // 0x10
    int32_t volume = 0;            // 0x14
    uint16_t sample = 0;           // 0x18: from 0
    uint16_t fine_tune = 0;        // 0x1C
    uint16_t pitch = 0;            // 0x1E
    int32_t effect = 0;            // 0x20: the row's effect for the ticks, or -1
    uint16_t volume_up = 0;        // 0x24
    uint16_t volume_down = 0;      // 0x26
    uint16_t arpeggio_1 = 0;       // 0x28: in pitches
    uint16_t arpeggio_2 = 0;       // 0x2A
    int16_t vibrato = 0;           // 0x2C: what the vibrato adds to the period
    uint16_t vibrato_depth = 0;    // 0x2E
    uint16_t vibrato_speed = 0;    // 0x30
    uint16_t vibrato_position = 0; // 0x32
    int16_t slide = 0;             // 0x34: what the slides have added to the period
    uint16_t slide_speed = 0;      // 0x36
    int16_t slide_step = 0;        // 0x38: what a tick adds to `slide`
    int16_t slide_target = 0;      // 0x3A: the pitch that a slide to a note ends at
    // The pattern loop of the build with long records, at 0x50 and 0x52 of its record: the place of the first cell of
    // the row that the loop goes back to, and the passes left.
    uint16_t loop_row = 0;
    uint16_t loop_count = 0;

    // What the driver doesn't keep. `starts` counts the notes and the changes of sample without a note. The other four
    // keep the sample as the last of those left it: the address of its first point, and its length and loop.
    uint32_t starts = 0;
    uint32_t first_point = 0;
    uint16_t whole_length = 0;
    uint16_t whole_loop_start = 0;
    uint16_t whole_loop_length = 0;
};

// The size of a channel's record in the driver's memory, and of the part that ChannelRecord() returns.
constexpr uint32_t kChannelSize = 0x4C;
constexpr uint32_t kChannelRecordSize = 0x3C;

// Plays a song as the game's driver does, a frame at a time.
class Player
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this. `music_volume` is the game's setting of the music's
    // volume, up to kFullVolume. The master volume and the sound effects' volume are full. `build` is the game's build
    // of the driver.
    Player(const Rom& rom, const Song& song, int32_t music_volume = kFullVolume, Build build = Build::kShortRecords);

    // Returns false if the song's module can't be read or its order isn't in the module.
    bool Valid() const;

    const Module& Header() const;

    // Sets a function that the player calls after each row, with true, and after each tick of effects, with false.
    void OnStep(std::function<void(bool)> callback);

    // Mixes a frame's samples. The driver's routine does the same at each of the game's calls.
    void Frame();

    // Returns the mixer's samples from the song's start to the last row or tick, or to the last frame's end if that
    // comes later. The count covers the samples at a frame's end that the driver leaves unmixed.
    uint64_t Elapsed() const;

    // The last row that the player ran: its order, its number in its pattern, and the row's length in samples.
    int RowOrder() const;
    int RowNumber() const;
    uint32_t RowSamples() const;

    uint32_t Speed() const;
    uint32_t Tempo() const;

    // Returns the pattern loop's marker in the build with short records.
    uint16_t LoopRow() const;

    // Returns the state of the pattern loops: the module's marker in the build with short records, and each channel's
    // row and count in the build with long records.
    std::vector<uint16_t> LoopState() const;

    // Returns what decides, besides the place in the module, how the module plays on: the speed, the tempo and the
    // state of the pattern loops.
    std::vector<uint32_t> RepeatState() const;

    const std::array<Channel, kMixChannels>& Channels() const;

    // Returns the volume that the mixer plays channel `number` at, with the record `channel`: the record's volume,
    // less the difference between kFullVolume and the music's volume for the music's channels, and not below 0. The
    // music's channels are the first kMusicChannels in the build with short records, and the module's in the build with
    // long records.
    int32_t MixVolume(const Channel& channel, int number) const;

    // Returns true if the mixer plays channel `number` with the record `channel`: it has a sample that hasn't stopped,
    // and in the build with short records a volume above 0.
    bool Sounding(const Channel& channel, int number) const;

    // Returns false once the player has met something that the driver can't play, such as a row without length.
    bool Playing() const;

    // The player's variables and a channel's record for a trace, in the layout of the memory of the build with short
    // records. In the build with long records, a channel's record ends with its pattern loop's row and count.
    std::vector<uint8_t> Globals() const;
    std::vector<uint8_t> ChannelRecord(int channel) const;

    const std::vector<std::string>& Warnings() const;

private:
    // Reads the row at the player's place and moves the place on.
    void Row();
    // Reads a channel's cell of a row. An `empty` row plays as cells without a note or an effect.
    void ReadRowCell(Channel& ch, uint32_t address, bool empty);
    // Runs the row's effect of a channel: `effect` with `operand`.
    void RowEffect(Channel& ch, uint16_t effect, uint8_t operand);
    // Runs the pattern loop effect of the build with short records, with the operand's low 4 bits in `count`.
    void PatternLoop(int count);
    // Runs the pattern loop effect of the build with long records on a channel.
    void ChannelLoop(Channel& ch, int count);
    // Sets the speed or the tempo, and the row's length from both.
    void SetSpeed(uint8_t operand);
    // Runs a tick of the effects of the first kEffectChannels channels.
    void Tick();
    void Arpeggio(Channel& ch);
    void SlideToNote(Channel& ch);
    void Vibrato(Channel& ch);
    void VolumeSlide(Channel& ch);
    // Returns the step of a channel at its pitch: the table's, or the period's step once a slide, a vibrato or a fine
    // tune moves it.
    uint32_t Step(const Channel& ch) const;
    // Returns the step for the period of `pitch`, a place in the table of periods, plus `offset`.
    uint32_t PeriodStep(uint32_t pitch, int32_t offset) const;
    // Moves a channel `samples` samples on, like the mixer.
    void Advance(Channel& ch, uint32_t samples);
    // Starts sample `sample`, from 0, on a channel: its points, and with `whole` its length and loop.
    void SetSample(Channel& ch, uint16_t sample, bool whole);
    void Warn(const std::string& text);

    const Rom& rom_;
    Song song_;
    int32_t music_shortfall_ = 0; // kFullVolume less the music's volume
    Build build_ = Build::kShortRecords;
    Module module_;
    bool valid_ = false;
    bool playing_ = false;
    std::array<uint32_t, kSamples> points_ = {};
    std::function<void(bool)> on_step_;
    std::vector<std::string> warnings_;

    // The driver's variables.
    uint32_t speed_ = kStartSpeed;
    uint32_t tempo_ = kStartTempo;
    uint32_t tick_rate_ = 50; // ticks a second at the tempo
    uint16_t order_ = 0;
    uint16_t place_ = 0;        // the next row's first cell, in cells from the pattern's start
    uint32_t row_samples_ = 0;  // a row's length
    uint32_t row_left_ = 0;     // the samples up to the next row
    uint32_t tick_samples_ = 0; // a tick's length
    uint32_t tick_left_ = 0;    // the samples up to the next tick
    uint16_t tick_ = 0;         // the ticks since the row
    uint16_t arpeggio_tick_ = 0;
    uint8_t broke_ = 0; // a row's break has run
    uint16_t loop_row_ = kNoLoopRow;
    std::array<Channel, kMixChannels> channels_ = {};

    // What the driver doesn't keep.
    uint64_t frames_ = 0;
    uint64_t elapsed_ = 0;
    int row_order_ = 0;
    int row_number_ = 0;
};

} // namespace ntdlib::logikstate
