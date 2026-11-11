// SPDX-License-Identifier: MIT

// libultra's two sequence players: the queue of events, the voices and the MIDI messages. al_player_sequences.cpp
// contains the readers of their sequences.

#include "ntdlib/n64/al_player.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "al_player_constants.h"

namespace ntdlib::n64
{

namespace
{

using al_player_constants::kStatusChannelPressure;
using al_player_constants::kStatusController;
using al_player_constants::kStatusNoteOff;
using al_player_constants::kStatusNoteOn;
using al_player_constants::kStatusPitchBend;
using al_player_constants::kStatusPolyPressure;
using al_player_constants::kStatusProgram;

constexpr int32_t kFrameMicroseconds = 16000;
constexpr int32_t kQueueEnd = 0x7FFFFFFF;
constexpr int32_t kGainChangeTime = 1000;
constexpr int32_t kKillTime = 50000;
constexpr int32_t kInitialTickTime = 488;
constexpr float kInitialTempo = 500000.0F;
constexpr uint8_t kSustainThreshold = 63;
constexpr uint8_t kPanCenter = 64;
constexpr uint8_t kVolumeFull = 127;
constexpr int32_t kOscillatorStep = 16000;
constexpr float kTwoPi = 6.2831854820251465F;
constexpr uint8_t kOscillatorTremoloSine = 1;
constexpr uint8_t kOscillatorTremoloSquare = 2;
constexpr uint8_t kOscillatorTremoloFall = 3;
constexpr uint8_t kOscillatorTremoloRise = 4;
constexpr uint8_t kOscillatorVibratoSine = 128;
constexpr uint8_t kOscillatorVibratoSquare = 129;
constexpr uint8_t kOscillatorVibratoFall = 130;
constexpr uint8_t kOscillatorVibratoRise = 131;
constexpr uint8_t kScaledFormVolume = 120;
constexpr uint8_t kDefaultPriority = 5;

constexpr uint8_t kControllerVolume = 0x07;
constexpr uint8_t kControllerChannelScale = 0x08;
constexpr uint8_t kControllerPlayerScale = 0x15;
constexpr uint8_t kControllerPan = 0x0A;
constexpr uint8_t kControllerPriority = 0x10;
constexpr uint8_t kControllerSustain = 0x40;
constexpr uint8_t kControllerFxMix = 0x5B;

// libultra's sine, in double precision from a single-precision angle.
float Sine(float angle)
{
    static constexpr double kFirst = -0.16666659550427756;
    static constexpr double kSecond = 0.008333066246082155;
    static constexpr double kThird = -0.0001980960290193795;
    static constexpr double kFourth = 2.605780637968037e-06;
    static constexpr double kOneOverPi = 0.3183098861837907;
    static constexpr double kPiHigh = 3.1415926218032837;
    static constexpr double kPiLow = 3.178650954705639e-08;

    const auto polynomial = [](double x)
    {
        const double square = x * x;
        const double fourth_term = kFourth * square;
        const double a = fourth_term + kThird;
        const double b = a * square;
        const double c = b + kSecond;
        const double d = c * square;
        const double sum = kFirst + d;
        const double cube = x * square;
        const double term = cube * sum;

        return term + x;
    };

    const int exponent = static_cast<int>((std::bit_cast<uint32_t>(angle) >> 22) & 0x1FF);
    if (exponent < 255)
    {
        return exponent >= 230 ? static_cast<float>(polynomial(angle)) : angle;
    }

    if (exponent >= 310)
    {
        return 0.0F;
    }

    const auto wide = static_cast<double>(angle);
    const double turns = wide * kOneOverPi;
    const auto half_turns = static_cast<int32_t>(turns >= 0.0 ? turns + 0.5 : turns - 0.5);
    const auto halves = static_cast<double>(half_turns);
    const double high = halves * kPiHigh;
    const double low = halves * kPiLow;
    const double first = wide - high;
    const double reduced = first - low;
    const auto result = static_cast<float>(polynomial(reduced));

    return (half_turns & 1) != 0 ? -result : result;
}

// The example code's depth of a vibrato in cents: a power of a fixed step.
float DepthToCents(uint8_t depth)
{
    float step = 1.0309929847717285F;
    float cents = 1.0F;
    for (int bits = depth; bits != 0; bits >>= 1)
    {
        if ((bits & 1) != 0)
        {
            cents *= step;
        }

        step *= step;
    }

    return cents;
}

int32_t UpdateOscillatorState(uint8_t type, bool& second_half, uint16_t period, uint16_t& count, uint8_t level,
                              uint8_t depth, uint8_t low_level, float cents, float low_ratio, float high_ratio,
                              int32_t start_cents, int32_t cents_range, float& value)
{
    const auto next_part = [&](bool wraps_at_period)
    {
        count++;
        if (wraps_at_period ? count >= period : count > period)
        {
            count = 0;
        }

        return static_cast<float>(count) / static_cast<float>(period);
    };

    switch (type)
    {
    case kOscillatorTremoloSine:
        {
            const float angle = next_part(true) * kTwoPi;
            const float wave = static_cast<float>(depth) * Sine(angle);
            value = static_cast<float>(level) + wave;
            break;
        }

    case kOscillatorTremoloSquare:
        value = static_cast<float>(second_half ? level : low_level);
        second_half = !second_half;

        return static_cast<int32_t>(period) * kOscillatorStep;

    case kOscillatorTremoloFall:
        {
            const float amount = next_part(false) * static_cast<float>(depth);
            value = static_cast<float>(level) - amount;
            break;
        }

    case kOscillatorTremoloRise:
        {
            const float amount = next_part(false) * static_cast<float>(depth);
            value = static_cast<float>(level) + amount;
            break;
        }

    case kOscillatorVibratoSine:
        {
            const float angle = next_part(true) * kTwoPi;
            const float amount = Sine(angle) * cents;
            value = CentsToRatio(static_cast<int32_t>(amount));
            break;
        }

    case kOscillatorVibratoSquare:
        value = second_half ? high_ratio : low_ratio;
        second_half = !second_half;

        return static_cast<int32_t>(period) * kOscillatorStep;

    case kOscillatorVibratoFall:
        {
            const float amount = next_part(false) * static_cast<float>(cents_range);
            const float now = static_cast<float>(start_cents) - amount;
            value = CentsToRatio(static_cast<int32_t>(now));
            break;
        }

    case kOscillatorVibratoRise:
        {
            const float amount = next_part(false) * static_cast<float>(cents_range);
            const float now = amount + static_cast<float>(start_cents);
            value = CentsToRatio(static_cast<int32_t>(now));
            break;
        }

    default:
        break;
    }

    return kOscillatorStep;
}

// The sum wraps at 8 bits, and a result over 127 becomes 127.
uint8_t OffsetFxMix(uint8_t mix, int8_t offset)
{
    const auto sum = static_cast<uint8_t>(mix + offset);

    return sum > 127 ? static_cast<uint8_t>(127) : sum;
}

// AlPlayerForm::kPlayerScale's rate of a decay, for each 8 microseconds. The table holds the base-2 logarithm of each
// value of a number's first four bits.
float DecayRate(float start, float target, int32_t time)
{
    static constexpr std::array<double, 8> kLogarithms = {-0.912537, -0.752072, -0.607683, -0.476438,
                                                          -0.356144, -0.245112, -0.142019, -0.045804};
    static constexpr double kLogOfTwo = 0.6931471805599453;
    if (time == 0)
    {
        return start <= target ? 32768.0F : 0.0F;
    }

    const double from = std::max(static_cast<double>(start), 1.0);
    const double to = std::max(static_cast<double>(target), 1.0);
    auto bits = static_cast<int32_t>(std::ldexp(1.0 / static_cast<double>(time), 30));
    int exponent = 0;
    const double mantissa = std::frexp(to / from, &exponent);
    const auto index = static_cast<std::size_t>(std::ldexp(mantissa, 4));
    const double sum = kLogarithms[index - 8] + static_cast<double>(exponent);
    const double logarithm = sum * kLogOfTwo;
    const double step = logarithm / std::ldexp(1.0, 30);
    double base = 1.0 + step;
    double power = 1.0;
    while (bits != 0)
    {
        if ((bits & 1) != 0)
        {
            power *= base;
        }

        base *= base;
        bits >>= 1;
    }

    const double squared = power * power;
    const double fourth = squared * squared;

    return static_cast<float>(fourth * fourth);
}

// The gain of a decay after `time` microseconds.
float DecayGain(float start, int32_t time, float rate)
{
    int32_t steps = time >> 3;
    if (steps == 0)
    {
        return start;
    }

    float power = 1.0F;
    for (int i = 0; i < 32; i++)
    {
        if ((steps & 1) != 0)
        {
            power *= rate;
        }

        steps >>= 1;
        if (steps == 0)
        {
            break;
        }

        rate *= rate;
    }

    return start * power;
}

} // namespace

float CentsToRatio(int32_t cents)
{
    float step = 1.00057779F;
    if (cents < 0)
    {
        step = 0.9994225441F;
        cents = -cents;
    }

    float ratio = 1.0F;
    while (cents != 0)
    {
        if ((cents & 1) != 0)
        {
            ratio *= step;
        }

        step *= step;
        cents >>= 1;
    }

    return ratio;
}

AlWave WaveOfSound(const AlSoundBank& bank, const BankSound& sound)
{
    const BankWave& wave = bank.file->waves[static_cast<std::size_t>(sound.wave)];
    AlWave out;
    out.raw = wave.type == kWaveRaw16;
    out.base = bank.sample_address + wave.base;
    out.length = wave.length;
    if (wave.loop != kNoObject)
    {
        const BankLoop& loop = bank.file->loops[static_cast<std::size_t>(wave.loop)];
        out.has_loop = true;
        out.loop_start = loop.start;
        out.loop_end = loop.end;
        out.loop_count = loop.count;
        constexpr uint32_t kLoopStateOffset = 12;
        out.loop_state = bank.file_address + loop.offset + kLoopStateOffset;
    }

    if (wave.book != kNoObject)
    {
        const BankBook& book = bank.file->books[static_cast<std::size_t>(wave.book)];
        constexpr uint32_t kBookCoefficients = 8;
        out.book = bank.file_address + book.offset + kBookCoefficients;
        out.book_size = 2 * book.book.order * book.book.predictor_count * 8;
    }

    return out;
}

AlPlayer::AlPlayer(const AlPlayerConfig& config, AlSynth& synth)
    : channels_(config.channels),
      max_events_(std::max(config.events, 1)),
      library_form_(config.form),
      oscillators_(config.oscillators)
{
    if (library_form_ == AlPlayerForm::kPlayerScale)
    {
        volume_ = 0x7800;
    }

    next_event_.type = EventType::kPeriodic;
    channels_state_.resize(static_cast<std::size_t>(channels_));
    voices_.resize(static_cast<std::size_t>(config.voices));
    const int first = synth.AddVoices(config.voices);

    // The free list's head is the voice added last. The back of the vector is the head.
    for (int i = 0; i < config.voices; i++)
    {
        voices_[static_cast<std::size_t>(i)].synth_voice = first + i;
        free_voices_.push_back(i);
    }

    samples_left = synth.CurrentSamples();
}

void AlPlayer::Post(const Event& event, int32_t delta)
{
    if (static_cast<int>(queue_.size()) >= max_events_)
    {
        return;
    }

    const bool at_end = delta == kQueueEnd;
    for (std::size_t i = 0; i <= queue_.size(); i++)
    {
        if (i == queue_.size())
        {
            queue_.push_back({event, at_end ? 0 : delta});
            return;
        }

        if (delta < queue_[i].delta)
        {
            queue_[i].delta -= delta;
            queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(i), QueueItem{event, delta});
            return;
        }

        delta -= queue_[i].delta;
    }
}

