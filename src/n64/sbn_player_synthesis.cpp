// SPDX-License-Identifier: MIT

// The synthesis driver in the model of the SBN engine: the voices and effects on the driver side, and the audio list
// of each chunk.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "xsflib/bytes.h"

#include "equal_power.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/sbn_player.h"
#include "sbn_player_constants.h"

namespace ntdlib::n64
{

namespace
{

using sbn_player_constants::kChunk;
using sbn_player_constants::kChunkBytes;
using sbn_player_constants::kDelayLeft;
using sbn_player_constants::kDelayNone;
using sbn_player_constants::kFxLength;

constexpr int kDecoderOut = 0x170;
constexpr int kDecoderIn = 0;
constexpr int kTemp = 0;
constexpr uint32_t kMainLeft = 0x4E0;
constexpr uint32_t kMainRight = 0x650;
constexpr uint32_t kAuxLeft = 0x7C0;
constexpr uint32_t kAuxRight = 0x930;
constexpr uint32_t kOpLoad = 0x04000000;
constexpr uint32_t kOpSave = 0x06000000;
constexpr uint32_t kOpMix = 0x0C000000;
constexpr uint32_t kOpMove = 0x0A000000;
constexpr uint32_t kAddressMask = 0xFFFFFF;
constexpr uint32_t kDmemMask = 0xFFF;
constexpr float kUnityPitch = 32768.0F;
constexpr float kMaxRatio = 1.99996F;
constexpr int kFrameSamples = 16;
constexpr int kFrameBytes = 9;

struct EffectSection
{
    int input;
    int output;
    int feed_back;
    int feed_forward;
    int gain;
    int rate;
    int depth;
    int cutoff;
};

struct EffectPreset
{
    int length;
    std::vector<EffectSection> sections;
};

// The presets of the game's code, in units of chunks.
const EffectPreset& PresetOf(uint8_t type)
{
    static const EffectPreset kNone = {0, {}};
    static const EffectPreset kSmallRoom = {
        11, {{0, 9, 9830, -9830, 0, 0, 0, 0}, {3, 7, 3276, -3276, 0x3FFF, 0, 0, 0}, {0, 10, 5000, 0, 0, 0, 0, 0x5000}}};
    static const EffectPreset kBigRoom = {14,
                                          {{0, 9, 9830, -9830, 0, 0, 0, 0},
                                           {2, 6, 3276, -3276, 0x3FFF, 0, 0, 0},
                                           {9, 12, 3276, -3276, 0x3FFF, 0, 0, 0},
                                           {0, 13, 6000, 0, 0, 0, 0, 0x5000}}};
    static const EffectPreset kEcho = {14, {{0, 13, 20000, 0, 0x7FFF, 0, 0, 0x7FFF}}};
    static const EffectPreset kChorus = {3, {{0, 1, 16384, 0, 0x7FFF, 7600, 700, 0}}};
    static const EffectPreset kFlange = {3, {{0, 1, 0, 0x5FFF, 0x7FFF, 380, 500, 0}}};
    switch (type)
    {
    case 1:
        return kSmallRoom;
    case 2:
        return kBigRoom;
    case 3:
        return kChorus;
    case 4:
        return kFlange;
    case 5:
        return kEcho;
    case 10:
        return kBigRoom;
    default:
        return kNone;
    }
}

int16_t Ramp(int16_t volume, int32_t samples, int16_t rate_high, uint16_t rate_low)
{
    const int32_t steps = samples >> 3;
    if (steps == 0)
    {
        return volume;
    }

    int32_t low = static_cast<int32_t>(rate_low) * steps;
    low >>= 16;
    low += static_cast<int32_t>(rate_high) * steps;

    return static_cast<int16_t>(volume + low);
}

int16_t Rate(double volume, double target, int32_t count, uint16_t& rate_low)
{
    if (count == 0)
    {
        if (target >= volume)
        {
            rate_low = 0xFFFF;

            return 0x7FFF;
        }

        rate_low = 0;

        return 0;
    }

    const double inverse = 1.0 / static_cast<double>(count);
    target = std::max(target, 1.0);
    if (volume <= 0.0)
    {
        volume = 1.0;
    }

    const double step = (target - volume) * inverse * 8.0;
    const auto whole = static_cast<int16_t>(static_cast<int32_t>(step));
    const double above = step - static_cast<double>(whole) + 1.0;
    const auto carry = static_cast<int16_t>(static_cast<int32_t>(above));
    rate_low = static_cast<uint16_t>(static_cast<uint32_t>((above - static_cast<double>(carry)) * 65535.0));

    return static_cast<int16_t>(whole - 1 + carry);
}

// Appends a command that loads `bytes` bytes from `address` to DMEM at `dmem`.
void PushLoad(AudioList& list, uint32_t bytes, uint32_t dmem, uint32_t address)
{
    list.commands.push_back({kOpLoad | (bytes << 12) | dmem, address});
}

// Appends a command that saves `bytes` bytes from DMEM at `dmem` to `address`.
void PushSave(AudioList& list, uint32_t bytes, uint32_t dmem, uint32_t address)
{
    list.commands.push_back({kOpSave | (bytes << 12) | dmem, address});
}

// Appends a command that mixes the buffer at `in` into the buffer at `out` with `gain`.
void PushMix(AudioList& list, uint16_t gain, uint32_t in, uint32_t out)
{
    list.commands.push_back({kOpMix | gain, (in << 16) | out});
}

// Appends a command that loads the `bytes` bytes at `memory_in` to DMEM at `dmem`, from the 8-byte boundary at or
// before `memory_in`. Returns the distance from that boundary to `memory_in`.
uint32_t LoadAligned(AudioList& list, int64_t memory_in, int32_t bytes, int dmem)
{
    const auto location = static_cast<uint32_t>(memory_in);
    const uint32_t align = location & 7;
    const uint32_t total = static_cast<uint32_t>(bytes) + align;
    list.commands.push_back(
        {kOpLoad | (((total + 8 - (total & 7)) & kDmemMask) << 12) | static_cast<uint32_t>(dmem), location - align});

    return align;
}

// Wraps a place before the start of a delay line around to the end of the line.
int64_t WrapDelayPlace(int64_t place, int32_t length)
{
    if (place < 0)
    {
        place += length;
    }

    return place;
}

} // namespace

void SbnPlayer::SynthStopVoice(int voice)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    held.playing = false;
    held.mixer_dirty = true;
    held.mixer_volume = 1;
    held.resampler_delta = 0.0F;
    held.resampler_first = true;
    held.last_sample = 0;
    held.decoder_first = true;
    held.sample = 0;
    if (held.physical_wave >= 0)
    {
        const Wave& wave = waves_[static_cast<std::size_t>(held.physical_wave)];
        held.memory_in = wave.data;
        if (wave.loop_end != 0)
        {
            held.loop_count = static_cast<int32_t>(wave.loop_count);
        }
    }
}

