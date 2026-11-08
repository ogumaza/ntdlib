// SPDX-License-Identifier: MIT

#include "ntdlib/konami/output.h"

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
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::konami
{
namespace
{

constexpr int kBlock = 16; // a block's samples
constexpr int kEchoBuses = 3;
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

// The ring that a ring mixer fills for each FIFO, and its voices for each FIFO.
constexpr int kRingBytes = 704;
constexpr int kRingVoices = 3;

// A voice of the revision without a mixer: a note waits for the frame's end, and a voice that plays at its sample's
// rate keeps its FIFO's other voice playing.
constexpr uint8_t kFifoVoicePending = 0x40;
constexpr uint8_t kFifoVoiceSampleRate = 0x20;
constexpr uint8_t kFifoVoiceReverse = 0x08;

// The prescaler of a timer's control register, and its on bit.
constexpr std::array<uint32_t, 4> kTimerPrescale = {1, 64, 256, 1024};
constexpr uint32_t kTimerOn = 0x0080;

// The IMA ADPCM tables that the 28-byte builds' mixer reads.
constexpr std::array<int8_t, 16> kAdpcmIndex = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr std::array<uint16_t, 89> kAdpcmStep = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

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

} // namespace

struct Output::Impl
{
    Impl(const agb::Rom& rom_in, const DriverInfo& info_in);

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
    void StageUltimate();
    void StageWct();
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

Output::Impl::Impl(const agb::Rom& rom_in, const DriverInfo& info_in) : rom(rom_in), info(info_in)
{
    if (info.revision == Revision::kDungeonDiceMonsters)
    {
        stage = Stage::kFifo;
        voice_size = 12;
        voice_count = 4;
        rules.has_echo = false;
        wave_gate = false;
        wave_keeps_restart = WaveKeepsRestart();
        echo_rules.first_delay_blocks = 0;
        voices.assign(size_t(voice_count), OutputVoice());
        block_cycles = kBlock;
        return;
    }

    // The sample table's loop code in the mixer: ldrsh r0, [r7, #12] / movs r0, r0, lsl #17.
    constexpr uint32_t kLoadKey = 0xE1D700FC;
    constexpr uint32_t kShiftKey = 0xE1B00880;
    uint32_t mixer = 0;
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 8); at += 4)
    {
        if (rom.U32(at) != kLoadKey || rom.U32(at + 4) != kShiftKey)
        {
            continue;
        }

        const auto literal = [this](uint32_t where, uint32_t opcode, uint32_t mask) -> uint32_t
        {
            const uint32_t word = rom.U32(where);
            return (word & mask) == opcode ? rom.U32(where + 8 + (word & 0xFFF)) : 0;
        };

        // ldrlo r5, =table / ldrhs r5, =second after the shift, or ldr r5, =table before the load and ldrhs r5, =second
        // after the shift.
        uint32_t main_table = literal(at + 8, 0x359F5000, 0xFFFFF000);
        uint32_t other = literal(at + 12, 0x259F5000, 0xFFFFF000);
        if (!main_table)
        {
            main_table = literal(at - 4, 0xE59F5000, 0xFFFFF000);
            other = literal(at + 8, 0x259F5000, 0xFFFFF000);
        }

        if (main_table == info.sample_table)
        {
            mixer = at;
            second_table = other;
            break;
        }
    }

    if (!mixer)
    {
        throw std::runtime_error("the driver's mixer wasn't found");
    }

    // The pitch table: 4096 * 2 ^ (pitch / 384), round its centre.
    constexpr std::array<uint16_t, 5> kPitchCentre = {4081, 4089, 4096, 4103, 4111};
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 10) && !pitch_table; at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < kPitchCentre.size() && match; i++)
        {
            match = rom.U16(at + uint32_t(2 * i)) == kPitchCentre[i];
        }
        if (match)
        {
            pitch_table = at + 4;
        }
    }

    if (!pitch_table)
    {
        throw std::runtime_error("the driver's pitch table wasn't found");
    }

    // The build's records: sub r7, r7, #size in the mixers that run from the last voice, and add r7, r7, #size in the
    // others.
    const auto holds = [&](uint32_t word)
    {
        for (uint32_t at = mixer - std::min(mixer - agb::kRomBase, 0x400u); at < mixer + 0x400; at += 4)
        {
            if (rom.U32(at) == word)
            {
                return true;
            }
        }
        return false;
    };

    const bool ring = info.revision == Revision::kEternalDuelist || info.revision == Revision::kCaptainTsubasa;
    voice_size = ring                                     ? 16
                 : holds(0xE247701C) || holds(0xE287701C) ? 28
                 : holds(0xE2477018) || holds(0xE2877018) ? 24
                                                          : 20;

    // The table of levels' load: ldr r0, =table before ldrb r8, [r7, #16].
    uint32_t level_table = 0;
    for (uint32_t at = mixer - std::min(mixer - agb::kRomBase, 0x400u); at < mixer + 0x400 && !level_table; at += 4)
    {
        if (rom.U32(at) == 0xE5D78010 && (rom.U32(at - 4) & 0xFFFFF000) == 0xE59F0000)
        {
            level_table = rom.U32(at - 4 + 8 + (rom.U32(at - 4) & 0xFFF));
        }
    }

    voice_count = 12;
    rules = MixRules();
    switch (info.revision)
    {
    case Revision::kUltimateMasters:
    case Revision::kLiveStart:
    case Revision::kPrinceOfTennis:
        if (voice_size == 28)
        {
            rules.unequal = rules.equal = rules.reverse = Scale::kRoundUp7;
            rules.coded = Coded::kAdpcm;
            rules.wet_rounds_up = true;
            clears_held = true;
        }

        pan_sets_levels = info.revision != Revision::kPrinceOfTennis;
        if (info.revision != Revision::kUltimateMasters)
        {
            echo_rules.feedback_ramp = 6;
            echo_rules.delay_ramp = 4;
            echo_rules.countdown = false;
            echo_rules.bus0_voices = 0x1FFF;
        }
        break;
    case Revision::kWct2004:
    case Revision::kMinnaNoOujisama:
        stage = Stage::kWct;
        levels_each_frame = true;
        echo_rules.feedback_ramp = 4;
        echo_rules.delay_ramp = 4;
        echo_rules.countdown = false;
        echo_rules.shares_settings = false;
        echo_rules.clears_routed = false;
        echo_rules.stop_clears_feedback = voice_size == 24;
        echo_rules.bus0_voices = 0x1FFF;
        rules.unequal = rules.reverse = Scale::kUp4;
        rules.reverse_two_levels = true;
        if (voice_size == 28)
        {
            rules.equal = Scale::kUp4;
            rules.keeps_path = true;
            rules.double_loop = false;
        }
        else
        {
            rules.equal = Scale::kDown4;
            rules.reverse_crosses = info.revision == Revision::kWct2004;
            rules.reverse_two_levels = info.revision == Revision::kWct2004;
        }
        break;
    case Revision::kKitaro:
        stage = Stage::kWct;
        levels_each_frame = true;
        echo_rules.feedback_ramp = 4;
        echo_rules.delay_ramp = 4;
        echo_rules.countdown = false;
        echo_rules.shares_settings = false;
        echo_rules.clears_routed = false;
        echo_rules.runs_with_feedback = false;
        echo_rules.stop_clears_feedback = false;
        echo_rules.delay_sets_present = false;
        rules.unequal = rules.equal = Scale::kTable;
        rules.reverse = Scale::kTableSigned;
        rules.reverse_two_levels = true;
        rules.keeps_path = true;
        rules.single_loop = true;
        rules.double_loop = false;
        rules.mask_each_block = true;
        rules.level_table = level_table;
        break;
    case Revision::kRaveMaster:
    case Revision::kHunterXHunter:
        stage = Stage::kWct;
        voice_count = 8;
        rules.unequal = rules.equal = Scale::kTable;
        rules.reverse = Scale::kTableSigned;
        rules.reverse_two_levels = true;
        rules.single_loop = true;
        rules.double_loop = false;
        rules.has_echo = false;
        rules.level_table = level_table;
        if (info.revision == Revision::kHunterXHunter)
        {
            rules.keeps_path = true;
        }
        else
        {
            rules.skips_silent = true;
            if (voice_size == 20)
            {
                rules.coded = Coded::kPackedRuns;
                rules.loop_from_header = true;
                wave_zero_check = false;
                wave_gate = false;
                nr51 = 0xFF;
            }
        }
        break;
    case Revision::kEternalDuelist:
    case Revision::kCaptainTsubasa:
        stage = Stage::kRing;
        voice_count = 6;
        rules.has_echo = false;
        wave_zero_check = false;
        wave_second_bank = false;
        wave_gate = false;
        nr51 = 0xFF;
        pitch_on_any_flag = info.revision == Revision::kEternalDuelist;
        nr51_twice = RingStoresNr51Twice();
        break;
    default:
        break;
    }

    echo_rules.first_delay_blocks = FirstDelayBlocks();
    voices.assign(size_t(voice_count), OutputVoice());
    work.assign(kWorkBytes, 0);
    const double rate = info.mix_rate > 0 ? info.mix_rate : 21024.0;
    block_cycles = kBlock * uint32_t(std::lround(double(agb::kSecondCycles) / rate));
}

bool Output::Impl::RingStoresNr51Twice() const
{
    // The output stage opens with the store and the setting's test: ldr r3, =NR51 / movs r2, #0xE0 / lsls r2, r2, #1 /
    // add r2, r8 / ldrb r0, [r2] / strb r0, [r3]. The other builds load the byte just before the one store.
    constexpr std::array<uint16_t, 5> kOpening = {0x22E0, 0x0052, 0x4442, 0x7810, 0x7018};
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 10); at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < kOpening.size() && match; i++)
        {
            match = rom.U16(at + uint32_t(2 * i)) == kOpening[i];
        }
        if (match)
        {
            return true;
        }
    }

    return false;
}

