// SPDX-License-Identifier: MIT

// A model of the Konami GBA driver's output stage and mixer. Output plays a song with a Sequencer. Each frame, it turns
// the sequencer's track outputs into stores to the PSG's registers and into the DirectSound voice records. The driver's
// output stage does the same. Mix() then runs the driver's mixer over the voices for the frame's blocks of 16 samples,
// and gives the bytes that go to the FIFOs. Each revision and build of the driver has its rules, and the games' code is
// the authority for each of them. The Dungeon Dice Monsters revision has no mixer: Mix() runs each FIFO on its timer,
// with the DMA interrupt's routine that fills the FIFO's next 16 bytes.
//
// The model plays songs alone. A game's sound effect tracks can take over voices and change the buses and the mixer's
// mask. The model leaves them out, and with them the game's calls that change the mixer's rate.
//
// In the enhanced mixing mode (StartEnhancedMixing), the console's mixer goes on as before: its voices' progress
// through their samples and the ends of their notes stay the console's. Mix() also gives agb::kEnhancedFrameSamples
// samples a frame in floating point. The mode keeps the console's samples, steps, levels and echo, and lifts the limits
// of the mixer's arithmetic:
//
// - The samples lie on the scale of the console's 8-bit bytes. Nothing wraps, saturates or rounds a sum. A voice's
//   level on a FIFO follows the build's scale without its rounding: the product over 128 or over 16, or in a build
//   with a table of levels, the table's span at the level over 255.
// - A sample's position moves on at the console's step for the time that the frame takes: the step times the frame's
//   cycles over the cycles of a console's point. The console's points fall in whole blocks, and a frame's blocks can
//   differ. A voice's samples lie at even shares of the frame's move. The band-limited interpolator
//   (wave::SincKernel) takes the sample's points around each position. A looped sample's points repeat its loop. Before
//   a sample's start and past its end, the points are 0. A coded sample's points are those that the console's decoder
//   gives on a first pass, and a pass of its loop.
// - A voice's level on each FIFO ramps across the frame from its value in the last frame: sample i of n takes
//   from + i * ((to - from) / n). A note that starts takes its levels at once.
// - An echo bus keeps its feedback and its delay in the console's blocks, in the time that those blocks take. Its line
//   holds floating-point samples. A bus's input is the console's choice for each voice. A value in a line below 2^-16
//   becomes 0.
// - A note's samples start in the frame that starts the note, and end with its voice.
//
// Each operation takes its operands in a fixed order, and every platform gives the same samples.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/sequencer.h"

namespace ntdlib::konami
{

// What the driver keeps in a DirectSound voice's record. A build's records hold only some of the fields.
struct OutputVoice
{
    uint32_t position = 0;   // the address of the sample's next byte
    int32_t count = 0;       // the samples left before the sample's end
    uint16_t step = 0;       // the step per mixer sample, in 1/4096 of a sample
    uint16_t fraction = 0;   // the position between two samples, in 1/4096 of a sample
    uint16_t key = 0;        // the sample number
    uint8_t flags = 0;       // kVoice* bits
    uint8_t volume = 0;      // the track's volume, or the level of both sides in the ring mixer's revisions
    uint8_t right = 0;       // the level of FIFO A's side
    uint8_t left = 0;        // the level of FIFO B's side
    uint8_t held = 0;        // ADPCM: the last sample. A packed sample: the value that its run or pair plays next.
    uint8_t run = 0;         // ADPCM: 1 if the next nibble is the byte's high one. Packed: the run's samples left.
    uint32_t start = 0;      // the address of the sample's data. A loop goes back there.
    uint8_t adpcm_index = 0; // the ADPCM decoder's index into its table of steps
    int16_t adpcm_value = 0; // the ADPCM decoder's value
    uint8_t mix_path = 0;    // the WCT 2004 and Hunter X Hunter builds keep the mixing loop that a note first took
    uint16_t period = 0;     // Dungeon Dice Monsters: the timer period of the note, or 0 for the sample's rate
};

constexpr uint8_t kVoiceOn = 0x80;      // the voice plays
constexpr uint8_t kVoiceLooped = 0x40;  // the voice's sample loops
constexpr uint8_t kVoiceCoded = 0x04;   // the voice's sample is in ADPCM, or packed
constexpr uint8_t kVoiceReverse = 0x02; // the voice plays its sample backwards

// An echo bus's record.
struct OutputEcho
{
    bool has_line = false;     // whether the bus has a delay line
    uint32_t line = 0;         // the delay line's offset in the driver's work buffer
    uint32_t routing = 0;      // the routing that the mixer's mask takes from the bus
    uint32_t routed = 0;       // the voices that F9 routes to the bus: bits (11 - voice) * 2, and bit 30 if any
    uint8_t delay_blocks = 0;  // the delay in blocks
    uint8_t delay = 0;         // the delay's present value
    uint8_t position = 0;      // the block that the mixer reads
    uint8_t feedback = 0;      // the feedback's present value
    uint8_t feedback_goal = 0; // the feedback's goal
    uint8_t delay_goal = 0;    // the delay's goal
    int8_t countdown = 0;      // the frames until a silent bus clears its line
};

// A change of a FIFO's timer in a frame of the revision without a mixer, at `cycle` of the frame that Mix() ran: from
// the moment of the frame's stores. The timer's next overflow comes `first` cycles later, and then one each `period`
// cycles. A period of 0 stops the timer.
struct FifoTimerChange
{
    uint32_t cycle = 0;
    int timer = 0;
    uint32_t period = 0;
    uint32_t first = 0;
};

class Output
{
public:
    // Keeps a reference to `rom`. `info` is what DetectDriver found in it. Throws std::runtime_error when it can't find
    // the mixer's tables. In a revision with a mixer, the model finds them by their code in the image.
    Output(const agb::Rom& rom, const DriverInfo& info);
    ~Output();