void SbnPlayer::SynthStartVoice(int voice)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    const Wave& wave = waves_[static_cast<std::size_t>(held.wave)];
    held.physical_bus = held.bus;
    held.physical_wave = held.wave;
    held.memory_in = wave.data;
    held.sample = 0;
    held.book_size = wave.book_size;
    if (wave.loop_end != 0)
    {
        held.loop_start_sample = wave.loop_start;
        held.loop_end_sample = wave.loop_end;
        held.loop_count = static_cast<int32_t>(wave.loop_count);
        if (wave.type == 0 && wave.loop_state != 0)
        {
            std::array<uint8_t, 32> state = {};
            build_memory_->Read(wave.loop_state, state.data(), state.size());
            build_memory_->Write(held.decoder_loop_state, state.data(), state.size());
        }
    }
    else
    {
        held.loop_start_sample = 0;
        held.loop_end_sample = 0;
        held.loop_count = 0;
    }

    held.playing = true;
    held.mixer_dirty = true;
    held.mixer_delta = 0;
    held.segment_end = held.delta;
    held.mixer_pan = held.pan;
    held.mixer_volume = static_cast<int16_t>((static_cast<int32_t>(held.volume) * held.volume) >> 15);
    held.dry = kEqualPower[held.reverb & 0x7F];
    held.wet = kEqualPower[127 - (held.reverb & 0x7F)];
    if (held.segment_end != 0)
    {
        held.current_left = 1;
        held.current_right = 1;
    }
    else
    {
        const std::size_t left = config_.stereo ? held.mixer_pan : 64;
        const std::size_t right = config_.stereo ? 127U - held.mixer_pan : 64;
        held.current_left = static_cast<int16_t>((held.mixer_volume * kEqualPower[left]) >> 15);
        held.current_right = static_cast<int16_t>((held.mixer_volume * kEqualPower[right]) >> 15);
    }

    held.ratio = held.pitch_ratio;
}

void SbnPlayer::RefreshMixer(Voice& voice)
{
    if (voice.mixer_delta >= voice.segment_end)
    {
        voice.mixer_delta = voice.segment_end;
        const std::size_t left = config_.stereo ? voice.mixer_pan : 64;
        const std::size_t right = config_.stereo ? 127U - voice.mixer_pan : 64;
        voice.current_left = static_cast<int16_t>((voice.mixer_volume * kEqualPower[left]) >> 15);
        voice.current_right = static_cast<int16_t>((voice.mixer_volume * kEqualPower[right]) >> 15);
    }
    else
    {
        voice.current_left = Ramp(voice.current_left, voice.mixer_delta, voice.rate_high_left, voice.rate_low_left);
        voice.current_right = Ramp(voice.current_right, voice.mixer_delta, voice.rate_high_right, voice.rate_low_right);
    }

    if (voice.current_left == 0)
    {
        voice.current_left = 1;
    }

    if (voice.current_right == 0)
    {
        voice.current_right = 1;
    }
}

