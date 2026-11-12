// SPDX-License-Identifier: MIT

// A model of the players of Logik State's later GBA sound drivers. They play GBAMOD30 modules. The model covers the
// rows and effects of a module, and the mixer's count of each channel's place in its sample. Eight groups of the
// drivers' code differ in details of the player and the mixer. The model takes the details as data (Mod30Build) and
// keeps the drivers' variables as their code does. It mixes no sound.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ntdlib/logikstate/mod30.h"

namespace ntdlib::logikstate
{

// The groups of the drivers' code that play GBAMOD30 modules.
enum class Mod30Group
{
    kA,      // GBAModPlay 2002 with 10 channel records of 0x80 bytes, as in Super Puzzle Fighter II
    kB,      // GBAModPlay 2002 with 8 records of 0x90 bytes, as in Stuntman
    kC,      // GBAModPlay 2003 with layout C, as in Medal of Honor: Infiltrator
    kCPrime, // group C's player with LS_Play's start-up and mixers, as in Asterix & Obelix XXL
    kD,      // GBAModPlay 2003 with layout B and LS_Play's player, as in Knights' Kingdom
    kE1,     // LS_Play 2003 with headers of 12 bytes in the bank
    kE2,     // LS_Play 2003
    kE3,     // LS_Play 2003 with 8 channels
};

// How a build lays out a channel's record.
enum class Mod30RecordLayout
{
    kA, // 0x80 bytes
    kB, // 0x90 bytes
    kC, // 0x94 bytes, groups C, C' and D
    kE, // 0x98 bytes
};

// The frame loop of a build: the mixes of a frame between the rows and the ticks.
enum class Mod30FrameLoop
{
    // Each pass counts as one sample more than it mixes, and the frame ends a few samples short.
    kShort,
    // As kShort, with group C''s list of channels for mixer mode 1.
    kShortList,
    // Each pass counts what it mixes.
    kFull,
};

// What a tick does to a channel's volume.
enum class Mod30VolumeSlide
{
    kOnce,       // the operand once a tick after the first, while either operand isn't 0; the clamp at those ticks
    kTwice,      // as kOnce with twice the operand
    kOnceClamp,  // the operand once a tick after the first, and the clamp at every tick but the first
    kOnceAlways, // the operand once a tick after the first, and the clamp at every tick
};

// The vibrato and the slide to a note in linear mode.
enum class Mod30LinearForm
{
    kAmigaOnly,   // only the Amiga form runs, and it moves nothing that the linear step uses
    kOwnPeriod,   // the linear period has a field of its own
    kNoteField,   // the note's field holds the linear period
    kSlideOffset, // as kNoteField, and the slide to a note moves the linear slide's offset
};

// Where a tick applies the volume column.
enum class Mod30TickColumn
{
    kRecordFour,  // once, to the record after the first four
    kAfterModule, // once, to the record after the module's last channel
    kEvery,       // to every channel of the module
};

// The details of a build of the drivers that change what the music plays.
struct Mod30Build
{
    Mod30Group group = Mod30Group::kE2;
    Mod30RecordLayout record_layout = Mod30RecordLayout::kE;
    int records = 12;          // the channel records
    uint32_t channel_cap = 12; // the most channels of a module that the load takes, or 0 for no cap
    int mix_cap = 0;           // the most channels that a frame mixes, or 0 for the module's
    bool loop_flag = true;     // the game can set a flag that loops a module at its last order
    bool play_request = true;  // the play routine leaves the module for the next frame to start
    Mod30FrameLoop frame_loop = Mod30FrameLoop::kFull;
    bool tick_with_row = true;         // a tick that falls due with a row runs
    bool speed_sets_tick = true;       // effect F sets the tick's length too
    bool short_lengths = false;        // a note's lengths are 4096ths of a point less one
    bool start_clears_records = false; // the start clears whole records, not only the volume and the lengths
    bool tick_all_channels = true;     // every channel of the module gets the tick's effects, not only the first four
    Mod30TickColumn tick_column = Mod30TickColumn::kEvery;
    Mod30VolumeSlide volume_slide = Mod30VolumeSlide::kOnceAlways;
    int32_t plain_slide_scale = 4; // what a plain linear slide moves the period by at a tick, times its operand
    Mod30LinearForm vibrato = Mod30LinearForm::kNoteField;
    Mod30LinearForm slide_to_note = Mod30LinearForm::kSlideOffset;
    bool vibrato_nibbles = true;   // effect 4 sets each of its nibbles that isn't 0, and restarts the vibrato
    bool keep_slide_on_3 = true;   // a note with effect 3 keeps the linear slide's offset
    bool linear_arpeggio = true;   // the arpeggio's tick in linear mode reads the steps' table at the linear period
    bool scaled_zero_waits = true; // in mixer mode 0, a channel whose scaled volume is 0 isn't mixed, and waits
    bool mode1_volume_zero = true; // mixer mode 1 sets the volume of a channel of length 1 or less to 0
};

// Returns the details of a group.
const Mod30Build& BuildOf(Mod30Group group);

// What a game gives the driver: the details of its build, its bank, its mixer's rate and mode, and its three volumes.
struct Mod30Settings
{
    Mod30Build build;
    uint32_t bank = 0;
    uint32_t header_size = kMod30ShortHeader;
    int rate_index = 0;    // 0 to 8, an index into kMod30Rates
    int mixer_mode = 0;    // 0, or 1 for the mixer of all channels in one pass
    int sfx_channels = 0;  // the sound effects' channels that the game asks for at the init
    int32_t master = 0x40; // the three volumes, each in 64ths
    int32_t music = 0x40;
    int32_t sfx = 0x40;
    bool loop = true;     // the loop flag
    uint32_t periods = 0; // the table of Amiga periods, a halfword a pitch
    uint32_t steps = 0;   // the steps' table of the rate, a word a pitch
    uint32_t linear = 0;  // the table of linear frequencies, a halfword for each 768th of an octave
    // The FIFOs that play the mix: 1 where the game starts the output with FIFO A alone (SOUNDCNT_H 0x0B0E), else 2.
    int fifos = 2;
    // A call from the game's code that switches the sound off (SOUNDCNT_X = 0) without a new init after it, or 0. In
    // mGBA 0.11.0 the FIFOs then play at half volume.
    uint32_t sound_off_call = 0;
};

// A song of a game: its module, and the loop flag that the game plays it with.
struct Mod30Song
{
    uint32_t module = 0;
    bool loop = true;
};

// Sets `settings` to what the game's code gives the driver, for the games in the table, and `songs` to the
// modules in the order of the game's songs, then the modules that the game's songs don't name, in their order in the
// ROM. `bank` is the bank that follows the game's last module. `modules` are the game's modules. Returns false for
// another game. The settings' loop flag is the one that most of the game's songs play with. Each song has the flag
// that the game plays it with.
bool GameSettings(const Rom& rom, const Mod30Bank& bank, const std::vector<uint32_t>& modules, Mod30Settings& settings,
                  std::vector<Mod30Song>& songs);

// The mixer's rates in hertz and the samples that it mixes a frame, by rate index.
constexpr std::array<uint32_t, 9> kMod30Rates = {10512, 13379, 15282, 18157, 21024, 26758, 31536, 36314, 44100};
constexpr std::array<uint32_t, 9> kMod30FrameSamples = {176, 224, 256, 304, 352, 446, 526, 606, 606};

// A channel of the mixer: the driver's record, and what the conversion follows of a note.
struct Mod30Channel
{
    uint32_t points = 0;   // the address of the point that the position counts from, or 0
    uint32_t position = 0; // in 4096ths of a point
    uint32_t step = 0;     // what a sample adds to the position
    uint32_t length = 0;   // in 4096ths of a point, up to the end, and after it the loop's
    uint32_t loop_start = 0;
    uint32_t loop_length = 0;
    int32_t volume = 0;
    int16_t relative_note = 0;
    uint16_t sample = 0; // from 0
    int16_t fine_tune = 0;
    uint16_t note = 0;  // the note, or in some builds the linear period after a note
    int32_t effect = 0; // the row's effect for the ticks, or -1
    uint16_t volume_up = 0;
    uint16_t volume_down = 0;
    uint16_t arpeggio_1 = 0;
    uint16_t arpeggio_2 = 0;
    int16_t vibrato = 0; // what the vibrato adds to the Amiga period
    uint16_t vibrato_depth = 0;
    uint16_t vibrato_speed = 0;
    uint16_t vibrato_position = 0;
    int16_t slide = 0; // what the slides have added to the Amiga period
    uint16_t slide_speed = 0;
    int16_t slide_step = 0;
    int16_t target = 0; // the note that a slide to a note ends at
    uint16_t loop_row = 0;
    uint16_t loop_count = 0;
    uint16_t packed = 0;
    int32_t linear_slide = 0;
    int32_t linear_vibrato = 0;
    uint32_t linear_slide_operand = 0;
    uint32_t volume_column = 0;
    uint32_t linear_period = 0; // in group B only
    uint32_t linear_target = 0;

