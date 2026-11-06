// SPDX-License-Identifier: MIT

#include "ntdlib/dse/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/text.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/tables.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::dse
{

namespace
{

using ntr::snd::CommandId;

uint16_t Get16(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

uint32_t Get32(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint32_t>(Get16(bytes, at)) | static_cast<uint32_t>(Get16(bytes, at + 2)) << 16;
}

void Put32(std::span<uint8_t> bytes, std::size_t at, uint32_t value)
{
    for (std::size_t i = 0; i < 4; i++)
    {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

// The driver's arithmetic: the ARM's 32-bit operations, which wrap.
int32_t Wrap(int64_t value)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
}

int32_t Divide(int32_t dividend, int32_t divisor)
{
    if (divisor == 0)
    {
        return 0;
    }
    if (dividend == INT32_MIN && divisor == -1)
    {
        return INT32_MIN;
    }
    return dividend / divisor;
}

uint32_t DivideUnsigned(uint32_t dividend, uint32_t divisor)
{
    return divisor == 0 ? 0 : dividend / divisor;
}

// The callbacks of the LFOs' waveforms (0x020B1250). The times of an envelope without a multiplier read them past the
// end of their table.
constexpr std::array<uint32_t, 16> kWaveformCallbacks = {
    0x020753C0, 0x020753FC, 0x02075444, 0x02075498, 0x020754FC, 0x02075538, 0x02075574, 0x020755B8,
    0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC,
};

// The flags of the voice's parameters that each of an LFO's outputs changes (0x020A9FA0).
constexpr std::array<uint16_t, 8> kOutputFlags = {0, 0x10, 0x20, 0x40, 0, 0, 0, 0};

// An entry of the table of an envelope's times with a multiplier (0x020B0F50). The driver reads entries past the
// table's 128 from the table that follows it.
uint32_t ScaledTime(uint8_t index)
{
    if (index < 128)
    {
        return tables::kEnvelopeTimes[index];
    }
    const uint32_t word = tables::kEnvelopeTimesUnscaled[(index - 128) / 2];
    return index % 2 == 0 ? word & 0xFFFF : word >> 16;
}

// An entry of the table of an envelope's times without a multiplier (0x020B1050). The driver reads entries past the
// table's 128 from the tables that follow it: the callbacks of the LFOs' waveforms, the pan curve, the keys' places
// and the frequencies.
uint32_t UnscaledTime(uint8_t index)
{
    if (index < 128)
    {
        return tables::kEnvelopeTimesUnscaled[index];
    }
    std::size_t word = index - 128;
    if (word < kWaveformCallbacks.size())
    {
        return kWaveformCallbacks[word];
    }
    word -= kWaveformCallbacks.size();
    const auto bytes = [word](const uint8_t* table)
    {
        return static_cast<uint32_t>(table[4 * word]) | static_cast<uint32_t>(table[4 * word + 1]) << 8 |
               static_cast<uint32_t>(table[4 * word + 2]) << 16 | static_cast<uint32_t>(table[4 * word + 3]) << 24;
    };
    if (word < 32)
    {
        return bytes(tables::kPanCurve);
    }
    word -= 32;
    if (word < 32)
    {
        return bytes(tables::kKeyPlaces);
    }
    word -= 32;
    return static_cast<uint32_t>(tables::kSemitoneFrequencies[2 * word]) |
           static_cast<uint32_t>(tables::kSemitoneFrequencies[2 * word + 1]) << 16;
}

// The ticks of a stage of an envelope (0x02074D58), without the driver's test of 0x7F.
int32_t SlideTicks(uint8_t multiplier, uint8_t index, int16_t tick)
{
    if (multiplier == 0)
    {
        return static_cast<int32_t>(DivideUnsigned(UnscaledTime(index) * 1000, static_cast<uint32_t>(tick)));
    }
    return Divide(Wrap(int64_t{multiplier} * ScaledTime(index) * 1000), tick);
}

struct Fade
{
    int32_t current = 0;
    int32_t delta = 0;
    int32_t target = 0;
    uint16_t ticks = 0;
};

// An envelope's parameters: 16 bytes of a split, or a channel's override.
struct EnvelopeParameters
{
    static EnvelopeParameters Read(std::span<const uint8_t> bytes)
    {
        return {
            .use = bytes[0],
            .multiplier = bytes[1],
            .unknown2 = Get16(bytes, 2),
            .unknown4 = Get32(bytes, 4),
            .attack_begin = bytes[8],
            .attack_time = bytes[9],
            .decay_time = bytes[10],
            .sustain_level = bytes[11],
            .hold_time = bytes[12],
            .sustain_time = bytes[13],
            .release_time = bytes[14],
            .unknown15 = bytes[15],
        };
    }

    // 0x02074C84: every value unset.
    void Reset()
    {
        *this = {.use = 0,
                 .multiplier = 0xFF,
                 .unknown2 = 0xFFFF,
                 .unknown4 = 0xFFFFFFFF,
                 .attack_begin = 0xFF,
                 .attack_time = 0xFF,
                 .decay_time = 0xFF,
                 .sustain_level = 0xFF,
                 .hold_time = 0xFF,
                 .sustain_time = 0xFF,
                 .release_time = 0xFF,
                 .unknown15 = 0xFF};
    }

    // 0x02074CA0: the override applies while one of its values is set.
    void SetUse()
    {
        use = multiplier > 0x7F && attack_begin > 0x7F && attack_time > 0x7F && decay_time > 0x7F &&
                      sustain_level > 0x7F && hold_time > 0x7F && sustain_time > 0x7F && release_time > 0x7F
                  ? 0
                  : 1;
    }

    uint8_t use = 0;
    uint8_t multiplier = 0;
    uint16_t unknown2 = 0;
    uint32_t unknown4 = 0;
    uint8_t attack_begin = 0;
    uint8_t attack_time = 0;
    uint8_t decay_time = 0;
    uint8_t sustain_level = 0;
    uint8_t hold_time = 0;
    uint8_t sustain_time = 0;
    uint8_t release_time = 0;
    uint8_t unknown15 = 0;
};

enum EnvelopeState : uint8_t
{
    kOff = 0,
    kConstant = 1,
    kDone = 2,
    kAttack = 3,
    kHold = 4,
    kDecay = 5,
    kSustain = 6,
    kRelease = 7,
    kReleaseEnd = 8,
};

// A voice's envelope. Its volume is in 7.23 fixed point: 127 << 23 is full.
struct Envelope
{
    // 0x02074D58
    void SlideTo(int32_t target_volume, int32_t index, int16_t tick)
    {
        if (index == 0x7F)
        {
            delta = 0;
            ticks = 0x7FFFFFFF;
            return;
        }
        target = static_cast<uint8_t>(target_volume);
        ticks = SlideTicks(parameters.multiplier, static_cast<uint8_t>(index), tick);
        delta = ticks != 0 ? Divide(Wrap((int64_t{target_volume} << 23) - volume), ticks) : 0;
    }

    // 0x02074E0C: the envelope's start.
    void Start(int16_t tick)
    {
        if (parameters.use)
        {
            if (parameters.attack_time != 0)
            {
                volume = static_cast<int32_t>(static_cast<uint32_t>(parameters.attack_begin) << 23);
                state = kAttack;
                SlideTo(0x7F, parameters.attack_time, tick);
            }
            else
            {
                volume = 0x3F800000;
                if (parameters.hold_time != 0)
                {
                    SlideTo(0x7F, parameters.hold_time, tick);
                    state = kHold;
                }
                else if (parameters.decay_time != 0)
                {
                    SlideTo(static_cast<int8_t>(parameters.sustain_level), parameters.decay_time, tick);
                    state = kDecay;
                }
                else
                {
                    SlideTo(0, parameters.sustain_time, tick);
                    state = kSustain;
                }
            }
            active = 1;
        }
        else
        {
            if (state == kConstant)
            {
                return;
            }
            state = kOff;
            volume = 0x3F800000;
        }
    }

    // 0x02074ED8
    void Release(int16_t tick)
    {
        if (state == kOff)
        {
            return;
        }
        SlideTo(0, parameters.release_time, tick);
        state = kRelease;
    }

    // 0x02074F04 and 0x02074F3C
    void Stop()
    {
        state = kOff;
        volume = 0;
        ticks = 0;
        active = 0;
    }

    // 0x02074F54: the level of the tick.
    int8_t Tick(int16_t tick)
    {
        if (state > kDone)
        {
            if (ticks == 0)
            {
                volume = static_cast<int32_t>(static_cast<uint32_t>(target) << 23);
                switch (state)
                {
                case kAttack:
                    if (parameters.hold_time != 0)
                    {
                        SlideTo(0x7F, parameters.hold_time, tick);
                        state = kHold;
                        break;
                    }
                    [[fallthrough]];
                case kHold:
                    if (parameters.decay_time != 0)
                    {
                        SlideTo(static_cast<int8_t>(parameters.sustain_level), parameters.decay_time, tick);
                        state = kDecay;
                        break;
                    }
                    volume = static_cast<int32_t>(static_cast<uint32_t>(parameters.sustain_level) << 23);
                    [[fallthrough]];
                case kDecay:
                    if (parameters.sustain_time != 0)
                    {
                        SlideTo(0, parameters.sustain_time, tick);
                        state = kSustain;
                        break;
                    }
                    [[fallthrough]];
                case kSustain:
                    SlideTo(0, 0, tick);
                    state = kDone;
                    break;
                case kRelease:
                    state = kReleaseEnd;
                    volume = 0;
                    ticks = 0;
                    break;
                default:
                    break;
                }
            }
            else
            {
                int32_t next = Wrap(int64_t{volume} + delta);
                next = std::clamp(next, 0, 0x3FFFFFFF);
                ticks--;
                volume = next;
            }
        }
        return static_cast<int8_t>(volume >> 23);
    }

    EnvelopeParameters parameters;
    int32_t volume = 0;
    int32_t delta = 0;
    int32_t ticks = 0;
    uint8_t state = kOff;
    uint8_t target = 0;
    uint8_t active = 0;
};

// An LFO's settings: 16 bytes of a program, or of a channel.
// A program's bytes as the driver keeps them: the header, the LFOs, 16 other bytes and the splits.
std::vector<uint8_t> ProgramBytes(const Program& program)
{
    std::vector<uint8_t> bytes(program.header.begin(), program.header.end());
    for (const auto& lfo : program.lfos)
    {
        bytes.insert(bytes.end(), lfo.begin(), lfo.end());
    }
    bytes.insert(bytes.end(), program.filler.begin(), program.filler.end());
    for (const auto& split : program.splits)
    {
        bytes.insert(bytes.end(), split.begin(), split.end());
    }
    return bytes;
}

struct LfoSettings
{
    static LfoSettings Read(std::span<const uint8_t> bytes)
    {
        return {
            .unknown0 = bytes[0],
            .type = bytes[1],
            .output = bytes[2],
            .waveform = bytes[3],
            .amplitude = static_cast<int32_t>(Get32(bytes, 4)),
            .phase_ms = Get16(bytes, 8),
            .delay_ms = Get16(bytes, 10),
            .envelope_ms = Get16(bytes, 12),
            .unknown14 = bytes[14],
            .unknown15 = bytes[15],
        };
    }

    uint8_t unknown0 = 0;
    uint8_t type = 0;
    uint8_t output = 0;
    uint8_t waveform = 0;
    int32_t amplitude = 0;
    uint16_t phase_ms = 0;
    uint16_t delay_ms = 0;
    uint16_t envelope_ms = 0;
    uint8_t unknown14 = 0;
    uint8_t unknown15 = 0;
};

struct Lfo
{
    uint8_t phase_flags = 0;
    uint8_t type = 0;
    uint16_t update_flags = 0;
    uint16_t ticks_per_phase = 0;
    uint16_t ticks_until_phase = 0;
    int32_t output = 0;
    int32_t amplitude = 0;
    int32_t delta = 0;
    uint16_t delay = 0;
    uint16_t envelope_ticks = 0;
    int32_t envelope_level = 0;
    int32_t envelope_delta = 0;
    uint8_t output_index = 0;
    uint8_t waveform = 0;
};

// A voice's LFOs. The driver links the LFOs whose settings give a type, in their order.
struct LfoBank
{
    std::array<int16_t, 6> outputs = {};
    std::array<Lfo, 4> lfos = {};
    std::array<uint8_t, 4> list = {};
    int count = 0;
};

// The commands from 0x90 on.
enum Code : uint8_t
{
    kWaitAgain = 0x90,
    kWaitChange = 0x91,
    kWait1Byte = 0x92,
    kWait2Bytes = 0x93,
    kWait3Bytes = 0x94,
    kWaitForVoices = 0x95,
    kMainLoop = 0x99,
    kRepeatStart = 0x9C,
    kRepeatEnd = 0x9D,
    kRepeatExit = 0x9E,
    kOctave = 0xA0,
    kOctaveChange = 0xA1,
    kTempo = 0xA4,
    kTempoAgain = 0xA5,
    kBank = 0xA8,
    kBankHigh = 0xA9,
    kBankLow = 0xAA,
    kSkipByte = 0xAB,
    kProgram = 0xAC,
    kSongFade = 0xAF,
    kEnvelopeDefaults = 0xB0,
    kAttackBegin = 0xB1,
    kAttackTime = 0xB2,
    kHoldTime = 0xB3,
    kDecayAndSustain = 0xB4,
    kSustainTime = 0xB5,
    kReleaseTime = 0xB6,
    kLengthMultiplier = 0xBC,
    kLfoLevel = 0xBE,
    kDamper = 0xBF,
    kLegato = 0xC0,
    kOptionalVolume = 0xC3,
    kSkipTwo = 0xCB,
    kTuning = 0xD0,
    kTuningSemitones = 0xD1,
    kTuningSteps = 0xD2,
    kTuningWord = 0xD3,
    kTuningSlide = 0xD4,
    kRandomKeys = 0xD5,
    kJitter = 0xD6,
    kKeyBend = 0xD7,
    kUnusedWord = 0xD8,
    kBendRange = 0xDB,
    kBendLfo = 0xDC,
    kBendLfoEnvelope = 0xDD,
    kBendLfoSwitch = 0xDF,
    kVolume = 0xE0,
    kVolumeChange = 0xE1,
    kVolumeSlide = 0xE2,
    kExpression = 0xE3,
    kVolumeLfo = 0xE4,
    kVolumeLfoEnvelope = 0xE5,
    kVolumeLfoSwitch = 0xE7,
    kPan = 0xE8,
    kPanChange = 0xE9,
    kPanSlide = 0xEA,
    kPanLfo = 0xEC,
    kPanLfoEnvelope = 0xED,
    kPanLfoSwitch = 0xEF,
    kLfo = 0xF0,
    kLfoEnvelope = 0xF1,
    kLfoParameter = 0xF2,
    kLfoSwitch = 0xF3,
    kSignalGame = 0xF6,
    kSkipTwoAgain = 0xF8,
};

constexpr int kNone = -1;

struct Bank;
struct Synth;

struct Voice
{
    uint8_t Format() const
    {
        return sample[3];
    }

    uint32_t Address() const
    {
        return Get32(sample, 0x14);
    }

    uint32_t sequence = 0;
    uint16_t note_off = 0;
    uint16_t flags = 0;
    uint16_t index = 0;
    uint16_t bit = 0;
    uint8_t key = 0;
    int8_t wave = 0;
    uint8_t bend_range = 0;
    int16_t key_bend = 0;
    int16_t note_key = 0;
    int16_t key_base = 0;
    uint8_t note_volume = 0;
    int8_t wave_pan = 0x40;
    uint16_t channel_key_group = 0;
    std::array<uint8_t, 0x20> sample = {};
    Envelope envelope;
    LfoBank lfos;
    int32_t timer = 0;
    int32_t volume = 0;
    int32_t pan = 0;
    // The envelope's level in bits 0 to 7, bit 8 while the voice sounds, and the voice's priority from bit 16 on.
    uint32_t state = 0;
    // The earlier build: the ticks until the voice's note ends. That build keeps no list of held notes.
    int32_t duration = 0;
    int next = kNone;
    struct Channel* channel = nullptr;
    uint32_t note = 0;        // the number of the note that started the voice, for a performer
    int track = -1;           // the track whose note started the voice, for a performer
    int16_t jitter = 0;       // the note's random change of pitch, for a performer
    uint8_t group_voices = 0; // the voices of a channel that the key group allows, or 0 for any, for a performer
};

// A node of the driver's list of the notes that the channels hold.
struct Note
{
    uint8_t on = 0;
    uint8_t held = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    int32_t duration = 0;
    int next = kNone;
};

struct Channel
{
    uint32_t sequence = 0;
    uint8_t flags = 0;
    uint16_t update = 0; // the earlier build: the parameters that the channel's voices update next (UpdateVoices)
    uint8_t index = 0;
    uint8_t unknown6 = 0;
    uint8_t allocation = 0;
    uint8_t unknown8 = 0;
    int8_t voices = 0;
    int16_t jitter = 0;
    uint8_t random_low = 0;
    uint8_t random_high = 0;
    uint16_t bank_id = 0;
    uint16_t program = 0;
    uint16_t bend = 0;
    uint16_t volume = 0;
    int16_t pan = 0;
    Fade bend_fade;
    Fade volume_fade;
    Fade pan_fade;
    uint8_t expression = 0;
    int8_t lfo_level = 0;
    int16_t key_bend = 0;
    int16_t tuning = 0;
    int16_t unknown56 = 0;
    uint8_t optional_volume = 0;
    uint8_t bend_range = 0;
    std::array<uint8_t, 10> unknown5a = {}; // byte 7 selects the LFO that events 0xF0 to 0xF3 set
    EnvelopeParameters envelope;
    std::array<LfoSettings, 4> lfos = {};
    int voice_list = kNone;
    int held = kNone;
    Bank* bank = nullptr;
    const std::vector<uint8_t>* instrument = nullptr;
    Synth* synth = nullptr;
};

struct Synth
{
    uint8_t channel_count = 0;
    int8_t global_volume = 0;
    int8_t global_volume_index = 0;
    int16_t bend = 0;
    int8_t volume = 0;
    int8_t pan = 0;
    int8_t combined_volume = 0;
    uint8_t paused = 0;
    uint8_t unknown_a = 0;
    uint8_t voices = 0;
    uint16_t update = 0; // the earlier build: the parameters that the synth's voices update next (UpdateVoices)
    std::vector<Channel> channels;
};

// A bank as the driver keeps it.
struct Bank
{
    uint16_t id = 0;
    uint16_t main_bank = 0;
    const WaveBank* file = nullptr;
    int16_t wave_count = 0;
    int16_t program_count = 0;
    uint8_t key_group_count = 0;
    uint32_t samples = 0;
    // Each wave's entry, which the driver changes as it places the wave.
    std::vector<std::optional<std::array<uint8_t, WaveInfo::kSize>>> waves;
    // Each program's bytes, as they lie in memory: the header, the LFOs, 16 other bytes and the splits.
    std::vector<std::optional<std::vector<uint8_t>>> programs;
};

struct SubLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
    uint8_t count = 0;
    uint8_t octave = 0;
};

struct TrackState
{
    uint16_t preamble = 0;
    int8_t playing = 0;
    uint8_t channel_number = 0;
    uint8_t octave = 0;
    uint8_t multiplier = 0;
    uint8_t loop_index = 0;
    uint8_t previous_key = 0;
    int8_t ignore_notes = 0;
    uint16_t loops = 0;
    uint32_t wait = 0;
    uint32_t previous_wait = 0;
    uint32_t previous_length = 0;
    uint32_t data = 0;
    uint32_t position = 0;
    uint32_t loop_start = 0;
    std::array<SubLoop, 4> sub_loops = {};
    Channel* channel = nullptr;
};

struct Sequence
{
    int8_t initialized = 0;
    int8_t stopped = 0;
    uint8_t looped = 0;
    std::array<int8_t, 4> status = {}; // playing, active, paused and how it ends at a volume of 0
    int8_t muted = 0;
    int8_t unknown9 = 0;
    int8_t unknown_a = 0;
    int8_t unknown_b = 0;
    uint8_t fades = 0;
    uint8_t tracks_playing = 0;
    int8_t track_count = 0;
    int8_t track_slots = 0;
    int8_t channel_count = 0;
    uint8_t unknown11 = 0;
    uint8_t priority = 0;
    uint8_t global_volume_index = 0;
    uint8_t group = 0;
    uint8_t ticks_per_eighth = 0;
    uint8_t signal = 0;
    uint8_t bpm = 0;
    int16_t delay = 0;
    uint32_t id = 0;
    uint16_t handle = 0;
    int32_t reset_ticks = 0;
    int32_t driver_ticks = 0;
    int32_t sequencer_ticks = 0;
    int32_t accumulator = 0;
    int32_t step = 0;
    int32_t microseconds_per_beat = 0;
    Fade tempo;
    Fade bend;
    Fade volume;
    Fade pan;
    int8_t volume_target = 0;
    std::vector<TrackState> tracks;
    Synth synth;
    std::shared_ptr<const std::vector<uint8_t>> file; // the song's or the bank of sound effects' bytes
};

// A sequence that the driver's state owns. A copy of the state copies the sequence (Driver's copy).
class OwnedSequence
{
public:
    OwnedSequence() = default;
    explicit OwnedSequence(std::unique_ptr<Sequence> sequence) : sequence_(std::move(sequence))
    {
    }
    OwnedSequence(const OwnedSequence& other)
        : sequence_(other.sequence_ ? std::make_unique<Sequence>(*other.sequence_) : nullptr)
    {
    }
    OwnedSequence& operator=(const OwnedSequence& other)
    {
        sequence_ = other.sequence_ ? std::make_unique<Sequence>(*other.sequence_) : nullptr;
        return *this;
    }
    OwnedSequence(OwnedSequence&&) = default;
    OwnedSequence& operator=(OwnedSequence&&) = default;
    ~OwnedSequence() = default;

    explicit operator bool() const
    {
        return sequence_ != nullptr;
    }
    Sequence& operator*() const
    {
        return *sequence_;
    }
    Sequence* operator->() const
    {
        return sequence_.get();
    }
    Sequence* get() const
    {
        return sequence_.get();
    }

private:
    std::unique_ptr<Sequence> sequence_;
};

// A bank of sound effects as the driver keeps it.
struct EffectBank
{
    uint16_t id = 0;
    std::shared_ptr<const std::vector<uint8_t>> file;
    int16_t count = 0;
    std::size_t sequences = 0; // where the seq chunk's data starts, or 0 for a bank whose byte 0x35 is 0
    int8_t unknown18 = 1;
};

} // namespace

struct Driver::State
{
    // 0x0206C8F4
    int32_t Random()
    {
        auto x = static_cast<int32_t>(seed ^ seed << 17);
        x ^= x >> 15;
        seed = static_cast<uint32_t>(x);
        return x & 0x7FFF;
    }

    Bank* BankById(uint16_t id)
    {
        for (Bank& bank : banks)
        {
            if (bank.id == id)
            {
                return &bank;
            }
        }
        return nullptr;
    }

    Voice& VoiceAt(int index)
    {
        return voices[static_cast<std::size_t>(index)];
    }

    Note& NoteAt(int index)
    {
        return notes[static_cast<std::size_t>(index)];
    }

    void FreeNote(int node)
    {
        Note& note = NoteAt(node);
        note.next = free_notes;
        free_notes = node;
        note.duration = -1;
        note.held = 0;
    }

    template <typename Function>
    void ForEachVoice(Channel& channel, const Function& function)
    {
        for (int v = channel.voice_list; v != kNone;)
        {
            Voice& voice = VoiceAt(v);
            const int next = voice.next;
            function(voice);
            v = next;
        }
    }

    // The earlier build marks the channel instead. Its voices take the marks when they update.
    void FlagVoices(Channel& channel, uint16_t flags)
    {
        if (build == Build::kEarlier)
        {
            channel.update |= flags;
            return;
        }
        ForEachVoice(channel, [flags](Voice& voice) { voice.flags |= flags; });
    }

    // The earlier build marks the synth instead. The voices of all its channels take the marks when they update.
    void FlagSynth(Synth& synth, uint16_t flags)
    {
        if (build == Build::kEarlier)
        {
            synth.update |= flags;
            return;
        }
        for (Channel& channel : synth.channels)
        {
            FlagVoices(channel, flags);
        }
    }

    // Voices

    // 0x020748A0
    void FreeVoice(Voice& voice)
    {
        if (voice.channel != nullptr)
        {
            Channel& channel = *voice.channel;
            const int self = static_cast<int>(&voice - voices.data());
            if (channel.voice_list == self)
            {
                channel.voice_list = voice.next;
            }
            else
            {
                for (int v = channel.voice_list; v != kNone; v = VoiceAt(v).next)
                {
                    if (VoiceAt(v).next == self)
                    {
                        VoiceAt(v).next = voice.next;
                        break;
                    }
                }
            }
        }
        voice.note_off = 0;
        voice.channel = nullptr;
        voice.next = kNone;
        voice.envelope.Stop();
        voice.state &= 0xFF;
        voice.sequence = 0;
        voice.channel_key_group = 0;
        voice.group_voices = 0;
        start_mask &= static_cast<uint16_t>(~voice.bit);
        deallocated_mask |= voice.bit;
    }

    // 0x020749A4
    void MarkStarted(Voice& voice)
    {
        voice.flags = 0xFF0;
        voice.state |= 0x1FF;
        stop_mask &= static_cast<uint16_t>(~voice.bit);
        deallocated_mask &= static_cast<uint16_t>(~voice.bit);
        start_mask |= voice.bit;
    }

    // 0x020749F4
    void MarkStopped(Voice& voice)
    {
        voice.state &= ~uint32_t{0xFF00};
        start_mask &= static_cast<uint16_t>(~voice.bit);
        stop_mask |= voice.bit;
    }

    // 0x02074A2C
    int CountActive(Channel& channel)
    {
        int count = 0;
        ForEachVoice(channel,
                     [&count](Voice& voice)
                     {
                         if ((voice.state & 0xFF) != 0)
                         {
                             count++;
                         }
                     });
        return count;
    }

    // 0x02074618: a voice for a note of `channel` with key group `group` (its 8 bytes), owner `owner` and priority
    // `priority`.
    Voice* TakeVoice(Channel& channel, const std::array<uint8_t, 8>& group, uint16_t owner, uint8_t priority)
    {
        uint32_t threshold = (uint32_t{priority} << 16) + 0x1FF;
        const int high = std::min<int>(group[5], voice_count - 1);
        const int low = group[4];
        switch (channel.allocation)
        {
        case 0:
            if (channel.voice_list != kNone)
            {
                return &VoiceAt(channel.voice_list);
            }
            break;
        case 1:
            break;
        default:
            return nullptr;
        }

        if (group[0] != 0)
        {
            int count = 0;
            int lowest = high;
            uint32_t level = threshold;
            for (int i = low; i <= high; i++)
            {
                Voice& voice = VoiceAt(i);
                if (voice.sequence == channel.sequence && voice.channel_key_group == owner)
                {
                    count++;
                    if (level > voice.state)
                    {
                        level = voice.state;
                        lowest = i;
                    }
                    if (count >= group[2])
                    {
                        break;
                    }
                }
            }
            if (count >= group[2])
            {
                Voice& voice = VoiceAt(lowest);
                FreeVoice(voice);
                return &voice;
            }
        }

        int pick = high + 1;
        for (int i = low; i <= high; i++)
        {
            const uint32_t state = VoiceAt(i).state;
            if (threshold > state)
            {
                pick = i;
                if (state == 0)
                {
                    break;
                }
                threshold = state;
            }
        }
        if (pick > high)
        {
            return nullptr;
        }
        Voice& voice = VoiceAt(pick);
        FreeVoice(voice);
        return &voice;
    }

    // 0x02074780
    void StartVoice(Channel& channel, Voice& voice, uint8_t priority)
    {
        voice.flags = static_cast<uint16_t>((voice.flags | 1) & ~2);
        voice.state |= (uint32_t{priority} << 16) + 0x1FF;
        voice.envelope.Start(tick);
        if (voice.channel != &channel)
        {
            voice.next = channel.voice_list;
            channel.voice_list = static_cast<int>(&voice - voices.data());
            voice.channel = &channel;
        }
    }

    // 0x0207485C
    void ReleaseVoice(Channel& channel, Voice& voice)
    {
        voice.note_off |= 2;
        if (channel.flags & 1)
        {
            return;
        }
        voice.state &= 0xFF;
        voice.envelope.Release(tick);
        voice.flags &= static_cast<uint16_t>(~1);
        if (hooks.on_release && Followed() != nullptr && channel.synth == &Followed()->synth)
        {
            hooks.on_release(static_cast<int>(&voice - voices.data()));
        }
    }

    // 0x02074150: releases the voices that play `key`.
    void ReleaseVoices(Channel& channel, uint8_t key)
    {
        ForEachVoice(channel,
                     [&](Voice& voice)
                     {
                         if (voice.key == key)
                         {
                             ReleaseVoice(channel, voice);
                         }
                     });
    }

    // 0x020750B4
    void StartLfos(LfoBank& bank, const std::array<LfoSettings, 4>& settings, int8_t level)
    {
        const int32_t constant = Wrap(int64_t{Divide(int32_t{level} << 8, 127)} << 16);
        bank.count = 0;
        for (std::size_t k = 0; k < 4; k++)
        {
            const LfoSettings& s = settings[k];
            if (s.type == 0)
            {
                continue;
            }
            Lfo& lfo = bank.lfos[k];
            lfo.type = s.type;
            lfo.phase_flags = 0;
            lfo.output = 0;
            if (s.type == 1)
            {
                lfo.delay = static_cast<uint16_t>(Divide(int32_t{s.delay_ms} * 1000, tick));
                const int32_t ticks = Divide(int32_t{s.envelope_ms} * 1000, tick);
                lfo.envelope_ticks = static_cast<uint16_t>(ticks);
                if (ticks != 0)
                {
                    lfo.envelope_level = 0;
                    lfo.envelope_delta = Divide(0x1000000, ticks);
                }
                else
                {
                    lfo.envelope_level = 0x1000000;
                    lfo.envelope_delta = 0;
                }
            }
            else
            {
                lfo.delay = 0;
                lfo.envelope_ticks = 0;
                lfo.envelope_level = constant;
            }
            lfo.ticks_until_phase = 0;
            lfo.delta = 0;
            if (s.phase_ms == 0)
            {
                lfo.ticks_until_phase = 0;
                lfo.ticks_per_phase = 0;
            }
            else
            {
                int32_t per = Divide(int32_t{s.phase_ms} * 1000, tick);
                if (per == 0)
                {
                    per = 1;
                }
                lfo.ticks_per_phase = static_cast<uint16_t>(per);
                lfo.ticks_until_phase = 0;
                lfo.amplitude = Wrap(int64_t{s.amplitude} << 10);
                lfo.delta = Divide(lfo.amplitude, per);
            }
            lfo.waveform = s.waveform;
            lfo.output_index = s.output;
            lfo.update_flags = s.output < kOutputFlags.size() ? kOutputFlags[s.output] : 0;
            bank.list[static_cast<std::size_t>(bank.count++)] = static_cast<uint8_t>(k);
        }
    }

    // 0x020752AC: gives each LFO of type 3, whose level is constant, the level of event 0xBE.
    static void SetConstantLevels(LfoBank& bank, int8_t level)
    {
        for (int i = 0; i < bank.count; i++)
        {
            Lfo& lfo = bank.lfos[bank.list[static_cast<std::size_t>(i)]];
            if (lfo.type == 3)
            {
                lfo.envelope_level = Wrap(int64_t{Divide(int32_t{level} << 8, 127)} * 0x10000);
            }
        }
    }

    // The waveforms (0x020753AC to 0x020755B8).
    int32_t Waveform(Lfo& lfo)
    {
        switch (lfo.waveform)
        {
        case 0:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                lfo.output = lfo.output != 0 ? 0 : lfo.amplitude;
            }
            lfo.ticks_until_phase--;
            return lfo.output;
        case 1:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                lfo.output = (lfo.phase_flags & 2) != 0 ? Wrap(-int64_t{lfo.amplitude}) : lfo.amplitude;
                lfo.phase_flags ^= 2;
            }
            lfo.ticks_until_phase--;
            return lfo.output;
        case 2:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                if (lfo.phase_flags & 1)
                {
                    lfo.delta = Wrap(-int64_t{lfo.delta});
                }
                lfo.phase_flags |= 1;
            }
            lfo.ticks_until_phase--;
            lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
            return lfo.output;
        case 3:
            {
                uint16_t left = lfo.ticks_until_phase;
                if (left == 0)
                {
                    left = lfo.ticks_per_phase;
                    if (lfo.phase_flags & 1)
                    {
                        lfo.delta = Wrap(-int64_t{lfo.delta});
                    }
                    else
                    {
                        left /= 2;
                        if (left < 1)
                        {
                            left = 1;
                        }
                    }
                    lfo.phase_flags |= 1;
                }
                lfo.ticks_until_phase = static_cast<uint16_t>(left - 1u);
                lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
                return lfo.output;
            }
        case 4:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                lfo.output = 0;
            }
            else
            {
                lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
            }
            lfo.ticks_until_phase--;
            return lfo.output;
        case 5:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                lfo.output = lfo.amplitude;
            }
            else
            {
                lfo.output = Wrap(int64_t{lfo.output} - lfo.delta);
            }
            lfo.ticks_until_phase--;
            return lfo.output;
        case 6:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                lfo.output = Wrap(int64_t{lfo.amplitude >> 16} * Random());
            }
            lfo.ticks_until_phase--;
            return lfo.output;
        case 7:
            if (lfo.ticks_until_phase == 0)
            {
                lfo.ticks_until_phase = lfo.ticks_per_phase;
                const int32_t amplitude = lfo.amplitude;
                lfo.output = Wrap(int64_t{amplitude >> 15} * Random() - (amplitude >> 1));
            }
            lfo.ticks_until_phase--;
            return lfo.output;
        default:
            lfo.phase_flags = 0;
            lfo.type = 0;
            return 0;
        }
    }

    // 0x020752F8: returns the flags of the parameters that the LFOs change.
    uint16_t StepLfos(LfoBank& bank)
    {
        uint16_t flags = 0;
        bank.outputs = {};
        for (int i = 0; i < bank.count; i++)
        {
            Lfo& lfo = bank.lfos[bank.list[static_cast<std::size_t>(i)]];
            if (lfo.delay != 0)
            {
                lfo.delay--;
                continue;
            }
            const int32_t output = Waveform(lfo) >> 8;
            if (lfo.envelope_ticks != 0)
            {
                lfo.envelope_ticks--;
                if (lfo.envelope_ticks != 0)
                {
                    lfo.envelope_level = Wrap(int64_t{lfo.envelope_level} + lfo.envelope_delta);
                }
                else
                {
                    lfo.envelope_level = 0x1000000;
                }
            }
            const uint32_t level = static_cast<uint32_t>(output) * (static_cast<uint32_t>(lfo.envelope_level) >> 8);
            if (lfo.output_index < bank.outputs.size())
            {
                int16_t& out = bank.outputs[lfo.output_index];
                out = static_cast<int16_t>(out + (static_cast<int32_t>(level) >> 16));
            }
            flags |= lfo.update_flags;
        }
        return flags;
    }

    // Notes of a channel

    // A random key in the channel's region for `note`, while the channel has a region.
    void RandomKey(const Channel& channel, Note& note)
    {
        if (channel.random_high != 0)
        {
            const int32_t span = channel.random_high - channel.random_low + 1;
            const int32_t product = Wrap(int64_t{span} * Random());
            note.key = static_cast<uint8_t>(channel.random_low + product / 32768);
        }
    }

    // 0x02073CD8: takes a node for `note`, and gives it a random key in the channel's region.
    int HoldNote(Channel& channel, Note& note)
    {
        RandomKey(channel, note);
        const int node = free_notes;
        if (node == kNone)
        {
            return kNone;
        }
        free_notes = NoteAt(node).next;
        Note& held = NoteAt(node);
        held.on = note.on;
        held.held = 1;
        held.key = note.key;
        held.velocity = note.velocity;
        held.duration = note.duration;
        held.next = channel.held;
        channel.held = node;
        return node;
    }

    // 0x02073D68: releases the voices of the first held note with `node`'s key, and frees that note.
    void ReleaseHeldNote(Channel& channel, int node)
    {
        const uint8_t key = NoteAt(node).key;
        int* link = &channel.held;
        int found = kNone;
        for (int n = channel.held; n != kNone; n = NoteAt(n).next)
        {
            if (NoteAt(n).key == key)
            {
                ReleaseVoices(channel, key);
                *link = NoteAt(n).next;
                found = n;
                break;
            }
            link = &NoteAt(n).next;
        }
        if (found != kNone)
        {
            FreeNote(found);
        }
    }

    // 0x02073E10: a legato. The first held note with key `previous` takes `note`, and its voices take the new key.
    // The earlier build (0x02064C1C in Explorers of Time's ARM9 binary) gives the key and the note's duration to the
    // channel's first voice of key `previous` alone.
    void Legato(Channel& channel, const Note& note, uint8_t previous)
    {
        if (build == Build::kEarlier)
        {
            for (int v = channel.voice_list; v != kNone; v = VoiceAt(v).next)
            {
                Voice& voice = VoiceAt(v);
                if (voice.key == previous)
                {
                    voice.key = note.key;
                    voice.duration = note.duration;
                    voice.note_key = static_cast<int16_t>(voice.key_base + (note.key << 8));
                    voice.flags |= 0x10;
                    return;
                }
            }
            return;
        }
        for (int n = channel.held; n != kNone; n = NoteAt(n).next)
        {
            Note& held = NoteAt(n);
            if (held.key == previous)
            {
                const int next = held.next;
                held = note;
                held.held = 1;
                held.next = next;
                break;
            }
        }
        ForEachVoice(channel,
                     [&](Voice& voice)
                     {
                         if (voice.key == previous)
                         {
                             voice.key = note.key;
                             voice.note_key = static_cast<int16_t>(voice.key_base + (note.key << 8));
                             voice.flags |= 0x10;
                         }
                     });
    }

    // 0x02073EA0
    void ReleaseKey(Channel& channel, uint8_t key)
    {
        for (int n = channel.held; n != kNone; n = NoteAt(n).next)
        {
            if (NoteAt(n).key == key)
            {
                ReleaseHeldNote(channel, n);
                return;
            }
        }
    }

    // 0x020747DC: releases the held notes whose time is up. The driver's code frees such a note twice: in the release
    // of its key (0x02073D68), then again here. Its list of free notes then leads back to the note, the next notes
    // share it, and the code soon walks a list that never ends. The model stops there.
    void ReleaseEndedNotes(Channel& channel)
    {
        if (channel.flags & 3)
        {
            return;
        }
        int* link = &channel.held;
        for (int n = channel.held; n != kNone;)
        {
            Note& note = NoteAt(n);
            const int next = note.next;
            if (note.duration > 0)
            {
                *link = n;
                link = &note.next;
            }
            else
            {
                throw std::runtime_error("the damper frees an ended note twice, and the driver's lists break");
            }
            n = next;
        }
        *link = kNone;
    }

    // 0x0206530C in Explorers of Time's ARM9 binary: the earlier build releases each voice that a release has
    // marked, those that release already among them.
    void ReleaseDampedVoices(Channel& channel)
    {
        if (channel.flags & 1)
        {
            return;
        }
        ForEachVoice(channel,
                     [&](Voice& voice)
                     {
                         if (voice.note_off & 2)
                         {
                             ReleaseVoice(channel, voice);
                         }
                     });
    }

    // A split of the channel's program: 48 bytes from `at`.
    static std::span<const uint8_t> SplitAt(const std::vector<uint8_t>& program, std::size_t at)
    {
        if (at + 0x30 > program.size())
        {
            return {};
        }
        return std::span(program).subspan(at, 0x30);
    }

    // 0x0206D5C8: the next split after the one at `previous` (0 for none) that holds `key` and `velocity`.
    static std::optional<std::size_t> NextSplit(const std::vector<uint8_t>& program,
                                                std::optional<std::size_t> previous, uint8_t key, int8_t velocity)
    {
        const uint8_t count = program[2];
        if (count == 0)
        {
            return std::nullopt;
        }
        std::size_t at = 0x60;
        if (previous)
        {
            const std::span<const uint8_t> split = SplitAt(program, *previous);
            if (split.empty() || split[1] >= count - 1)
            {
                return std::nullopt;
            }
            at = *previous + 0x30;
        }
        while (true)
        {
            const std::span<const uint8_t> split = SplitAt(program, at);
            if (split.empty())
            {
                return std::nullopt;
            }
            if (key >= split[4] && key <= split[5] && velocity >= split[8] && velocity <= split[9])
            {
                return at;
            }
            if (split[1] >= count - 1)
            {
                return std::nullopt;
            }
            at += 0x30;
        }
    }

    // 0x02073EDC: starts a voice for each split that holds the note. The earlier build (0x0206496C in Explorers of
    // Time's ARM9 binary) first tests the program, then draws the jitter, then the key in the channel's region. Each of
    // its voices takes the note's duration.
    uint16_t StartNote(Channel& channel, const Note& given)
    {
        const uint32_t number = ++notes_started;
        uint16_t started = 0;
        if (build == Build::kEarlier && channel.instrument == nullptr)
        {
            return 0;
        }
        int16_t jitter = 0;
        if (channel.jitter != 0)
        {
            const int32_t product = Wrap(int64_t{channel.jitter} * 2 * Random());
            jitter = static_cast<int16_t>(product / 32768 - channel.jitter);
        }
        Note note = given;
        if (build == Build::kEarlier)
        {
            RandomKey(channel, note);
        }
        if (channel.instrument == nullptr || channel.bank == nullptr)
        {
            return 0;
        }
        const std::vector<uint8_t>& program = *channel.instrument;
        Bank& bank = *channel.bank;
        std::optional<std::size_t> at;
        while ((at = NextSplit(program, at, note.key, static_cast<int8_t>(note.velocity))))
        {
            const std::span<const uint8_t> split = SplitAt(program, *at);
            std::size_t group_index = split[0x1A];
            if (group_index >= bank.key_group_count)
            {
                group_index = 0;
            }
            std::array<uint8_t, 8> group = {};
            if (bank.file->instruments && group_index < bank.file->instruments->key_groups.size())
            {
                group = bank.file->instruments->key_groups[group_index];
            }
            const auto wave_index = static_cast<int16_t>(Get16(split, 0x12));
            if (wave_index < 0 || wave_index >= bank.wave_count ||
                static_cast<std::size_t>(wave_index) >= bank.waves.size() ||
                !bank.waves[static_cast<std::size_t>(wave_index)])
            {
                continue;
            }
            const std::array<uint8_t, WaveInfo::kSize>& wave = *bank.waves[static_cast<std::size_t>(wave_index)];
            const auto owner = static_cast<uint16_t>(split[0x1A] + (channel.index << 8));
            Voice* found = TakeVoice(channel, group, owner, group[3]);
            if (found == nullptr)
            {
                continue;
            }
            Voice& voice = *found;
            voice.note_off = 0;
            voice.sequence = channel.sequence;
            voice.channel_key_group = owner;
            voice.group_voices = group[0] != 0 ? group[2] : 0;
            voice.wave = static_cast<int8_t>(wave_index);
            std::copy_n(wave.begin() + 0x10, 0x20, voice.sample.begin());
            Put32(voice.sample, 0x14, voice.Address() + bank.samples);
            voice.key = note.key;
            voice.key_base = static_cast<int16_t>(static_cast<int16_t>(Get16(split, 0x14)) +
                                                  (static_cast<int8_t>(split[0x17]) << 8) + jitter);
            voice.note_key = static_cast<int16_t>(voice.key_base + (note.key << 8));
            voice.jitter = jitter;
            voice.wave_pan = static_cast<int8_t>(split[0x19]);
            if (velocity_mode == 7)
            {
                const int32_t product = channel.optional_volume * note.velocity * program[4] * split[0x18];
                voice.note_volume = static_cast<uint8_t>(product / (127 * 127 * 127));
            }
            else
            {
                const int32_t product = note.velocity * program[4] * split[0x18];
                voice.note_volume = static_cast<uint8_t>(product / (127 * 127));
            }
            const uint8_t range = channel.bend_range != 0 ? channel.bend_range : split[2];
            voice.key_bend = static_cast<int16_t>(Wrap(int64_t{range} * (channel.key_bend << 8)) / 8192);
            voice.bend_range = range;
            voice.duration = note.duration;
            voice.flags |= 0xFF0;
            StartLfos(voice.lfos, channel.lfos, channel.lfo_level);
            voice.envelope.parameters = EnvelopeParameters::Read(split.subspan(0x20, 16));
            voice.envelope.parameters.use = 1;
            if (channel.envelope.use != 0)
            {
                SetEnvelopeOverride(voice.envelope.parameters, channel.envelope);
            }
            StartVoice(channel, voice, group[3]);
            voice.note = number;
            voice.track = -1;
            started |= voice.bit;
        }
        return started;
    }

    // 0x02074CF4
    static void SetEnvelopeOverride(EnvelopeParameters& parameters, const EnvelopeParameters& override)
    {
        const auto set = [](uint8_t& value, uint8_t given)
        {
            if (given <= 0x7F)
            {
                value = given;
            }
        };
        set(parameters.multiplier, override.multiplier);
        set(parameters.attack_begin, override.attack_begin);
        set(parameters.attack_time, override.attack_time);
        set(parameters.decay_time, override.decay_time);
        set(parameters.sustain_level, override.sustain_level);
        set(parameters.hold_time, override.hold_time);
        set(parameters.sustain_time, override.sustain_time);
        set(parameters.release_time, override.release_time);
    }

    // Channels

    // 0x02073A90. The earlier build then stops the freed voices (0x02064770 in Explorers of Time's ARM9 binary, North
    // America).
    void FreeVoices(Channel& channel)
    {
        uint32_t freed = 0;
        while (channel.voice_list != kNone)
        {
            Voice& voice = VoiceAt(channel.voice_list);
            freed |= voice.bit;
            FreeVoice(voice);
        }
        if (build == Build::kEarlier)
        {
            commands.push_back({CommandId::kStopTimer, {freed, 0, 0, 0}});
        }
    }

    // 0x02073BC8
    void SelectProgram(Channel& channel, uint16_t index)
    {
        if (channel.bank == nullptr)
        {
            channel.instrument = nullptr;
            channel.program = 0;
        }
        else
        {
            const auto signed_index = static_cast<int16_t>(index);
            channel.program = static_cast<uint16_t>(signed_index);
            const Bank& bank = *channel.bank;
            const std::vector<uint8_t>* program = nullptr;
            if (signed_index < bank.program_count && signed_index >= 0 &&
                static_cast<std::size_t>(signed_index) < bank.programs.size() &&
                bank.programs[static_cast<std::size_t>(signed_index)])
            {
                program = &*bank.programs[static_cast<std::size_t>(signed_index)];
            }
            if (program != nullptr)
            {
                // The earlier build keeps the bend range (0x020647F8 in Explorers of Time's ARM9 binary).
                if (build != Build::kEarlier)
                {
                    channel.bend_range = 0;
                }
                for (std::size_t k = 0; k < 4; k++)
                {
                    const std::size_t at = 0x10 + 0x10 * k;
                    if (at + 16 <= program->size() && (*program)[at + 1] != 0)
                    {
                        channel.lfos[k] = LfoSettings::Read(std::span(*program).subspan(at, 16));
                    }
                    else
                    {
                        channel.lfos[k].type = 0;
                    }
                }
            }
            channel.instrument = program;
        }
        channel.envelope.Reset();
    }

    // 0x02073B90
    void SelectBank(Channel& channel, uint16_t id)
    {
        channel.bank_id = id;
        channel.bank = BankById(id);
        if (channel.bank == nullptr)
        {
            channel.instrument = nullptr;
            return;
        }
        SelectProgram(channel, channel.program);
    }

    // 0x02073C90
    void ApplyKeyBend(Channel& channel, int16_t value)
    {
        channel.key_bend = value;
        ForEachVoice(channel,
                     [value](Voice& voice)
                     {
                         voice.key_bend = static_cast<int16_t>(Wrap(int64_t{voice.bend_range} * (value << 8)) / 8192);
                         voice.flags |= 0x10;
                     });
    }

    // 0x020738DC: the channel's settings from the driver's defaults.
    void ResetChannel(Channel& channel, uint32_t id)
    {
        channel.sequence = id;
        channel.flags = 0;
        channel.unknown6 = 0;
        channel.allocation = 1;
        channel.unknown8 = 0;
        channel.voices = static_cast<int8_t>(voice_count);
        channel.jitter = 0;
        channel.random_low = 0;
        channel.random_high = 0;
        channel.bank_id =
            static_cast<uint16_t>(static_cast<int8_t>(defaults[32]) + (static_cast<int8_t>(defaults[0]) << 8));
        channel.program = 0;
        channel.bend_fade.current = 0;
        channel.bend_fade.ticks = 0;
        channel.volume_fade.current = defaults[7] << 16;
        channel.volume_fade.ticks = 0;
        channel.pan_fade.current = defaults[10] << 16;
        channel.pan_fade.ticks = 0;
        channel.tuning = 0;
        channel.expression = defaults[11];
        channel.lfo_level = static_cast<int8_t>(defaults[1]);
        channel.key_bend = 0;
        channel.unknown56 = 0;
        channel.optional_volume = 0x7F;
        channel.bend_range = 0;
        std::fill_n(channel.unknown5a.begin(), 7, uint8_t{0});
        for (LfoSettings& lfo : channel.lfos)
        {
            lfo.type = 0;
            lfo.output = 0;
        }
        channel.envelope.Reset();
        channel.voice_list = kNone;
        for (int n = channel.held; n != kNone;)
        {
            const int next = NoteAt(n).next;
            FreeNote(n);
            n = next;
        }
        channel.held = kNone;
        channel.bank = nullptr;
        channel.instrument = nullptr;
        channel.bend = 0;
        channel.volume = static_cast<uint16_t>(defaults[7] * defaults[11] * 127 / (127 * 127));
        channel.pan = 0x40;
    }

    // Synths

    void ChannelVolumes(Synth& synth)
    {
        for (Channel& channel : synth.channels)
        {
            channel.volume = static_cast<uint16_t>(
                synth.combined_volume * ((channel.volume_fade.current >> 16) * channel.expression) / (127 * 127));
        }
    }

    // 0x02073208
    void ResetSynth(Synth& synth)
    {
        synth.global_volume = global_volumes[1];
        synth.global_volume_index = 1;
        synth.bend = 0;
        synth.volume = 0x7F;
        synth.pan = 0x40;
        synth.combined_volume = static_cast<int8_t>(synth.global_volume * synth.volume / 127);
        synth.paused = 0;
        synth.unknown_a = 0;
        synth.voices = static_cast<uint8_t>(voice_count);
    }

    // 0x0207356C. The earlier build (0x02064518 in Explorers of Time's ARM9 binary) leaves the channels' volumes
    // until the voices update with a mark of the volume.
    void SelectGlobalVolume(Synth& synth, uint8_t index, int flag)
    {
        synth.global_volume = global_volumes[index & 0xF];
        synth.global_volume_index = static_cast<int8_t>(index);
        synth.combined_volume = static_cast<int8_t>(synth.global_volume * synth.volume / 127);
        if (build != Build::kEarlier)
        {
            ChannelVolumes(synth);
        }
        if (flag == 1)
        {
            FlagSynth(synth, 0x20);
        }
    }

    // 0x02073654, 0x020736E8 and 0x020737B8. The earlier build (0x02064568, 0x02064580 and 0x020645C0 in Explorers
    // of Time's ARM9 binary) only marks the synth. Its channels work out their values when the voices update.
    void SetSynthBend(Synth& synth, int16_t bend)
    {
        synth.bend = bend;
        if (build == Build::kLater)
        {
            for (Channel& channel : synth.channels)
            {
                channel.bend = static_cast<uint16_t>(synth.bend + channel.tuning +
                                                     static_cast<int16_t>(channel.bend_fade.current >> 16));
            }
        }
        FlagSynth(synth, 0x10);
    }

    void SetSynthVolume(Synth& synth, int8_t volume)
    {
        synth.volume = volume;
        synth.combined_volume = static_cast<int8_t>(synth.global_volume * synth.volume / 127);
        if (build == Build::kLater)
        {
            ChannelVolumes(synth);
        }
        FlagSynth(synth, 0x20);
    }

    void SetSynthPan(Synth& synth, uint8_t pan)
    {
        synth.pan = static_cast<int8_t>(pan);
        if (build == Build::kLater)
        {
            for (Channel& channel : synth.channels)
            {
                channel.pan = static_cast<int16_t>(synth.pan - 0x40 + ((channel.pan_fade.current >> 16) & 0xFF));
            }
        }
        FlagSynth(synth, 0x40);
    }

    // 0x0207384C
    void ResetChannels(Synth& synth, int bank, uint32_t id)
    {
        for (Channel& channel : synth.channels)
        {
            FreeVoices(channel);
            ResetChannel(channel, id);
            if (bank >= 0)
            {
                SelectBank(channel, static_cast<uint16_t>(bank));
                SelectProgram(channel, static_cast<uint16_t>(bank));
            }
        }
    }

    // 0x020733BC
    void FreeHeldNotes(Synth& synth)
    {
        for (Channel& channel : synth.channels)
        {
            for (int n = channel.held; n != kNone;)
            {
                const int next = NoteAt(n).next;
                FreeNote(n);
                n = next;
            }
            channel.held = kNone;
        }
    }

    // 0x02073480
    void FreeAllVoices(Synth& synth)
    {
        for (Channel& channel : synth.channels)
        {
            FreeVoices(channel);
        }
    }

    // 0x020734AC
    void SilenceVoices(Synth& synth, uint8_t flag)
    {
        synth.paused = flag;
        for (Channel& channel : synth.channels)
        {
            if (flag == 1)
            {
                channel.flags |= 0x40;
                ForEachVoice(channel,
                             [](Voice& voice)
                             {
                                 voice.timer = 0;
                                 voice.volume = 0;
                                 voice.flags |= 0x30;
                             });
            }
            else
            {
                FlagVoices(channel, 0x30);
                channel.flags &= static_cast<uint8_t>(~0x40);
            }
        }
    }

    // 0x02073504
    void ReplayHeldNotes(Synth& synth)
    {
        for (Channel& channel : synth.channels)
        {
            for (int n = channel.held; n != kNone; n = NoteAt(n).next)
            {
                StartNote(channel, NoteAt(n));
            }
        }
    }

    // Sequences

    uint8_t Byte(Sequence& sequence, TrackState& track, uint32_t at)
    {
        const std::vector<uint8_t>& bytes = *sequence.file;
        if (at >= bytes.size())
        {
            // The driver reads on into the memory after the file. The model stops the track there.
            track.playing = 0;
            return kEndTrack;
        }
        return bytes[at];
    }

    // 0x0206E26C
    void RestartSequence(Sequence& sequence)
    {
        ResetSynth(sequence.synth);
        ResetChannels(sequence.synth, 0, sequence.id);
        SelectGlobalVolume(sequence.synth, sequence.global_volume_index, 0);
        sequence.status = {};
        sequence.looped = 0;
        sequence.unknown9 = 1;
        sequence.tracks_playing = 0;
        sequence.reset_ticks = total_ticks;
        sequence.driver_ticks = 0;
        sequence.sequencer_ticks = 0;
        sequence.bpm = 120;
        sequence.accumulator = 0;
        sequence.step = Wrap(int64_t{sequence.ticks_per_eighth << 3} * tick);
        sequence.microseconds_per_beat = 500'000;
        sequence.fades = 0;
        sequence.tempo.current = 0x1000000;
        sequence.tempo.ticks = 0;
        sequence.bend.current = 0;
        sequence.bend.ticks = 0;
        sequence.volume.current = 0x7F0000;
        sequence.volume.ticks = 0;
        sequence.pan.current = 0x400000;
        sequence.pan.ticks = 0;
        sequence.volume_target = 0x7F;
        for (int i = 0; i < sequence.track_slots; i++)
        {
            TrackState& track = sequence.tracks[static_cast<std::size_t>(i)];
            if (track.data != 0)
            {
                track.playing = 1;
                track.loops = 0;
                track.position = track.data;
                track.wait = 0;
                track.previous_length = 0;
                track.multiplier = 0x7F;
                track.octave = 4;
                track.loop_index = 0;
                track.loop_start = 0;
                if (track.channel_number >= sequence.synth.channels.size())
                {
                    throw std::runtime_error("a track plays on a channel that the song doesn't have");
                }
                track.channel = &sequence.synth.channels[track.channel_number];
            }
            sequence.tracks_playing++;
        }
        sequence.initialized = 1;
    }

    // The ticks of a fade of `ms` milliseconds: at least 1 for a time other than 0.
    uint16_t FadeTicks(uint32_t ms) const
    {
        if (ms == 0)
        {
            return 0;
        }
        const auto ticks =
            static_cast<uint16_t>(DivideUnsigned(ms * 1000u, static_cast<uint32_t>(static_cast<int32_t>(tick))));
        return ticks == 0 ? uint16_t{1} : ticks;
    }

    // Sets `fade`'s target to `value` (16.16), and returns its ticks, or 0 where the fade sets the value at once.
    static uint16_t SetFade(Fade& fade, int32_t value, uint16_t ticks)
    {
        fade.target = value;
        uint16_t stored = ticks;
        if (ticks == 0)
        {
            fade.current = value;
        }
        else
        {
            const int32_t difference = Wrap(int64_t{value} - fade.current);
            if (difference == 0)
            {
                stored = 0;
            }
            else
            {
                fade.delta = Divide(difference, ticks);
            }
        }
        fade.ticks = stored;
        return ticks;
    }

    // 0x0206DD70: the fades of the sequence's tempo, bend, volume and pan, as `parameters` selects them.
    void StartFades(Sequence& sequence, const StartParameters& parameters)
    {
        const uint8_t flags = parameters.flags;
        if (flags & 1)
        {
            const int16_t tempo = parameters.tempo != 0 ? parameters.tempo : int16_t{0x100};
            if (SetFade(sequence.tempo, tempo * 65536, FadeTicks(parameters.tempo_ms)) != 0)
            {
                sequence.fades |= 1;
            }
            else
            {
                const auto beat = static_cast<int32_t>(static_cast<int16_t>(sequence.tempo.current >> 16) *
                                                       static_cast<int16_t>(sequence.bpm));
                uint32_t eighths = static_cast<uint32_t>(beat) >> 8;
                if (eighths == 0)
                {
                    eighths = 1;
                }
                sequence.microseconds_per_beat = static_cast<int32_t>(60'000'000u / eighths);
            }
        }
        if (flags & 2)
        {
            if (SetFade(sequence.bend, parameters.bend * 65536, FadeTicks(parameters.bend_ms)) != 0)
            {
                sequence.fades |= 2;
            }
            else
            {
                SetSynthBend(sequence.synth, static_cast<int16_t>(sequence.bend.current >> 16));
            }
        }
        if (flags & 4)
        {
            int16_t volume = parameters.volume;
            int16_t ms = static_cast<int16_t>(parameters.volume_ms);
            sequence.volume_target = static_cast<int8_t>(volume);
            if (sequence.muted == 1)
            {
                volume = 0;
                ms = 0;
            }
            const int8_t value = static_cast<int8_t>(volume);
            if (SetFade(sequence.volume, value * 65536, FadeTicks(static_cast<uint16_t>(ms))) != 0)
            {
                sequence.fades |= 4;
            }
            else
            {
                SetSynthVolume(sequence.synth, value);
            }
        }
        if (flags & 8)
        {
            const auto pan = static_cast<int8_t>(parameters.pan);
            if (SetFade(sequence.pan, pan * 65536, FadeTicks(parameters.pan_ms)) != 0)
            {
                sequence.fades |= 0x10;
            }
            else
            {
                SetSynthPan(sequence.synth, static_cast<uint8_t>(sequence.pan.current >> 16));
            }
        }
        if (sequence.delay < 0)
        {
            sequence.delay = 0;
        }
    }

    // 0x0206DB7C: starts the sequence with `parameters`, for `handle` and in `group`.
    void StartSequence(Sequence& sequence, uint16_t handle, uint8_t group, const StartParameters& parameters)
    {
        sequence.handle = handle;
        sequence.group = group;
        sequence.delay =
            parameters.delay_ms < 0 ? int16_t{-1} : static_cast<int16_t>(Divide(parameters.delay_ms * 1000, tick));
        sequence.status[3] = 0;
        StartFades(sequence, parameters);
        if (sequence.muted == 0)
        {
            ReplayHeldNotes(sequence.synth);
            sequence.status[1] = 1;
        }
        sequence.status[0] = 1;
    }

    // 0x0206E3C4: stops the sequence, or fades it out over `ms` milliseconds while its volume isn't 0. A stop at once
    // frees the voices of the sequence's channels, and with `clear` 1 the notes that they hold.
    void StopSequence(Sequence& sequence, uint32_t ms, int clear)
    {
        if (sequence.stopped == 1 || sequence.status[0] == 0)
        {
            return;
        }
        if (ms != 0 && (sequence.volume.current >> 16) != 0)
        {
            sequence.status[3] = 1;
            sequence.volume_target = 0;
            if (SetFade(sequence.volume, 0, FadeTicks(ms)) != 0)
            {
                sequence.fades |= 4;
            }
            else
            {
                SetSynthVolume(sequence.synth, 0);
            }
            return;
        }
        sequence.status = {};
        sequence.muted = 0;
        FreeAllVoices(sequence.synth);
        if (clear == 1)
        {
            FreeHeldNotes(sequence.synth);
        }
    }

    // 0x0206E85C
    static void ClearTracks(Sequence& sequence)
    {
        for (int i = 0; i < sequence.track_slots; i++)
        {
            TrackState& track = sequence.tracks[static_cast<std::size_t>(i)];
            track.playing = 0;
            track.preamble = 0;
            track.channel_number = 0;
            track.loops = 0;
            track.data = 0;
            track.position = 0;
            track.loop_start = 0;
            track.channel = nullptr;
        }
    }

    // 0x0206E6A0: a sequence with `tracks` tracks and a synth of `channels` channels.
    std::unique_ptr<Sequence> MakeSequence(uint8_t tracks, uint8_t channels)
    {
        auto sequence = std::make_unique<Sequence>();
        Sequence& q = *sequence;
        q.ticks_per_eighth = 6;
        q.track_count = static_cast<int8_t>(tracks);
        q.track_slots = static_cast<int8_t>(tracks);
        q.channel_count = static_cast<int8_t>(channels);
        q.tracks.resize(tracks);
        q.synth.channel_count = channels;
        q.synth.channels.resize(channels);
        for (std::size_t i = 0; i < q.synth.channels.size(); i++)
        {
            q.synth.channels[i].index = static_cast<uint8_t>(i);
            q.synth.channels[i].synth = &q.synth;
        }
        ResetSynth(q.synth);
        q.bpm = 120;
        q.step = Wrap(int64_t{q.ticks_per_eighth << 3} * tick);
        q.microseconds_per_beat = 500'000;
        ClearTracks(q);
        return sequence;
    }

    // 0x0206E554: the sequence's settings, from the settings at `info` in its file, and its tracks, from the track
    // chunks that follow them. Returns false for a sequence with more tracks than `sequence` holds.
    bool LoadSequence(Sequence& sequence, std::size_t info)
    {
        const std::vector<uint8_t>& bytes = *sequence.file;
        if (info + SequenceInfo::kSize > bytes.size())
        {
            throw std::runtime_error("a sequence's settings lie past the end of its file");
        }
        const std::span<const uint8_t> settings = std::span(bytes).subspan(info, SequenceInfo::kSize);
        if (settings[6] > static_cast<uint8_t>(sequence.track_count))
        {
            return false;
        }
        sequence.ticks_per_eighth = static_cast<uint8_t>(Get16(settings, 2) >> 3);
        sequence.unknown11 = settings[4];
        sequence.track_slots = static_cast<int8_t>(settings[6]);
        sequence.channel_count = static_cast<int8_t>(settings[7]);
        sequence.priority = settings[0x1B];
        sequence.global_volume_index = settings[0x19];
        sequence.group = settings[0x1A];
        sequence.unknown_b = (settings[0x18] & 0x80) ? 1 : 0;
        std::size_t at = info + SequenceInfo::kSize;
        std::size_t slot = 0;
        while (true)
        {
            if (at + 16 > bytes.size())
            {
                throw std::runtime_error("a sequence's chunks run past the end of its file");
            }
            if (std::memcmp(bytes.data() + at, "eoc ", 4) == 0)
            {
                return true;
            }
            if (std::memcmp(bytes.data() + at, "trk ", 4) == 0)
            {
                if (slot >= sequence.tracks.size())
                {
                    // The driver writes the track past those of the sequence.
                    throw std::runtime_error("a sequence has more track chunks than its settings give");
                }
                TrackState& track = sequence.tracks[slot++];
                track.data = static_cast<uint32_t>(at + 0x14);
                track.preamble = bytes[at + 0x10];
                track.channel_number = bytes[at + 0x11];
            }
            const uint32_t alignment = bytes[at + 8];
            if (alignment == 0)
            {
                // The driver's walk of the chunks then stays on the chunk.
                throw std::runtime_error("a chunk's alignment is 0");
            }
            at += (Get32(bytes, at + 12) + 16 + alignment - 1) & ~(alignment - 1);
        }
    }

    EffectBank* EffectBankById(uint16_t id)
    {
        for (EffectBank& bank : effect_banks)
        {
            if (bank.id == id)
            {
                return &bank;
            }
        }
        return nullptr;
    }

    // 0x0206EFB0: where the settings of effect `index` of `bank` lie in the bank's file, or nothing.
    static std::optional<std::size_t> EffectAt(const EffectBank& bank, int16_t index)
    {
        if (index < 0 || bank.count <= index)
        {
            return std::nullopt;
        }
        const std::vector<uint8_t>& bytes = *bank.file;
        const bool chunks = bytes[0x35] != 0;
        const std::size_t table = chunks ? bank.sequences : 0x40;
        const std::size_t base = chunks ? bank.sequences : 0;
        const std::size_t entry = table + 2 * static_cast<std::size_t>(index);
        if (entry + 2 > bytes.size())
        {
            throw std::runtime_error("a bank's table of sound effects runs past the end of its file");
        }
        const uint16_t offset = Get16(bytes, entry);
        if (offset == 0)
        {
            return std::nullopt;
        }
        return base + offset;
    }

    // 0x0206EFF8 and 0x0206F100: whether the instances that `matches` selects among the pool's sequences that play
    // refuse a new one: `most` or more of them. With `stop` 1, the driver stops the oldest of them instead, until fewer
    // than `most` play, and refuses nothing.
    template <typename Match>
    bool TooManyInstances(const Match& matches, uint8_t most, int stop)
    {
        if (most == 0)
        {
            return false;
        }
        if (stop == 1)
        {
            Sequence* oldest = nullptr;
            int count = 0;
            do
            {
                count = 0;
                uint32_t lowest = 0x7FFFFFFF;
                for (const OwnedSequence& q : effects)
                {
                    if (q->status[0] == 1 && matches(*q))
                    {
                        count++;
                        if (lowest > static_cast<uint32_t>(q->reset_ticks))
                        {
                            lowest = static_cast<uint32_t>(q->reset_ticks);
                            oldest = q.get();
                        }
                    }
                }
                if (count >= most && oldest != nullptr)
                {
                    StopSequence(*oldest, 0, 1);
                }
            } while (count > most);
            return false;
        }
        int count = 0;
        for (const OwnedSequence& q : effects)
        {
            if (q->status[0] == 1 && matches(*q))
            {
                count++;
            }
        }
        return count >= most;
    }

    // 0x0206F208: whether a sequence of the pool that plays has `handle`. With `stop` 1, the driver stops each such
    // sequence instead, and the function returns false.
    bool HandlePlays(uint16_t handle, int stop)
    {
        for (const OwnedSequence& q : effects)
        {
            if (q->status[0] == 1 && q->handle == handle)
            {
                if (stop != 1)
                {
                    return true;
                }
                StopSequence(*q, 0, 1);
            }
        }
        return false;
    }

    // 0x0206F2C4: the first sequence of the pool that doesn't play, else the oldest of those whose priority is at
    // least `priority`, or none.
    Sequence* SequenceForEffect(uint8_t priority)
    {
        Sequence* found = nullptr;
        uint32_t lowest = 0xFFFFFFFF;
        for (const OwnedSequence& q : effects)
        {
            if (q->status[0] == 0)
            {
                return q.get();
            }
            if (q->priority >= priority && lowest > static_cast<uint32_t>(q->reset_ticks))
            {
                lowest = static_cast<uint32_t>(q->reset_ticks);
                found = q.get();
            }
        }
        return found;
    }

    // 0x02071784
    void StepFades(Sequence& sequence)
    {
        const auto step = [](Fade& fade)
        {
            fade.ticks--;
            fade.current = fade.ticks == 0 ? fade.target : Wrap(int64_t{fade.current} + fade.delta);
        };
        if (sequence.tempo.ticks != 0)
        {
            step(sequence.tempo);
            uint32_t beat = static_cast<uint32_t>((sequence.tempo.current >> 16) * sequence.bpm) >> 8;
            if (beat == 0)
            {
                beat = 1;
            }
            sequence.microseconds_per_beat = static_cast<int32_t>(60'000'000u / beat);
            if (sequence.tempo.ticks == 0)
            {
                sequence.fades &= static_cast<uint8_t>(~1);
            }
        }
        if (sequence.bend.ticks != 0)
        {
            step(sequence.bend);
            SetSynthBend(sequence.synth, static_cast<int16_t>(sequence.bend.current >> 16));
            if (sequence.bend.ticks == 0)
            {
                sequence.fades &= static_cast<uint8_t>(~2);
            }
        }
        if (sequence.volume.ticks != 0)
        {
            step(sequence.volume);
            SetSynthVolume(sequence.synth, static_cast<int8_t>(sequence.volume.current >> 16));
            if (sequence.volume.ticks == 0)
            {
                sequence.fades &= static_cast<uint8_t>(~4);
            }
        }
        if (sequence.pan.ticks != 0)
        {
            step(sequence.pan);
            SetSynthPan(sequence.synth, static_cast<uint8_t>(sequence.pan.current >> 16));
            if (sequence.pan.ticks == 0)
            {
                sequence.fades &= static_cast<uint8_t>(~0x10);
            }
        }
    }

    // 0x02071160. The earlier build (0x02062A00 in Explorers of Time's ARM9 binary) counts down each voice's
    // duration instead, and releases the voice when the count reaches 0. A voice that starts with 0 never releases.
    void StepNoteLengths(Sequence& sequence)
    {
        if (build == Build::kEarlier)
        {
            for (Channel& channel : sequence.synth.channels)
            {
                ForEachVoice(channel,
                             [&](Voice& voice)
                             {
                                 if (!(channel.flags & 2) && voice.duration > 0 && --voice.duration == 0)
                                 {
                                     ReleaseVoice(channel, voice);
                                 }
                             });
            }
            return;
        }
        for (Channel& channel : sequence.synth.channels)
        {
            int* link = &channel.held;
            for (int n = channel.held; n != kNone;)
            {
                Note& note = NoteAt(n);
                const int next = note.next;
                int32_t left = note.duration - 1;
                if (left <= 0)
                {
                    if (channel.flags & 3)
                    {
                        left = 0;
                    }
                    else
                    {
                        ReleaseVoices(channel, note.key);
                        FreeNote(n);
                        left = -1;
                    }
                }
                note.duration = left;
                if (left >= 0)
                {
                    *link = n;
                    link = &note.next;
                }
                n = next;
            }
            *link = kNone;
        }
    }

    void RunEvent(Sequence& sequence, TrackState& track, uint8_t code, uint32_t& at);

    // 0x02071224
    void RunTrack(Sequence& sequence, TrackState& track)
    {
        Channel& channel = *track.channel;
        uint32_t at = track.position;
        if (track.wait == 0)
        {
            for (int commands = 0;; commands++)
            {
                if (commands == kCommandBudget)
                {
                    throw Hang(static_cast<int>(&track - sequence.tracks.data()), at);
                }
                const uint32_t start = at;
                const uint8_t code = Byte(sequence, track, at++);
                if (track.playing == 0)
                {
                    break;
                }
                if (hooks.on_event && &sequence == Followed())
                {
                    const std::vector<uint8_t>& bytes = *sequence.file;
                    const int arguments = ArgumentBytes(code, at < bytes.size() ? bytes[at] : 0);
                    hooks.on_event({.track = static_cast<int>(&track - sequence.tracks.data()),
                                    .offset = start,
                                    .next = at + static_cast<uint32_t>(std::max(arguments, 0)),
                                    .code = code,
                                    .repeats = track.loop_index});
                }
                if (code >= 0x90)
                {
                    RunEvent(sequence, track, code, at);
                }
                else if (code >= 0x80)
                {
                    track.previous_wait = tables::kPauseLengths[code - 0x80];
                    track.wait = track.previous_wait;
                }
                else
                {
                    Note note;
                    note.velocity = code;
                    const uint8_t b = Byte(sequence, track, at);
                    track.octave = static_cast<uint8_t>(static_cast<int8_t>(track.octave + (((b >> 4) & 3) - 2)));
                    note.key = static_cast<uint8_t>(static_cast<int8_t>(track.octave) * 12 + (b & 0xF));
                    int count = b >> 6;
                    at++;
                    uint32_t length = track.previous_length;
                    if (count != 0)
                    {
                        length = 0;
                        for (; count != 0; count--)
                        {
                            length = Byte(sequence, track, at++) + (length << 8);
                        }
                        track.previous_length = length;
                    }
                    note.duration = static_cast<int32_t>(length * track.multiplier / 127);
                    note.on = 0;
                    if (track.ignore_notes == 0)
                    {
                        NoteEvent event{.track = static_cast<int>(&track - sequence.tracks.data()),
                                        .channel = channel.index,
                                        .key = note.key,
                                        .velocity = note.velocity,
                                        .offset = start,
                                        .length = static_cast<uint32_t>(note.duration)};
                        if (channel.flags & 2)
                        {
                            Legato(channel, note, track.previous_key);
                            event.legato = true;
                        }
                        else if (build == Build::kEarlier)
                        {
                            // 0x02062B98 in Explorers of Time's ARM9 binary: the earlier build releases no voice
                            // of the key first.
                            channel.bend_fade.current = 0;
                            event.voices = StartNote(channel, note);
                            event.number = notes_started;
                        }
                        else
                        {
                            ReleaseKey(channel, note.key);
                            channel.bend_fade.current = 0;
                            const int node = HoldNote(channel, note);
                            if (node != kNone)
                            {
                                event.voices = StartNote(channel, NoteAt(node));
                                event.number = notes_started;
                            }
                        }
                        for (Voice& voice : voices)
                        {
                            if (event.voices & voice.bit)
                            {
                                voice.track = event.track;
                            }
                        }
                        if (hooks.on_note && &sequence == Followed())
                        {
                            hooks.on_note(event);
                        }
                    }
                    track.previous_key = note.key;
                    channel.flags &= static_cast<uint8_t>(~2);
                }
                if (track.playing == 0 || track.wait != 0)
                {
                    break;
                }
            }
        }
        track.wait--;
        track.position = at;
    }

    // 0x020713E8. The earlier build (0x02062C14 in Explorers of Time's ARM9 binary) has no delay, and steps the fades
    // on every tick. It stops a sequence that fades out before the fades step, and runs the rest of the step.
    void StepSequence(Sequence& sequence)
    {
        if (sequence.status[1] != 1)
        {
            return;
        }
        if (build == Build::kEarlier)
        {
            if (sequence.status[3] == 1 && sequence.volume.current == 0)
            {
                sequence.status = {};
                FreeAllVoices(sequence.synth);
            }
        }
        else if (sequence.delay != 0)
        {
            if (sequence.delay > 0)
            {
                sequence.delay--;
            }
            return;
        }
        sequence.driver_ticks++;
        if (sequence.fades != 0 || build == Build::kEarlier)
        {
            StepFades(sequence);
        }
        sequence.accumulator = Wrap(int64_t{sequence.accumulator} + sequence.step);
        while (sequence.accumulator > sequence.microseconds_per_beat)
        {
            if (hooks.on_tick && &sequence == Followed())
            {
                hooks.on_tick();
            }
            StepNoteLengths(sequence);
            Synth& synth = sequence.synth;
            for (Channel& channel : synth.channels)
            {
                if (channel.bend_fade.ticks != 0)
                {
                    channel.bend_fade.ticks--;
                    channel.bend_fade.current =
                        channel.bend_fade.ticks == 0
                            ? channel.bend_fade.target
                            : Wrap(int64_t{channel.bend_fade.current} + channel.bend_fade.delta);
                    channel.bend =
                        static_cast<uint16_t>(channel.tuning + (channel.bend_fade.current >> 16) + synth.bend);
                    FlagVoices(channel, 0x10);
                }
                if (channel.volume_fade.ticks != 0)
                {
                    channel.volume_fade.ticks--;
                    channel.volume_fade.current =
                        channel.volume_fade.ticks == 0
                            ? channel.volume_fade.target
                            : Wrap(int64_t{channel.volume_fade.current} + channel.volume_fade.delta);
                    const int32_t product = Wrap(int64_t{synth.combined_volume} *
                                                 ((channel.volume_fade.current >> 16) * channel.expression));
                    channel.volume = static_cast<uint16_t>(product / (127 * 127));
                    FlagVoices(channel, 0x20);
                }
                if (channel.pan_fade.ticks != 0)
                {
                    channel.pan_fade.ticks--;
                    channel.pan_fade.current = channel.pan_fade.ticks == 0
                                                   ? channel.pan_fade.target
                                                   : Wrap(int64_t{channel.pan_fade.current} + channel.pan_fade.delta);
                    channel.pan = static_cast<int16_t>(synth.pan - 0x40 + (channel.pan_fade.current >> 16));
                    FlagVoices(channel, 0x40);
                }
            }
            sequence.tracks_playing = 0;
            for (int i = 0; i < sequence.track_slots; i++)
            {
                TrackState& track = sequence.tracks[static_cast<std::size_t>(i)];
                if (track.playing == 1)
                {
                    RunTrack(sequence, track);
                    if (track.playing == 1)
                    {
                        sequence.tracks_playing++;
                    }
                }
            }
            sequence.sequencer_ticks++;
            sequence.accumulator = Wrap(int64_t{sequence.accumulator} - sequence.microseconds_per_beat);
        }

        if (sequence.tracks_playing == 0)
        {
            sequence.status = {};
            FreeHeldNotes(sequence.synth);
        }
        else if (build == Build::kLater && sequence.status[3] != 0 && sequence.volume.current == 0)
        {
            if (sequence.status[3] == 1)
            {
                sequence.status = {};
                FreeAllVoices(sequence.synth);
                if (sequence.unknown_a == 1)
                {
                    sequence.tracks_playing = 0;
                }
            }
            else
            {
                sequence.status[2] = 1;
                SilenceVoices(sequence.synth, 1);
            }
        }
    }

    // The voices' steps of a tick

    // 0x0207448C
    void StepVoices()
    {
        for (int i = 0; i < voice_count; i++)
        {
            Voice& voice = VoiceAt(i);
            const int8_t level = voice.envelope.Tick(tick);
            if (static_cast<int8_t>(voice.envelope.active) == 1)
            {
                voice.flags |= StepLfos(voice.lfos);
                if (voice.envelope.state == kReleaseEnd)
                {
                    voice.envelope.state = kDone;
                    voice.flags |= 2;
                }
                if ((voice.state & 0xFF) != static_cast<uint8_t>(level))
                {
                    voice.flags |= 0x20;
                }
            }
            voice.state = (voice.state & ~uint32_t{0xFF}) | static_cast<uint8_t>(level);
        }
    }

    // 0x02064C74 in Explorers of Time's ARM9 binary: the earlier build works out a channel's pitch, volume and pan as
    // its voices update, for the parameters that the channel's and the synth's marks name. Each of the channel's voices
    // takes the marks.
    void UpdateChannels(Synth& synth)
    {
        for (Channel& channel : synth.channels)
        {
            const uint16_t update = channel.update | synth.update;
            if (update == 0)
            {
                continue;
            }
            if (update & 0x10)
            {
                channel.bend = static_cast<uint16_t>(synth.bend + channel.tuning +
                                                     static_cast<int16_t>(channel.bend_fade.current >> 16));
            }
            if (update & 0x20)
            {
                const int32_t product =
                    Wrap(int64_t{synth.combined_volume} * ((channel.volume_fade.current >> 16) * channel.expression));
                channel.volume = static_cast<uint16_t>(product / (127 * 127));
            }
            if (update & 0x40)
            {
                channel.pan = static_cast<int16_t>(synth.pan - 0x40 + (channel.pan_fade.current >> 16));
            }
            ForEachVoice(channel, [update](Voice& voice) { voice.flags |= update; });
            channel.update = 0;
        }
        synth.update = 0;
    }

    // 0x02074194
    void UpdateVoices()
    {
        if (build == Build::kEarlier)
        {
            if (song)
            {
                UpdateChannels(song->synth);
            }
            for (const OwnedSequence& effect : effects)
            {
                UpdateChannels(effect->synth);
            }
        }
        for (int i = 0; i < voice_count; i++)
        {
            Voice& voice = VoiceAt(i);
            if (voice.channel == nullptr || voice.flags == 0)
            {
                continue;
            }
            const Channel& channel = *voice.channel;
            if (voice.flags & 0x10)
            {
                const auto pitch =
                    static_cast<uint16_t>(channel.bend + voice.note_key + voice.key_bend + voice.lfos.outputs[1]);
                voice.timer = VoiceTimer(pitch, voice.Format());
            }
            if (voice.flags & 0x20)
            {
                const int32_t level = std::clamp(voice.note_volume + (voice.lfos.outputs[2] >> 6), 0, 0x7F);
                const int32_t product = Wrap(int64_t{channel.volume} * level * (voice.state & 0xFF));
                const int32_t scaled = product / 8032;
                voice.volume = Wrap(int64_t{scaled} * scaled) / 512;
            }
            if (voice.flags & 0x40)
            {
                int32_t pan = voice.wave_pan - 0x40 + channel.pan + (voice.lfos.outputs[3] >> 6);
                pan = std::clamp(pan, 0, 0x7F);
                voice.pan = tables::kPanCurve[pan];
            }
        }
    }

    // 0x02074A58, and 0x0206553C in Explorers of Time's ARM9 binary for the earlier build
    void WriteCommands(ntr::snd::CommandList& list)
    {
        for (int i = 0; i < voice_count; i++)
        {
            Voice& voice = VoiceAt(i);
            const uint16_t flags = voice.flags;
            if (flags == 0)
            {
                continue;
            }
            if (flags & 1)
            {
                const uint32_t loop = static_cast<int8_t>(voice.sample[5]) != 0 ? 1 : 2;
                const auto timer = static_cast<uint32_t>(voice.timer);
                const auto volume = static_cast<uint32_t>(voice.volume);
                const auto pan = static_cast<uint32_t>(voice.pan);
                switch (voice.Format())
                {
                case 0:
                case 1:
                case 2:
                    list.push_back(
                        {CommandId::kSetUpPcmChannel,
                         {voice.index | timer << 16, voice.Address(), Get32(voice.sample, 0x1C) | volume << 24,
                          Get32(voice.sample, 0x18) | pan << 16 | uint32_t{voice.Format()} << 24 | loop << 26}});
                    break;
                case 3:
                    list.push_back(
                        {CommandId::kSetUpPsgChannel, {voice.index, volume, pan | timer << 8, voice.sample[9]}});
                    break;
                case 4:
                    list.push_back({CommandId::kSetUpNoiseChannel, {voice.index, volume, pan | timer << 8, 0}});
                    break;
                default:
                    break;
                }
                MarkStarted(voice);
            }
            else
            {
                uint16_t update = flags;
                if (update & 2)
                {
                    MarkStopped(voice);
                    update = 0;
                }
                if (update & 0x10)
                {
                    list.push_back({CommandId::kChannelTimer, {voice.bit, static_cast<uint32_t>(voice.timer), 0, 0}});
                }
                if (update & 0x20)
                {
                    list.push_back({CommandId::kChannelVolume, {voice.bit, static_cast<uint32_t>(voice.volume), 0, 0}});
                }
                if (update & 0x40)
                {
                    list.push_back({CommandId::kChannelPan, {voice.bit, static_cast<uint32_t>(voice.pan), 0, 0}});
                }
            }
            voice.flags = 0;
        }

        last_start_mask = start_mask;
        if (start_mask != 0 || build == Build::kEarlier)
        {
            list.push_back({CommandId::kStartTimer, {start_mask, 0, 0, 0}});
            start_mask = 0;
        }
        // The earlier build keeps a freed voice in the stops of the lists that follow until the voice starts again.
        if ((stop_mask | deallocated_mask) != 0 || build == Build::kEarlier)
        {
            list.push_back({CommandId::kStopTimer, {uint32_t{stop_mask} | deallocated_mask, 0, 0, 0}});
            if (build == Build::kLater)
            {
                deallocated_mask = 0;
            }
            stop_mask = 0;
        }
    }

    // 0x0207457C
    void CheckEndedChannels(uint16_t channel_status)
    {
        auto status = static_cast<uint16_t>(channel_status | last_start_mask);
        for (int i = 0; i < voice_count; i++)
        {
            Voice& voice = VoiceAt(i);
            if (static_cast<int8_t>(voice.envelope.active) == 1)
            {
                if (!(status & 1))
                {
                    voice.envelope.Stop();
                    voice.state &= ~uint32_t{0xFFFF};
                }
            }
            else if (status & 1)
            {
                MarkStopped(voice);
            }
            status = static_cast<uint16_t>(status >> 1);
        }
    }

    // After a copy of `from`: points the copy's pointers at the copy's channels, synths, banks and programs where they
    // point at from's.
    void Repoint(const State& from)
    {
        std::map<const Channel*, Channel*> channels;
        std::map<const Synth*, Synth*> synths;
        const auto pair_sequences = [&](const Sequence& old_one, Sequence& new_one)
        {
            synths[&old_one.synth] = &new_one.synth;
            for (std::size_t i = 0; i < old_one.synth.channels.size(); i++)
            {
                channels[&old_one.synth.channels[i]] = &new_one.synth.channels[i];
            }
        };
        if (from.song)
        {
            pair_sequences(*from.song, *song);
        }
        for (std::size_t i = 0; i < from.effects.size(); i++)
        {
            pair_sequences(*from.effects[i], *effects[i]);
        }

        std::map<const Bank*, Bank*> kept_banks;
        std::map<const std::vector<uint8_t>*, const std::vector<uint8_t>*> programs;
        auto new_one = banks.begin();
        for (auto old_one = from.banks.begin(); old_one != from.banks.end(); ++old_one, ++new_one)
        {
            kept_banks[&*old_one] = &*new_one;
            for (std::size_t i = 0; i < old_one->programs.size(); i++)
            {
                if (old_one->programs[i])
                {
                    programs[&*old_one->programs[i]] = &*new_one->programs[i];
                }
            }
        }

        const auto find = [](const auto& map, auto* pointer)
        {
            return pointer == nullptr ? nullptr : map.at(pointer);
        };
        for (Voice& voice : voices)
        {
            voice.channel = find(channels, voice.channel);
        }
        const auto repoint_sequence = [&](Sequence& sequence)
        {
            for (Channel& channel : sequence.synth.channels)
            {
                channel.bank = find(kept_banks, channel.bank);
                channel.instrument = find(programs, channel.instrument);
                channel.synth = find(synths, channel.synth);
            }
            for (TrackState& track : sequence.tracks)
            {
                track.channel = find(channels, track.channel);
            }
        };
        if (song)
        {
            repoint_sequence(*song);
        }
        for (const OwnedSequence& effect : effects)
        {
            repoint_sequence(*effect);
        }
    }

    Build build = Build::kLater;
    ntr::snd::CommandList commands; // the commands since the last list, which the next list starts with
    int16_t tick = Driver::kTickMicroseconds;
    int32_t total_ticks = 0;
    uint32_t seed = 0xBC614E;
    uint8_t velocity_mode = 2;
    std::array<int8_t, 16> global_volumes = {};
    std::array<uint8_t, 128> defaults = {};
    std::array<Note, 128> notes = {};
    int free_notes = kNone;
    std::array<Voice, Driver::kVoices> voices = {};
    int voice_count = Driver::kVoices;
    uint16_t last_start_mask = 0;
    uint16_t start_mask = 0;
    uint16_t stop_mask = 0;
    uint16_t deallocated_mask = 0;
    std::list<Bank> banks;
    std::list<EffectBank> effect_banks;
    OwnedSequence song;
    std::vector<OwnedSequence> effects; // the pool, in the order of the driver's list

    // For a performer: the sequence that it follows, its hooks, and the notes that have started.
    int followed = kSong;
    SequenceHooks hooks;
    uint32_t notes_started = 0;

    Sequence* Followed() const
    {
        return followed == kSong ? song.get() : effects.at(static_cast<std::size_t>(followed)).get();
    }
};