void SbnPlayer::SynthSetMixer(int voice, int16_t volume, int32_t delta, uint8_t pan, uint8_t reverb)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    RefreshMixer(held);
    held.mixer_delta = 0;
    held.segment_end = delta;
    held.mixer_pan = pan;
    held.mixer_volume = static_cast<int16_t>((static_cast<int32_t>(volume) * volume) >> 15);
    held.dry = kEqualPower[reverb & 0x7F];
    held.wet = kEqualPower[127 - (reverb & 0x7F)];
    held.mixer_dirty = true;
}

void SbnPlayer::SynthSetPanAndMix(int voice, uint8_t pan, uint8_t reverb)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    RefreshMixer(held);
    held.mixer_pan = pan;
    held.dry = kEqualPower[reverb & 0x7F];
    held.wet = kEqualPower[127 - (reverb & 0x7F)];
    held.mixer_dirty = true;
}

void SbnPlayer::SynthSetVolumeDelta(int voice, int16_t volume, int32_t delta)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    RefreshMixer(held);
    held.mixer_volume = static_cast<int16_t>((static_cast<int32_t>(volume) * volume) >> 15);
    held.mixer_delta = 0;
    held.segment_end = delta;
    held.mixer_dirty = true;
}

void SbnPlayer::InitFilter(LowPass& filter, AudioMemory& memory)
{
    constexpr int kScale = 16384;
    filter.first = true;
    const int16_t time_constant = static_cast<int16_t>(filter.cutoff >> 1);
    filter.gain = static_cast<int16_t>(kScale - time_constant);
    filter.coefficients.fill(0);
    filter.coefficients[8] = time_constant;
    double attenuation = static_cast<double>(time_constant) / kScale;
    for (std::size_t i = 9; i < filter.coefficients.size(); i++)
    {
        attenuation *= static_cast<double>(time_constant) / kScale;
        filter.coefficients[i] = static_cast<int16_t>(attenuation * kScale);
    }

    std::array<uint8_t, 32> bytes = {};
    for (std::size_t i = 0; i < filter.coefficients.size(); i++)
    {
        xsflib::StoreBe16(bytes.data() + i * 2, static_cast<uint16_t>(filter.coefficients[i]));
    }

    memory.Write(filter.coefficients_address, bytes.data(), bytes.size());
}

void SbnPlayer::LoadEffect(Bus& bus, Effect& effect, uint8_t type)
{
    (void)bus;
    constexpr double kConvert = 173123.404906676;
    const EffectPreset& preset = PresetOf(type);
    effect.delay_count = static_cast<int>(preset.sections.size());
    effect.length = preset.length * kChunk;
    effect.input = 0;
    const std::vector<uint8_t> zeros(kFxLength * 2, 0);
    build_memory_->Write(effect.base, zeros.data(), zeros.size());
    for (std::size_t i = 0; i < preset.sections.size(); i++)
    {
        const EffectSection& section = preset.sections[i];
        Delay& delay = effect.delays[i];
        delay.input = static_cast<uint32_t>(section.input * kChunk);
        delay.output = static_cast<uint32_t>(section.output * kChunk);
        delay.feed_back = static_cast<int16_t>(section.feed_back);
        delay.feed_forward = static_cast<int16_t>(section.feed_forward);
        delay.gain = static_cast<int16_t>(section.gain);
        delay.has_resampler = section.rate != 0;
        if (delay.has_resampler)
        {
            delay.mod_increment = static_cast<float>(
                (2.0 * static_cast<double>(static_cast<float>(section.rate) / 1000.0F)) / config_.output_rate);
            delay.mod_gain = static_cast<float>(static_cast<double>(section.depth) / kConvert) *
                             static_cast<float>(delay.output - delay.input);
            delay.mod_value = 1.0F;
            delay.mod_delta = 0;
            delay.resampler_delta = 0.0F;
            delay.resampler_first = true;
        }

        delay.has_filter = section.cutoff != 0;
        if (delay.has_filter)
        {
            delay.filter.cutoff = static_cast<int16_t>(section.cutoff);
            InitFilter(delay.filter, *build_memory_);
        }
    }
}

void SbnPlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    build_memory_ = &memory;
    for (Player& player : players_)
    {
        player.frames++;
    }

    uint32_t place = output;
    for (int left = samples; left > 0; left -= kChunk)
    {
        RunChunkClients();
        BuildChunk(list, memory, place);
        place += kChunk * 4;
    }
}

void SbnPlayer::BuildChunk(AudioList& list, AudioMemory& memory, uint32_t place)
{
    constexpr uint32_t kBoth = kChunkBytes * 2;
    std::array<std::vector<int>, kBuses> by_bus;
    for (int i = 0; i < kVoices; i++)
    {
        const uint8_t bus = voices_[static_cast<std::size_t>(i)].physical_bus;
        if (bus < kBuses)
        {
            by_bus[bus].push_back(i);
        }
    }

    bool first_bus = true;
    for (int b = 0; b < kBuses; b++)
    {
        if (by_bus[static_cast<std::size_t>(b)].empty())
        {
            continue;
        }

        Bus& bus = buses_[static_cast<std::size_t>(b)];
        list.ClearBuffer(kMainLeft, 8 * kChunk);
        for (const int voice : by_bus[static_cast<std::size_t>(b)])
        {
            list.MarkVoice(voices_[static_cast<std::size_t>(voice)].host_track);
            PullVoice(list, voice, memory);
        }

        list.MarkVoicesEnd();

        if (bus.effect_type != 0)
        {
            PullEffect(list, bus.effects[0], kAuxLeft);
            PullEffect(list, bus.effects[1], kAuxRight);
        }

        if (b == delayed_bus_ && delayed_channel_ != kDelayNone)
        {
            DelayBusChannel(list);
        }

        AccumulateBus(list, bus, first_bus);
        first_bus = false;
    }

    list.MarkChunkEnd();
    list.commands.push_back({kOpMove, (kMainLeft << 16) | kBoth});
    PushLoad(list, kBoth, kAuxLeft, wet_accumulator_);
    PushMix(list, 0x7FFF, kAuxLeft, kMainLeft);
    PushMix(list, 0x7FFF, kAuxRight, kMainRight);
    list.commands.push_back({static_cast<uint32_t>(kAudioInterleave) << 24, 0});
    PushSave(list, kBoth, 0, place);
    delay_counter_++;
}

// Delays one channel of the delayed bus. Saves the main and aux chunks of the channel to the delay lines. Loads older
// chunks back from the lines.
void SbnPlayer::DelayBusChannel(AudioList& list) const
{
    constexpr uint32_t kSize = kChunkBytes;
    const uint32_t main_out = delayed_channel_ == kDelayLeft ? kMainLeft : kMainRight;
    const uint32_t aux_out = delayed_channel_ == kDelayLeft ? kAuxLeft : kAuxRight;
    const auto now = static_cast<uint32_t>(delay_counter_ % delay_count_) * kSize;
    const auto next = static_cast<uint32_t>((delay_counter_ + 1) % delay_count_) * kSize;
    PushSave(list, kSize, main_out, delay_main_ + now);
    PushLoad(list, kSize, main_out, delay_main_ + next);
    PushSave(list, kSize, aux_out, delay_aux_ + now);
    PushLoad(list, kSize, aux_out, delay_aux_ + next);
}

// Mixes the aux and main buffers of a bus at the bus gain into the wet and dry accumulators. The first bus of a chunk
// starts each accumulator from silence.
void SbnPlayer::AccumulateBus(AudioList& list, const Bus& bus, bool first_bus) const
{
    constexpr uint32_t kSize = kChunkBytes;
    constexpr uint32_t kBoth = kChunkBytes * 2;
    if (first_bus)
    {
        list.ClearBuffer(kTemp, kBoth);
    }
    else
    {
        PushLoad(list, kBoth, kTemp, wet_accumulator_);
    }

    PushMix(list, bus.gain, kAuxLeft, kTemp);
    PushMix(list, bus.gain, kAuxRight, kTemp + kSize);
    PushSave(list, kBoth, kTemp, wet_accumulator_);
    if (first_bus)
    {
        list.ClearBuffer(kTemp, kBoth);
    }
    else
    {
        PushLoad(list, kBoth, kTemp, dry_accumulator_);
    }

    PushMix(list, bus.gain, kMainLeft, kTemp);
    PushMix(list, bus.gain, kMainRight, kTemp + kSize);
    PushSave(list, kBoth, kTemp, dry_accumulator_);
}