bool Output::Impl::WaveKeepsRestart() const
{
    // The later build's output stage stores the entry as it stands: movs r3, #0x18 / ldrsh r0, [r6, r3] /
    // lsls r0, r0, #1 / adds r0, r0, r1 / ldrh r0, [r0] / strh r0, [r2]. The others take bit 15 off it first.
    constexpr std::array<uint16_t, 6> kStore = {0x2318, 0x5EF0, 0x0040, 0x1840, 0x8800, 0x8010};
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 12); at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < kStore.size() && match; i++)
        {
            match = rom.U16(at + uint32_t(2 * i)) == kStore[i];
        }
        if (match)
        {
            return true;
        }
    }

    return false;
}

uint8_t Output::Impl::FirstDelayBlocks() const
{
    // The init sets the mixer's rate, and the rate's routine works out the delay of each bus in blocks from a table of
    // scales. Some builds' routine has already put the timer's setting in the register that held the rate, and reads
    // the table at twice that setting: at whatever data lies there.
    constexpr uint32_t kFullDelay = 2 * 0x7F;
    constexpr std::array<uint16_t, 5> kScales = {4096, 3537, 3072, 2606, 2048};
    // ldr r3, [r0] / movs r0, #0x80 / lsls r0, r0, #16 / orrs r0, r3 / str r0, [r2], then lsls r0, r3, #1 after the two
    // literals' loads.
    constexpr std::array<uint16_t, 5> kStray = {0x6803, 0x2080, 0x0400, 0x4318, 0x6010};
    // The later revisions' init leaves the rate to the game's code, and the delays at 0.
    if (stage != Stage::kUltimate)
    {
        return 0;
    }

    if (!info.timer_table)
    {
        return uint8_t(kFullDelay);
    }

    const auto find = [this](const auto& words) -> uint32_t
    {
        for (uint32_t at = agb::kRomBase; rom.Contains(at, uint32_t(2 * words.size())); at += 2)
        {
            bool match = true;
            for (size_t i = 0; i < words.size() && match; i++)
            {
                match = rom.U16(at + uint32_t(2 * i)) == words[i];
            }
            if (match)
            {
                return at;
            }
        }
        return 0;
    };

    const uint32_t stray = find(kStray);
    const uint32_t scales = find(kScales);
    if (!stray || !scales || rom.U16(stray + 14) != 0x0058)
    {
        return uint8_t(kFullDelay);
    }

    const uint32_t scale = rom.U16(scales + rom.U32(info.timer_table) * 2);
    return uint8_t(((scale * 0x7F) >> 12) * 2);
}

void Output::Impl::Play(int song, const Request& request)
{
    sequencer = Sequencer::Create(rom, info, song, request);
    stop_pending = false;
    idle = false;
    first_frame = true;
    wave_started = false;
}

void Output::Impl::WaveLoad(int wave, int volume)
{
    if (volume != 0 || !wave_zero_check)
    {
        uint32_t table = info.wave_table;
        if (wave_second_bank && (wave & 0x80))
        {
            table = info.sfx_wave_table;
            wave &= 0x7F;
        }

        const uint32_t image = table + uint32_t((wave << 4) + volume) * 16;
        for (uint32_t i = 0; i < 4; i++)
        {
            Store(kWaveRam + 4 * i, 4, rom.U32(image + 4 * i));
        }
    }
    else
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            Store(kWaveRam + 4 * i, 4, 0);
        }
    }

    wave_bank ^= 1;
    Store(kSound3CntL, 2, wave_bank ? 0x80 : 0xC0);
}

void Output::Impl::WaveLoadFifo(int wave)
{
    Store(kSound3CntL, 2, 0x40);
    for (uint32_t i = 0; i < 8; i++)
    {
        Store(kWaveRam + 2 * i, 2, rom.U16(info.wave_table + uint32_t(wave) * 16 + 2 * i));
    }

    Store(kSound3CntL, 2, 0x80);
    if (!wave_keeps_restart)
    {
        Store(kSound3CntX, 2, 0x8000);
    }
}

void Output::Impl::PlainReset()
{
    if (stage == Stage::kFifo)
    {
        Store(kSound1CntL, 2, 0);
        WaveLoadFifo(0);
        return;
    }

    if (stage == Stage::kRing)
    {
        Store(kNr51, 1, 0xFF);
        Store(kSoundCntH, 2, 0x330E);
    }

    Store(kSound3CntH, 2, 0);
    WaveLoad(0, 0);
}

void Output::Impl::ClearLine(int bus)
{
    const OutputEcho& e = echo[size_t(bus)];
    if (e.has_line)
    {
        std::fill_n(work.begin() + e.line, kLineBytes, int8_t(0));
        if (enhanced)
        {
            std::vector<float>& line = enhanced->lines[e.line == 0 ? 0 : 1];
            std::fill(line.begin(), line.end(), 0.0f);
        }
    }
}

void Output::Impl::RouteUltimate(int bus, int voice, bool on)
{
    if (bus > 2 || voice > 11)
    {
        return;
    }

    OutputEcho& e = echo[size_t(bus)];
    if (!e.has_line || voice < 0)
    {
        e.routing = 0;
        e.routed = 0;
        return;
    }

    const uint32_t bit = 1u << ((11 - voice) * 2);
    uint32_t routed = (e.routed << 2) >> 2;
    routed |= bit;
    if (!on)
    {
        routed &= ~bit;
    }
    if (routed)
    {
        routed |= 0x40000000;
    }

    e.routed = routed;
}

void Output::Impl::FeedbackUltimate(int bus, int value, bool now)
{
    OutputEcho& e = echo[size_t(bus)];
    if (!e.has_line)
    {
        e.feedback = 0;
        e.feedback_goal = 0;
        return;
    }

    e.feedback_goal = uint8_t(value);
    if (now)
    {
        e.feedback = uint8_t(value);
        if (bus == 2)
        {
            echo[1].feedback = uint8_t(value);
        }
    }
}

void Output::Impl::DelayUltimate(int bus, int value, bool now)
{
    OutputEcho& e = echo[size_t(bus)];
    if (!e.has_line)
    {
        e.delay = 0x7F;
        e.delay_goal = 0x7F;
        return;
    }

    if (now)
    {
        e.delay_goal = uint8_t(value);
        if (echo_rules.delay_sets_present)
        {
            e.delay = uint8_t(value);
        }
    }

    e.delay_blocks = uint8_t(2 * value);
}

void Output::Impl::UpdateEchoUltimate()
{
    if (!rules.has_echo)
    {
        return;
    }

    const EchoRules& r = echo_rules;
    for (int i = 0; i < 2; i++)
    {
        OutputEcho& e = echo[size_t(i)];
        const OutputEcho* settings = &e;
        uint32_t routing = e.routed;
        if (i == 1 && echo[2].routed != 0)
        {
            routing |= echo[2].routed;
            settings = &echo[2];
            if (r.shares_settings)
            {
                e.feedback = echo[2].feedback;
                e.delay_blocks = echo[2].delay_blocks;
            }
        }

        e.routing = routing;
        uint8_t feedback = e.feedback;
        const uint8_t goal = settings->feedback_goal;
        if (feedback != goal)
        {
            if (feedback > goal)
            {
                feedback = uint8_t(std::max(feedback - r.feedback_ramp, int(goal)));
            }
            else
            {
                feedback = uint8_t(std::min(feedback + r.feedback_ramp, int(goal)));
            }

            e.feedback = feedback;
            if (feedback == 0)
            {
                e.countdown = r.countdown ? 0x3C : e.countdown;
                if (r.clears_routed)
                {
                    e.routed = 0;
                }
                e.routing = 0;
            }
            else
            {
                e.countdown = r.countdown ? 0 : e.countdown;
                e.routing = routing;
            }
        }

        if (r.countdown)
        {
            if (feedback == 0 && e.countdown >= 0)
            {
                e.routing |= 0x40000000;
                e.countdown = int8_t(e.countdown - 1);
                if (e.countdown < 0)
                {
                    ClearLine(i);
                }
            }
        }
        else if (r.runs_with_feedback && e.feedback != 0 && e.routing == 0)
        {
            e.routing = 0x40000000;
        }

        if (e.delay != settings->delay_goal)
        {
            // The routine sets the blocks from the new value, and keeps the present value.
            DelayUltimate(i, std::max(e.delay - r.delay_ramp, int(settings->delay_goal)), false);
        }
    }

    uint32_t mask = echo[0].routing & ~echo[1].routing & r.bus0_voices;
    if (mask != 0 || (voice_size == 28 && r.countdown && echo[0].countdown != 0))
    {
        mask |= 0x40000000;
    }

    echo_mask = (echo[1].routing << 1) | mask;
}

void Output::Impl::RunEchoCommandsUltimate()
{
    for (const StageCommand& c : sequencer->StageCommands())
    {
        switch (c.op)
        {
        case Op::kEchoRoute:
            {
                const int voice = c.value >> 4;
                const int bus = (c.value & 0x0F) - 1;
                if (bus < 0)
                {
                    RouteUltimate(0, voice, false);
                    RouteUltimate(1, voice, false);
                }
                else
                {
                    RouteUltimate(bus, voice, true);
                }
                break;
            }
        case Op::kEchoDelay:
            DelayUltimate(c.value > 0x7F ? 1 : 0, c.value > 0x7F ? c.value - 0x80 : c.value, true);
            break;
        case Op::kEchoFeedback:
            if (stage == Stage::kWct)
            {
                FeedbackUltimate(0, c.value & 0x7F, false);
            }
            else
            {
                FeedbackUltimate(c.value > 0x7F ? 1 : 0, c.value > 0x7F ? c.value - 0x80 : c.value, false);
            }
            break;
        case Op::kWave:
            WaveLoad(c.value, c.volume);
            break;
        default:
            break;
        }
    }
}