// The handlers of the commands, from the table at 0x020B0B90. `at` points past the code, and each handler moves it past
// what it reads.
void Driver::State::RunEvent(Sequence& sequence, TrackState& track, uint8_t code, uint32_t& at)
{
    Channel& channel = *track.channel;
    Synth& synth = sequence.synth;
    const auto next = [&]()
    {
        return Byte(sequence, track, at++);
    };
    const auto next16 = [&]()
    {
        const uint8_t low = next();
        return static_cast<uint16_t>(low | next() << 8);
    };
    const auto update_bend = [&]()
    {
        channel.bend =
            static_cast<uint16_t>(channel.tuning + static_cast<int16_t>(channel.bend_fade.current >> 16) + synth.bend);
        FlagVoices(channel, 0x10);
    };
    const auto update_volume = [&](int32_t volume)
    {
        channel.volume =
            static_cast<uint16_t>(Wrap(int64_t{synth.combined_volume} * (volume * channel.expression)) / (127 * 127));
        FlagVoices(channel, 0x20);
    };
    const auto set_fade = [&](Fade& fade)
    {
        const uint16_t ticks = next16();
        const int32_t target = static_cast<int8_t>(next()) << 16;
        fade.target = target;
        uint16_t stored = ticks;
        if (ticks == 0)
        {
            fade.current = target;
        }
        else
        {
            const int32_t difference = Wrap(int64_t{target} - fade.current);
            if (difference == 0)
            {
                stored = 0;
            }
            else
            {
                fade.delta = difference / ticks;
            }
        }
        fade.ticks = stored;
    };
    const auto lfo_setup = [&](LfoSettings& lfo)
    {
        const uint8_t b0 = next();
        const uint8_t b1 = next();
        const uint8_t b2 = next();
        const uint8_t b3 = next();
        lfo.waveform = next();
        lfo.amplitude = static_cast<int16_t>(b0 + (b1 << 8));
        lfo.phase_ms = static_cast<uint16_t>(b2 + (b3 << 8));
        lfo.delay_ms = 0;
        lfo.envelope_ms = 0;
        lfo.unknown14 = 0;
    };
    const auto lfo_envelope = [&](LfoSettings& lfo)
    {
        lfo.delay_ms = next16();
        lfo.envelope_ms = next16();
    };
    const auto use_lfo = [&](LfoSettings& lfo, uint8_t output)
    {
        uint8_t type = next();
        if (type == 2)
        {
            type = 1;
        }
        lfo.type = type;
        lfo.output = type == 0 ? 0 : output;
    };
    // The LFO that events 0xF0 to 0xF3 set. The model keeps a selection past the 4 LFOs off the channel's settings.
    LfoSettings spare;
    const auto selected_lfo = [&]() -> LfoSettings&
    {
        const uint8_t index = channel.unknown5a[7];
        return index < channel.lfos.size() ? channel.lfos[index] : spare;
    };
    SubLoop spare_loop;
    const auto sub_loop = [&](int index) -> SubLoop&
    {
        return index >= 0 && index < static_cast<int>(track.sub_loops.size())
                   ? track.sub_loops[static_cast<std::size_t>(index)]
                   : spare_loop;
    };

    switch (code)
    {
    case kWaitAgain:
        track.wait = track.previous_wait;
        break;
    case kWaitChange:
        track.previous_wait += static_cast<uint32_t>(static_cast<int8_t>(next()));
        track.wait = track.previous_wait;
        break;
    case kWait1Byte:
        track.previous_wait = next();
        track.wait = track.previous_wait;
        break;
    case kWait2Bytes:
        track.previous_wait = next16();
        track.wait = track.previous_wait;
        break;
    case kWait3Bytes:
        {
            const uint32_t low = next16();
            track.previous_wait = low + (uint32_t{next()} << 16);
            track.wait = track.previous_wait;
            break;
        }
    case kWaitForVoices:
        if (CountActive(channel) > 0)
        {
            track.wait = Byte(sequence, track, at);
            at--;
        }
        else
        {
            at++;
        }
        break;
    case kEndTrack:
        if (track.loop_start == 0)
        {
            track.playing = 0;
            at--;
            FreeVoices(channel);
        }
        else
        {
            track.loops++;
            sequence.looped = 1;
            at = track.loop_start;
            if (hooks.on_loop && &sequence == Followed())
            {
                hooks.on_loop(static_cast<int>(&track - sequence.tracks.data()));
            }
        }
        break;
    case kMainLoop:
        track.loop_start = at;
        if (hooks.on_loop_start && &sequence == Followed())
        {
            hooks.on_loop_start(static_cast<int>(&track - sequence.tracks.data()));
        }
        break;
    case kRepeatStart:
        {
            const uint8_t count = next();
            SubLoop& loop = sub_loop(track.loop_index);
            loop.start = at;
            loop.end = 0;
            loop.count = count;
            loop.octave = track.octave;
            track.loop_index++;
            break;
        }
    case kRepeatEnd:
        {
            const int index = track.loop_index - 1;
            SubLoop& loop = sub_loop(index);
            const int count = loop.count - 1;
            if (count != 0)
            {
                loop.end = at;
                loop.count = static_cast<uint8_t>(count);
                // The earlier build keeps the octave of the repeat's end (0x02063098 in Explorers of Time's ARM9
                // binary).
                if (build != Build::kEarlier)
                {
                    track.octave = loop.octave;
                }
                at = loop.start;
            }
            else
            {
                track.loop_index = static_cast<uint8_t>(index);
            }
            break;
        }
    case kRepeatExit:
        {
            const uint8_t n = track.loop_index;
            SubLoop& loop = sub_loop(n - 1);
            if (loop.count - 1 == 0)
            {
                at = loop.end;
                track.loop_index = static_cast<uint8_t>(n - 1);
            }
            break;
        }
    case kOctave:
        track.octave = next();
        break;
    case kOctaveChange:
        track.octave = static_cast<uint8_t>(track.octave + static_cast<int8_t>(next()));
        break;
    case kTempo:
    case kTempoAgain:
        {
            const uint8_t bpm = next();
            uint32_t beat = static_cast<uint32_t>((sequence.tempo.current >> 16) * bpm) >> 8;
            if (beat == 0)
            {
                beat = 1;
            }
            sequence.microseconds_per_beat = static_cast<int32_t>(60'000'000u / beat);
            sequence.bpm = bpm;
            break;
        }
    case kBank:
        {
            const uint8_t high = next();
            SelectBank(channel, static_cast<uint16_t>((high << 8) + next()));
            break;
        }
    case kBankHigh:
        SelectBank(channel, static_cast<uint16_t>((next() << 8) + (channel.bank_id & 0xFF)));
        break;
    case kBankLow:
        SelectBank(channel, static_cast<uint16_t>((channel.bank_id & ~0xFF) + next()));
        break;
    case kSkipByte:
        at++;
        break;
    case kProgram:
        SelectProgram(channel, next());
        break;
    case kSongFade:
        {
            const uint16_t ms = next16();
            uint16_t ticks = 0;
            if (build == Build::kEarlier)
            {
                // 0x0206326C in Explorers of Time's ARM9 binary: the earlier build multiplies only the high byte of
                // the time by 1000, and a fade can take 0 ticks.
                const auto earlier = static_cast<int32_t>((ms & 0xFF00) * 1000 + (ms & 0xFF));
                ticks = static_cast<uint16_t>(Divide(earlier, tick));
            }
            else if (ms != 0)
            {
                ticks = static_cast<uint16_t>(DivideUnsigned(ms * 1000u, static_cast<uint32_t>(int32_t{tick})));
                if (ticks == 0)
                {
                    ticks = 1;
                }
            }
            const int32_t target = next() << 16;
            sequence.volume.target = target;
            if (ticks == 0)
            {
                sequence.volume.current = target;
            }
            else
            {
                const int32_t difference = Wrap(int64_t{target} - sequence.volume.current);
                if (difference == 0)
                {
                    ticks = 0;
                }
                else
                {
                    sequence.volume.delta = Divide(difference, ticks);
                }
            }
            sequence.volume.ticks = ticks;
            break;
        }
    case kEnvelopeDefaults:
        channel.envelope.Reset();
        break;
    case kAttackBegin:
        channel.envelope.attack_begin = next();
        channel.envelope.SetUse();
        break;
    case kAttackTime:
        channel.envelope.attack_time = next();
        channel.envelope.SetUse();
        break;
    case kHoldTime:
        channel.envelope.hold_time = next();
        channel.envelope.SetUse();
        break;
    case kDecayAndSustain:
        {
            const uint8_t decay = next();
            const uint8_t sustain = next();
            if (decay != 0xFF)
            {
                channel.envelope.decay_time = decay;
            }
            if (sustain != 0xFF)
            {
                channel.envelope.sustain_level = sustain;
            }
            channel.envelope.SetUse();
            break;
        }
    case kSustainTime:
        channel.envelope.sustain_time = next();
        channel.envelope.SetUse();
        break;
    case kReleaseTime:
        channel.envelope.release_time = next();
        channel.envelope.SetUse();
        break;
    case kLengthMultiplier:
        track.multiplier = next();
        break;
    case kLfoLevel:
        {
            const auto level = static_cast<int8_t>(next());
            channel.lfo_level = level;
            // The earlier build also marks the channel's pitch (0x02064894 in Explorers of Time's ARM9 binary).
            if (build == Build::kEarlier)
            {
                FlagVoices(channel, 0x10);
            }
            ForEachVoice(channel, [level](Voice& voice) { SetConstantLevels(voice.lfos, level); });
            break;
        }
    case kDamper:
        if (next() >= 0x40)
        {
            track.channel->flags |= 1;
        }
        else
        {
            track.channel->flags &= static_cast<uint8_t>(~1);
            if (build == Build::kEarlier)
            {
                ReleaseDampedVoices(*track.channel);
            }
            else
            {
                ReleaseEndedNotes(*track.channel);
            }
        }
        break;
    case kLegato:
        channel.flags |= 2;
        break;
    case kOptionalVolume:
        channel.optional_volume = next();
        break;
    case kSkipTwo:
    case kSkipTwoAgain:
        at += 2;
        break;
    case kTuning:
        channel.tuning = static_cast<int16_t>(static_cast<int8_t>(next()) << 8);
        update_bend();
        break;
    case kTuningSemitones:
        channel.tuning = static_cast<int16_t>((static_cast<int8_t>(next()) << 8) + channel.tuning);
        update_bend();
        break;
    case kTuningSteps:
        channel.tuning = static_cast<int16_t>((static_cast<int8_t>(next()) << 2) + channel.tuning);
        update_bend();
        break;
    case kTuningWord:
        channel.tuning = static_cast<int16_t>(channel.tuning + next16());
        update_bend();
        break;
    case kTuningSlide:
        {
            uint16_t ticks = next16();
            const uint8_t semitones = next();
            if (!(channel.flags & 2))
            {
                channel.bend_fade.current = 0;
            }
            const auto target = static_cast<int32_t>(
                uint32_t{static_cast<uint16_t>((channel.bend_fade.current >> 16) + (semitones << 8))} << 16);
            channel.bend_fade.target = target;
            if (ticks == 0)
            {
                channel.bend_fade.current = target;
            }
            else
            {
                const int32_t difference = Wrap(int64_t{target} - channel.bend_fade.current);
                if (difference == 0)
                {
                    ticks = 0;
                }
                else
                {
                    channel.bend_fade.delta = Divide(difference, ticks);
                }
            }
            channel.bend_fade.ticks = ticks;
            channel.bend = static_cast<uint16_t>(synth.bend + channel.tuning +
                                                 static_cast<uint16_t>(channel.bend_fade.current >> 16));
            FlagVoices(channel, 0x10);
            break;
        }
    case kRandomKeys:
        {
            const uint8_t a = next();
            const uint8_t b = next();
            channel.random_low = std::min(a, b);
            channel.random_high = std::max(a, b);
            break;
        }
    case kJitter:
        channel.jitter = static_cast<int16_t>(next16());
        break;
    case kKeyBend:
        {
            const uint8_t high = next();
            ApplyKeyBend(channel, static_cast<int16_t>((high << 8) + next()));
            break;
        }
    case kUnusedWord:
        {
            const uint8_t high = next();
            channel.unknown56 = static_cast<int16_t>((high << 8) + next());
            break;
        }
    case kBendRange:
        channel.bend_range = next();
        break;
    case kBendLfo:
        {
            LfoSettings& lfo = channel.lfos[0];
            const uint8_t b0 = next();
            const uint8_t b1 = next();
            const uint8_t b2 = next();
            const uint8_t b3 = next();
            const uint8_t b4 = next();
            lfo.type = 1;
            lfo.output = 1;
            lfo.waveform = b4;
            lfo.amplitude = static_cast<int16_t>(b0 + (b1 << 8));
            lfo.phase_ms = static_cast<uint16_t>(b2 + (b3 << 8));
            lfo.delay_ms = 0;
            lfo.envelope_ms = 0;
            lfo.unknown14 = 0;
            break;
        }
    case kBendLfoEnvelope:
        lfo_envelope(channel.lfos[0]);
        break;
    case kBendLfoSwitch:
        use_lfo(channel.lfos[0], 1);
        break;
    case kVolume:
        {
            // The earlier build reads the volume unsigned (0x020636C8 in Explorers of Time's ARM9 binary).
            const uint8_t byte = next();
            const int32_t volume = build == Build::kEarlier ? int32_t{byte} : int32_t{static_cast<int8_t>(byte)};
            channel.volume_fade.target = volume << 16;
            channel.volume_fade.current = volume << 16;
            channel.volume_fade.ticks = 0;
            update_volume(volume);
            break;
        }
    case kVolumeChange:
        {
            const int32_t volume =
                std::clamp(static_cast<int8_t>(next()) + (channel.volume_fade.current >> 16), 0, 0x7F);
            channel.volume_fade.target = volume << 16;
            channel.volume_fade.current = volume << 16;
            channel.volume_fade.ticks = 0;
            update_volume(volume);
            break;
        }
    case kVolumeSlide:
        set_fade(channel.volume_fade);
        if (build == Build::kEarlier)
        {
            // The earlier build updates the channel's volume at once (0x02063738 in Explorers of Time's ARM9 binary).
            update_volume(channel.volume_fade.current >> 16);
        }
        break;
    case kExpression:
        channel.expression = next();
        update_volume(channel.volume_fade.current >> 16);
        break;
    case kVolumeLfo:
        lfo_setup(channel.lfos[1]);
        channel.lfos[1].type = 1;
        channel.lfos[1].output = 2;
        break;
    case kVolumeLfoEnvelope:
        lfo_envelope(channel.lfos[1]);
        break;
    case kVolumeLfoSwitch:
        use_lfo(channel.lfos[1], 2);
        break;
    case kPan:
        {
            const int32_t pan = std::min<int32_t>(next(), 0x7F);
            channel.pan_fade.target = pan << 16;
            channel.pan_fade.current = pan << 16;
            channel.pan_fade.ticks = 0;
            channel.pan = static_cast<int16_t>(pan + (synth.pan - 0x40));
            FlagVoices(channel, 0x40);
            break;
        }
    case kPanChange:
        {
            const int32_t pan = std::clamp(static_cast<int8_t>(next()) + (channel.pan_fade.current >> 16), 0, 0x7F);
            channel.pan_fade.target = pan << 16;
            channel.pan_fade.current = pan << 16;
            channel.pan_fade.ticks = 0;
            channel.pan = static_cast<int16_t>(pan + (synth.pan - 0x40));
            FlagVoices(channel, 0x40);
            break;
        }
    case kPanSlide:
        set_fade(channel.pan_fade);
        if (build == Build::kEarlier)
        {
            // The earlier build updates the channel's pan at once (0x020638E0 in Explorers of Time's ARM9 binary).
            channel.pan = static_cast<int16_t>((channel.pan_fade.current >> 16) + (synth.pan - 0x40));
            FlagVoices(channel, 0x40);
        }
        break;
    case kPanLfo:
        lfo_setup(channel.lfos[2]);
        channel.lfos[2].type = 1;
        channel.lfos[2].output = 3;
        break;
    case kPanLfoEnvelope:
        lfo_envelope(channel.lfos[2]);
        break;
    case kPanLfoSwitch:
        use_lfo(channel.lfos[2], 3);
        break;
    case kLfo:
        lfo_setup(selected_lfo());
        break;
    case kLfoEnvelope:
        lfo_envelope(selected_lfo());
        break;
    case kLfoParameter:
        {
            const uint8_t parameter = next();
            const uint8_t value = next();
            LfoSettings& lfo = selected_lfo();
            switch (parameter)
            {
            case 1:
                channel.unknown5a[7] = value;
                break;
            case 2:
                lfo.type = value;
                break;
            case 3:
                lfo.output = value;
                break;
            case 4:
                lfo.waveform = value;
                break;
            case 5:
                switch (lfo.output)
                {
                case 1:
                case 4:
                    lfo.amplitude = value * 10;
                    break;
                case 2:
                    lfo.amplitude = value * -20;
                    break;
                default:
                    lfo.amplitude = value * 20;
                    break;
                }
                break;
            case 6:
                lfo.phase_ms = static_cast<uint16_t>(value * 5);
                break;
            case 7:
                lfo.delay_ms = static_cast<uint16_t>(value * 20);
                break;
            case 8:
                lfo.delay_ms = static_cast<uint16_t>((lfo.delay_ms & 0xFF00) | value);
                break;
            case 9:
                lfo.delay_ms = static_cast<uint16_t>((lfo.delay_ms & 0xFF) | value << 8);
                break;
            case 10:
                lfo.envelope_ms = static_cast<uint16_t>(value * 20);
                break;
            default:
                break;
            }
            break;
        }
    case kLfoSwitch:
        {
            const uint8_t index = next();
            uint8_t type = next();
            const uint8_t output = next();
            channel.unknown5a[7] = index;
            LfoSettings& lfo = selected_lfo();
            if (type == 2)
            {
                type = 1;
            }
            lfo.type = type;
            lfo.output = output;
            break;
        }
    case kSignalGame:
        sequence.signal = next();
        break;
    default:
        track.playing = 0;
        break;
    }
}