    // What the driver doesn't keep: the notes and the changes of sample without a note, the sample as the last of them
    // left it (its first point, and its length and loop in points as the last note set them), and whether the channel
    // has reached the loop.
    uint32_t starts = 0;
    uint32_t first_point = 0;
    uint32_t whole_length = 0;
    uint32_t whole_loop_start = 0;
    uint32_t whole_loop_length = 0;
    bool looped = false;
};

// The most channel records of a build.
constexpr int kMod30Records = 12;

// The note of a module that a sample without a relative note or a fine tune plays at its rate in the bank: XM's C-4.
constexpr uint16_t kMod30RootNote = 49;

// Plays a module as a game's driver does, a frame at a time.
class Mod30Player
{
public:
    // Keeps a reference to `rom`. `rom` has to outlive the player. Plays the module at `module` from its first order.
    Mod30Player(const Rom& rom, uint32_t module, const Mod30Settings& settings);

    // Returns false if the module or the bank can't be read.
    bool Valid() const;

    const Mod30Module& Header() const;
    const Mod30Bank& Bank() const;

    // Sets a function that the player calls after each row, with true, and after each tick, with false.
    void OnStep(std::function<void(bool)> callback);

    // Runs a frame of the game's calls of the driver.
    void Frame();

    // Returns the mixer's samples from the start to the last row or tick, or to the last frame's end if that comes
    // later.
    uint64_t Elapsed() const;

