// SPDX-License-Identifier: MIT

// A model of the build of Logik State's GBA sound driver that plays GBAMOD1 modules: the player of a module's rows and
// effects, and the mixer's count of each channel's place in its sample. Pinball Challenge Deluxe uses the build. The
// model
// keeps the driver's variables and steps them as the driver's code does, with its 32-bit and 16-bit wrapping and its
// bugs. It mixes no sound.
//
// The build differs from the GBAMOD2 builds that Player models. Its pitches index a table of steps, with no periods.
// It has 4 channels of music and 2 of sound effects. The play routine takes a range of orders. The range changes what
// a pattern's end does. Several effects work otherwise or do nothing.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/player.h"

namespace ntdlib::logikstate
{

// The mixer's channels of the GBAMOD1 build, and the mix buffer's samples. The driver mixes that many samples at each
// of the game's calls, whatever the rate.
constexpr int kMod1MixChannels = 6;
constexpr uint32_t kMod1FrameSamples = 352;

// Plays a song of a GBAMOD1 module as the game's driver does, a frame at a time.
class Mod1Player
{
public:
    // Keeps a reference to `rom`. The ROM has to outlive this. `music_volume` is the game's setting of the music's
    // volume, up to kFullVolume. The master volume and the sound effects' volume are full. The song sets the order to
    // start at, the range of orders and the scale of the slide to a note.
    Mod1Player(const Rom& rom, const Song& song, int32_t music_volume = kFullVolume);

    // Returns false if the song's module can't be read, isn't a GBAMOD1 module or hasn't the song's order.
    bool Valid() const;

    const Module& Header() const;

    // Sets a function that the player calls after each row, with true, and after each tick of effects, with false.
    void OnStep(std::function<void(bool)> callback);

    // Mixes a frame's samples. The driver's routine does the same at each of the game's calls.
    void Frame();

    // Returns the mixer's samples from the song's start to the last row or tick, or to the last frame's end if that
    // comes later. The count covers the samples at a frame's end that the driver leaves unmixed.
    uint64_t Elapsed() const;

    // The last row that the player ran: its order, its number in its pattern, its length in samples, and whether it
    // jumped (effect B).
    int RowOrder() const;
    int RowNumber() const;
    uint32_t RowSamples() const;
    bool RowJumped() const;

    // Returns what decides, besides the place in the module, how the module plays on: a row's length.
    std::vector<uint32_t> RepeatState() const;

    const std::array<Channel, kMod1MixChannels>& Channels() const;

    // Returns the volume that the mixer plays channel `number` at, with the record `channel`: the record's volume, less
    // the difference between kFullVolume and the music's volume for the module's channels, and not below 0.
    int32_t MixVolume(const Channel& channel, int number) const;

    // Returns true if the mixer plays channel `number` with the record `channel`: it has points, a length other than
    // kStoppedLength, and a volume above 0. A sample without a loop that has ended stays in the mixer with the length
    // 0, and plays the point at its loop's start as a constant.
    bool Sounding(const Channel& channel, int number) const;

    // Returns false once the player has met something that the driver can't play: an order that names a pattern that
    // the module doesn't have.
    bool Playing() const;

    // The player's variables and a channel's record for a trace, in the layout of the driver's memory.
    std::vector<uint8_t> Globals() const;
    std::vector<uint8_t> ChannelRecord(int channel) const;

    const std::vector<std::string>& Warnings() const;

private:
    // Mixes a frame's samples: rows and ticks where they fall due, and the samples up to the next of them.
    void Mix();
    // Reads the row at the player's place and moves the place on.
    void Row();
    // Reads a channel's cell of a row at `address`, or an empty cell where `address` is 0.
    void ReadRowCell(Channel& ch, uint32_t address);
    // Runs the row's effect of a channel: `effect` with `operand`.
    void RowEffect(Channel& ch, uint16_t effect, uint8_t operand);
    // Runs a tick of the effects of the module's channels.
    void Tick();
    // Returns the step of a channel: the table's at its pitch, its fine tune and half its vibrato, plus its slide.
    uint32_t Step(const Channel& ch) const;
    // Returns the word of the table of steps at `index`. The driver doesn't limit some of the indexes, and reads
    // what follows the table.
    uint32_t TableStep(int32_t index) const;
    // Moves a channel `samples` samples on, like the mixer.
    void Advance(Channel& ch, uint32_t samples);
    // Starts the points of `sample` on a channel, from 0, and notes the start for the conversion.
    void StartSample(Channel& ch, uint16_t sample);
    void Warn(const std::string& text);

    const Rom& rom_;
    Song song_;
    int32_t music_shortfall_ = 0; // kFullVolume less the music's volume
    Module module_;
    bool valid_ = false;
    bool playing_ = false;
    std::array<uint32_t, kSamples> points_ = {};
    std::function<void(bool)> on_step_;
    std::vector<std::string> warnings_;

    // The driver's variables.
    uint16_t order_ = 0;
    uint16_t place_ = 0; // the next row's first cell, in cells from the pattern's start
    uint16_t first_order_ = 0;
    uint16_t last_order_ = 0;
    uint32_t row_samples_ = 0;  // a row's length
    uint32_t row_left_ = 0;     // the samples up to the next row
    uint32_t tick_samples_ = 0; // a tick's length
    uint32_t tick_left_ = 0;    // the samples up to the next tick
    uint16_t tick_ = 0;         // the ticks since the row
    uint16_t arpeggio_tick_ = 0;
    uint8_t broke_ = 0; // a row's break has run
    std::array<Channel, kMod1MixChannels> channels_ = {};

    // What the driver doesn't keep.
    uint64_t frames_ = 0;
    uint64_t elapsed_ = 0;
    int row_order_ = 0;
    int row_number_ = 0;
    bool row_jumped_ = false;
};

} // namespace ntdlib::logikstate