void Output::Impl::NoteStart(OutputVoice& v, const TrackOutput& r, int right, int left)
{
    v.flags = 0;
    const uint32_t key = r.key;
    uint32_t table = info.sample_table;
    if (stage == Stage::kUltimate)
    {
        if (key & 0xA000)
        {
            table = key & 0x2000 ? 0 : second_table;
        }

        v.key = uint16_t(key & ~0x1000u);
    }
    else
    {
        if (key & 0x8000)
        {
            table = second_table;
        }

        v.key = uint16_t(key & 0xCFFFu);
    }

    const uint32_t header = rom.U32(table + (key & 0xFFF) * 4);
    v.run = 0;
    if (clears_held)
    {
        v.held = 0;
    }

    v.volume = r.vol;
    const uint32_t word = Pitched(r.pitch, rom.U32(header));
    v.step = uint16_t(word);
    v.fraction = uint16_t(word >> 16);
    v.start = header + 12;
    int32_t length = rom.S32(header + 4);
    uint8_t flags = kVoiceOn;
    if (length < 0)
    {
        length &= 0x00FFFFFF;
        flags |= kVoiceCoded;
        if (rules.coded == Coded::kAdpcm)
        {
            v.adpcm_index = 0;
            v.adpcm_value = 0;
        }
        else if (rules.coded == Coded::kPacked)
        {
            // The table of pairs comes first.
            v.start = header + 12 + 64;
        }
    }

    v.position = v.start;
    v.count = length;
    if (key & 0x4000)
    {
        v.position = v.start + uint32_t(rom.S32(header + 4)) - 1;
        flags |= kVoiceReverse;
    }
    else if (rom.S32(header + 8) >= 0)
    {
        flags |= kVoiceLooped;
    }

    v.right = uint8_t(right);
    v.left = uint8_t(left);
    v.mix_path = 0;
    v.flags = flags;
    if (enhanced)
    {
        const bool reverse = flags & kVoiceReverse;
        StartEnhanced(int(&v - voices.data()), v.start, uint32_t(length), rom.S32(header + 8),
                      !reverse && (flags & kVoiceCoded), reverse);
    }
}

void Output::Impl::StageUltimate()
{
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    uint32_t mix = nr51 << 4;
    for (int c = 0; c < 4; c++)
    {
        const TrackOutput& r = records[size_t(c)];
        mix >>= 1;
        if (any(r) && r.vol != 0)
        {
            mix |= 0x88;
            if (r.pan_l < r.pan_r)
            {
                mix &= ~0x80u;
            }
            if (r.pan_l > r.pan_r)
            {
                mix &= ~0x08u;
            }
        }
    }

    if (!wave_started)
    {
        if (records[2].trig != 0 && records[2].vol != 0)
        {
            wave_started = true;
        }
        else
        {
            mix &= ~0x44u;
        }
    }

    if (nr51 != mix)
    {
        nr51 = mix;
        Store(kNr51, 1, mix);
    }

    const auto frequency = [this](int pitch)
    {
        return rom.U16(info.psg_freq_table + uint32_t(pitch * 2));
    };

    if (any(records[0]))
    {
        const TrackOutput& r = records[0];
        const uint32_t f = frequency(r.pitch);
        if (r.trig)
        {
            Store(kSound1CntH, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound1CntX, 2, f);
        }
        else
        {
            Store(kSound1CntX, 2, f & 0x7FF);
        }
    }

    if (any(records[1]))
    {
        const TrackOutput& r = records[1];
        uint32_t f;
        if (r.pitch >= 0 && (r.pitch & 0x4000))
        {
            f = (uint32_t(uint16_t(r.pitch)) & ~0x4000u) | 0x8000;
        }
        else
        {
            f = frequency(r.pitch);
        }

        if (r.trig)
        {
            Store(kSound2CntL, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound2CntH, 2, f);
        }
        else
        {
            Store(kSound2CntH, 2, f & 0x7FF);
        }
    }

    if (any(records[2]))
    {
        const TrackOutput& r = records[2];
        const uint32_t f = frequency(r.pitch) & 0x7FF;
        if (r.vol == 0)
        {
            Store(kSound3CntH, 2, 0);
        }
        else if (r.trig)
        {
            WaveLoad(r.b2, r.vol);
            Store(kSound3CntH, 2, 0x2000);
        }

        Store(kSound3CntX, 2, f);
    }

    if (any(records[3]))
    {
        const TrackOutput& r = records[3];
        Store(kSound4CntL, 2, uint32_t(r.vol) << 12);
        if ((r.trig & 2) || (r.flags & 2))
        {
            Store(kSound4CntH, 2, uint16_t(r.pitch));
        }
        else
        {
            Store(kSound4CntH, 2, rom.U16(info.noise_table + uint32_t(r.pitch * 2)));
        }
    }

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = records[size_t(n + 4)];
        if (!any(r))
        {
            continue;
        }

        const uint32_t levels = info.volume_table + uint32_t(r.vol) * 64;
        const int right = rom.U8(levels + r.pan_r);
        const int left = rom.U8(levels + r.pan_l);
        if (r.flags & kOutStop)
        {
            v.flags = 0;
            v.volume = 0;
        }
        else if (r.flags != 0)
        {
            if (r.flags & kOutNoteOn)
            {
                NoteStart(v, r, right, left);
            }

            if (r.flags & kOutPitch)
            {
                const uint32_t table = r.key & 0x8000 ? second_table : info.sample_table;
                const uint32_t header = rom.U32(table + (r.key & 0xFFFu) * 4);
                v.step = uint16_t(Pitched(r.pitch, rom.U32(header)));
            }
        }

        if (r.trig != 0 || (pan_sets_levels && (r.flags & kOutPan)))
        {
            v.volume = r.vol;
            v.right = uint8_t(right);
            v.left = uint8_t(left);
        }
    }
}

void Output::Impl::StageWct()
{
    std::array<TrackOutput, kTracks> rec = records;
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    const auto plain = [](uint8_t pan)
    {
        return pan == 0xFF || pan == 0x0F || pan == 0xF0;
    };

    const auto levels = [this](int volume)
    {
        return info.volume_table + uint32_t(volume) * 16;
    };

    uint32_t mix = 0;
    channel_tracks = {0, 1, 2, 3};
    for (int t = voice_count + 3; t >= 0; t--)
    {
        TrackOutput& r = rec[size_t(t)];
        const uint8_t pan = r.pan;
        if (t > 3)
        {
            const OutputVoice& v = voices[size_t(t - 4)];
            const bool had_output = any(r);
            if (had_output || (levels_each_frame && (v.flags & kVoiceOn)))
            {
                const uint32_t row = levels(r.vol);
                r.pan_r = rom.U8(row + (pan & 15));
                r.pan_l = rom.U8(row + (pan >> 4));
                if (levels_each_frame && !had_output)
                {
                    r.trig = 1;
                }
            }
            continue;
        }

        // A plain pan after one off centre leaves square 2 at volume 0 with square 1's output.
        if (t == 0 && !any(rec[1]) && plain(pan) && !plain(r.pan_start))
        {
            rec[1] = r;
            rec[1].vol = 0;
        }

        mix = (mix << 1) | 0x11;
        switch (pan)
        {
        case 0xF0:
            mix &= ~0x01u;
            break;
        case 0x0F:
            mix &= ~0x10u;
            break;
        case 0xFF:
            break;
        default:
            {
                // The channel plays the right side, and the next channel the left.
                TrackOutput& next = rec[size_t(t + 1)];
                next = r;
                if (t < 3)
                {
                    channel_tracks[size_t(t + 1)] = uint8_t(t);
                }

                const uint32_t row = levels(r.vol);
                r.vol = rom.U8(row + (pan & 15));
                next.vol = rom.U8(row + (pan >> 4));
                mix &= ~0x33u;
                mix |= 0x21;
            }
            break;
        }
    }

    if (wave_gate && !wave_started)
    {
        if (rec[2].trig != 0 && rec[2].vol != 0)
        {
            wave_started = true;
        }
        else
        {
            mix &= ~0x44u;
        }
    }

    if (nr51 != mix)
    {
        nr51 = mix;
        Store(kNr51, 1, mix);
    }

    const auto frequency = [this](int pitch)
    {
        return rom.U16(info.psg_freq_table + uint32_t(pitch * 2));
    };

    if (any(rec[0]))
    {
        const TrackOutput& r = rec[0];
        const uint32_t f = frequency(r.pitch);
        if (r.trig)
        {
            Store(kSound1CntH, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound1CntX, 2, f);
        }
        else
        {
            Store(kSound1CntX, 2, f & 0x7FF);
        }
    }

    if (any(rec[1]))
    {
        const TrackOutput& r = rec[1];
        uint32_t f;
        if (r.pitch >= 0 && (r.pitch & 0x4000))
        {
            f = (uint32_t(uint16_t(r.pitch)) & ~0x4000u) | 0x8000;
        }
        else
        {
            f = frequency(r.pitch);
        }

        if (r.trig)
        {
            Store(kSound2CntL, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound2CntH, 2, f);
        }
        else
        {
            Store(kSound2CntH, 2, f & 0x7FF);
        }
    }

    if (any(rec[2]))
    {
        const TrackOutput& r = rec[2];
        const uint32_t f = frequency(r.pitch) & 0x7FF;
        if (r.vol == 0)
        {
            Store(kSound3CntH, 2, 0);
        }
        else
        {
            WaveLoad(r.b2, r.vol);
            Store(kSound3CntH, 2, 0x2000);
        }

        Store(kSound3CntX, 2, f);
    }

    if (any(rec[3]))
    {
        const TrackOutput& r = rec[3];
        Store(kSound4CntL, 2, uint32_t(r.vol) << 12);
        if ((r.trig & 2) || (r.flags & 2))
        {
            Store(kSound4CntH, 2, uint16_t(r.pitch));
        }
        else
        {
            Store(kSound4CntH, 2, rom.U16(info.noise_table + uint32_t(r.pitch * 2)));
        }
    }

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = rec[size_t(n + 4)];
        if (r.flags & kOutStop)
        {
            v.flags = 0;
            v.volume = 0;
        }
        else if (r.flags != 0)
        {
            if (r.flags & kOutNoteOn)
            {
                NoteStart(v, r, r.pan_r, r.pan_l);
            }

            if (r.flags & kOutPitch)
            {
                const uint32_t table = r.key & 0x8000 ? second_table : info.sample_table;
                const uint32_t header = rom.U32(table + (r.key & 0xFFFu) * 4);
                v.step = uint16_t(Pitched(r.pitch, rom.U32(header)));
            }
        }

        if (r.trig != 0)
        {
            v.volume = r.vol;
            v.right = r.pan_r;
            v.left = r.pan_l;
        }
    }
}