int32_t AlPlayer::NextEvent(Event& event)
{
    if (queue_.empty())
    {
        event.type = EventType::kPeriodic;

        return 0;
    }

    event = queue_.front().event;
    const int32_t delta = queue_.front().delta;
    queue_.erase(queue_.begin());

    return delta;
}

void AlPlayer::FlushType(EventType type)
{
    for (std::size_t i = 0; i < queue_.size();)
    {
        if (queue_[i].event.type == type)
        {
            if (i + 1 < queue_.size())
            {
                queue_[i + 1].delta += queue_[i].delta;
            }

            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            i++;
        }
    }
}

void AlPlayer::StopOscillators(int voice)
{
    VoiceState& state = voices_[static_cast<std::size_t>(voice)];
    for (std::size_t i = 0; i < queue_.size() && state.oscillators != 0;)
    {
        const Event& event = queue_[i].event;
        if ((event.type != EventType::kTremolo && event.type != EventType::kVibrato) || event.voice != voice)
        {
            i++;
            continue;
        }

        state.oscillators &= event.type == EventType::kTremolo ? uint8_t{0xFE} : uint8_t{0xFD};
        free_oscillators_.push_back(event.oscillator);
        if (i + 1 < queue_.size())
        {
            queue_[i + 1].delta += queue_[i].delta;
        }

        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

int32_t AlPlayer::StartOscillator(int& state, float& value, uint8_t type, uint8_t rate, uint8_t depth, uint8_t delay)
{
    if (free_oscillators_.empty())
    {
        free_oscillators_.push_back(static_cast<int>(oscillator_states_.size()));
        oscillator_states_.emplace_back();
    }

    state = free_oscillators_.back();
    free_oscillators_.pop_back();
    Oscillator& oscillator = oscillator_states_[static_cast<std::size_t>(state)];
    oscillator = {};
    oscillator.type = type;
    const auto long_period = static_cast<uint16_t>(256 - rate);
    const auto sine_period = static_cast<uint16_t>(259 - rate);
    switch (type)
    {
    case kOscillatorTremoloSine:
        oscillator.period = sine_period;
        oscillator.depth = static_cast<uint8_t>(depth >> 1);
        oscillator.level = static_cast<uint8_t>(kVolumeFull - oscillator.depth);
        value = static_cast<float>(oscillator.level);
        break;

    case kOscillatorTremoloSquare:
        oscillator.period = long_period;
        oscillator.count = long_period;
        oscillator.low_level = static_cast<uint8_t>(kVolumeFull - depth);
        oscillator.level = kVolumeFull;
        value = static_cast<float>(kVolumeFull);
        break;

    case kOscillatorTremoloFall:
        oscillator.period = long_period;
        oscillator.level = kVolumeFull;
        oscillator.depth = depth;
        value = static_cast<float>(kVolumeFull);
        break;

    case kOscillatorTremoloRise:
        oscillator.period = long_period;
        oscillator.depth = depth;
        oscillator.level = static_cast<uint8_t>(kVolumeFull - depth);
        value = static_cast<float>(oscillator.level);
        break;

    case kOscillatorVibratoSine:
        oscillator.period = sine_period;
        oscillator.cents = DepthToCents(depth);
        value = 1.0F;
        break;

    case kOscillatorVibratoSquare:
        {
            oscillator.period = long_period;
            oscillator.count = long_period;
            const auto cents = static_cast<int32_t>(DepthToCents(depth));
            oscillator.low_ratio = CentsToRatio(-cents);
            oscillator.high_ratio = CentsToRatio(cents);
            value = oscillator.high_ratio;
            break;
        }

    case kOscillatorVibratoFall:
        {
            oscillator.period = long_period;
            oscillator.count = long_period;
            const auto cents = static_cast<int32_t>(DepthToCents(depth));
            oscillator.start_cents = cents;
            oscillator.cents_range = cents * 2;
            value = CentsToRatio(cents);
            break;
        }

    case kOscillatorVibratoRise:
        {
            oscillator.period = long_period;
            oscillator.count = long_period;
            const auto cents = static_cast<int32_t>(DepthToCents(depth));
            oscillator.start_cents = -cents;
            oscillator.cents_range = cents * 2;
            value = CentsToRatio(-cents);
            break;
        }

    default:
        break;
    }

    return static_cast<int32_t>(delay) << 14;
}

void AlPlayer::RemoveEnvelopeEvents(int voice)
{
    for (std::size_t i = 0; i < queue_.size();)
    {
        if (queue_[i].event.type == EventType::kEnvelope && queue_[i].event.voice == voice)
        {
            if (i + 1 < queue_.size())
            {
                queue_[i + 1].delta += queue_[i].delta;
            }

            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            i++;
        }
    }
}

bool AlPlayer::NeedsNoteKill(int voice, int32_t kill_time)
{
    int32_t item_time = 0;
    for (std::size_t i = 0; i < queue_.size(); i++)
    {
        item_time += queue_[i].delta;
        if (queue_[i].event.type == EventType::kNoteEnd && queue_[i].event.voice == voice)
        {
            if (item_time > kill_time)
            {
                if (i + 1 < queue_.size())
                {
                    queue_[i + 1].delta += queue_[i].delta;
                }

                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));

                return true;
            }

            return false;
        }
    }

    return true;
}

