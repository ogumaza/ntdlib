// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/voice.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/wave/sinc.h"
#include "nx/audio/arithmetic.h"

namespace ntdlib::nx::audio
{
namespace
{

// The scratch buffer holds the converter's history and a batch's input: 0x3F00 samples at most.
constexpr int kScratchSamples = 0x3F00 + 8;

// A channel's place in its wave buffers, as a frame's decoding moves it: the offset in the current buffer, the current
// buffer, the samples played and the buffers consumed.
struct Place
{
    int32_t offset = 0;
    int32_t index = 0;
    uint64_t played = 0;
    int32_t consumed = 0;
};

// One wave buffer's part of a frame: decodes from the channel's place in it, up to `wanted` samples, with the
// DSP-ADPCM context `context`.
int32_t Decode(const DecodeFromWaveBuffersArgs& args, AdpcmContext& context, const WaveBuffer& buffer, bool looped,
               int32_t offset, int32_t wanted, int16_t* out)
{
    DecodeArg arg;
    arg.buffer = buffer.buffer;
    arg.start = looped ? buffer.loop_start : buffer.start;
    arg.end = looped ? buffer.loop_end : buffer.end;
    arg.channel_count = args.channel_count;
    arg.channel = args.channel;
    arg.coefficients = args.coefficients;
    arg.context = &context;
    arg.offset = offset;
    arg.count = wanted;
    switch (args.format)
    {
    case SampleFormat::kPcmInt16:
        return DecodePcm16(out, arg);
    case SampleFormat::kAdpcm:
        return DecodeAdpcm(out, arg);
    case SampleFormat::kPcmFloat:
        return DecodePcmFloat(out, arg);
    default:
        // Nintendo's code takes -1 samples from a buffer of any other format and goes round for ever. Here such a voice
        // is silent.
        return 0;
    }
}

// Decodes up to `need` samples into `input` from `place`, buffer after buffer. A buffer that ends is consumed unless it
// loops. A looping buffer plays its loop range until its loops are done. Returns the samples decoded, and sets
// `starved` when no buffer was left before `need`.
int32_t Walk(const DecodeFromWaveBuffersArgs& args, VoiceState& state, Place& place, int32_t need, int16_t* input,
             bool& starved)
{
    const std::array<WaveBuffer, kWaveBufferCount>& buffers = *args.buffers;

    // A buffer is done with: the next one plays.
    const auto consume = [&](const WaveBuffer& buffer)
    {
        state.valid[static_cast<std::size_t>(place.index)] = false;
        state.loop_count = 0;
        ++place.consumed;
        place.index = place.index > 2 ? 0 : place.index + 1;
        if (buffer.end_of_stream)
        {
            place.played = 0;
        }
    };

    int32_t decoded = 0;
    starved = false;
    while (need >= 1)
    {
        if (place.index >= kWaveBufferCount)
        {
            state.valid = {};
            place.played = 0;
            place.index = 0;
            place.consumed = kWaveBufferCount;
        }
        if (!state.valid[static_cast<std::size_t>(place.index)])
        {
            starved = true;
            break;
        }

        const WaveBuffer& buffer = buffers[static_cast<std::size_t>(place.index)];
        if (place.offset == 0 && args.format == SampleFormat::kAdpcm && buffer.context != nullptr)
        {
            state.adpcm = *buffer.context;
        }
        const bool looped = buffer.loop && state.loop_count >= 1 && buffer.loop_end >= buffer.loop_start;
        const int32_t got = Decode(args, state.adpcm, buffer, looped, place.offset, need - decoded, input + decoded);
        place.played += static_cast<uint64_t>(static_cast<int64_t>(got));
        decoded += got;
        if (got != 0)
        {
            place.offset += got;
            const int32_t length = looped ? buffer.loop_end - buffer.loop_start : buffer.end - buffer.start;
            if (place.offset < length)
            {
                if (need > decoded)
                {
                    continue;
                }

                break;
            }
        }

        // The buffer has ended, or gave nothing: it loops, or the next one plays.
        if (buffer.loop)
        {
            ++state.loop_count;
            if (buffer.loop_count >= 0 && (got == 0 || state.loop_count > buffer.loop_count))
            {
                consume(buffer);
            }
            if (got == 0)
            {
                place.offset = 0;
                starved = true;
                break;
            }

            if (args.reset_played_at_loop)
            {
                place.played = 0;
            }
        }
        else
        {
            consume(buffer);
        }
        place.offset = 0;
        if (need <= decoded)
        {
            break;
        }
    }

    return decoded;
}

// The ratio of a frame's step in Q15, from the rates and the pitch in single precision.
int32_t StepRatio(const DecodeFromWaveBuffersArgs& args)
{
    const float rates = static_cast<float>(args.source_rate) / static_cast<float>(args.target_rate);
    const float step = rates * static_cast<float>(args.pitch);

    return ToInt(step);
}

// Whether a frame decodes nothing: it runs backwards, or requires more input than the scratch buffer holds.
bool Refused(const DecodeFromWaveBuffersArgs& args, int32_t ratio, int history)
{
    const int32_t frame_end = WrapAdd(WrapMul(args.count, ratio), args.state->fraction);

    return frame_end < 0 || history + (frame_end >> 15) > 0x3F00;
}

// The samples of each batch: those whose input fits, in practice the whole frame at once.
int32_t BatchOf(const DecodeFromWaveBuffersArgs& args, int32_t ratio)
{
    const int32_t limit = ratio == 0 ? 0 : (0x1F800000 - args.state->fraction) / ratio;

    return limit < args.count ? limit : args.count;
}

} // namespace

void DecodeFromWaveBuffers(const DecodeFromWaveBuffersArgs& args)
{
    VoiceState& state = *args.state;
    const int32_t ratio = StepRatio(args);
    const int history = args.quality == SrcQuality::kHigh ? 8 : 4;
    if (Refused(args, ratio, history))
    {
        return;
    }

    const int32_t batch = BatchOf(args, ratio);
    int32_t* out = args.out;
    int32_t remaining = args.count;
    int32_t fraction = state.fraction;
    Place place{state.offset, state.index, state.played, state.consumed};
    std::array<int16_t, kScratchSamples> scratch;
    while (remaining >= 1)
    {
        const int32_t count = remaining < batch ? remaining : batch;
        const int32_t batch_end = WrapAdd(WrapMul(count, ratio), fraction);
        for (int k = 0; k < history; ++k)
        {
            scratch[static_cast<std::size_t>(k)] = state.history[static_cast<std::size_t>(k)];
        }
        int16_t* input = scratch.data() + history;

        // Decode the samples the batch requires, buffer after buffer.
        const int32_t need = batch_end >> 15;
        bool starved = false;
        const int32_t decoded = Walk(args, state, place, need, input, starved);

        // Convert the batch, with zeros for what the buffers didn't give, and keep the history for the next one.
        for (int32_t i = decoded; i < need; ++i)
        {
            input[i] = 0;
        }
        if (out != nullptr)
        {
            Resample(out, scratch.data(), ratio, fraction, count, args.quality, need != count || args.pitch != 0x8000);
        }
        else
        {
            SkipResample(ratio, fraction, count, args.quality);
        }
        for (int k = 0; k < history; ++k)
        {
            state.history[static_cast<std::size_t>(k)] = scratch[static_cast<std::size_t>(need + k)];
        }

        remaining -= count;
        if (starved)
        {
            break;
        }

        if (out != nullptr)
        {
            out += count;
        }
    }

    state.consumed = place.consumed;
    state.offset = place.offset;
    state.played = place.played;
    state.index = place.index;
    state.fraction = fraction;
}

void DecodeFromWaveBuffersEnhanced(const DecodeFromWaveBuffersArgs& args, const EnhancedDecodeBuffers& buffers,
                                   float* out)
{
    VoiceState& state = *args.state;
    const int32_t ratio = StepRatio(args);
    const int history = args.quality == SrcQuality::kHigh ? 8 : 4;
    if (Refused(args, ratio, history))
    {
        return;
    }

    // The kernel for the step. An output's left neighbour lies `behind` samples behind the batch's input, and the
    // kernel's window starts half - 1 samples before it. The last output of a batch takes up to `ahead` samples after
    // the batch's input. Without outputs, the batch takes none.
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(static_cast<double>(ratio) / 32768.0);
    const int half = kernel.HalfWidth();
    const int behind = args.quality == SrcQuality::kHigh ? 5 : (args.quality == SrcQuality::kLow ? 4 : 3);
    const int32_t ahead = out != nullptr ? half - behind + 1 : 0;
    const int start = kWideHistory - behind - (half - 1);

    const int32_t batch = BatchOf(args, ratio);
    float* output = out;
    int32_t remaining = args.count;
    int32_t fraction = state.fraction;
    Place place{state.offset, state.index, state.played, state.consumed};
    std::vector<int16_t>& decoded = *buffers.decoded;
    std::vector<float>& input = *buffers.input;
    while (remaining >= 1)
    {
        const int32_t count = remaining < batch ? remaining : batch;
        const int32_t batch_end = WrapAdd(WrapMul(count, ratio), fraction);
        const int32_t need = batch_end >> 15;
        const auto total = static_cast<std::size_t>(need + ahead);
        decoded.resize(total);

        // The batch's input, then the samples after it from a copy of the state, with zeros for what the buffers didn't
        // give.
        bool starved = false;
        const int32_t got = Walk(args, state, place, need, decoded.data(), starved);
        std::fill(decoded.begin() + got, decoded.begin() + need, int16_t{0});
        if (ahead > 0)
        {
            VoiceState next_state = state;
            Place next_place = place;
            bool next_starved = false;
            const int32_t peeked = Walk(args, next_state, next_place, ahead, decoded.data() + need, next_starved);
            std::fill(decoded.begin() + need + peeked, decoded.end(), int16_t{0});
        }

        // The past samples and the input, as floats.
        input.resize(kWideHistory + total);
        std::copy(buffers.history.begin(), buffers.history.end(), input.begin());
        for (std::size_t i = 0; i < total; ++i)
        {
            input[kWideHistory + i] = static_cast<float>(decoded[i]);
        }

        // Each output at its position. Without a conversion, the medium quality's outputs all take the first one's
        // phase, one input sample apart, as the console's filter does. Without outputs, the fraction moves on by the
        // steps' sum, as in both loops.
        const bool converting = need != count || args.pitch != 0x8000;
        if (output == nullptr)
        {
            fraction = WrapAdd(fraction, WrapMul(count, ratio)) & 0x7FFF;
        }
        else if (args.quality == SrcQuality::kMedium && !converting)
        {
            const auto phase = static_cast<uint32_t>(fraction) << 1;
            for (int32_t i = 0; i < count; ++i)
            {
                output[i] = kernel.At(&input[static_cast<std::size_t>(start + i)], phase);
            }
            fraction = WrapAdd(fraction, WrapMul(count, ratio)) & 0x7FFF;
        }
        else
        {
            int32_t at = 0;
            for (int32_t i = 0; i < count; ++i)
            {
                output[i] =
                    kernel.At(&input[static_cast<std::size_t>(start + at)], static_cast<uint32_t>(fraction) << 1);
                fraction = WrapAdd(fraction, ratio);
                at += fraction >> 15;
                fraction &= 0x7FFF;
            }
        }

        // The past samples for the next batch, and the console's history, as DecodeFromWaveBuffers keeps it.
        std::array<int16_t, 8> console = state.history;
        for (int k = 0; k < history; ++k)
        {
            const int32_t at = need + k;
            console[static_cast<std::size_t>(k)] = at < history ? state.history[static_cast<std::size_t>(at)]
                                                                : decoded[static_cast<std::size_t>(at - history)];
        }
        state.history = console;
        std::copy(input.begin() + need, input.begin() + need + kWideHistory, buffers.history.begin());

        remaining -= count;
        if (starved)
        {
            break;
        }

        if (output != nullptr)
        {
            output += count;
        }
    }

    state.consumed = place.consumed;
    state.offset = place.offset;
    state.played = place.played;
    state.index = place.index;
    state.fraction = fraction;
}

} // namespace ntdlib::nx::audio