void SbnPlayer::DecodeChunk(AudioList& list, Voice& voice, const Wave& wave, int tsam, int bytes, int16_t out,
                            uint32_t flags)
{
    (void)wave;
    uint32_t align = 0;
    if (bytes > 0)
    {
        align = LoadAligned(list, voice.memory_in, bytes, kDecoderIn);
    }

    if ((flags & kAudioFlagLoop) != 0)
    {
        list.SetLoop(voice.decoder_loop_state);
    }

    list.commands.push_back({static_cast<uint32_t>(kAudioAdpcm) << 24 | (voice.decoder_state & kAddressMask),
                             (flags << 28) | ((static_cast<uint32_t>(tsam * 2) & kDmemMask) << 16) |
                                 ((align & 0xF) << 12) | (static_cast<uint32_t>(out) & kDmemMask)});
    voice.decoder_first = false;
}

void SbnPlayer::PullVoice(AudioList& list, int index, AudioMemory& memory)
{
    (void)memory;
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    if (!voice.playing)
    {
        return;
    }

    const Wave& wave = waves_[static_cast<std::size_t>(voice.physical_wave)];
    int out = kDecoderOut;
    voice.ratio = std::min(voice.ratio, kMaxRatio);
    voice.ratio = static_cast<float>(static_cast<int32_t>(voice.ratio * kUnityPitch));
    voice.ratio = voice.ratio / kUnityPitch;
    const float exact = voice.resampler_delta + voice.ratio * static_cast<float>(kChunk);
    const auto count = static_cast<int32_t>(exact);
    voice.resampler_delta = exact - static_cast<float>(count);
    if (count != 0)
    {
        const auto loop_end = static_cast<int64_t>(voice.loop_end_sample);
        const bool looped = loop_end < static_cast<int64_t>(count) + voice.sample && voice.loop_count != 0;
        if (wave.type == 0)
        {
            list.LoadAdpcm(voice.book_size, wave.book);
            if (looped)
            {
                DecodeAdpcmLoop(list, voice, wave, count, out);
            }
            else
            {
                DecodeAdpcm(list, voice, wave, count, out);
            }
        }
        else if (looped)
        {
            LoadPcmLoop(list, voice, wave, count, out);
        }
        else
        {
            LoadPcm(list, voice, wave, count, out);
        }
    }

    const auto increment = static_cast<int32_t>(voice.ratio * kUnityPitch);
    list.commands.push_back({static_cast<uint32_t>(kAudioResample) << 24 | (voice.resampler_state & kAddressMask),
                             (voice.resampler_first ? 1U << 30 : 0U) |
                                 ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                                 ((static_cast<uint32_t>(out) & kDmemMask) << 2)});
    voice.resampler_first = false;
    MixVoice(list, voice);
    voice.mixer_delta += kChunk;
    if (voice.segment_end < voice.mixer_delta)
    {
        voice.mixer_delta = voice.segment_end;
    }

    if (!voice.playing)
    {
        voice.mixer_dirty = true;
        voice.mixer_volume = 1;
        voice.resampler_delta = 0.0F;
        voice.resampler_first = true;
        voice.last_sample = 0;
        voice.decoder_first = true;
        voice.sample = 0;
        voice.memory_in = wave.data;
        voice.loop_count = static_cast<int32_t>(wave.loop_count);
        voice.priority = 0;
        voice.has_commands = false;
    }
}

// Decodes `count` ADPCM samples of a voice that reaches its loop end in this chunk. The samples run up to the loop end,
// then from the loop start for the rest of the count.
void SbnPlayer::DecodeAdpcmLoop(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out)
{
    const auto loop_end = static_cast<int64_t>(voice.loop_end_sample);
    int32_t samples = static_cast<int32_t>(loop_end - voice.sample);
    const int32_t left = voice.last_sample != 0 ? kFrameSamples - voice.last_sample : 0;
    int32_t tsam = std::max(samples - left, 0);
    int32_t frames = (tsam + kFrameSamples - 1) >> 4;
    int32_t bytes = frames * kFrameBytes;
    DecodeChunk(list, voice, wave, tsam, bytes, static_cast<int16_t>(out), voice.decoder_first ? kAudioFlagInit : 0U);
    out += voice.last_sample != 0 ? voice.last_sample << 1 : kFrameSamples << 1;
    voice.last_sample = static_cast<int32_t>(voice.loop_start_sample & 0xF);
    voice.memory_in = static_cast<int64_t>(wave.data) + kFrameBytes * (voice.loop_start_sample >> 4);
    voice.sample = static_cast<int32_t>(voice.loop_start_sample);
    int buffer_end = out;
    while (count > samples)
    {
        count -= samples;
        const int place = (buffer_end + ((frames + 1) << 5) + 16) & ~0x1F;
        buffer_end += samples << 1;
        if (voice.loop_count != -1 && voice.loop_count != 0)
        {
            voice.loop_count--;
        }

        samples = std::min<int32_t>(count, static_cast<int32_t>(voice.loop_end_sample - voice.loop_start_sample));
        tsam = std::max(samples - kFrameSamples + voice.last_sample, 0);
        frames = (tsam + kFrameSamples - 1) >> 4;
        bytes = frames * kFrameBytes;
        DecodeChunk(list, voice, wave, tsam, bytes, static_cast<int16_t>(place),
                    (voice.decoder_first ? kAudioFlagInit : 0U) | kAudioFlagLoop);
        list.commands.push_back({kOpMove | (static_cast<uint32_t>(place + (voice.last_sample << 1)) & kAddressMask),
                                 (static_cast<uint32_t>(buffer_end) << 16) | static_cast<uint32_t>(samples << 1)});
    }

    voice.last_sample = (count + voice.last_sample) & 0xF;
    voice.sample += count;
    voice.memory_in += static_cast<int64_t>(kFrameBytes) * frames;
}