Hang::Hang(int track, uint32_t offset)
    : std::runtime_error("track " + std::to_string(track) + " runs " + std::to_string(kCommandBudget) +
                         " commands in a tick without waiting, up to offset " + xsflib::Hex(offset, 0, true) +
                         ": the sequence hangs"),
      track_(track),
      offset_(offset)
{
}

int Hang::TrackIndex() const
{
    return track_;
}

uint32_t Hang::Offset() const
{
    return offset_;
}

Build BuildOf(std::span<const uint8_t> arm9)
{
    // ldr rT, [rN, #0x14C], then subs rT, rT, #1 within three words, then str rT, [rN, #0x14C].
    const std::size_t words = arm9.size() / 4;
    const auto word = [&](std::size_t i)
    {
        return Get32(arm9, 4 * i);
    };
    for (std::size_t i = 0; i + 4 < words; i++)
    {
        const uint32_t load = word(i);
        if ((load & 0xFFF00FFF) != 0xE590014C)
        {
            continue;
        }
        const uint32_t base = load >> 16 & 0xF;
        const uint32_t value = load >> 12 & 0xF;
        for (std::size_t j = i + 1; j <= i + 3 && j + 1 < words; j++)
        {
            if (word(j) == (0xE2500001 | value << 16 | value << 12) &&
                word(j + 1) == (0xE580014C | base << 16 | value << 12))
            {
                return Build::kEarlier;
            }
        }
    }
    return Build::kLater;
}