void AlPlayer::SetBank(const AlSoundBank& bank)
{
    file_ = bank.file;
    bank_ = bank.bank;
    file_address_ = bank.file_address;
    sample_address_ = bank.sample_address;
    Event event;
    event.type = EventType::kSetBank;
    Post(event, 0);
}

void AlPlayer::SetSequence(AlSequenceForm form, std::span<const uint8_t> bytes)
{
    pending_bytes_.assign(bytes.begin(), bytes.end());
    form_ = form;
    Event event;
    event.type = EventType::kSetSequence;
    Post(event, 0);
}

void AlPlayer::SetVolume(int16_t volume)
{
    Event event;
    event.type = EventType::kPlayerVolume;
    event.player_volume = volume;
    Post(event, 0);
}

void AlPlayer::SetFxOffset(int8_t offset)
{
    fx_offset_ = offset;
}

void AlPlayer::Play()
{
    Event event;
    event.type = EventType::kPlay;
    Post(event, 0);
}

void AlPlayer::Stop()
{
    Event event;
    event.type = EventType::kStopping;
    Post(event, 0);
}

void AlPlayer::SetLoop(int32_t start_ticks, int32_t end_ticks, int32_t count)
{
    Event event;
    event.type = EventType::kSetLoop;
    event.loop_start = start_ticks;
    event.loop_end = end_ticks;
    event.loop_count = count;
    Post(event, 0);
}

