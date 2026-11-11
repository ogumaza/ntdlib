// SPDX-License-Identifier: MIT

// The state of the Konami output stage and mixer (Output::Impl), and the types that the state uses. output.cpp and
// output_stages.cpp define the stages, and output_mixer.cpp defines the mixer.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/wave/sinc.h"
namespace ntdlib::konami
{

// The echo's line and work buffer, the sound registers that the output stage writes, and the flags of the FIFO
// revision's voices. output.cpp and output_stages.cpp use them.
constexpr uint32_t kLineBytes = 0x2000; // an echo line: 256 blocks of 16 stereo samples

constexpr uint32_t kWorkBytes = 2 * kLineBytes;

// The PSG's registers.
constexpr uint32_t kSound1CntH = 0x04000062;

constexpr uint32_t kSound1CntX = 0x04000064;

constexpr uint32_t kSound2CntL = 0x04000068;

constexpr uint32_t kSound2CntH = 0x0400006C;

constexpr uint32_t kSound3CntL = 0x04000070;

constexpr uint32_t kSound3CntH = 0x04000072;

constexpr uint32_t kSound3CntX = 0x04000074;

constexpr uint32_t kSound4CntL = 0x04000078;

constexpr uint32_t kSound4CntH = 0x0400007C;

constexpr uint32_t kSound1CntL = 0x04000060;

constexpr uint32_t kNr50 = 0x04000080;

constexpr uint32_t kNr51 = 0x04000081;

constexpr uint32_t kSoundCntHHigh = 0x04000083;

constexpr uint32_t kSoundCntH = 0x04000082;

constexpr uint32_t kSoundCntX = 0x04000084;

constexpr uint32_t kSoundBias = 0x04000088;

constexpr uint32_t kWaveRam = 0x04000090;

// A voice of the revision without a mixer: a note waits for the frame's end, and a voice that plays at its sample's
// rate keeps its FIFO's other voice playing.
constexpr uint8_t kFifoVoicePending = 0x40;

constexpr uint8_t kFifoVoiceSampleRate = 0x20;

constexpr int kBlock = 16; // a block's samples

constexpr int kEchoBuses = 3;

// The ring that a ring mixer fills for each FIFO, and its voices for each FIFO.
constexpr int kRingBytes = 704;

constexpr uint32_t kTimerOn = 0x0080;

// How a build's mixer works out a sample's value at a level.
enum class Scale
{
    kRoundUp7,   // the product, plus 128 if it's negative, over 128
    kDown7,      // the product over 128, rounded down
    kUp4,        // the product over 16, rounded down, plus 1 if that's negative
    kDown4,      // the product over 16, rounded down
    kTable,      // a table of 256 bytes for each level, at the sample's byte taken as unsigned
    kTableSigned // the same table, at the sample's byte taken as signed
};

// A voice's mixing loops. A voice takes one at a block's start.
enum class Path : uint8_t
{
    kNone,    // no loop kept
    kUnequal, // one sample byte, a value for each side
    kEqual,   // one sample byte, one value
    kDouble,  // the equal loop at a step of 0x800: two outputs for each sample byte
    kReverse, // backwards
    kCoded    // ADPCM or packed
};

// A build's coded samples.
enum class Coded
{
    kAdpcm,     // 4-bit ADPCM
    kPacked,    // codes for runs of one value and for pairs
    kPackedRuns // the same without pairs
};

// A build's mixer.
struct MixRules
{
    Scale unequal = Scale::kDown7;
    Scale equal = Scale::kDown7;
    Scale reverse = Scale::kDown7;
    Coded coded = Coded::kPacked;
    bool double_loop = true;         // the equal loop has its faster form at a step of 0x800
    bool keeps_path = false;         // the record keeps the loop that a note first took, until the next note
    bool single_loop = false;        // no separate loop for equal levels
    bool reverse_two_levels = false; // the backwards loop works out a value for each side
    bool reverse_crosses = false;    // the backwards loop adds each side's value to the other's sum
    bool skips_silent = false;       // a voice at volume 0 doesn't run
    bool loop_from_header = false;   // a loop goes back to the header's data rather than the record's start
    bool wet_rounds_up = false;      // an echo's product rounds toward zero before its shift, not after it
    bool mask_each_block = false;    // the mixer reads the buses' routing, and no mask that the frame sets
    bool has_echo = true;
    uint32_t level_table = 0;
};

// A driver's output stage.
enum class Stage
{
    kUltimate, // pan levels in the records, and the stops of a song in the frame after it
    kWct,      // pan bytes in the records: a PSG track off centre takes two channels
    kRing,     // no pan in the records, and a voice with one volume: the games with a ring mixer
    kFifo,     // no mixer: a FIFO and a timer for each two voices
};

// A build's frame update of the echo buses.
struct EchoRules
{
    int feedback_ramp = 8;            // the change of a bus's feedback in a frame
    int delay_ramp = 6;               // the change of a bus's delay in a frame
    bool countdown = true;            // a bus without feedback runs on for a while, and its line is then cleared
    bool shares_settings = true;      // bus 1 takes bus 2's feedback and delay while bus 2 has voices routed to it
    bool clears_routed = true;        // a bus whose feedback falls to 0 loses its routed voices
    bool runs_with_feedback = true;   // a bus with feedback and no routed voices still runs
    bool stop_clears_feedback = true; // the end of a song takes the buses' feedback to 0
    bool delay_sets_present = true;   // a delay command sets the bus's present delay as well as its goal
    uint32_t bus0_voices = 0x00FFFFFF;
    uint8_t first_delay_blocks = 0;
};

// Holds the song's sequencer. A copy of the output copies the sequencer's state.
class SongHolder
{
public:
    SongHolder() = default;
    SongHolder(const SongHolder& other) : song_(other.song_ ? other.song_->Clone() : nullptr)
    {
    }
    SongHolder& operator=(std::unique_ptr<Sequencer> song)
    {
        song_ = std::move(song);
        return *this;
    }