Driver::Driver(Build build) : state_(std::make_unique<State>())
{
    State& s = *state_;
    s.build = build;
    s.defaults[7] = 100;
    s.defaults[10] = 0x40;
    s.defaults[11] = 0x7F;
    s.global_volumes.fill(0x7F);
    s.global_volumes[0] = 0;
    for (std::size_t i = 0; i < s.notes.size(); i++)
    {
        s.notes[i].next = i + 1 < s.notes.size() ? static_cast<int>(i + 1) : kNone;
        s.notes[i].duration = -1;
    }
    s.free_notes = 0;
    for (int i = 0; i < kVoices; i++)
    {
        Voice& voice = s.VoiceAt(i);
        voice.index = static_cast<uint16_t>(i);
        voice.bit = static_cast<uint16_t>(1 << i);
    }
    // The pool of the sound effects' sequences (0x0206E920).
    for (int i = 0; i < kEffectSequences; i++)
    {
        s.effects.emplace_back(s.MakeSequence(kEffectTracks, kEffectTracks));
    }
}

Driver::~Driver() = default;

Driver::Driver(const Driver& other) : state_(std::make_unique<State>(*other.state_))
{
    state_->Repoint(*other.state_);
}

Driver& Driver::operator=(const Driver& other)
{
    if (this != &other)
    {
        auto copy = std::make_unique<State>(*other.state_);
        copy->Repoint(*other.state_);
        state_ = std::move(copy);
    }
    return *this;
}