void Output::Impl::NoteStartRing(OutputVoice& v, const TrackOutput& r)
{
    const uint32_t key = r.key & 0xCFFFu;
    const uint32_t entry = key & 0x8000 ? second_table + (key & 0x3FFF) * 4 : info.sample_table + key * 4;
    const uint32_t header = rom.U32(entry);
    const uint32_t word = Pitched(r.pitch, rom.U32(header));
    v.step = uint16_t(word);
    v.fraction = uint16_t(word >> 16);
    v.count = rom.S32(header + 4);
    v.position = header + 12;
    v.key = uint16_t(key & 0x8FFFu);
    v.volume = r.vol;
    v.flags = rom.S32(header + 8) >= 0 ? kVoiceOn | kVoiceLooped : kVoiceOn;
    if (enhanced)
    {
        StartEnhanced(int(&v - voices.data()), v.position, uint32_t(v.count), rom.S32(header + 8), false, false);
    }
}

void Output::Impl::StageRing()
{
    const std::array<TrackOutput, kTracks>& rec = records;
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    Store(kNr51, 1, nr51);
    if (nr51_twice)
    {
        Store(kNr51, 1, nr51);
    }

    const auto frequency = [this](int pitch)
    {
        return rom.U16(info.psg_freq_table + uint32_t(pitch * 2));
    };

    if (any(rec[0]))
    {
        const TrackOutput& r = rec[0];
        const uint32_t f = frequency(r.pitch);
        if (r.trig)
        {
            Store(kSound1CntH, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound1CntX, 2, f);
        }
        else
        {
            Store(kSound1CntX, 2, f & 0x7FF);
        }
    }

    if (any(rec[1]))
    {
        const TrackOutput& r = rec[1];
        uint32_t f;
        if (r.pitch >= 0 && (r.pitch & 0x4000))
        {
            f = (uint32_t(uint16_t(r.pitch)) & ~0x4000u) | 0x8000;
        }
        else
        {
            f = frequency(r.pitch);
        }

        if (r.trig)
        {
            Store(kSound2CntL, 2, uint32_t(r.vol) << 12 | r.b2);
            Store(kSound2CntH, 2, f);
        }
        else
        {
            Store(kSound2CntH, 2, f & 0x7FF);
        }
    }

    if (any(rec[2]))
    {
        const TrackOutput& r = rec[2];
        const uint32_t f = frequency(r.pitch) & 0x7FF;
        if (r.vol == 0)
        {
            Store(kSound3CntH, 2, 0);
        }
        else
        {
            WaveLoad(r.b2, r.vol);
            Store(kSound3CntH, 2, 0x2000);
        }

        Store(kSound3CntX, 2, f);
    }

    if (any(rec[3]))
    {
        const TrackOutput& r = rec[3];
        Store(kSound4CntL, 2, uint32_t(r.vol) << 12);
        if ((r.trig & 2) || (r.flags & 2))
        {
            Store(kSound4CntH, 2, uint16_t(r.pitch));
        }
        else
        {
            Store(kSound4CntH, 2, rom.U16(info.noise_table + uint32_t(r.pitch * 2)));
        }
    }

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = rec[size_t(n + 4)];
        if (r.flags != 0)
        {
            bool pitch = false;
            if (pitch_on_any_flag)
            {
                if (r.flags & kOutNoteOn)
                {
                    NoteStartRing(v, r);
                }
                else if (r.flags & kOutStop)
                {
                    v.flags = 0;
                }
                else
                {
                    pitch = true;
                }
            }
            else if (r.flags & kOutStop)
            {
                v.flags = 0;
            }
            else
            {
                if (r.flags & kOutNoteOn)
                {
                    NoteStartRing(v, r);
                }

                pitch = r.flags & kOutPitch;
            }

            if (pitch)
            {
                const uint32_t entry =
                    r.key & 0x8000 ? second_table + (r.key & 0x3FFFu) * 4 : info.sample_table + uint32_t(r.key) * 4;
                v.step = uint16_t(Pitched(r.pitch, rom.U32(rom.U32(entry))));
            }
        }

        if (r.trig != 0)
        {
            v.volume = r.vol;
        }
    }
}

void Output::Impl::RunStage()
{
    switch (stage)
    {
    case Stage::kUltimate:
        StageUltimate();
        break;
    case Stage::kWct:
        StageWct();
        break;
    case Stage::kRing:
        StageRing();
        break;
    case Stage::kFifo:
        StageFifo();
        break;
    }
}

void Output::Impl::StageFifo()
{
    const std::array<TrackOutput, kTracks>& rec = records;
    const auto any = [](const TrackOutput& r)
    {
        return r.trig != 0 || r.flags != 0;
    };

    const auto frequency = [this](int pitch)
    {
        return rom.U16(info.psg_freq_table + uint32_t(pitch * 2));
    };

    if (any(rec[0]))
    {
        const TrackOutput& r = rec[0];
        Store(kSound1CntH, 2, uint32_t(r.vol) << 12 | r.b2 | r.key);
        Store(kSound1CntX, 2, frequency(r.pitch));
    }

    if (any(rec[1]))
    {
        const TrackOutput& r = rec[1];
        Store(kSound2CntL, 2, uint32_t(r.vol) << 12 | r.b2 | r.key);
        Store(kSound2CntH, 2, frequency(r.pitch));
    }

    Store(kNr50, 1, sequencer->PsgVolume());
    if (any(rec[2]))
    {
        const TrackOutput& r = rec[2];
        Store(kSound3CntH, 2, rom.U16(info.wave_volume_table + uint32_t(r.vol) * 2) | r.b2);
        const uint32_t f = frequency(r.pitch);
        Store(kSound3CntX, 2, wave_keeps_restart ? f : f & 0x7FFF);
    }

    if (any(rec[3]))
    {
        const TrackOutput& r = rec[3];
        Store(kSound4CntL, 2, uint32_t(r.vol) << 12);
        Store(kSound4CntH, 2, frequency(r.pitch));
    }

    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const TrackOutput& r = rec[size_t(n + 4)];
        v.volume = r.vol;
        if (r.flags == 0)
        {
            continue;
        }

        if (r.flags & kOutDungeonDicePeriod)
        {
            fifos[size_t(n / 2)].goal = uint16_t(r.pitch);
        }
        else if (r.vol == 0)
        {
            v.flags = 0;
            v.position = 0;
        }
        else
        {
            v.key = r.b2;
            v.period = rom.U16(info.sample_period_table + uint32_t(r.pitch * 2));
            v.flags |= kFifoVoicePending;
        }
    }
}

void Output::Impl::StartNotesFifo()
{
    std::array<uint16_t, 4> starts{};
    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        if (!(v.flags & kFifoVoicePending))
        {
            continue;
        }

        v.flags = 0;
        const uint32_t entry = info.sample_table + uint32_t(v.key) * 8;
        const uint32_t word = rom.U32(entry + 4);
        uint32_t period = v.period;
        if (period == 0)
        {
            period = word & 0xFFFF;
            v.flags = kFifoVoiceSampleRate;
        }

        v.count = int32_t(word >> 20);
        v.position = rom.U32(entry);
        starts[size_t(n)] = uint16_t(0 - period);
        if (enhanced)
        {
            StartEnhanced(n, v.position, uint32_t(v.count) * kBlock, -1, false, false);
        }
    }

    for (int f = 0; f < 2; f++)
    {
        OutputVoice& first = voices[size_t(f * 2)];
        OutputVoice& second = voices[size_t(f * 2 + 1)];
        uint16_t first_start = starts[size_t(f * 2)];
        uint16_t second_start = starts[size_t(f * 2 + 1)];
        uint32_t reload = 0;
        if (first_start != 0)
        {
            if (!(second.flags & kFifoVoiceSampleRate))
            {
                second.flags = 0;
                second_start = 0;
            }

            first.flags |= kVoiceOn;
            reload = first_start;
        }

        if (second_start != 0)
        {
            if ((first.flags & kFifoVoiceSampleRate) && !(second.flags & kFifoVoiceSampleRate))
            {
                second.flags = 0;
            }
            else
            {
                second.flags |= kVoiceOn;
                reload = second_start;
            }
        }

        if (reload == 0)
        {
            continue;
        }

        Fifo& fifo = fifos[size_t(f)];
        if (!fifo.running)
        {
            // The DMA and the timer stop, and the FIFO starts again with 4 bytes of 0. FIFO B also takes timer 1.
            timers[size_t(f)] = 0;
            Store(kSoundCntHHigh, 1, sound_control_high | (f == 0 ? 0x08 : 0xF3));
            sound_control_high = (sound_control_high | (f == 0 ? 0x08 : 0xF3)) & 0x77;
            fifo.restarted = true;
            fifo.running = true;
        }

        fifo.dma_on = true;
        fifo.goal = uint16_t(0 - reload);
        fifo.period = fifo.goal;
        timers[size_t(f)] = reload | kTimerOn << 16;
    }
}