void AlPlayer::SendMidi(uint8_t status, uint8_t byte1, uint8_t byte2)
{
    Event event;
    event.type = EventType::kMidi;
    event.status = status;
    event.byte1 = byte1;
    event.byte2 = byte2;
    Post(event, 0);
}

AlPlayerState AlPlayer::State() const
{
    return state_;
}

int32_t AlPlayer::Time() const
{
    return time_;
}

int64_t AlPlayer::TickPosition() const
{
    return form_ == AlSequenceForm::kCompressed ? last_ticks_ : standard_ticks_;
}

int32_t AlPlayer::Handle(AlSynth& synth)
{
    int32_t next_delta = 0;
    do
    {
        switch (next_event_.type)
        {
        case EventType::kSequence:
            HandleSequenceEvent(synth);
            break;

        case EventType::kPeriodic:
            {
                Event event;
                event.type = EventType::kPeriodic;
                Post(event, kFrameMicroseconds);
                break;
            }

        case EventType::kNoteEnd:
            EndVoice(synth, next_event_.voice);
            break;

        case EventType::kTremolo:
        case EventType::kVibrato:
            StepOscillator(synth);
            break;

        case EventType::kEnvelope:
            NextEnvelopeSegment(synth);
            break;

        case EventType::kMidi:
        case EventType::kNoteOff:
            HandleMidi(synth, next_event_);
            break;

        case EventType::kPlayerVolume:
            volume_ = next_event_.player_volume;
            for (const int index : active_voices_)
            {
                ChangeVoiceLevel(synth, index);
            }

            break;

        case EventType::kPlay:
            if (state_ != AlPlayerState::kPlaying)
            {
                state_ = AlPlayerState::kPlaying;
                PostNextSequenceEvent();
            }

            break;

        case EventType::kStop:
            FinishStop(synth);
            break;

        case EventType::kStopping:
            StartStopping(synth);
            break;

        case EventType::kSetSequence:
            LoadSequence();
            break;

        case EventType::kSetBank:
            InitFromBank();
            break;

        case EventType::kSetLoop:
            loop_start_ = MakeMarker(next_event_.loop_start);
            loop_end_ = MakeMarker(next_event_.loop_end);
            loop_count_ = next_event_.loop_count;
            break;
        }

        next_delta = NextEvent(next_event_);
    } while (next_delta == 0);

    time_ += next_delta;

    return next_delta;
}

// The envelope segment that the event starts: the voice's target level and the time to reach it.
void AlPlayer::NextEnvelopeSegment(AlSynth& synth)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(next_event_.voice)];
    if (voice.envelope_phase == EnvelopePhase::kAttack)
    {
        voice.envelope_phase = EnvelopePhase::kDecay;
    }

    const int32_t delta = next_event_.delta;
    voice.envelope_gain = next_event_.volume;
    voice.envelope_end = time_ + delta;
    synth.SetVolume(voice.synth_voice, VoiceVolume(voice), delta);
}

// Stops a voice and frees it.
void AlPlayer::EndVoice(AlSynth& synth, int voice)
{
    const int synth_voice = voices_[static_cast<std::size_t>(voice)].synth_voice;
    synth.StopVoice(synth_voice);
    synth.FreeVoice(synth_voice);
    StopOscillators(voice);
    UnmapVoice(voice);
}

// The next step of a voice's tremolo or vibrato.
void AlPlayer::StepOscillator(AlSynth& synth)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(next_event_.voice)];
    Oscillator& oscillator = oscillator_states_[static_cast<std::size_t>(next_event_.oscillator)];
    float value = 0.0F;
    const int32_t delay = UpdateOscillatorState(
        oscillator.type, oscillator.second_half, oscillator.period, oscillator.count, oscillator.level,
        oscillator.depth, oscillator.low_level, oscillator.cents, oscillator.low_ratio, oscillator.high_ratio,
        oscillator.start_cents, oscillator.cents_range, value);
    if (next_event_.type == EventType::kTremolo)
    {
        voice.tremolo = static_cast<uint8_t>(static_cast<int32_t>(value));
        synth.SetVolume(voice.synth_voice, VoiceVolume(voice), VoiceDelta(voice));
    }
    else
    {
        voice.vibrato = value;
        const float pitch = voice.pitch * voice.vibrato;
        synth.SetPitch(voice.synth_voice, channels_state_[voice.channel].pitch_bend * pitch);
    }

    Post(next_event_, delay);
}

// The end of a stop: every voice ends, and the player stops.
void AlPlayer::FinishStop(AlSynth& synth)
{
    if (state_ != AlPlayerState::kStopping)
    {
        return;
    }

    while (!active_voices_.empty())
    {
        EndVoice(synth, active_voices_.front());
    }

    // Only ALSeqPlayer starts its time again.
    if (form_ == AlSequenceForm::kStandard)
    {
        time_ = 0;
    }

    state_ = AlPlayerState::kStopped;
}