std::optional<uint16_t> Driver::AddBank(const WaveBank& bank, uint32_t samples)
{
    Bank kept;
    kept.id = static_cast<uint16_t>(bank.header[0x0E] | bank.header[0x0F] << 8);
    if (state_->BankById(kept.id) != nullptr)
    {
        return std::nullopt;
    }
    kept.file = &bank;
    kept.wave_count = static_cast<int16_t>(bank.header[0x46] | bank.header[0x47] << 8);
    kept.program_count = static_cast<int16_t>(bank.header[0x48] | bank.header[0x49] << 8);
    kept.key_group_count = bank.header[0x4A];
    if (bank.header[0x4B] == 2)
    {
        kept.main_bank = static_cast<uint16_t>(bank.header[0x40] | bank.header[0x41] << 8);
    }
    kept.samples = bank.samples ? samples : 0;
    for (const std::optional<WaveInfo>& wave : bank.waves)
    {
        kept.waves.push_back(wave ? std::optional(wave->bytes) : std::nullopt);
    }
    if (bank.instruments)
    {
        for (const std::optional<Program>& program : bank.instruments->programs)
        {
            if (!program)
            {
                kept.programs.emplace_back();
                continue;
            }
            kept.programs.emplace_back(ProgramBytes(*program));
        }
    }
    state_->banks.push_back(std::move(kept));
    return state_->banks.back().id;
}