    Sequencer* operator->() const
    {
        return song_.get();
    }
    explicit operator bool() const
    {
        return song_ != nullptr;
    }
    const Sequencer* get() const
    {
        return song_.get();
    }

private:
    std::unique_ptr<Sequencer> song_;
};

// The enhanced mixing mode's state of a voice: its sample, and its position from the sample's first point in 2^-32 of
// a point.
struct EnhancedVoice
{
    uint32_t data = 0;       // the address of the sample's first point
    uint32_t size = 0;       // the sample's points
    uint32_t loop_start = 0; // the point where its loop starts
    uint16_t decoded = 0;    // 1 + the entry of Enhanced::decoded for a coded sample, or 0
    bool loops = false;
    bool reverse = false;
    bool starting = false;
    uint64_t position = 0;
    std::array<float, 2> levels{};
};

// A coded sample's points as the console's decoder gives them: a first pass, and a pass of its loop. The copies of an
// output share them: they depend on the ROM alone.
struct DecodedSample
{
    uint32_t start = 0;
    std::vector<int8_t> first;
    std::vector<int8_t> loop;
};

// The enhanced mixing mode's state.
struct Enhanced
{
    bool frames = true; // the frame mixes in the mode
    std::vector<EnhancedVoice> voices;
    std::vector<float> out;                   // the frame's output: FIFO A's, then FIFO B's
    std::array<std::vector<float>, 2> inputs; // each of the first two buses' input for the frame: FIFO A's, then B's
    std::array<std::vector<float>, 2> lines;  // each bus's delay line: FIFO A's samples, then FIFO B's
    std::array<size_t, 2> line_position{};    // each line's next sample to write
    size_t line_samples = 0;                  // the samples of each FIFO in a line
    std::vector<std::shared_ptr<const DecodedSample>> decoded;
};

struct Output::Impl
{
    Impl(const agb::Rom& rom_in, const DriverInfo& info_in);

    // The constructor's steps for the revisions with a mixer. FindMixer returns the address of the sample table's loop
    // code in the mixer, and sets second_table. FindPitchTable sets pitch_table. VoiceSizeNear returns the size of a
    // voice's record, and LevelTableNear the table of levels, from the code around the mixer. SetRevisionRules sets
    // the rules of the revision's mixer and echo.
    uint32_t FindMixer();
    void FindPitchTable();
    int VoiceSizeNear(uint32_t mixer) const;
    uint32_t LevelTableNear(uint32_t mixer) const;
    void SetRevisionRules(uint32_t level_table);

    void Store(uint32_t address, int size, uint32_t value)
    {
        const uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
        writes.push_back({address, uint8_t(size), value & mask});
    }

    uint32_t Pitched(int pitch, uint32_t step) const
    {
        return uint32_t(int32_t(rom.U16(pitch_table + uint32_t(pitch * 2)) * step) >> 12);
    }

    void Play(int song, const Request& request);
    bool Frame();
    void Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b);

    // The Ultimate Masters revision and its builds.
    void WaveLoad(int wave, int volume);
    void PlainReset();
    void StorePsg(const std::array<TrackOutput, kTracks>& rec, bool wave_on_trigger);
    void StageUltimate();
    void StageWct();
    // StageWct's steps. LevelsAndPansWct sets the levels of the voices and the pans of the PSG channels in `rec`, and
    // returns the NR51 bits. UpdateVoicesWct starts, stops and sets the voices from `rec`.
    uint32_t LevelsAndPansWct(std::array<TrackOutput, kTracks>& rec);
    void UpdateVoicesWct(const std::array<TrackOutput, kTracks>& rec);
    void StageRing();
    void RunStage();
    void StageFifo();
    void StartNotesFifo();
    void WaveLoadFifo(int wave);
    bool WaveKeepsRestart() const;
    void NoteStartRing(OutputVoice& v, const TrackOutput& r);
    void NoteStart(OutputVoice& v, const TrackOutput& r, int right, int left);
    void RouteUltimate(int bus, int voice, bool on);
    void FeedbackUltimate(int bus, int value, bool now);
    void DelayUltimate(int bus, int value, bool now);
    uint8_t FirstDelayBlocks() const;
    bool RingStoresNr51Twice() const;
    void UpdateEchoUltimate();
    void RunEchoCommandsUltimate();
    void ClearLine(int bus);