    // Returns false once the driver has stopped the module, or has met something that it can't play.
    bool Playing() const;

    int RowOrder() const;
    int RowNumber() const;
    uint32_t Speed() const;
    uint32_t Tempo() const;
    uint32_t RowSamples() const;
    uint32_t TickSamples() const;
    uint32_t Rate() const;

    // The channels of the module after the load's cap, and the records.
    int Channels() const;
    const std::array<Mod30Channel, kMod30Records>& Records() const;

    // Returns the volume that the mixer plays a music channel at: the record's, scaled by the three volumes.
    int32_t MixVolume(const Mod30Channel& channel) const;

    // Returns true if the mixer plays music channel `number` with the record `channel`: the frame mixes the channel,
    // and it has a sample that hasn't stopped. In the builds whose mixer of mode 0 leaves a channel at volume 0 alone,
    // the volume is above 0 too.
    bool Sounding(const Mod30Channel& channel, int number) const;

    // Returns the rate in hertz at which a channel plays the bank's sample `sample` at the module's note
    // kMod30RootNote, with the sample's relative note and fine tune.
    double RootRate(int sample) const;

    // Returns what decides, besides the place in the module, how the module plays on: the speed, the tempo, each
    // channel's pattern loop and the place of each of its streams.
    std::vector<uint32_t> RepeatState() const;

    // Returns the count of orders after which the orders' patterns repeat, where the module loops. Two orders that
    // count apart play on alike. A module that doesn't loop gets its count of orders. In some modules every order plays
    // the same pattern.
    uint32_t RepeatOrders() const;

    // The player's variables, in the order of the trace: the order, the row, the speed, the tempo, the ticks a second,
    // a row's and a tick's lengths and the samples left of each, the ticks of the row twice, the break's flag, the
    // playing flag and the fade.
    std::vector<uint8_t> Globals() const;

    // Returns a channel's record in the build's layout, with the fields that the model keeps and zeros elsewhere.
    std::vector<uint8_t> Record(int channel) const;