uint32_t Driver::PlaceWaves(uint16_t id, const WaveBank& main_bank, uint32_t address)
{
    Bank* bank = state_->BankById(id);
    if (bank == nullptr)
    {
        throw std::runtime_error("no bank of that id is loaded");
    }
    uint32_t total = 0;
    uint32_t place = address;
    for (int i = 0; i < bank->wave_count; i++)
    {
        const auto index = static_cast<std::size_t>(i);
        if (index >= bank->waves.size() || !bank->waves[index])
        {
            continue;
        }
        std::array<uint8_t, WaveInfo::kSize>& wave = *bank->waves[index];
        Put32(wave, 0x24, place);
        if (index >= main_bank.waves.size() || !main_bank.waves[index])
        {
            continue;
        }
        const uint32_t size = (Get32(wave, 0x2C) + Get32(wave, 0x28)) << 2;
        total += size;
        place += size;
    }
    return total;
}

void Driver::PlaySong(const Song& song, const StartParameters& parameters)
{
    State& s = *state_;
    if (s.song)
    {
        s.FreeAllVoices(s.song->synth);
        s.FreeHeldNotes(s.song->synth);
    }
    auto file = std::make_shared<const std::vector<uint8_t>>(song.Write());
    constexpr std::size_t kInfo = Song::kHeaderSize + 16;

    // The driver's load of a song: the sequence, its synth and its tracks (0x0206D808).
    std::unique_ptr<Sequence> sequence = s.MakeSequence((*file)[kInfo + 6], (*file)[kInfo + 7]);
    Sequence& q = *sequence;
    q.id = 0x10000 + uint32_t{Get16(*file, 0x0E)};
    q.file = std::move(file);
    s.LoadSequence(q, kInfo);
    q.unknown_a = 0;
    s.song = OwnedSequence(std::move(sequence));

    // The start of the song from its beginning (0x0206D9BC). The earlier build then resets the channels again, without
    // a bank (0x02060C04 in Explorers of Time's ARM9 binary).
    s.RestartSequence(q);
    if (s.build == Build::kEarlier)
    {
        s.ResetChannels(q.synth, -1, q.id);
    }
    q.volume.current = 0;
    q.volume.ticks = 0;
    q.muted = 0;
    s.StartSequence(q, 0, 0, parameters);
}