// Decodes `count` ADPCM samples of a voice that doesn't loop in this chunk. Clears the samples past the end of the wave
// and stops the voice there.
void SbnPlayer::DecodeAdpcm(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out)
{
    const int32_t wave_length = static_cast<int32_t>(wave.length / kFrameBytes * kFrameBytes);
    const int32_t left = voice.last_sample != 0 ? kFrameSamples - voice.last_sample : 0;
    const int32_t tsam = std::max(count - left, 0);
    const int32_t frames = (tsam + kFrameSamples - 1) >> 4;
    int32_t bytes = frames * kFrameBytes;
    const int32_t samples = frames << 4;
    const int64_t over = voice.memory_in + bytes - (static_cast<int64_t>(wave.data) + wave_length);
    int32_t overflow = 0;
    if (over > 0)
    {
        overflow = static_cast<int32_t>(over);
        voice.playing = false;
    }

    int32_t over_samples = (overflow / kFrameBytes) << 4;
    over_samples = std::min(over_samples, samples + left);
    bytes -= overflow;
    bool decoded = false;
    if (over_samples - (over_samples & 0xF) < count)
    {
        decoded = true;
        DecodeChunk(list, voice, wave, samples - over_samples, bytes, static_cast<int16_t>(out),
                    voice.decoder_first ? kAudioFlagInit : 0U);
        out += voice.last_sample != 0 ? voice.last_sample << 1 : kFrameSamples << 1;
        voice.last_sample = (count + voice.last_sample) & 0xF;
        voice.sample += count;
        voice.memory_in += static_cast<int64_t>(kFrameBytes) * frames;
    }
    else
    {
        voice.last_sample = 0;
        voice.memory_in += static_cast<int64_t>(kFrameBytes) * frames;
    }

    if (over_samples != 0)
    {
        voice.last_sample = 0;
        const int32_t start = decoded ? (left + samples - over_samples) << 1 : 0;
        list.ClearBuffer(static_cast<uint32_t>(start + out), static_cast<uint32_t>(over_samples << 1));
    }
}

// Loads `count` 16-bit samples of a voice that reaches its loop end in this chunk. The samples run up to the loop end,
// then from the loop start for the rest of the count.
void SbnPlayer::LoadPcmLoop(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out)
{
    const auto loop_end = static_cast<int64_t>(voice.loop_end_sample);
    int32_t samples = static_cast<int32_t>(loop_end - voice.sample);
    uint32_t align = 0;
    if (samples > 0)
    {
        align = LoadAligned(list, voice.memory_in, samples << 1, out);
    }

    out += static_cast<int>(align);
    voice.memory_in = static_cast<int64_t>(wave.data) + (voice.loop_start_sample << 1);
    voice.sample = static_cast<int32_t>(voice.loop_start_sample);
    int place = out;
    while (count > samples)
    {
        place += samples << 1;
        count -= samples;
        if (voice.loop_count != -1 && voice.loop_count != 0)
        {
            voice.loop_count--;
        }

        samples = std::min<int32_t>(count, static_cast<int32_t>(voice.loop_end_sample - voice.loop_start_sample));
        const int dmem_align = (place & 7) != 0 ? 8 - (place & 7) : 0;
        const uint32_t ram_align = LoadAligned(list, voice.memory_in, samples << 1, place + dmem_align);
        if (ram_align != 0 || dmem_align != 0)
        {
            list.commands.push_back(
                {kOpMove | (static_cast<uint32_t>(place + static_cast<int>(ram_align) + dmem_align) & kAddressMask),
                 (static_cast<uint32_t>(place) << 16) | static_cast<uint32_t>(samples * 2)});
        }
    }

    voice.sample += count;
    voice.memory_in += static_cast<int64_t>(count) << 1;
}