    const std::vector<std::string>& Warnings() const;

private:
    // Returns the module's channels, up to the build's cap.
    int LoadedChannels() const;
    // Returns the address of the first block of the pattern that order `order` plays.
    uint32_t PatternBlock(uint32_t order) const;
    // The play routine's start of the module, at order 0.
    void Start();
    // Mixes a frame of samples, as the mix loop does.
    void MixFrame();
    // Mixes `samples` samples of the channels from `first` to `end`, as mixer mode 0 does.
    void MixChannels(int first, int end, uint32_t samples);
    // Runs a row and a tick where they fall due, and returns the samples up to the next of them, at most to the frame's
    // end. `done` counts the frame's samples so far.
    int32_t RowAndTick(int32_t done);
    // Builds mixer mode 1's list for a pass of `samples` samples, counts its entries in `listed`, and returns the
    // samples that the pass mixes.
    int32_t ListChannels(int end, int32_t samples, int& listed);
    // Advances the channels of a pass of mixer mode 1.
    void AdvanceListed(int end, int32_t samples);
    // Moves a channel `samples` samples on, as the mixer of mode 0 does.
    void Advance(Mod30Channel& ch, uint32_t samples);
    void Row();
    // Reads the next cell of channel `c`. Row 0 starts the streams from the order's pattern.
    void Decode(int c, uint32_t& block);
    void PlayCell(Mod30Channel& ch, int c);
    // Runs a row's effect and the volume column. Returns after the column.
    void RowEffect(Mod30Channel& ch, uint8_t effect, uint8_t operand);
    void SetSpeed(uint8_t operand);
    void Tick();
    void TickEffect(Mod30Channel& ch);
    void LinearSlide(Mod30Channel& ch, bool up, uint32_t operand);
    void SlideToNote(Mod30Channel& ch);
    void Vibrato(Mod30Channel& ch);
    void VolumeSlide(Mod30Channel& ch);
    void VolumeColumn(Mod30Channel& ch);
    // The linear period of a note on a channel, with its sample's relative note and fine tune.
    uint32_t LinearPeriod(const Mod30Channel& ch, uint32_t note) const;
    // The step of a linear period.
    uint32_t LinearStep(int32_t period) const;
    // Recomputes a channel's step from its linear period and offsets.
    void Recompute(Mod30Channel& ch);
    // The step of an Amiga period.
    uint32_t PeriodStep(int32_t period) const;
    uint32_t AmigaStep(const Mod30Channel& ch) const;
    uint16_t Period(uint32_t pitch) const;
    uint32_t TableStep(uint32_t pitch) const;
    void Warn(const std::string& text);

    // A stream of runs: where it reads, the rows left of its run, and the run's value.
    struct Stream
    {
        uint32_t address = 0;
        int32_t count = 0;
        uint32_t value = 0;
    };

    struct Decoder
    {
        std::array<Stream, 3> streams;
        uint16_t note = kMod30NoNote;
        uint8_t sample = 0;
        uint8_t volume = 0;
        uint8_t effect = 0;
        uint8_t operand = 0;
    };

    const Rom& rom_;
    Mod30Settings settings_;
    Mod30Build build_;
    Mod30Module module_;
    Mod30Bank bank_;
    bool valid_ = false;
    bool started_ = false;
    std::function<void(bool)> on_step_;
    std::vector<std::string> warnings_;

    // The driver's variables.
    uint32_t rate_ = 0;
    uint32_t frame_samples_ = 0;
    int channels_ = 0;
    int end_channel_ = 0;
    uint32_t speed_ = 0;
    uint32_t tempo_ = 0;
    uint32_t hz_ = 0;
    uint32_t order_ = 0;
    uint32_t row_ = 0;
    int32_t row_samples_ = 0;
    int32_t row_left_ = 0;
    int32_t tick_samples_ = 0;
    int32_t tick_left_ = 0;
    uint16_t tick_ = 0;
    uint16_t arpeggio_tick_ = 0;
    uint8_t broke_ = 0;
    uint16_t playing_ = 0;
    uint32_t fade_ = 0;
    std::array<Mod30Channel, kMod30Records> records_ = {};
    std::array<Decoder, kMod30Records> decoders_ = {};

    // What the driver doesn't keep.
    uint64_t frames_ = 0;
    uint64_t elapsed_ = 0;
    int row_order_ = 0;
    int row_number_ = 0;
};

} // namespace ntdlib::logikstate