std::optional<uint16_t> Driver::AddEffects(const SoundEffects& effects)
{
    State& s = *state_;
    EffectBank bank;
    bank.id = static_cast<uint16_t>(effects.header[0x0E] | effects.header[0x0F] << 8);
    if (s.EffectBankById(bank.id) != nullptr)
    {
        return std::nullopt;
    }
    bank.file = std::make_shared<const std::vector<uint8_t>>(effects.Write());
    const std::vector<uint8_t>& bytes = *bank.file;
    bank.count = static_cast<int16_t>(Get16(bytes, 0x30));
    if (bytes[0x35] != 0)
    {
        // The chunks up to the end chunk: the driver keeps where the seq chunk's data starts.
        std::size_t at = SoundEffects::kHeaderSize;
        while (at + 16 <= bytes.size() && std::memcmp(bytes.data() + at, "eod ", 4) != 0)
        {
            if (std::memcmp(bytes.data() + at, "seq ", 4) == 0)
            {
                bank.sequences = at + 16;
            }
            const uint32_t alignment = bytes[at + 8];
            if (alignment == 0)
            {
                throw std::runtime_error("a chunk's alignment is 0");
            }
            at += (Get32(bytes, at + 12) + 16 + alignment - 1) & ~(alignment - 1);
        }
    }
    s.effect_banks.push_back(std::move(bank));
    return s.effect_banks.back().id;
}