// The start of a stop: the sequence's events leave the queue, and the voices release over the kill time.
void AlPlayer::StartStopping(AlSynth& synth)
{
    if (state_ != AlPlayerState::kPlaying)
    {
        return;
    }

    FlushType(EventType::kSequence);
    if (form_ == AlSequenceForm::kCompressed)
    {
        FlushType(EventType::kNoteOff);
    }

    FlushType(EventType::kMidi);
    for (const int index : std::vector<int>(active_voices_))
    {
        if (NeedsNoteKill(index, kKillTime))
        {
            ReleaseVoice(synth, index, kKillTime);
        }
    }

    state_ = AlPlayerState::kStopping;
    Event event;
    event.type = EventType::kStop;
    Post(event, kQueueEnd);
}

// Switches to the sequence that SetSequence queued, and starts its reader at the first tempo.
void AlPlayer::LoadSequence()
{
    bytes_ = pending_bytes_;
    has_sequence_ = true;
    if (form_ == AlSequenceForm::kCompressed)
    {
        NewCompressed();
    }
    else
    {
        NewStandard();
    }

    SetTempo(kInitialTempo);
    if (file_ != nullptr)
    {
        InitFromBank();
    }
}

void AlPlayer::PostNextSequenceEvent()
{
    if (state_ != AlPlayerState::kPlaying || !has_sequence_)
    {
        return;
    }

    int32_t ticks = 0;
    if (form_ == AlSequenceForm::kCompressed)
    {
        if (!CompressedNextDelta(ticks))
        {
            return;
        }
    }
    else
    {
        if (!StandardNextDelta(ticks))
        {
            return;
        }

        if (loop_count_ != 0 && standard_ticks_ + ticks >= loop_end_.current_ticks)
        {
            place_ = loop_start_.place;
            last_status_ = loop_start_.last_status;
            standard_ticks_ = loop_start_.last_ticks;
            if (loop_count_ != -1)
            {
                loop_count_--;
            }
        }
    }

    Event event;
    event.type = EventType::kSequence;
    Post(event, ticks * microseconds_per_tick_);
}

void AlPlayer::HandleSequenceEvent(AlSynth& synth)
{
    if (!has_sequence_)
    {
        return;
    }

    const SequenceEvent next = form_ == AlSequenceForm::kCompressed ? CompressedNextEvent() : StandardNextEvent();
    switch (next.type)
    {
    case SequenceEventType::kMidi:
        {
            Event event;
            event.type = EventType::kMidi;
            event.status = next.status;
            event.byte1 = next.byte1;
            event.byte2 = next.byte2;
            event.duration = next.duration;
            event.offset = next.offset;
            HandleMidi(synth, event);
            PostNextSequenceEvent();
            break;
        }

    case SequenceEventType::kTempo:
        HandleTempo(next);
        PostNextSequenceEvent();
        break;

    case SequenceEventType::kEnd:
        {
            state_ = AlPlayerState::kStopping;
            Event event;
            event.type = EventType::kStop;
            Post(event, kQueueEnd);
            break;
        }

    case SequenceEventType::kTrackEnd:
    case SequenceEventType::kLoopStart:
    case SequenceEventType::kLoopEnd:
        PostNextSequenceEvent();
        break;
    }
}

void AlPlayer::HandleMidi(AlSynth& synth, const Event& event)
{
    const auto status = static_cast<uint8_t>(event.status & 0xF0);
    const auto channel_number = static_cast<uint8_t>(event.status & 0x0F);
    const uint8_t byte1 = event.byte1;
    const uint8_t byte2 = event.byte2;
    if (channel_number >= channels_state_.size())
    {
        return;
    }

    const bool note_on = status == kStatusNoteOn && byte2 != 0;
    if (note_on)
    {
        StartNote(synth, event, channel_number);

        return;
    }

    switch (status)
    {
    case kStatusNoteOn:
    case kStatusNoteOff:
        ReleaseNote(synth, channel_number, byte1);
        break;

    case kStatusPolyPressure:
        {
            const int index = LookupVoice(byte1, channel_number);
            if (index >= 0)
            {
                VoiceState& voice = voices_[static_cast<std::size_t>(index)];
                voice.velocity = byte2;
                synth.SetVolume(voice.synth_voice, VoiceVolume(voice), VoiceDelta(voice));
            }

            break;
        }

    case kStatusChannelPressure:
        for (const int index : active_voices_)
        {
            VoiceState& voice = voices_[static_cast<std::size_t>(index)];
            if (voice.channel == channel_number)
            {
                voice.velocity = byte1;
                synth.SetVolume(voice.synth_voice, VoiceVolume(voice), VoiceDelta(voice));
            }
        }

        break;

    case kStatusController:
        HandleController(synth, channel_number, byte1, byte2);
        break;

    case kStatusProgram:
        {
            const std::vector<int32_t>& instruments = file_->banks[bank_].instruments;
            if (byte1 < instruments.size() && instruments[byte1] != kNoObject)
            {
                SetInstrument(file_->instruments[static_cast<std::size_t>(instruments[byte1])], channel_number);
            }

            break;
        }

    case kStatusPitchBend:
        SetPitchBend(synth, channel_number, byte1, byte2);
        break;

    default:
        break;
    }
}

// A note off: the release of the voice that plays the key, or its wait for the sustain pedal.
void AlPlayer::ReleaseNote(AlSynth& synth, uint8_t channel_number, uint8_t byte1)
{
    const int index = LookupVoice(byte1, channel_number);
    if (index < 0)
    {
        return;
    }

    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    if (voice.phase == NotePhase::kSustain)
    {
        voice.phase = NotePhase::kSustainRelease;
    }
    else
    {
        voice.phase = NotePhase::kRelease;
        const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(voice.sound->envelope)];
        ReleaseVoice(synth, index, envelope.release_time);
    }
}