bool Output::Impl::Frame()
{
    writes.clear();
    for (Fifo& fifo : fifos)
    {
        fifo.restarted = false;
    }

    UpdateEchoUltimate();
    if (idle || !sequencer || !sequencer->Valid())
    {
        return false;
    }

    if (stop_pending)
    {
        for (TrackOutput& r : records)
        {
            // Without a mixer, a record's stop leaves the envelope that the frame before gave the same place.
            const uint16_t envelope = r.key;
            r = TrackOutput();
            r.trig = 1;
            r.flags = stage == Stage::kFifo ? kOutDungeonDiceNote : kOutStop;
            r.pan_r = 0x3F;
            r.pan_l = 0x3F;
            if (stage == Stage::kFifo)
            {
                r.key = envelope;
            }
        }

        if (echo_rules.stop_clears_feedback)
        {
            FeedbackUltimate(0, 0, false);
            FeedbackUltimate(1, 0, false);
        }

        RunStage();
        if (stage == Stage::kFifo)
        {
            StartNotesFifo();
        }

        stop_pending = false;
        idle = true;
        return true;
    }

    if (first_frame)
    {
        first_frame = false;
        RouteUltimate(0, -1, false);
        RouteUltimate(1, -1, false);
        FeedbackUltimate(0, 0, false);
        FeedbackUltimate(1, 0, false);
        PlainReset();
    }

    records = sequencer->Step();
    if (stage == Stage::kFifo)
    {
        // The wave loads and a loop's reset come in the tracks' order.
        for (const StageCommand& c : sequencer->StageCommands())
        {
            if (c.op == Op::kWave)
            {
                WaveLoadFifo(c.value);
            }
            else if (c.op == Op::kJump)
            {
                PlainReset();
            }
        }

        RunStage();
        StartNotesFifo();
        if (sequencer->Stopped())
        {
            stop_pending = true;
        }

        return true;
    }

    if (sequencer->LoopedLastFrame())
    {
        PlainReset();
    }

    RunEchoCommandsUltimate();
    if (stage == Stage::kRing)
    {
        // The frame ends with NR51's value: the OR of what the PSG tracks ask for.
        nr51 = uint32_t(records[0].pan | records[1].pan | records[2].pan | records[3].pan);
    }

    RunStage();

    if (sequencer->Stopped())
    {
        stop_pending = true;
    }

    return true;
}

int Output::Impl::Value(Scale scale, int sample, int level) const
{
    const int product = sample * level;
    switch (scale)
    {
    case Scale::kRoundUp7:
        return (product < 0 ? product + 128 : product) >> 7;
    case Scale::kDown7:
        return product >> 7;
    case Scale::kUp4:
        {
            const int value = product >> 4;
            return value < 0 ? value + 1 : value;
        }
    case Scale::kDown4:
        return product >> 4;
    case Scale::kTable:
        return rom.S8(uint32_t(rules.level_table) + uint32_t(level) * 256 + uint32_t(uint8_t(sample)));
    case Scale::kTableSigned:
        return rom.S8(uint32_t(rules.level_table) + uint32_t(level) * 256 + uint32_t(sample));
    }

    return 0;
}

int Output::Impl::NextAdpcm(OutputVoice& v, uint32_t& position, int advance)
{
    if (advance == 0)
    {
        return int8_t(v.held);
    }

    int value = v.adpcm_value;
    for (int i = 0; i < advance; i++)
    {
        const bool high = v.run & 1;
        v.run ^= 1;
        uint32_t nibble = rom.U8(position);
        if (high)
        {
            nibble >>= 4;
            position++;
        }
        nibble &= 15;

        const int step = kAdpcmStep[v.adpcm_index];
        const int change = (step * int((nibble & 7) * 2 + 1)) >> 3;
        if (nibble & 8)
        {
            value -= change;
            if (value < -32768)
            {
                value = -32768;
            }
        }
        else
        {
            value += change;
            if (value > 32767)
            {
                value = 32767;
            }
        }

        int index = v.adpcm_index + kAdpcmIndex[nibble];
        index = std::clamp(index, 0, 88);
        v.adpcm_index = uint8_t(index);
    }

    v.adpcm_value = int16_t(value);
    const int sample = value >> 8;
    v.held = uint8_t(sample);
    return sample;
}

int Output::Impl::NextPacked(OutputVoice& v, uint32_t& position, int advance)
{
    int left = advance;
    if (v.run != 0)
    {
        left = v.run - left;
        if (left > 0)
        {
            v.run = uint8_t(left);
            return int8_t(v.held);
        }

        v.run = 0;
        left = ~left;
    }

    for (;;)
    {
        const int code = rom.S8(position++);
        if (code >= (rules.coded == Coded::kPackedRuns ? -96 : -64))
        {
            // A single sample. Moving on takes the bytes that follow, and the next sample stays unread.
            left--;
            if (left >= 0)
            {
                continue;
            }

            position--;
            return rom.S8(position);
        }

        if (code >= -96)
        {
            // A pair: its high byte now, and its low byte next.
            const uint32_t pair = v.start - 64 + uint32_t(code + 96) * 2;
            const int16_t both = int16_t(rom.U16(pair));
            v.held = uint8_t(both);
            v.run = 2;
            return both >> 8;
        }

        int count = (code + 96 + 16) >= 0 ? code + 96 + 16 : code + 96 + 32;
        int sample = 0;
        const bool zeros = code + 96 + 16 >= 0;
        if (count == 0)
        {
            count = rom.U8(position++);
        }
        if (!zeros)
        {
            sample = rom.S8(position++);
        }

        v.held = uint8_t(sample);
        v.run = uint8_t(count);
        return sample;
    }
}

int Output::Impl::NextCoded(OutputVoice& v, uint32_t& position, int advance)
{
    return rules.coded == Coded::kAdpcm ? NextAdpcm(v, position, advance) : NextPacked(v, position, advance);
}

Path Output::Impl::PathOf(const OutputVoice& v) const
{
    if (rules.keeps_path && v.mix_path != uint8_t(Path::kNone))
    {
        return Path(v.mix_path);
    }

    if (v.flags & kVoiceReverse)
    {
        return Path::kReverse;
    }

    if (v.flags & kVoiceCoded)
    {
        return Path::kCoded;
    }

    if (rules.single_loop || v.right != v.left)
    {
        return Path::kUnequal;
    }

    return rules.double_loop && v.step == 0x800 ? Path::kDouble : Path::kEqual;
}

void Output::Impl::MixVoice(OutputVoice& v, int16_t* a, int16_t* b)
{
    uint32_t position = v.position;
    int32_t count = v.count;
    uint32_t fraction = v.fraction;
    const uint32_t step = v.step;
    const int level_a = v.right;
    const int level_b = v.left;
    int n = kBlock;
    int advance = 0;
    int out = 0;

    const Path path = PathOf(v);
    if (rules.keeps_path)
    {
        v.mix_path = uint8_t(path);
    }

    for (;;)
    {
        if (path == Path::kDouble)
        {
            n -= 2;
            if (n < 0)
            {
                break;
            }

            const int value = Value(rules.equal, rom.S8(position++), level_a);
            a[out] = int16_t(a[out] + value);
            b[out] = int16_t(b[out] + value);
            a[out + 1] = int16_t(a[out + 1] + value);
            b[out + 1] = int16_t(b[out + 1] + value);
            out += 2;
            count--;
        }
        else
        {
            int sample;
            int value_a;
            int value_b;
            bool crossed = false;
            switch (path)
            {
            case Path::kEqual:
                position += uint32_t(advance);
                sample = rom.S8(position);
                value_a = Value(rules.equal, sample, level_a);
                value_b = value_a;
                break;
            case Path::kReverse:
                position -= uint32_t(advance);
                sample = rom.S8(position);
                value_a = Value(rules.reverse, sample, level_a);
                value_b = rules.reverse_two_levels ? Value(rules.reverse, sample, level_b) : value_a;
                crossed = rules.reverse_crosses;
                break;
            case Path::kCoded:
                sample = NextCoded(v, position, advance);
                value_a = Value(rules.unequal, sample, level_a);
                value_b = Value(rules.unequal, sample, level_b);
                break;
            default:
                position += uint32_t(advance);
                sample = rom.S8(position);
                value_a = Value(rules.unequal, sample, level_a);
                value_b = Value(rules.unequal, sample, level_b);
                break;
            }

            bool block_done = false;
            for (;;)
            {
                n--;
                if (n < 0)
                {
                    block_done = true;
                    break;
                }

                if (crossed)
                {
                    const int16_t old_a = a[out];
                    a[out] = int16_t(b[out] + value_a);
                    b[out] = int16_t(old_a + value_b);
                }
                else
                {
                    a[out] = int16_t(a[out] + value_a);
                    b[out] = int16_t(b[out] + value_b);
                }

                out++;
                fraction += step;
                advance = int(fraction >> 12);
                if (advance != 0)
                {
                    break;
                }
            }

            if (block_done)
            {
                break;
            }

            fraction &= 0xFFF;
            count -= advance;
        }

        if (count > 0)
        {
            continue;
        }

        // The sample's end. Going backwards ends the note whatever the flags say.
        if (path == Path::kReverse || !(v.flags & kVoiceLooped))
        {
            v.flags = 0;
            v.volume = 0;
            break;
        }

        const uint32_t table = v.key & 0x8000 ? second_table : info.sample_table;
        const uint32_t header = rom.U32(table + (v.key & 0x7FFFu) * 4);
        const int32_t loop = rom.S32(header + 8);
        count = rom.S32(header + 4) - loop;
        position = (rules.loop_from_header ? header + 12 : v.start) + uint32_t(loop);
        advance = 0;
    }

    v.position = position;
    v.count = count;
    v.fraction = uint16_t(fraction);
}