// Loads `count` 16-bit samples of a voice that doesn't loop in this chunk. Clears the samples past the end of the wave
// and stops the voice there.
void SbnPlayer::LoadPcm(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out)
{
    int32_t bytes = count << 1;
    const int64_t over = voice.memory_in + bytes - (static_cast<int64_t>(wave.data) + wave.length);
    int32_t overflow = 0;
    if (over > 0)
    {
        overflow = static_cast<int32_t>(over);
        voice.playing = false;
    }

    overflow = std::min(overflow, bytes);
    if (overflow < bytes)
    {
        if (count > 0)
        {
            bytes -= overflow;
            out += static_cast<int>(LoadAligned(list, voice.memory_in, bytes, out));
        }

        voice.sample += count;
        voice.memory_in += static_cast<int64_t>(count) << 1;
    }
    else
    {
        voice.memory_in += static_cast<int64_t>(count) << 1;
    }

    if (overflow != 0)
    {
        const int32_t start = std::max((count << 1) - overflow, 0);
        list.ClearBuffer(static_cast<uint32_t>(start + out), static_cast<uint32_t>(overflow));
    }
}

// Appends the envelope mixer commands of a voice. Sets new targets and rates first when the mixer settings have
// changed.
void SbnPlayer::MixVoice(AudioList& list, Voice& voice)
{
    if (voice.mixer_dirty)
    {
        constexpr uint32_t kSetTargetLeft = 0x09000000;
        constexpr uint32_t kSetCurrentLeft = 0x09060000;
        constexpr uint32_t kSetTargetRight = 0x09040000;
        constexpr uint32_t kEnvMixerInit = 0x03010000;
        const auto pair = [](int16_t high, uint16_t low)
        {
            return (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16) | low;
        };

        voice.mixer_dirty = false;
        const std::size_t left = config_.stereo ? voice.mixer_pan : 64;
        const std::size_t right = config_.stereo ? 127U - voice.mixer_pan : 64;
        voice.target_left = static_cast<int16_t>((voice.mixer_volume * kEqualPower[left]) >> 15);
        voice.target_right = static_cast<int16_t>((voice.mixer_volume * kEqualPower[right]) >> 15);
        voice.rate_high_left = Rate(voice.current_left, voice.target_left, voice.segment_end, voice.rate_low_left);
        voice.rate_high_right = Rate(voice.current_right, voice.target_right, voice.segment_end, voice.rate_low_right);
        list.commands.push_back({kSetTargetLeft | static_cast<uint16_t>(voice.target_left),
                                 pair(voice.rate_high_left, voice.rate_low_left)});
        list.commands.push_back({kSetCurrentLeft | static_cast<uint16_t>(voice.current_left),
                                 pair(voice.dry, static_cast<uint16_t>(voice.wet))});
        list.commands.push_back({kSetTargetRight | static_cast<uint16_t>(voice.target_right),
                                 pair(voice.rate_high_right, voice.rate_low_right)});
        list.commands.push_back({kEnvMixerInit | static_cast<uint16_t>(voice.current_right), voice.mixer_state});
    }
    else
    {
        list.EnvMixer(0, voice.mixer_state);
    }
}

void SbnPlayer::LoadDelayLine(AudioList& list, const Effect& effect, int64_t position, int buffer, int count)
{
    const int64_t end = position + count;
    const auto address = static_cast<uint32_t>(effect.base + position * 2);
    if (end > effect.length)
    {
        const auto before = static_cast<uint32_t>(effect.length - position) << 1;
        const auto after = static_cast<uint32_t>(end - effect.length) << 1;
        list.commands.push_back({kOpLoad | ((before & kDmemMask) << 12) | static_cast<uint32_t>(buffer), address});
        list.commands.push_back(
            {kOpLoad | ((after & kDmemMask) << 12) | (static_cast<uint32_t>(buffer) + before), effect.base});
    }
    else
    {
        list.commands.push_back(
            {kOpLoad | ((static_cast<uint32_t>(count << 1) & kDmemMask) << 12) | static_cast<uint32_t>(buffer),
             address});
    }
}