// A pitch bend of a channel, on each of its voices.
void AlPlayer::SetPitchBend(AlSynth& synth, uint8_t channel_number, uint8_t byte1, uint8_t byte2)
{
    ChannelState& channel = channels_state_[channel_number];
    const int32_t bend = (byte2 << 7) + byte1 - 8192;
    const int32_t cents = (channel.bend_range * bend) / 8192;
    const float ratio = CentsToRatio(cents);
    channel.pitch_bend = ratio;
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number)
        {
            synth.SetPitch(voice.synth_voice, voice.pitch * ratio * voice.vibrato);
        }
    }
}

// Starts a note: a voice with the sound that the key and velocity pick, its envelope and its oscillators.
void AlPlayer::StartNote(AlSynth& synth, const Event& event, uint8_t channel_number)
{
    const uint8_t byte1 = event.byte1;
    const uint8_t byte2 = event.byte2;
    const ChannelState& channel = channels_state_[channel_number];
    if (state_ != AlPlayerState::kPlaying)
    {
        return;
    }

    const BankSound* sound = LookupSound(byte1, byte2, channel_number);
    if (sound == nullptr)
    {
        return;
    }

    const int index = MapVoice(byte1, byte2, channel_number);
    if (index < 0)
    {
        return;
    }

    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    synth.AllocVoice(voice.synth_voice, channel.priority, false);
    synth.SetVoiceTag(voice.synth_voice, channel_number);
    const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(sound->envelope)];
    const BankKeyMap& key_map = file_->key_maps[static_cast<std::size_t>(sound->key_map)];
    voice.sound = sound;
    voice.envelope_phase = EnvelopePhase::kAttack;
    voice.phase = channel.sustain > kSustainThreshold ? NotePhase::kSustain : NotePhase::kNoteOn;
    const auto cents = static_cast<int16_t>((byte1 - key_map.key_base) * 100 + key_map.detune);
    voice.pitch = CentsToRatio(cents);
    voice.envelope_gain = envelope.attack_volume;
    voice.envelope_end = time_ + envelope.attack_time;
    StartVoiceOscillators(index, channel);
    const float pitch = voice.pitch * channel.pitch_bend * voice.vibrato;
    synth.StartVoiceParams(voice.synth_voice, WaveOf(*sound), pitch, VoiceVolume(voice), VoicePan(voice),
                           FxMix(channel), envelope.attack_time);

    Event decay;
    decay.type = EventType::kEnvelope;
    decay.voice = index;
    decay.volume = envelope.decay_volume;
    decay.delta = envelope.decay_time;
    Post(decay, envelope.attack_time);

    if (form_ == AlSequenceForm::kCompressed && event.duration != 0)
    {
        Event note_off;
        note_off.type = EventType::kNoteOff;
        note_off.status = static_cast<uint8_t>(channel_number | kStatusNoteOff);
        note_off.byte1 = byte1;
        Post(note_off, microseconds_per_tick_ * static_cast<int32_t>(event.duration));
    }

    if (on_note)
    {
        on_note({channel_number, byte1, byte2, event.duration, event.offset, time_});
    }
}

// The tremolo and the vibrato of a new voice, from the instrument of its channel.
void AlPlayer::StartVoiceOscillators(int index, const ChannelState& channel)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    voice.tremolo = kVolumeFull;
    voice.vibrato = 1.0F;
    voice.oscillators = 0;
    if (oscillators_ && channel.instrument != nullptr)
    {
        const BankInstrument& instrument = *channel.instrument;
        float value = static_cast<float>(kVolumeFull);
        if (instrument.tremolo_type != 0)
        {
            Event wave;
            wave.type = EventType::kTremolo;
            wave.voice = index;
            const int32_t delay =
                StartOscillator(wave.oscillator, value, instrument.tremolo_type, instrument.tremolo_rate,
                                instrument.tremolo_depth, instrument.tremolo_delay);
            if (delay != 0)
            {
                Post(wave, delay);
                voice.oscillators |= 1;
            }
        }

        voice.tremolo = static_cast<uint8_t>(static_cast<int32_t>(value));
        value = 1.0F;
        if (instrument.vibrato_type != 0)
        {
            Event wave;
            wave.type = EventType::kVibrato;
            wave.voice = index;
            const int32_t delay =
                StartOscillator(wave.oscillator, value, instrument.vibrato_type, instrument.vibrato_rate,
                                instrument.vibrato_depth, instrument.vibrato_delay);
            if (delay != 0)
            {
                Post(wave, delay);
                voice.oscillators |= 2;
            }
        }

        voice.vibrato = value;
    }
}

// A controller's change on a channel.
void AlPlayer::HandleController(AlSynth& synth, uint8_t channel_number, uint8_t byte1, uint8_t byte2)
{
    ChannelState& channel = channels_state_[channel_number];
    switch (byte1)
    {
    case kControllerPan:
        channel.pan = byte2;
        ApplyChannelPan(synth, channel_number);
        break;

    case kControllerVolume:
        channel.volume = byte2;
        ApplyChannelLevel(synth, channel_number);
        break;

    case kControllerChannelScale:
        if (library_form_ == AlPlayerForm::kPlayerScale)
        {
            channel.scale = static_cast<int8_t>(byte2);
            ApplyChannelLevel(synth, channel_number);
        }

        break;

    case kControllerPlayerScale:
        if (library_form_ == AlPlayerForm::kPlayerScale)
        {
            player_scale_ = byte2;
            for (const int index : active_voices_)
            {
                RestartDecay(synth, index);
            }
        }

        break;

    case kControllerPriority:
        channel.priority = byte2;
        break;

    case kControllerSustain:
        channel.sustain = byte2;
        ApplySustain(synth, channel_number, byte2);
        break;

    case kControllerFxMix:
        channel.fx_mix = byte2;
        ApplyFxMix(synth, channel_number, byte2);
        break;

    default:
        break;
    }
}

// The pan of a channel, on each of its voices.
void AlPlayer::ApplyChannelPan(AlSynth& synth, uint8_t channel_number)
{
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number)
        {
            synth.SetPan(voice.synth_voice, VoicePan(voice));
            if (library_form_ == AlPlayerForm::kPlayerScale)
            {
                RestartDecay(synth, index);
            }
        }
    }
}