    // The mixer of the games whose records run under one block at a time.
    void MixBlock(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b);
    void MixVoice(OutputVoice& v, int16_t* a, int16_t* b);

    // The mixer of the games with a ring: a frame's samples at once.
    void MixRing(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b, uint64_t blocks);
    void MixRingPart(OutputVoice& v, int8_t* out, int samples, int level);
    bool RingEnd(OutputVoice& v);

    // The revision without a mixer: its FIFOs, and the DMA interrupt's routine that fills a FIFO's next 16 bytes.
    void MixFifos(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b);
    void TickFifo(int index, std::vector<int8_t>& out);
    void FifoInterrupt(int index);
    uint32_t FifoCycles(int index) const;
    void NoteFifoTimer(int index, uint32_t& applied, uint32_t period);
    int Value(Scale scale, int sample, int level) const;
    int NextCoded(OutputVoice& v, uint32_t& position, int advance);
    int NextAdpcm(OutputVoice& v, uint32_t& position, int advance);
    int NextPacked(OutputVoice& v, uint32_t& position, int advance);
    Path PathOf(const OutputVoice& v) const;

    // The meters.
    bool Muted(int voice) const
    {
        return muted_tracks >> (voice + 4) & 1;
    }
    void MeterPart(int voice, int fifo, float part);

    // The enhanced mixing mode.
    void StartEnhanced(int voice, uint32_t data, uint32_t size, int32_t loop, bool coded, bool reverse);
    int DecodedSampleOf(uint32_t header_data, uint32_t size, int32_t loop);
    float Gain(Scale scale, int level) const;
    float PointOf(const EnhancedVoice& e, int64_t point) const;
    void MixEnhanced();
    void MixEnhancedVoice(int voice, float* a, float* b, std::array<float, 2> to, uint32_t step, uint32_t period);
    void MixEnhancedBus(int bus, const OutputEcho& e);

    const agb::Rom& rom;
    DriverInfo info;
    MixRules rules;
    EchoRules echo_rules;
    Stage stage = Stage::kUltimate;
    bool levels_each_frame = false; // a playing voice takes its levels in every frame, not only at a retrigger
    bool clears_held = false;       // a note clears the record's held sample
    bool wave_zero_check = true;    // the wave RAM loader fills the RAM with 0 for a volume of 0
    bool wave_gate = true;          // the wave channel stays out of NR51 until its first note
    bool pan_sets_levels = true;    // a pan change sets a playing voice's levels without a retrigger
    bool wave_second_bank = true;   // bit 7 of a wave's number chooses the second table of wave images
    bool pitch_on_any_flag = false; // any flag but a note start or a stop works out a ring voice's step again
    bool nr51_twice = false;        // the output stage stores NR51 twice
    uint32_t second_table = 0;
    uint32_t pitch_table = 0;
    int voice_size = 0;
    int voice_count = 0;

    SongHolder sequencer;
    std::array<TrackOutput, kTracks> records{};
    std::vector<OutputVoice> voices;
    std::vector<agb::RegisterWrite> writes;

    bool stop_pending = false;
    bool idle = true;
    bool first_frame = false;
    bool wave_started = false; // the output stage has seen a note on the wave channel
    uint32_t wave_bank = 0;
    uint32_t nr51 = 0xBB; // the sides that the init leaves: every channel on both, but the wave channel

    std::array<OutputEcho, kEchoBuses> echo{};
    uint32_t echo_mask = 0;
    bool echo_heard = true; // the echo buses' lines take their input (Output::SetEcho)
    std::vector<int8_t> work;

    std::array<uint8_t, 4> channel_tracks = {0, 1, 2, 3}; // the track that plays each PSG channel
    uint16_t muted_tracks = 0;
    bool metering = false;
    std::array<std::array<float, 2>, kTracks> voice_peaks{};
    std::optional<Enhanced> enhanced;

    uint32_t block_cycles = 0;
    uint64_t due_cycles = 0;
    int ring_position = 0; // where the DMA has reached in the ring

    // The revision without a mixer.
    struct Fifo
    {
        std::array<int8_t, kBlock> buffer{}; // what the DMA takes next
        int queued = 0;                      // the bytes that the FIFO has yet to play
        bool ticking = false;
        int64_t next_tick = 0; // the frame's cycle of the timer's next overflow
        bool dma_on = false;
        bool running = false;   // the note start has reset the FIFO
        bool restarted = false; // the frame reset the FIFO
        uint16_t goal = 0;      // the period that the interrupt's routine gives the timer
        uint16_t period = 0;
    };

    std::array<Fifo, 2> fifos;
    std::array<uint32_t, 2> timers{};          // each timer's reload and control
    std::array<uint32_t, 2> applied_periods{}; // each timer's period in the changes that the host has had
    std::vector<FifoTimerChange> timer_changes;
    uint32_t sound_control_high = 0x33;
    bool wave_keeps_restart = false; // the wave channel's frequency store keeps bit 15, and a wave load stores none
};

} // namespace ntdlib::konami