    // A copy plays on from where the original stands. It keeps the reference to the ROM.
    Output(const Output& other);
    Output& operator=(const Output& other);

    // Starts `song` in the state that `request` leaves.
    void Play(int song, const Request& request = Request());

    // Runs a frame of the sequencer and the output stage. Returns false if the output stage didn't run: the song has
    // stopped. SoundWrites() holds the frame's stores.
    bool Frame();

    // The last frame's stores to the sound registers before the FIFOs, in order: the PSG's registers, the sound control
    // registers and the wave RAM, from agb::kRegSound1CntL up to agb::kRegFifoA.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // Runs the mixer for the blocks of the frame's cycles, and gives the frame's bytes for FIFO A and FIFO B. The bytes
    // are signed 8-bit samples, and the sums wrap. The cycles that a frame's whole blocks leave over count in the next
    // frame. FIFO A plays on the right and FIFO B on the left. In the ring mixers' revisions and Dungeon Dice Monsters,
    // each FIFO plays on both sides. Without a mixer, each FIFO takes 16 bytes at a time when its timer's overflows
    // reach them, and a FIFO's bytes have the timer's rate (FifoCycles()).
    void Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b);

    // Whether the last Frame() reset FIFO `fifo`, in the revision without a mixer. The game then stores 4 bytes of 0 in
    // the FIFO, before any byte that Mix() gives.
    bool FifoRestarted(int fifo) const;

    // The changes of the FIFOs' timers in the frame that Mix() last ran, in the order of their cycles. Only the
    // revision without a mixer's timers change. The other revisions' FIFOs take a point each FifoCycles().
    const std::vector<FifoTimerChange>& TimerChanges() const;

    // The points of its time that a FIFO's new byte waits behind, at the frame's stores: what the console's mixer and
    // DMA hold back. The stores of a frame reach the next block that an interrupt mixes. That block joins the FIFO a
    // block later, behind 16 bytes. A ring mixer's bytes play once the DMA comes round the ring. In the revision
    // without a mixer, the FIFOs start from the note's stores.
    uint32_t FifoLead() const;

    // The track that plays PSG channel `channel` (0 to 3) in the last frame: the channel's track, or in the revisions
    // with pan bytes in their records, the track before it while that track is off centre.
    int PsgChannelTrack(int channel) const;

    // The sound registers from agb::kRegSound1CntL up to agb::kRegFifoA as the game's init leaves them, with the BIOS's
    // bias of 0x200.
    std::array<uint8_t, agb::kSoundRegisterBytes> SoundRegisters() const;

    // Leaves the DirectSound voices of the tracks in `tracks` (bit t: track t) out of the bytes and the samples that
    // Mix() gives, from the next Mix() on. Voice v plays track v + 4. Such a voice goes on through its sample. The echo
    // buses go on too, and every record, store and timer stays as it was. Only the voice's points leave the sums and
    // its echo bus's input. A mute changes no sum but by the voice's points: a voice that wrapped a sum no longer wraps
    // it. The backwards loop of a build that adds each side's value to the other's sum leaves the sums uncrossed.
    void SetMutedTracks(uint16_t tracks);

    // From the next Mix() on, feeds the echo buses' lines their input when `on`. The driver always does. When off, the
    // lines go silent and stay so: each echo bus outputs only the voices that the mask routes to it, and adds no echo.
    // The buses' records, the mask and the voices go on as before. A voice that the console's mask leaves out of every
    // sum stays out. On at the start.
    void SetEcho(bool on);

    // From the next Mix() on, keeps each voice's peaks when `on`: the largest part that the voice gives a sum on FIFO A
    // and on FIFO B, on the scale of the FIFOs' bytes. The part leaves out the echo.
    void MeterVoices(bool on);

    // The peaks of track `track`'s voice in the last Mix(), FIFO A's then FIFO B's. 0 for a track without a voice and a
    // muted track. In the enhanced mode, the enhanced samples' peaks.
    std::array<float, 2> VoicePeaks(int track) const;

    // After Play(), mixes in the enhanced mixing mode too, from the next Mix() on.
    void StartEnhancedMixing();

    // Whether the Mix() calls from the next on mix in the enhanced mode. A Mix() without moves the voices on as before.
    // Its enhanced output is silent, and the echo buses take silence as their input. A host runs such frames to reach a
    // place in a song quickly.
    void SetEnhancedFrames(bool on);

    // The enhanced mode's output for the last Mix(), for FIFO A and FIFO B. Empty until the model mixes in the mode.
    std::span<const float> EnhancedFifoA() const;
    std::span<const float> EnhancedFifoB() const;

    // Whether no voice plays and the echo lines have fallen silent.
    bool MixerSilent() const;

    // The sequencer's outputs that the last frame's output stage read. Stopped songs give the driver's silent records.
    const std::array<TrackOutput, kTracks>& Records() const;

    // The voices, up to the revision's count.
    const std::vector<OutputVoice>& Voices() const;

    // Echo bus `bus` (0-2).
    const OutputEcho& Echo(int bus) const;

    // The mask that the output stage last gave the mixer: bits (11 - voice) * 2 choose each voice's bus, and bits 30
    // and 31 say that buses 0 and 1 run.
    uint32_t EchoMask() const;

    // The rate in Hz of the FIFOs' samples, or 0 in the revision without a mixer.
    double Rate() const;

    // The CPU cycles for each of FIFO `fifo`'s samples, or 0 while the FIFO's timer is stopped.
    uint32_t FifoCycles(int fifo) const;

    const Sequencer* Song() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ntdlib::konami