// The level of a channel, on each of its voices that isn't in its release.
void AlPlayer::ApplyChannelLevel(AlSynth& synth, uint8_t channel_number)
{
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number && voice.envelope_phase != EnvelopePhase::kRelease)
        {
            ChangeVoiceLevel(synth, index);
        }
    }
}

// The sustain pedal of a channel, on each of its voices.
void AlPlayer::ApplySustain(AlSynth& synth, uint8_t channel_number, uint8_t byte2)
{
    for (const int index : std::vector<int>(active_voices_))
    {
        VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel != channel_number || voice.phase == NotePhase::kRelease)
        {
            continue;
        }

        if (byte2 > kSustainThreshold)
        {
            if (voice.phase == NotePhase::kNoteOn)
            {
                voice.phase = NotePhase::kSustain;
            }
        }
        else if (voice.phase == NotePhase::kSustain)
        {
            voice.phase = NotePhase::kNoteOn;
        }
        else if (voice.phase == NotePhase::kSustainRelease)
        {
            voice.phase = NotePhase::kRelease;
            const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(voice.sound->envelope)];
            ReleaseVoice(synth, index, envelope.release_time);
        }
    }
}

// The effect mix of a channel, on its voices.
void AlPlayer::ApplyFxMix(AlSynth& synth, uint8_t channel_number, uint8_t byte2)
{
    // The offset adds up over the channel's voices. Pokémon Snap's player sets the mix on every voice.
    uint8_t mix = byte2;
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number)
        {
            mix = OffsetFxMix(mix, fx_offset_);
            synth.SetFxMix(voice.synth_voice, mix);
        }
        else if (library_form_ == AlPlayerForm::kPlayerScale)
        {
            synth.SetFxMix(voice.synth_voice, mix);
        }
    }
}

void AlPlayer::HandleTempo(const SequenceEvent& event)
{
    const int32_t old_tick_time = microseconds_per_tick_;
    const int32_t tempo = (event.byte1 << 16) | (event.byte2 << 8) | event.byte3;
    SetTempo(static_cast<float>(tempo));
    if (form_ != AlSequenceForm::kCompressed)
    {
        return;
    }

    // The queued note offs keep their ticks. Each goes back in the queue at the new tempo, with the last taken first.
    std::vector<QueueItem> note_offs;
    int32_t current = 0;
    for (std::size_t i = 0; i < queue_.size();)
    {
        current += queue_[i].delta;
        if (queue_[i].event.type == EventType::kNoteOff)
        {
            QueueItem item = queue_[i];
            const int32_t delta = item.delta;
            item.delta = current;
            if (i + 1 < queue_.size())
            {
                current -= delta;
                queue_[i + 1].delta += delta;
            }

            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));

            // The first stays first. Each later one goes just after it. The rest therefore run in reverse.
            if (note_offs.empty())
            {
                note_offs.push_back(item);
            }
            else
            {
                note_offs.insert(note_offs.begin() + 1, item);
            }
        }
        else
        {
            i++;
        }
    }

    for (QueueItem& item : note_offs)
    {
        const auto ticks = static_cast<uint32_t>(item.delta) / static_cast<uint32_t>(old_tick_time);
        int32_t delta = static_cast<int32_t>(ticks) * microseconds_per_tick_;
        bool placed = false;
        for (std::size_t i = 0; i < queue_.size(); i++)
        {
            if (delta < queue_[i].delta)
            {
                queue_[i].delta -= delta;
                queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(i), QueueItem{item.event, delta});
                placed = true;
                break;
            }

            delta -= queue_[i].delta;
        }

        if (!placed)
        {
            queue_.push_back({item.event, delta});
        }
    }
}

void AlPlayer::ReleaseVoice(AlSynth& synth, int voice_index, int32_t time)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(voice_index)];
    if (voice.envelope_phase == EnvelopePhase::kAttack)
    {
        RemoveEnvelopeEvents(voice_index);
    }

    voice.velocity = 0;
    voice.envelope_phase = EnvelopePhase::kRelease;
    voice.envelope_gain = 0;
    voice.envelope_end = time_ + time;
    synth.SetPriority(voice.synth_voice, 0);
    synth.SetVolume(voice.synth_voice, 0, time);

    Event event;
    event.type = EventType::kNoteEnd;
    event.voice = voice_index;
    Post(event, time);
}

int AlPlayer::MapVoice(uint8_t key, uint8_t velocity, uint8_t channel)
{
    if (free_voices_.empty())
    {
        return -1;
    }

    const int index = free_voices_.back();
    free_voices_.pop_back();
    active_voices_.push_back(index);
    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    voice.channel = channel;
    voice.key = key;
    voice.velocity = velocity;

    return index;
}

void AlPlayer::UnmapVoice(int voice)
{
    const auto place = std::find(active_voices_.begin(), active_voices_.end(), voice);
    if (place != active_voices_.end())
    {
        active_voices_.erase(place);
        free_voices_.push_back(voice);
    }
}

int AlPlayer::LookupVoice(uint8_t key, uint8_t channel) const
{
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.key == key && voice.channel == channel && voice.phase != NotePhase::kRelease &&
            voice.phase != NotePhase::kSustainRelease)
        {
            return index;
        }
    }

    return -1;
}

const BankSound* AlPlayer::LookupSound(uint8_t key, uint8_t velocity, uint8_t channel) const
{
    const BankInstrument* instrument = channels_state_[channel].instrument;
    if (instrument == nullptr)
    {
        return nullptr;
    }

    // A binary search. The library counts from 1.
    int left = 1;
    int right = static_cast<int>(instrument->sounds.size());
    while (right >= left)
    {
        const int middle = (left + right) / 2;
        const BankSound& sound =
            file_->sounds[static_cast<std::size_t>(instrument->sounds[static_cast<std::size_t>(middle - 1)])];
        const BankKeyMap& key_map = file_->key_maps[static_cast<std::size_t>(sound.key_map)];
        if (key >= key_map.key_min && key <= key_map.key_max && velocity >= key_map.velocity_min &&
            velocity <= key_map.velocity_max)
        {
            return &sound;
        }

        if (key < key_map.key_min || (velocity < key_map.velocity_min && key <= key_map.key_max))
        {
            right = middle - 1;
        }
        else
        {
            left = middle + 1;
        }
    }

    return nullptr;
}