void Output::Impl::MixBlock(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    // A dry sum for each FIFO, and one for each of the echo buses.
    std::array<std::array<int16_t, kBlock>, 8> sums{};
    const uint32_t routing = rules.mask_each_block ? echo[0].routing | (echo[1].routing << 1) : echo_mask;
    uint32_t mask = routing;
    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const uint32_t choice = mask & 3;
        mask >>= 2;
        if (!(v.flags & kVoiceOn) || (rules.skips_silent && v.volume == 0))
        {
            continue;
        }

        // A muted voice runs on scratch sums. A meter takes a voice's points from what it adds to the bus's sums.
        std::array<int16_t, kBlock>& to_a = sums[choice * 2];
        std::array<int16_t, kBlock>& to_b = sums[choice * 2 + 1];
        if (Muted(n))
        {
            std::array<int16_t, kBlock> unheard_a{};
            std::array<int16_t, kBlock> unheard_b{};
            MixVoice(v, unheard_a.data(), unheard_b.data());
        }
        else if (metering)
        {
            const std::array<int16_t, kBlock> before_a = to_a;
            const std::array<int16_t, kBlock> before_b = to_b;
            MixVoice(v, to_a.data(), to_b.data());
            for (size_t i = 0; i < kBlock; i++)
            {
                MeterPart(n, 0, float(int16_t(to_a[i] - before_a[i])));
                MeterPart(n, 1, float(int16_t(to_b[i] - before_b[i])));
            }
        }
        else
        {
            MixVoice(v, to_a.data(), to_b.data());
        }
    }

    const uint32_t buses = rules.has_echo ? routing >> 30 : 0;
    for (int bus = 0; bus < 2; bus++)
    {
        if (!(buses & (1u << bus)))
        {
            continue;
        }

        OutputEcho& e = echo[size_t(bus)];
        const std::array<int16_t, kBlock>& in_a = sums[size_t(2 + bus * 2)];
        const std::array<int16_t, kBlock>& in_b = sums[size_t(3 + bus * 2)];
        const uint32_t read = e.line + uint32_t(e.position) * 32;
        const uint32_t write = e.line + uint32_t(uint8_t(e.position + e.delay_blocks)) * 32;
        e.position++;
        for (uint32_t i = 0; i < kBlock; i++)
        {
            const auto wet = [&](int stored)
            {
                const int product = stored * e.feedback;
                if (rules.wet_rounds_up)
                {
                    return (product < 0 ? product + 128 : product) >> 8;
                }

                const int value = product >> 8;
                return value < 0 ? value + 1 : value;
            };

            // Without the echo, the line stays silent, and the bus outputs only its input.
            const int a = wet(work[read + i * 2 + 1]) + in_a[i];
            const int b = wet(work[read + i * 2]) + in_b[i];
            if (echo_heard)
            {
                work[write + i * 2 + 1] = int8_t(a);
                work[write + i * 2] = int8_t(b);
            }
            sums[0][i] = int16_t(sums[0][i] + a);
            sums[1][i] = int16_t(sums[1][i] + b);
        }
    }

    for (size_t i = 0; i < kBlock; i++)
    {
        fifo_a.push_back(int8_t(sums[0][i]));
        fifo_b.push_back(int8_t(sums[1][i]));
    }
}

uint32_t Output::Impl::FifoCycles(int index) const
{
    // SOUNDCNT_H chooses a FIFO's timer.
    const uint32_t word = timers[(sound_control_high >> (2 + 4 * index)) & 1];
    const uint32_t control = word >> 16;
    return control & kTimerOn ? (0x10000 - (word & 0xFFFF)) * kTimerPrescale[control & 3] : 0;
}

void Output::Impl::FifoInterrupt(int index)
{
    Fifo& fifo = fifos[size_t(index)];
    fifo.dma_on = false;

    // The first voice that sounds goes into the buffer, and the second into `second` when a first has sounded.
    std::array<int8_t, kBlock> second{};
    int active = 0;
    bool into_second = false;
    for (int n = 0; n < 2; n++)
    {
        OutputVoice& v = voices[size_t(index * 2 + n)];
        if (!(v.flags & kVoiceOn))
        {
            continue;
        }

        v.count = int32_t(uint16_t(v.count - 1));
        if (int16_t(v.count) < 0)
        {
            v.flags &= 0x5F;
            continue;
        }

        // FIFO B's voices alone play backwards. The first voice counts only with a sound to mix.
        const bool reverse = index == 1 && (v.flags & kFifoVoiceReverse);
        const int level = v.volume + 1;
        uint32_t position = v.position;
        if (!reverse)
        {
            v.position += kBlock;
        }

        if (n == 1)
        {
            active++;
        }

        std::array<int8_t, kBlock>& out = into_second ? second : fifo.buffer;
        if (level <= 1)
        {
            out.fill(0);
            continue;
        }

        for (int i = 0; i < kBlock; i++)
        {
            out[size_t(i)] = int8_t((rom.S8(position) * level) >> 5);
            position += reverse ? uint32_t(-1) : 1;
            if (metering && !Muted(index * 2 + n))
            {
                MeterPart(index * 2 + n, index, float(out[size_t(i)]));
            }
        }

        if (Muted(index * 2 + n))
        {
            out.fill(0);
        }

        if (reverse)
        {
            v.position = position;
        }

        if (n == 0)
        {
            into_second = true;
            active = 1;
        }
    }

    if (active == 0)
    {
        timers[size_t(index)] = 0;
        fifo.running = false;
        return;
    }

    if (active == 2)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            fifo.buffer[i] = int8_t(fifo.buffer[i] + second[i]);
        }
    }

    fifo.dma_on = true;
    if (fifo.goal != fifo.period)
    {
        fifo.period = fifo.goal;
        timers[size_t(index)] = ((0x10000 - uint32_t(fifo.goal)) | kTimerOn << 16);
    }
}

void Output::Impl::TickFifo(int index, std::vector<int8_t>& out)
{
    Fifo& fifo = fifos[size_t(index)];
    if (fifo.queued > 0)
    {
        fifo.queued--;
    }

    if (fifo.queued <= kBlock && fifo.dma_on)
    {
        fifo.queued += kBlock;
        out.insert(out.end(), fifo.buffer.begin(), fifo.buffer.end());
        FifoInterrupt(index);
    }
}

void Output::Impl::MixFifos(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    timer_changes.clear();
    for (int f = 0; f < 2; f++)
    {
        Fifo& fifo = fifos[size_t(f)];
        if (fifo.restarted)
        {
            fifo.queued = 4;
        }

        std::vector<int8_t>& out = f == 0 ? fifo_a : fifo_b;
        uint32_t period = FifoCycles(f);
        if (period && !fifo.ticking)
        {
            fifo.ticking = true;
            fifo.next_tick = period;
        }

        // A FIFO on the other's timer only follows it. Each timer's changes come from the FIFO whose note sets it. An
        // overflow at the frame's first cycle has already reached the hardware.
        const bool own_timer = ((sound_control_high >> (2 + 4 * f)) & 1) == uint32_t(f);
        if (own_timer && applied_periods[size_t(f)] != period)
        {
            applied_periods[size_t(f)] = period;
            timer_changes.push_back({0, f, period,
                                     !period               ? 0
                                     : fifo.next_tick == 0 ? period
                                                           : uint32_t(fifo.next_tick)});
        }

        while (period && fifo.next_tick < int64_t(agb::kFrameCycles))
        {
            TickFifo(f, out);
            const uint32_t next_period = FifoCycles(f);
            if (own_timer && next_period != period)
            {
                applied_periods[size_t(f)] = next_period;
                timer_changes.push_back({uint32_t(fifo.next_tick), f, next_period, next_period});
            }

            period = next_period;
            if (period)
            {
                fifo.next_tick += period;
            }
        }

        if (period)
        {
            fifo.next_tick -= int64_t(agb::kFrameCycles);
        }
        else
        {
            fifo.ticking = false;
        }
    }

    std::stable_sort(timer_changes.begin(), timer_changes.end(),
                     [](const FifoTimerChange& a, const FifoTimerChange& b) { return a.cycle < b.cycle; });
}

void Output::Impl::Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    fifo_a.clear();
    fifo_b.clear();
    if (metering)
    {
        voice_peaks = {};
    }

    if (enhanced)
    {
        MixEnhanced();
    }

    if (stage == Stage::kFifo)
    {
        MixFifos(fifo_a, fifo_b);
        return;
    }

    due_cycles += agb::kFrameCycles;
    const uint64_t blocks = due_cycles / block_cycles;
    due_cycles %= block_cycles;
    if (stage == Stage::kRing)
    {
        MixRing(fifo_a, fifo_b, blocks);
        return;
    }

    for (uint64_t b = 0; b < blocks; b++)
    {
        MixBlock(fifo_a, fifo_b);
    }
}

bool Output::Impl::RingEnd(OutputVoice& v)
{
    if (!(v.flags & kVoiceLooped))
    {
        v.flags = 0;
        return false;
    }

    const uint32_t table = v.key & 0x8000 ? second_table : info.sample_table;
    const uint32_t header = rom.U32(table + (v.key & 0x7FFFu) * 4);
    const uint32_t loop = rom.U32(header + 8);
    v.count = int32_t(rom.U32(header + 4) - loop);
    v.position = header + 12 + loop;
    return true;
}