bool Driver::PlayEffect(uint16_t bank, uint16_t index, const StartParameters& parameters, uint16_t handle)
{
    State& s = *state_;
    const EffectBank* kept = s.EffectBankById(bank);
    if (kept == nullptr)
    {
        return false;
    }
    const std::optional<std::size_t> at = s.EffectAt(*kept, static_cast<int16_t>(index));
    if (!at)
    {
        return false;
    }
    if (*at + SequenceInfo::kSize > kept->file->size())
    {
        throw std::runtime_error("a sound effect's settings lie past the end of its bank");
    }
    const std::span<const uint8_t> settings = std::span(*kept->file).subspan(*at, SequenceInfo::kSize);
    const uint32_t id = bank | uint32_t{index} << 16;
    const uint8_t group = settings[0x1A];
    const int stop = (settings[0x18] & 1) == 0 ? 1 : 0;
    bool refused = false;
    if (parameters.flags & 0x40)
    {
        refused = s.HandlePlays(handle, stop);
    }
    else if (group == 0)
    {
        refused = s.TooManyInstances([id](const Sequence& q) { return q.id == id; }, settings[0x1C], stop);
    }
    else
    {
        refused = s.TooManyInstances([group](const Sequence& q) { return q.group == group; }, settings[0x1C], stop);
    }
    if (refused)
    {
        return false;
    }
    Sequence* found = s.SequenceForEffect(settings[0x1B]);
    if (found == nullptr)
    {
        return false;
    }
    Sequence& q = *found;
    if (q.status[0] == 1)
    {
        s.StopSequence(q, 0, 1);
    }
    q.id = id;
    q.file = kept->file;
    s.ClearTracks(q);
    if (!s.LoadSequence(q, *at))
    {
        return false;
    }
    s.RestartSequence(q);
    q.volume.current = 0;
    q.volume.ticks = 0;
    q.unknown9 = kept->unknown18;
    // The driver's mute (0x0206C688), which the model keeps off, and the bank's byte.
    q.muted = 0;
    q.unknown_a = 1;
    s.StartSequence(q, handle, group, parameters);
    return true;
}

void Driver::StopSong(uint32_t ms)
{
    if (state_->song)
    {
        state_->StopSequence(*state_->song, ms, 0);
    }
}

void Driver::StopEffect(uint16_t bank, uint16_t index, uint16_t handle, uint32_t ms)
{
    const uint32_t id = bank | uint32_t{index} << 16;
    for (const OwnedSequence& q : state_->effects)
    {
        if (q->status[0] == 1 && q->id == id && (handle == 0 || q->handle == handle))
        {
            state_->StopSequence(*q, ms, 1);
        }
    }
}

void Driver::StopHandle(uint16_t handle, uint32_t ms)
{
    for (const OwnedSequence& q : state_->effects)
    {
        if (q->status[0] == 1 && q->handle == handle)
        {
            state_->StopSequence(*q, ms, 1);
        }
    }
}

void Driver::StopEffects(uint32_t ms)
{
    for (const OwnedSequence& q : state_->effects)
    {
        if (q->status[0] == 1)
        {
            state_->StopSequence(*q, ms, 1);
        }
    }
}

ntr::snd::CommandList Driver::Tick(uint16_t channel_status)
{
    State& s = *state_;
    s.total_ticks++;
    if (s.song)
    {
        s.StepSequence(*s.song);
    }
    for (const OwnedSequence& effect : s.effects)
    {
        s.StepSequence(*effect);
    }
    s.StepVoices();
    s.UpdateVoices();
    s.WriteCommands(s.commands);
    s.CheckEndedChannels(channel_status);
    ntr::snd::CommandList list = std::move(s.commands);
    s.commands.clear();
    return list;
}

bool Driver::Playing() const
{
    return state_->song && state_->song->status[0] != 0;
}

int Driver::EffectsPlaying() const
{
    int count = 0;
    for (const OwnedSequence& effect : state_->effects)
    {
        count += effect->status[0] != 0 ? 1 : 0;
    }
    return count;
}

void Driver::ReleaseFollowedNotes()
{
    Sequence* sequence = state_->Followed();
    if (sequence == nullptr)
    {
        return;
    }
    for (Channel& channel : sequence->synth.channels)
    {
        // The earlier build counts down each voice's duration, and the later one each note's. A voice or a note that
        // holds has none to count down.
        state_->ForEachVoice(channel,
                             [](Voice& voice)
                             {
                                 if (voice.duration > 0)
                                 {
                                     voice.duration = 1;
                                 }
                             });
        for (int n = channel.held; n != kNone; n = state_->NoteAt(n).next)
        {
            Note& note = state_->NoteAt(n);
            if (note.duration > 0)
            {
                note.duration = 1;
            }
        }
    }
}

void Driver::Follow(int sequence)
{
    if (sequence != kSong && (sequence < 0 || static_cast<std::size_t>(sequence) >= state_->effects.size()))
    {
        throw std::out_of_range("the pool of sound effects has no sequence " + std::to_string(sequence));
    }
    state_->followed = sequence;
}

void Driver::SetHooks(SequenceHooks hooks)
{
    state_->hooks = std::move(hooks);
}

const SequenceHooks& Driver::GetHooks() const
{
    return state_->hooks;
}

bool Driver::SequencePlays() const
{
    const Sequence* sequence = state_->Followed();
    return sequence && sequence->status[0] != 0;
}

uint32_t Driver::SequenceTick() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? static_cast<uint32_t>(sequence->sequencer_ticks) : 0;
}

uint16_t Driver::SequenceTimebase() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? static_cast<uint16_t>(sequence->ticks_per_eighth << 3) : 0;
}

int32_t Driver::SequenceTempo() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? sequence->microseconds_per_beat : 0;
}

int Driver::ChannelCount() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? static_cast<int>(sequence->synth.channels.size()) : 0;
}

ChannelView Driver::GetChannel(int channel) const
{
    const Sequence& sequence = *state_->Followed();
    const Channel& c = sequence.synth.channels.at(static_cast<std::size_t>(channel));
    ChannelView view{.bank = c.bank_id,
                     .program = c.program,
                     .has_program = c.instrument != nullptr,
                     .volume = static_cast<uint8_t>(c.volume_fade.current >> 16),
                     .expression = c.expression,
                     .synth_volume = sequence.synth.combined_volume,
                     .pan = c.pan,
                     .bend = c.bend,
                     .key_bend = c.key_bend,
                     .bend_range = c.bend_range,
                     .damper = (c.flags & 1) != 0,
                     .jitter = c.jitter,
                     .random_keys = c.random_high != 0,
                     .lfo_level = c.lfo_level};
    for (std::size_t i = 0; i < c.lfos.size(); i++)
    {
        const LfoSettings& lfo = c.lfos[i];
        view.lfos[i] = {.type = lfo.type,
                        .output = lfo.output,
                        .waveform = lfo.waveform,
                        .amplitude = lfo.amplitude,
                        .phase_ms = lfo.phase_ms,
                        .delay_ms = lfo.delay_ms,
                        .envelope_ms = lfo.envelope_ms};
    }
    if (c.envelope.use != 0)
    {
        const EnvelopeParameters& e = c.envelope;
        view.envelope = {.multiplier = e.multiplier,
                         .attack_begin = e.attack_begin,
                         .attack_time = e.attack_time,
                         .decay_time = e.decay_time,
                         .sustain_level = e.sustain_level,
                         .hold_time = e.hold_time,
                         .sustain_time = e.sustain_time,
                         .release_time = e.release_time};
    }
    return view;
}

int Driver::TrackCount() const
{
    const Sequence* sequence = state_->Followed();
    return sequence ? sequence->track_slots : 0;
}

bool Driver::TrackPlays(int track) const
{
    const Sequence* sequence = state_->Followed();
    return sequence && sequence->tracks.at(static_cast<std::size_t>(track)).playing == 1;
}

std::optional<int32_t> EnvelopeTicks(uint8_t multiplier, uint8_t index)
{
    if (index == 0x7F)
    {
        return std::nullopt;
    }
    return SlideTicks(multiplier, index, Driver::kTickMicroseconds);
}

std::vector<uint8_t> EnvelopeLevels(const EnvelopeValues& values, int ticks, std::optional<int> release)
{
    Envelope envelope;
    envelope.parameters.Reset();
    envelope.parameters.use = 1;
    envelope.parameters.multiplier = values.multiplier;
    envelope.parameters.attack_begin = values.attack_begin;
    envelope.parameters.attack_time = values.attack_time;
    envelope.parameters.decay_time = values.decay_time;
    envelope.parameters.sustain_level = values.sustain_level;
    envelope.parameters.hold_time = values.hold_time;
    envelope.parameters.sustain_time = values.sustain_time;
    envelope.parameters.release_time = values.release_time;
    envelope.Start(Driver::kTickMicroseconds);

    std::vector<uint8_t> levels;
    levels.reserve(static_cast<std::size_t>(std::max(ticks, 0)));
    for (int k = 0; k < ticks; k++)
    {
        if (release && k == *release)
        {
            envelope.Release(Driver::kTickMicroseconds);
        }
        levels.push_back(static_cast<uint8_t>(std::max<int8_t>(envelope.Tick(Driver::kTickMicroseconds), 0)));
    }
    return levels;
}

int32_t VoiceTimer(uint16_t pitch, uint8_t format)
{
    const uint8_t place = tables::kKeyPlaces[(pitch >> 8) & 0x7F];
    uint32_t frequency = tables::kSemitoneFrequencies[(pitch & 0xFF) + ((place & 0xF) << 8)];
    const int shift = 4 - (place >> 4);
    frequency = shift >= 0 ? frequency >> shift : frequency << -shift;
    auto divisor = static_cast<int32_t>(frequency);
    if (format >= 3)
    {
        divisor /= 16;
    }
    return Divide(0x00FFB0FF, divisor);
}

VoiceView Driver::GetVoice(int voice) const
{
    const Voice& v = state_->voices.at(static_cast<std::size_t>(voice));
    VoiceView view;
    if (v.channel == nullptr)
    {
        return view;
    }
    view.allocated = true;
    const Sequence* followed = state_->Followed();
    view.followed = followed && v.channel->synth == &followed->synth;
    view.channel = v.channel->index;
    view.track = v.track;
    view.note = v.note;
    view.key = v.key;
    view.started = static_cast<int8_t>(v.envelope.active) == 1;
    view.releasing = v.envelope.state == kRelease || v.envelope.state == kReleaseEnd;
    view.level = static_cast<uint8_t>(v.state & 0xFF);
    const Envelope& envelope = v.envelope;
    if (view.releasing)
    {
        if (envelope.ticks != 0x7FFFFFFF)
        {
            view.release_ticks = envelope.ticks;
        }
    }
    else if (envelope.state != kOff && envelope.parameters.release_time != 0x7F)
    {
        view.release_ticks = SlideTicks(envelope.parameters.multiplier, envelope.parameters.release_time, state_->tick);
    }
    view.key_bend = v.key_bend;
    view.jitter = v.jitter;
    view.wave = v.wave;
    view.key_group = static_cast<uint8_t>(v.channel_key_group & 0xFF);
    view.key_group_voices = v.group_voices;
    return view;
}

std::array<LfoValues, 4> ProgramLfos(const WaveBank& bank, uint16_t program)
{
    std::array<LfoValues, 4> lfos;
    const auto count = static_cast<int16_t>(bank.header[0x48] | bank.header[0x49] << 8);
    if (!bank.instruments || static_cast<int16_t>(program) < 0 || static_cast<int16_t>(program) >= count ||
        program >= bank.instruments->programs.size() || !bank.instruments->programs[program])
    {
        return lfos;
    }

    const std::vector<uint8_t> bytes = ProgramBytes(*bank.instruments->programs[program]);
    for (std::size_t k = 0; k < lfos.size(); k++)
    {
        const std::size_t at = 0x10 + 0x10 * k;
        if (at + 16 <= bytes.size() && bytes[at + 1] != 0)
        {
            const LfoSettings lfo = LfoSettings::Read(std::span(bytes).subspan(at, 16));
            lfos[k] = {.type = lfo.type,
                       .output = lfo.output,
                       .waveform = lfo.waveform,
                       .amplitude = lfo.amplitude,
                       .phase_ms = lfo.phase_ms,
                       .delay_ms = lfo.delay_ms,
                       .envelope_ms = lfo.envelope_ms};
        }
    }

    return lfos;
}

} // namespace ntdlib::dse