int16_t AlPlayer::VoiceVolume(const VoiceState& voice) const
{
    const ChannelState& state = channels_state_[voice.channel];
    auto note = static_cast<uint32_t>((voice.tremolo * voice.velocity * voice.envelope_gain) >> 6);
    auto channel = static_cast<uint32_t>((voice.sound->volume * volume_ * state.volume) >> 14);
    if (library_form_ == AlPlayerForm::kPlayerScale)
    {
        channel = static_cast<uint32_t>((player_scale_ * volume_ * state.volume) >> 14);
        if (state.scale >= 0)
        {
            channel = (static_cast<uint32_t>(state.scale) * channel) >> 7;
        }
    }

    note *= channel;
    note >>= 15;

    return static_cast<int16_t>(note);
}

uint8_t AlPlayer::FxMix(const ChannelState& channel) const
{
    return OffsetFxMix(channel.fx_mix, fx_offset_);
}

void AlPlayer::ChangeVoiceLevel(AlSynth& synth, int voice)
{
    if (library_form_ == AlPlayerForm::kPlayerScale && RestartDecay(synth, voice))
    {
        return;
    }

    const VoiceState& state = voices_[static_cast<std::size_t>(voice)];
    synth.SetVolume(state.synth_voice, VoiceVolume(state), VoiceDelta(state));
}

bool AlPlayer::RestartDecay(AlSynth& synth, int voice)
{
    VoiceState& state = voices_[static_cast<std::size_t>(voice)];
    const int32_t left = state.envelope_end - time_;
    if (state.envelope_phase != EnvelopePhase::kDecay || left <= kGainChangeTime)
    {
        return false;
    }

    const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(state.sound->envelope)];
    const auto attack = static_cast<float>(envelope.attack_volume);
    const float rate = DecayRate(attack, static_cast<float>(state.envelope_gain), envelope.decay_time);
    const float gain = DecayGain(attack, envelope.decay_time - left + kGainChangeTime, rate);
    state.envelope_gain = static_cast<uint8_t>(static_cast<int32_t>(gain));
    synth.SetVolume(state.synth_voice, VoiceVolume(state), kGainChangeTime);
    state.envelope_phase = EnvelopePhase::kAttack;

    Event decay;
    decay.type = EventType::kEnvelope;
    decay.voice = voice;
    decay.volume = envelope.decay_volume;
    decay.delta = left - kGainChangeTime;
    Post(decay, kGainChangeTime);

    return true;
}

int32_t AlPlayer::VoiceDelta(const VoiceState& voice) const
{
    const int32_t delta = voice.envelope_end - time_;

    return delta >= 0 ? delta : kGainChangeTime;
}

uint8_t AlPlayer::VoicePan(const VoiceState& voice) const
{
    const int pan = channels_state_[voice.channel].pan - kPanCenter + voice.sound->pan;

    return static_cast<uint8_t>(std::clamp(pan, 0, 127));
}

AlWave AlPlayer::WaveOf(const BankSound& sound) const
{
    return WaveOfSound({file_, bank_, file_address_, sample_address_}, sound);
}

void AlPlayer::InitFromBank()
{
    const Bank& bank = file_->banks[bank_];
    const BankInstrument* first = nullptr;
    for (const int32_t index : bank.instruments)
    {
        if (index != kNoObject)
        {
            first = &file_->instruments[static_cast<std::size_t>(index)];
            break;
        }
    }

    for (int i = 0; i < channels_; i++)
    {
        ResetChannel(i);
        if (first != nullptr)
        {
            SetInstrument(*first, i);
        }
    }

    if (bank.percussion != kNoObject && channels_ > 9)
    {
        SetInstrument(file_->instruments[static_cast<std::size_t>(bank.percussion)], 9);
    }
}

void AlPlayer::SetInstrument(const BankInstrument& instrument, int channel)
{
    ChannelState& state = channels_state_[static_cast<std::size_t>(channel)];
    state.instrument = &instrument;
    state.pan = instrument.pan;
    state.volume = instrument.volume;
    state.priority = instrument.priority;
    state.bend_range = instrument.bend_range;
}

void AlPlayer::ResetChannel(int channel)
{
    ChannelState& state = channels_state_[static_cast<std::size_t>(channel)];
    state.fx_mix = 0;
    state.pan = kPanCenter;
    state.volume = library_form_ == AlPlayerForm::kPlayerScale ? kScaledFormVolume : kVolumeFull;
    state.priority = kDefaultPriority;
    state.sustain = 0;
    state.bend_range = 200;
    state.pitch_bend = 1.0F;
}

void AlPlayer::SetTempo(float tempo)
{
    microseconds_per_tick_ =
        has_sequence_ ? static_cast<int32_t>(tempo * ticks_per_quarter_inverse_) : kInitialTickTime;
}

void BuildAlFrame(AlSynth& synth, AlPlayer& player, AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    // The player runs for every event that comes before the frame's end. Each call sets the driver's time for updates
    // to the event's time, on a multiple of 16 samples.
    int32_t next = player.samples_left;
    while (next - synth.CurrentSamples() < samples)
    {
        synth.SetParamSamples(next & ~0xF);
        player.samples_left += synth.TimeToSamplesNoRound(player.Handle(synth));
        next = player.samples_left;
    }

    synth.SetParamSamples(next & ~0xF);
    synth.BuildCommands(list, memory, output, samples);
}

} // namespace ntdlib::n64
