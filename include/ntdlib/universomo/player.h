// SPDX-License-Identifier: MIT

// A model of Universomo's sequencer on the DS, from Beat City's code (USA, NTR-BEYE, 0x0202CA44 to 0x0202DC30). The
// sequencer plays a song of a tracker format (formats.h) on the sound unit's channels, one track to a channel, through
// the SDK's direct channel commands. The game calls its update once a frame: each call is one tick.
//
// The model gives the sound commands that the driver sends for one song, a list for each call of the update. They are
// the commands that the game's code sends in the same order, with the same values: the set-up of a channel for a note,
// the start of its timer, a change of its volume and a change of its timer.
//
// How a song plays:
//
// - A row is `speed` ticks. On the first tick of a row the update reads the row's events, then counts the row. After
//   the last row of a pattern it goes to the next order. After the last order a looped song goes to the restart order,
//   with the channels' state kept. A song that doesn't loop stops on the tick that reads its last row: that row never
//   sounds, and every channel goes to volume 0.
// - A note's pitch is (relative note + note - 48) * 64 + finetune / 2, in 64ths of a semitone. Pitch 0 plays a sample
//   at 16,756,991 / 2003 Hz, about 8366 Hz (SND_CalcTimer of 0x7D3).
// - A channel's volume runs from 0 to 4095. The hardware value is x = volume * song volume / 256, sent as x / 2 with
//   the divider of 16, x / 8 with 4, x / 16 with 2 or x / 32 with 1. The level is therefore x / 4096, in steps.
// - Even channels pan to 74 and odd ones to 54, of 0 to 127.
// - No note ends by itself. A track's sound ends with its sample, at a volume of 0, or at its next note.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/universomo/formats.h"

namespace ntdlib::universomo
{

// The period of a sample's timer at pitch 0.
constexpr int kBaseTimer = 0x7D3;

// The pans of the even and the odd hardware channels.
constexpr uint8_t kEvenPan = 74;
constexpr uint8_t kOddPan = 54;

// The volume of a song that the game starts: 100 of 100. The game's code turns it into 255 of 256.
constexpr int kFullSongVolume = 255;

// A sound command that the driver sends to the sound unit.
struct Command
{
    enum class Kind : uint8_t
    {
        kSetup,  // SND_SetupChannelPcm
        kStart,  // SND_StartTimer for the channel
        kVolume, // SND_SetChannelVolume
        kTimer,  // SND_SetChannelTimer
    };

    bool operator==(const Command&) const = default;

    Kind kind = Kind::kSetup;
    uint8_t channel = 0;

    // kSetup: the format (0 for 8-bit PCM, 1 for 16-bit), the loop mode (1 repeats from `loop_start`, 2 plays once),
    // and the loop's start and length, or the length of the data to play, in words of 4 bytes.
    uint8_t format = 0;
    uint8_t loop_mode = 0;
    uint32_t loop_start = 0;
    uint32_t length = 0;

    // kSetup and kVolume: the volume (0 to 127) and the divider (0 to 3 for 1, 2, 4 and 16).
    uint8_t volume = 0;
    uint8_t divider = 0;

    // kSetup and kTimer: the timer's period.
    uint16_t timer = 0;

    // kSetup: the pan, the sample's number in the bank and the offset in bytes into its data where the note starts.
    uint8_t pan = 0;
    uint8_t sample = 0;
    uint32_t offset = 0;
};

// The hardware volume and divider of a channel at `volume` (0 to 4095) in a song at `song_volume` (0 to 255), as
// 0x0202CA44 gives them.
struct HardwareVolume
{
    uint8_t volume = 0;
    uint8_t divider = 0;
};

HardwareVolume HardwareVolumeOf(int volume, int song_volume);

// The level of a channel at `volume` and `divider` against full level: volume / 128 over the divider's 1, 2, 4 or 16.
double LevelOf(uint8_t volume, uint8_t divider);

// Plays one song. The song and the bank have to outlive the player.
class Player
{
public:
    // A song that `loop` is false for stops after its last row. `first_channel` is the hardware channel of track 0.
    // Throws xsflib::FormatError for a song that the sequencer can't play: no orders, more tracks than channels, or an
    // instrument that names a missing sample.
    Player(const Song& song, const SampleBank& bank, bool loop = true, int song_volume = kFullSongVolume,
           int first_channel = 0);

    // One call of the update: one tick. Returns the commands that it sends, in their order. Throws xsflib::FormatError
    // for an event that names a missing instrument.
    const std::vector<Command>& Update();

    // The commands of the last update.
    const std::vector<Command>& Commands() const;

    bool Playing() const;
    uint32_t Order() const;
    uint32_t Row() const;
    uint32_t Tick() const;
    uint32_t Speed() const;

    // The number of times the song has gone back to its restart order.
    uint32_t Wraps() const;

    // The number of updates so far.
    uint64_t Updates() const;

private:
    // A track's channel record (the song's +0x94, 72 bytes each).
    struct Channel
    {
        int instrument = 0;
        int pitch = 0;
        int slide_start = 0;
        int slide_end = 0;
        int volume = 0;
        int target = 0; // the pending note's pitch, or the last note's
        int vibrato_position = 0;
        int vibrato_speed = 0;
        int vibrato_depth = 0;
        std::array<int, 2> effects = {};
        int volume_slide = 0;
        int parameter = 0;
        uint32_t offset = 0;
        int delay = 0;
        bool pending = false;
        bool no_retrigger = false;
        uint8_t event_volume = 0;
        uint8_t event_instrument = 0;
    };

    const Pattern& CurrentPattern() const;
    const Instrument& InstrumentAt(int index) const;
    uint8_t ChannelOf(std::size_t track) const;
    void EnterPattern();
    void SendVolume(std::size_t track);
    void SetEffect(std::size_t track, int effect, int parameter, int slot);
    void VolumeColumn(std::size_t track, int value);
    void RunEvent(std::size_t track, const Event& event);
    void RunRow();
    void StartNote(std::size_t track);
    void StepEffects(std::size_t track);

    const Song& song_;
    const SampleBank& bank_;
    bool loop_ = true;
    int song_volume_ = kFullSongVolume;
    int first_channel_ = 0;

    uint32_t speed_ = 0;
    uint32_t tick_ = 0;
    uint32_t row_ = 0;
    uint32_t order_ = 0;
    bool first_ = true;
    bool playing_ = true;
    uint32_t wraps_ = 0;
    uint64_t updates_ = 0;
    std::vector<Channel> channels_;
    std::vector<uint8_t> waits_;          // each track's rows to its next event
    std::vector<std::size_t> next_event_; // each track's next event in the pattern
    std::vector<Command> commands_;
};

} // namespace ntdlib::universomo