void SbnPlayer::PullEffect(AudioList& list, Effect& effect, int16_t wet)
{
    constexpr uint32_t kSize = kChunkBytes;
    const auto load = [&](int buffer, uint32_t address)
    {
        list.commands.push_back({kOpLoad | (kSize << 12) | static_cast<uint32_t>(buffer), address});
    };

    const auto save = [&](int buffer, uint32_t address)
    {
        list.commands.push_back({kOpSave | (kSize << 12) | static_cast<uint32_t>(buffer), address});
    };

    const auto mix = [&](int16_t gain, int in, int out)
    {
        list.commands.push_back(
            {kOpMix | static_cast<uint16_t>(gain), (static_cast<uint32_t>(in) << 16) | static_cast<uint32_t>(out)});
    };

    int input_buffer = kTemp;
    int output_buffer = kTemp + kChunkBytes;
    const auto place_of = [&](int64_t position)
    {
        return static_cast<uint32_t>(effect.base + position * 2);
    };

    save(wet, place_of(effect.input));
    list.ClearBuffer(static_cast<uint32_t>(wet), kSize);
    int64_t previous_output = -0x10000000;
    for (int i = 0; i < effect.delay_count; i++)
    {
        Delay& delay = effect.delays[static_cast<std::size_t>(i)];
        const int64_t in_place = WrapDelayPlace(effect.input - static_cast<int64_t>(delay.input), effect.length);
        const int64_t out_place = WrapDelayPlace(effect.input - static_cast<int64_t>(delay.output), effect.length);
        if (in_place == previous_output)
        {
            std::swap(input_buffer, output_buffer);
        }
        else
        {
            load(input_buffer, place_of(in_place));
        }

        if (delay.has_resampler)
        {
            ResampleDelay(list, effect, delay, output_buffer);
        }
        else
        {
            load(output_buffer, place_of(out_place));
        }

        if (delay.feed_forward != 0)
        {
            mix(delay.feed_forward, input_buffer, output_buffer);
            if (!delay.has_resampler && !delay.has_filter)
            {
                save(output_buffer, place_of(out_place));
            }
        }

        if (delay.feed_back != 0)
        {
            mix(delay.feed_back, output_buffer, input_buffer);
            save(input_buffer, place_of(in_place));
        }

        if (delay.has_filter)
        {
            list.LoadAdpcm(32, delay.filter.coefficients_address);
            list.commands.push_back(
                {static_cast<uint32_t>(kAudioPoleFilter) << 24 | (delay.filter.first ? 1U << 16 : 0U) |
                     static_cast<uint16_t>(delay.filter.gain),
                 ((static_cast<uint32_t>(output_buffer) >> 8) << 24) | (delay.filter.state & kAddressMask)});
            delay.filter.first = false;
        }

        if (!delay.has_resampler)
        {
            save(output_buffer, place_of(out_place));
        }

        if (delay.gain != 0)
        {
            mix(delay.gain, output_buffer, wet);
        }

        previous_output = effect.input + static_cast<int64_t>(delay.output);
    }

    effect.input += kChunk;
    if (effect.input >= effect.length)
    {
        effect.input = 0;
    }
}

// Reads the output of a modulated delay into `output_buffer`. Steps the modulation, then resamples the delay line at
// the ratio that the modulation sets.
void SbnPlayer::ResampleDelay(AudioList& list, const Effect& effect, Delay& delay, int output_buffer)
{
    const int resample_buffer = kTemp + kChunkBytes * 2;
    const auto length = static_cast<int32_t>(delay.output - delay.input);
    delay.mod_value += delay.mod_increment * static_cast<float>(kChunk);
    if (static_cast<double>(delay.mod_value) > 2.0)
    {
        delay.mod_value = static_cast<float>(static_cast<double>(delay.mod_value) - 4.0);
    }

    float shape = delay.mod_value;
    if (shape < 0.0F)
    {
        shape = -shape;
    }

    shape = static_cast<float>(static_cast<double>(shape) - 1.0);
    float delta = delay.mod_gain * shape;
    delta /= static_cast<float>(length);
    delta = static_cast<float>(static_cast<int32_t>(delta * kUnityPitch));
    delta = delta / kUnityPitch;
    const auto ratio = static_cast<float>(1.0 - static_cast<double>(delta));
    const float exact = delay.resampler_delta + ratio * static_cast<float>(kChunk);
    const auto samples = static_cast<int32_t>(exact);
    delay.resampler_delta = exact - static_cast<float>(samples);
    int64_t resample_place = effect.input - (static_cast<int64_t>(delay.output) - delay.mod_delta);
    const int align = static_cast<int>((effect.base + resample_place * 2) & 7) >> 1;
    resample_place = WrapDelayPlace(resample_place - align, effect.length);
    LoadDelayLine(list, effect, resample_place, resample_buffer, samples + align);
    const auto increment = static_cast<int32_t>(ratio * kUnityPitch);
    list.commands.push_back({static_cast<uint32_t>(kAudioResample) << 24 | (delay.resampler_state & kAddressMask),
                             (delay.resampler_first ? 1U << 30 : 0U) |
                                 ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                                 ((static_cast<uint32_t>((align << 1) + resample_buffer) & kDmemMask) << 2) |
                                 ((static_cast<uint32_t>(output_buffer) >> 8) & 3)});
    delay.resampler_first = false;
    delay.mod_delta += samples - kChunk;
}

} // namespace ntdlib::n64