void Output::Impl::MixRingPart(OutputVoice& v, int8_t* out, int samples, int level)
{
    const auto add = [&out](int value)
    {
        *out = int8_t(*out + value);
        out++;
    };

    if (v.step == 0x800)
    {
        // Two outputs for each sample byte, and the fraction stays.
        do
        {
            do
            {
                samples -= 2;
                if (samples < 0)
                {
                    return;
                }

                const int value = (rom.S8(v.position) * level) >> 4;
                v.position++;
                add(value);
                add(value);
                v.count--;
            } while (v.count > 0);
        } while (RingEnd(v));

        return;
    }

    uint32_t fraction = v.fraction;
    do
    {
        uint32_t advance = 0;
        do
        {
            v.position += advance;
            const int value = (rom.S8(v.position) * level) >> 4;
            do
            {
                samples--;
                if (samples < 0)
                {
                    v.fraction = uint16_t(fraction);
                    return;
                }

                add(value);
                fraction += v.step;
                advance = fraction >> 12;
            } while (advance == 0);

            fraction &= 0xFFF;
            v.count -= int32_t(advance);
        } while (v.count > 0);
    } while (RingEnd(v));

    v.fraction = uint16_t(fraction);
}

void Output::Impl::MixRing(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b, uint64_t blocks)
{
    if (blocks == 0)
    {
        return;
    }

    const int from = ring_position;
    for (uint64_t b = 0; b < blocks; b++)
    {
        ring_position += kBlock;
        if (ring_position >= kRingBytes)
        {
            ring_position = 0;
        }
    }

    // The part up to the ring's end, then the part from its start. A voice that ends in the first part still runs in
    // the second: the mixer checks the voice's flags once.
    const int first = ring_position > from ? ring_position - from : kRingBytes - from;
    const int second = ring_position > from ? 0 : ring_position;
    for (int f = 0; f < 2; f++)
    {
        std::vector<int8_t>& out = f == 0 ? fifo_a : fifo_b;
        out.assign(size_t(first + second), 0);
        for (int n = f * kRingVoices; n < (f + 1) * kRingVoices; n++)
        {
            OutputVoice& v = voices[size_t(n)];
            if (!(v.flags & kVoiceOn) || v.volume == 0)
            {
                continue;
            }

            const int level = v.volume + 1;
            if (!Muted(n) && !metering)
            {
                MixRingPart(v, out.data(), first, level);
                MixRingPart(v, out.data() + first, second, level);
                continue;
            }

            // A muted or metered voice mixes into a part of its own. A metered voice's part then joins the sum.
            std::vector<int8_t> part(out.size(), 0);
            MixRingPart(v, part.data(), first, level);
            MixRingPart(v, part.data() + first, second, level);
            if (Muted(n))
            {
                continue;
            }

            for (size_t i = 0; i < part.size(); i++)
            {
                MeterPart(n, f, float(part[i]));
                out[i] = int8_t(out[i] + part[i]);
            }
        }
    }
}

void Output::Impl::MeterPart(int voice, int fifo, float part)
{
    float& peak = voice_peaks[size_t(voice + 4)][size_t(fifo)];
    peak = std::max(peak, std::fabs(part));
}

void Output::Impl::StartEnhanced(int voice, uint32_t data, uint32_t size, int32_t loop, bool coded, bool reverse)
{
    EnhancedVoice& e = enhanced->voices[size_t(voice)];
    e = EnhancedVoice();
    e.data = data;
    e.size = size;
    e.loops = loop >= 0 && !reverse && uint32_t(loop) < size;
    e.loop_start = e.loops ? uint32_t(loop) : 0;
    e.reverse = reverse;
    e.starting = true;
    e.position = uint64_t(voices[size_t(voice)].fraction) << 20;
    if (coded)
    {
        e.decoded = uint16_t(DecodedSampleOf(data, size, loop) + 1);
    }
}

int Output::Impl::DecodedSampleOf(uint32_t data, uint32_t size, int32_t loop)
{
    std::vector<std::shared_ptr<const DecodedSample>>& decoded = enhanced->decoded;
    for (size_t i = 0; i < decoded.size(); i++)
    {
        if (decoded[i]->start == data)
        {
            return int(i);
        }
    }

    // The console's decoder takes each point in turn. A new pass starts at the loop's byte, with the decoder as the
    // first pass left it.
    const auto decode = [this](uint32_t from, uint32_t points, OutputVoice& v, std::vector<int8_t>& out)
    {
        uint32_t position = from;
        out.resize(points);
        for (uint32_t k = 0; k < points; k++)
        {
            out[k] = int8_t(NextCoded(v, position, k == 0 ? 0 : 1));
        }
    };

    auto sample = std::make_shared<DecodedSample>();
    sample->start = data;
    OutputVoice state;
    state.start = data;
    decode(data, size, state, sample->first);
    if (loop >= 0 && uint32_t(loop) < size)
    {
        decode(data + uint32_t(loop), size - uint32_t(loop), state, sample->loop);
    }

    decoded.push_back(std::move(sample));
    return int(decoded.size()) - 1;
}

float Output::Impl::Gain(Scale scale, int level) const
{
    switch (scale)
    {
    case Scale::kRoundUp7:
    case Scale::kDown7:
        return float(level) / 128.0f;
    case Scale::kUp4:
    case Scale::kDown4:
        return float(level) / 16.0f;
    case Scale::kTable:
    case Scale::kTableSigned:
        {
            const uint32_t row = uint32_t(rules.level_table) + uint32_t(level) * 256;
            return float(rom.S8(row + 0x7F) - rom.S8(row + 0x80)) / 255.0f;
        }
    }

    return 0.0f;
}

float Output::Impl::PointOf(const EnhancedVoice& e, int64_t point) const
{
    if (point < 0)
    {
        return 0.0f;
    }

    uint64_t at = uint64_t(point);
    const uint64_t loop_length = e.size - e.loop_start;
    if (e.decoded)
    {
        const DecodedSample& sample = *enhanced->decoded[size_t(e.decoded - 1)];
        if (at < e.size)
        {
            return float(sample.first[size_t(at)]);
        }

        return e.loops ? float(sample.loop[size_t((at - e.size) % loop_length)]) : 0.0f;
    }

    if (at >= e.size)
    {
        if (!e.loops)
        {
            return 0.0f;
        }

        at = e.loop_start + (at - e.loop_start) % loop_length;
    }

    return float(rom.S8(e.reverse ? e.data + e.size - 1 - uint32_t(at) : e.data + uint32_t(at)));
}

void Output::Impl::MixEnhancedVoice(int voice, float* a, float* b, std::array<float, 2> to, uint32_t step,
                                    uint32_t period)
{
    EnhancedVoice& e = enhanced->voices[size_t(voice)];
    const std::array<float, 2> from = e.starting ? to : e.levels;
    e.levels = to;
    e.starting = false;

    // A sample's position moves by the step for each of the console's points that the frame's time holds.
    const uint64_t move = (uint64_t(step) * agb::kFrameCycles << 20) / period;
    const uint64_t start = e.position;
    e.position += move;
    const bool heard = from[0] != 0.0f || from[1] != 0.0f || to[0] != 0.0f || to[1] != 0.0f;
    if (!enhanced->frames || !heard || Muted(voice))
    {
        return;
    }

    constexpr double kUnit = 4294967296.0;
    const size_t samples = agb::kEnhancedFrameSamples;
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * kUnit));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(start >> 32) - (width - 1);
    const int64_t last = int64_t((start + move) >> 32) + width;
    std::vector<float> input(size_t(last - first + 1));
    for (int64_t n = first; n <= last; n++)
    {
        input[size_t(n - first)] = PointOf(e, n);
    }

    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    for (size_t i = 0; i < samples; i++)
    {
        const uint64_t position = start + uint64_t(i) * move / samples;
        const int64_t point = int64_t(position >> 32);
        const float value = kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position >> 16) & 0xFFFF);
        const float part_a = value * (from[0] + float(i) * slopes[0]);
        const float part_b = value * (from[1] + float(i) * slopes[1]);
        a[i] += part_a;
        b[i] += part_b;
        if (metering)
        {
            MeterPart(voice, 0, part_a);
            MeterPart(voice, 1, part_b);
        }
    }
}

void Output::Impl::MixEnhancedBus(int bus, const OutputEcho& e)
{
    // A delay of no blocks reads the line's oldest block: the line's whole length.
    const size_t samples = agb::kEnhancedFrameSamples;
    const uint32_t blocks = e.delay_blocks ? e.delay_blocks : 256;
    const uint64_t delay = (uint64_t(blocks) * block_cycles * samples + agb::kFrameCycles / 2) / agb::kFrameCycles;
    std::vector<float>& line = enhanced->lines[size_t(bus)];
    const size_t length = enhanced->line_samples;
    size_t write = enhanced->line_position[size_t(bus)];
    const float feedback = float(e.feedback) / 256.0f;
    float* out_a = enhanced->out.data();
    float* out_b = out_a + samples;
    const float* in_a = enhanced->inputs[size_t(bus)].data();
    const float* in_b = in_a + samples;
    const auto keep = [](float value)
    {
        return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
    };

    for (size_t i = 0; i < samples; i++)
    {
        const size_t read = (write + length - size_t(delay)) % length;
        const float a = line[read] * feedback + in_a[i];
        const float b = line[length + read] * feedback + in_b[i];
        if (echo_heard)
        {
            line[write] = keep(a);
            line[length + write] = keep(b);
        }
        out_a[i] += a;
        out_b[i] += b;
        write = (write + 1) % length;
    }

    enhanced->line_position[size_t(bus)] = write;
}

void Output::Impl::MixEnhanced()
{
    const size_t samples = agb::kEnhancedFrameSamples;
    Enhanced& e = *enhanced;
    std::fill(e.out.begin(), e.out.end(), 0.0f);
    float* out_a = e.out.data();
    float* out_b = out_a + samples;
    if (stage == Stage::kFifo)
    {
        // Each FIFO's two voices move on at the FIFO's timer, a point for each overflow.
        for (int n = 0; n < voice_count; n++)
        {
            const OutputVoice& v = voices[size_t(n)];
            const int f = n / 2;
            const uint32_t period = FifoCycles(f);
            if (!(v.flags & kVoiceOn) || period == 0)
            {
                continue;
            }

            const int level = v.volume + 1;
            const float gain = level <= 1 ? 0.0f : float(level) / 32.0f;
            MixEnhancedVoice(n, out_a, out_b, {f == 0 ? gain : 0.0f, f == 1 ? gain : 0.0f}, 0x1000, period);
        }

        return;
    }

    const uint32_t period = block_cycles / kBlock;
    if (stage == Stage::kRing)
    {
        for (int n = 0; n < voice_count; n++)
        {
            const OutputVoice& v = voices[size_t(n)];
            if (!(v.flags & kVoiceOn) || v.volume == 0)
            {
                continue;
            }

            const float gain = Gain(Scale::kDown4, v.volume + 1);
            const bool on_a = n < kRingVoices;
            MixEnhancedVoice(n, out_a, out_b, {on_a ? gain : 0.0f, on_a ? 0.0f : gain}, v.step, period);
        }

        return;
    }

    // The console's mask sends each voice to the dry sums or to its bus's input.
    for (std::vector<float>& input : e.inputs)
    {
        std::fill(input.begin(), input.end(), 0.0f);
    }

    const uint32_t routing = rules.mask_each_block ? echo[0].routing | (echo[1].routing << 1) : echo_mask;
    uint32_t mask = routing;
    std::array<float, 2 * agb::kEnhancedFrameSamples> unused{};
    for (int n = voice_count - 1; n >= 0; n--)
    {
        const OutputVoice& v = voices[size_t(n)];
        const uint32_t choice = mask & 3;
        mask >>= 2;
        if (!(v.flags & kVoiceOn) || (rules.skips_silent && v.volume == 0))
        {
            continue;
        }

        // A path with one value for both sides takes the right side's level. A keeping build's path stays the note's.
        const Path path = PathOf(v);
        const Scale scale = path == Path::kReverse                          ? rules.reverse
                            : path == Path::kEqual || path == Path::kDouble ? rules.equal
                                                                            : rules.unequal;
        const float to_a = Gain(scale, v.right);
        const bool one_value =
            path == Path::kEqual || path == Path::kDouble || (path == Path::kReverse && !rules.reverse_two_levels);
        const float to_b = one_value ? to_a : Gain(scale, v.left);
        float* a = choice == 0 ? out_a : choice == 3 ? unused.data() : e.inputs[choice - 1].data();
        float* b = choice == 0 ? out_b : a + samples;
        MixEnhancedVoice(n, a, b, {to_a, to_b}, v.step, period);
    }

    if (!rules.has_echo)
    {
        return;
    }

    const uint32_t buses = routing >> 30;
    for (int bus = 0; bus < 2; bus++)
    {
        if (buses & (1u << bus))
        {
            MixEnhancedBus(bus, echo[size_t(bus)]);
        }
    }

    // A line rings on through a frame without the mode's mixing, without any of it reaching the output.
    if (!e.frames)
    {
        std::fill(e.out.begin(), e.out.end(), 0.0f);
    }
}

Output::Output(const agb::Rom& rom, const DriverInfo& info) : impl_(std::make_unique<Impl>(rom, info))
{
    // The init gives bus 0 the work buffer's first 8 KB, and buses 1 and 2 the rest.
    impl_->echo[0].has_line = true;
    impl_->echo[1].has_line = true;
    impl_->echo[2].has_line = true;
    impl_->echo[1].line = kLineBytes;
    impl_->echo[2].line = kLineBytes;
    for (OutputEcho& e : impl_->echo)
    {
        e.delay = 0x7F;
        e.delay_goal = 0x7F;
        e.delay_blocks = impl_->echo_rules.first_delay_blocks;
    }
}

Output::~Output() = default;

Output::Output(const Output& other) : impl_(std::make_unique<Impl>(*other.impl_))
{
}

Output& Output::operator=(const Output& other)
{
    impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

void Output::Play(int song, const Request& request)
{
    impl_->Play(song, request);
}

bool Output::Frame()
{
    return impl_->Frame();
}

const std::vector<agb::RegisterWrite>& Output::SoundWrites() const
{
    return impl_->writes;
}

void Output::Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    impl_->Mix(fifo_a, fifo_b);
}

const std::array<TrackOutput, kTracks>& Output::Records() const
{
    return impl_->records;
}

const std::vector<OutputVoice>& Output::Voices() const
{
    return impl_->voices;
}

const OutputEcho& Output::Echo(int bus) const
{
    return impl_->echo[size_t(bus)];
}

uint32_t Output::EchoMask() const
{
    return impl_->echo_mask;
}

double Output::Rate() const
{
    return impl_->stage == Stage::kFifo ? 0.0 : 16.0 * double(agb::kSecondCycles) / impl_->block_cycles;
}

uint32_t Output::FifoCycles(int fifo) const
{
    return impl_->stage == Stage::kFifo ? impl_->FifoCycles(fifo) : impl_->block_cycles / kBlock;
}

const Sequencer* Output::Song() const
{
    return impl_->sequencer.get();
}

bool Output::FifoRestarted(int fifo) const
{
    return impl_->fifos[size_t(fifo)].restarted;
}

const std::vector<FifoTimerChange>& Output::TimerChanges() const
{
    return impl_->timer_changes;
}

uint32_t Output::FifoLead() const
{
    switch (impl_->stage)
    {
    case Stage::kFifo:
        return 0;
    case Stage::kRing:
        return uint32_t(kRingBytes) - uint32_t(agb::kFrameCycles * kBlock / impl_->block_cycles);
    default:
        return 3 * kBlock;
    }
}

int Output::PsgChannelTrack(int channel) const
{
    return impl_->channel_tracks[size_t(channel)];
}

std::array<uint8_t, agb::kSoundRegisterBytes> Output::SoundRegisters() const
{
    const Impl& m = *impl_;
    std::array<uint8_t, agb::kSoundRegisterBytes> registers{};
    const auto set = [&registers](uint32_t address, uint8_t value)
    {
        registers[address - agb::kRegSound1CntL] = value;
    };

    // The init restarts the squares and the wave channel, without a level. The revision without a mixer restarts the
    // noise too, and loads the wave channel's first wave. A ring mixer's revision gives the wave channel full volume.
    const bool fifo = m.stage == Stage::kFifo;
    const bool ring = m.stage == Stage::kRing;
    set(kSound1CntX + 1, 0x80);
    set(kSound2CntH + 1, 0x80);
    set(kSound3CntL, fifo ? 0x80 : 0xC0);
    set(kSound3CntH + 1, ring ? 0x20 : 0x00);
    set(kSound3CntX + 1, 0x80);
    set(kSound4CntH + 1, fifo ? 0x80 : 0x00);
    set(kNr50, 0x77);
    set(kNr51, fifo || ring || m.voice_size == 20 ? 0xFF : 0xBB);
    set(kSoundCntH, 0x0E);
    set(kSoundCntHHigh, fifo || ring ? 0x33 : 0x21);
    set(kSoundCntX, 0x80);
    set(kSoundBias + 1, fifo ? 0x02 : 0x42);
    if (fifo)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            set(kWaveRam + i, m.rom.U8(m.info.wave_table + i));
        }
    }

    return registers;
}

void Output::SetMutedTracks(uint16_t tracks)
{
    impl_->muted_tracks = tracks;
}

void Output::SetEcho(bool on)
{
    // The revisions without an echo have no lines: the revision without a mixer has no work buffer.
    impl_->echo_heard = on;
    if (!on && impl_->rules.has_echo)
    {
        impl_->ClearLine(0);
        impl_->ClearLine(1);
    }
}

void Output::MeterVoices(bool on)
{
    impl_->metering = on;
    if (!on)
    {
        impl_->voice_peaks = {};
    }
}

std::array<float, 2> Output::VoicePeaks(int track) const
{
    return impl_->voice_peaks[size_t(track)];
}

void Output::StartEnhancedMixing()
{
    Impl& m = *impl_;
    Enhanced& e = m.enhanced.emplace();
    const size_t samples = agb::kEnhancedFrameSamples;
    e.voices.assign(m.voices.size(), EnhancedVoice());
    e.out.assign(2 * samples, 0.0f);
    if (m.rules.has_echo && m.stage != Stage::kFifo && m.stage != Stage::kRing)
    {
        // A line holds the console's 256 blocks, and a frame's samples more.
        e.line_samples = size_t(uint64_t(256) * m.block_cycles * samples / agb::kFrameCycles) + samples + 1;
        for (size_t bus = 0; bus < 2; bus++)
        {
            e.inputs[bus].assign(2 * samples, 0.0f);
            e.lines[bus].assign(2 * e.line_samples, 0.0f);
        }
    }
}

void Output::SetEnhancedFrames(bool on)
{
    if (impl_->enhanced)
    {
        impl_->enhanced->frames = on;
    }
}

std::span<const float> Output::EnhancedFifoA() const
{
    if (!impl_->enhanced)
    {
        return {};
    }

    return std::span<const float>(impl_->enhanced->out).first(agb::kEnhancedFrameSamples);
}

std::span<const float> Output::EnhancedFifoB() const
{
    if (!impl_->enhanced)
    {
        return {};
    }

    return std::span<const float>(impl_->enhanced->out).last(agb::kEnhancedFrameSamples);
}

bool Output::MixerSilent() const
{
    const Impl& m = *impl_;
    for (const OutputVoice& v : m.voices)
    {
        if (v.flags & kVoiceOn)
        {
            return false;
        }
    }

    return std::all_of(m.work.begin(), m.work.end(), [](int8_t byte) { return byte == 0; });
}

} // namespace ntdlib::konami
