// SPDX-License-Identifier: MIT

#include "ntdlib/n64/enhanced_mix.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "ntdlib/wave/sinc.h"

namespace ntdlib::n64
{

namespace
{

using wave::SincKernel;

constexpr std::size_t kDmemSamples = AudioRsp::kDmemSize / 2;
constexpr std::size_t kMostInputsBeside = 128;
constexpr int64_t kFractionOne = 65536;

int FactorFor(int game_rate, int output_rate)
{
    if (output_rate <= game_rate)
    {
        return 1;
    }

    return (output_rate + game_rate - 1) / game_rate;
}

// The inputs of a window: the stream's where it has them. Past its end, zeros for a voice that has stopped, and the
// last input for a voice that the frames ahead don't reach far enough into.
const float* Window(const std::vector<float>& samples, int64_t base, int64_t start, int count, float past_end,
                    std::array<float, kMostInputsBeside * 2>& scratch)
{
    const int64_t end = base + static_cast<int64_t>(samples.size());
    if (start >= base && start + count <= end)
    {
        return samples.data() + (start - base);
    }

    for (int i = 0; i < count; i++)
    {
        const int64_t at = start + i;
        float value = 0.0F;
        if (at >= base)
        {
            value = at < end ? samples[static_cast<std::size_t>(at - base)] : past_end;
        }

        scratch[static_cast<std::size_t>(i)] = value;
    }

    return scratch.data();
}

} // namespace

EnhancedMix::EnhancedMix(int game_rate, std::vector<VoiceArea> areas, uint32_t output, int output_rate)
    : factor_(FactorFor(game_rate, output_rate)),
      rate_(game_rate * factor_),
      output_rate_(output_rate == 0 ? game_rate : output_rate),
      areas_(std::move(areas)),
      output_(output)
{
    track_gains_.fill(1.0F);
    low_.dmem.resize(kDmemSamples);
    high_.scale = factor_;
    high_.dmem.resize(kDmemSamples * static_cast<std::size_t>(factor_));
    std::size_t before = 0;
    std::size_t most = 0;
    for (const VoiceArea& area : areas_)
    {
        before += area.bytes / 2 * static_cast<std::size_t>(factor_);
        most = std::max<std::size_t>(most, area.bytes / 2 * static_cast<std::size_t>(factor_));
    }

    before_.resize(before);
    sums_.resize(static_cast<std::size_t>(kTracks) * 2 * most);
    if (factor_ > 1)
    {
        const int down_half = SincKernel::ForStep(static_cast<double>(factor_)).HalfWidth();
        const int up_half = SincKernel::ForStep(1.0).HalfWidth();
        lag_ = static_cast<std::size_t>(down_half + (factor_ * up_half));
        delay_.resize(lag_ * 2);
        for (std::vector<float>& up : ups_)
        {
            up.resize(static_cast<std::size_t>(up_half) * 2);
        }
    }

    skip_ = lag_;
    if (rate_ != output_rate_)
    {
        const int half = SincKernel::ForStep(static_cast<double>(rate_) / output_rate_).HalfWidth();
        final_base_ = -half;
        for (std::vector<float>& side : final_)
        {
            side.resize(static_cast<std::size_t>(half));
        }
    }
}

int EnhancedMix::OutputRate() const
{
    return output_rate_;
}

std::size_t EnhancedMix::Queued() const
{
    return queue_.size();
}

AudioOps& EnhancedMix::Recording()
{
    return recording_;
}

bool EnhancedMix::OldestBusy() const
{
    return !queue_.empty() && queue_.front().busy;
}

void EnhancedMix::EndRecord(int samples, bool busy)
{
    Frame frame;
    frame.ops = recording_;
    frame.samples = samples;
    frame.busy = busy;
    frame.serial = ++serial_;
    recording_.Clear();
    Resolve(frame);
    frame.ops.inputs = {};
    queue_.push_back(std::move(frame));
}

uint32_t EnhancedMix::StreamOf(const AudioResampleOp& op)
{
    const auto held = streams_by_state_.find(op.state);
    const bool have = held != streams_by_state_.end();
    if (have && !op.init && op.mode == 0)
    {
        return held->second;
    }

    // A new stream. The inputs before the state's start from silence.
    Stream stream;
    stream.samples.assign(kMostInputsBeside, 0.0F);
    if (have && !op.init)
    {
        // The inputs change their rate here. The new stream's past takes the old stream's at the new rate.
        Stream& old = streams_.at(held->second);
        std::vector<float> past;
        if (op.mode == 2)
        {
            for (std::size_t i = old.samples.size(); i >= 4 && past.size() < kMostInputsBeside; i -= 2)
            {
                past.push_back(old.samples[i - 4 + 2]);
            }
        }
        else
        {
            for (std::size_t i = old.samples.size(); i >= 1 && past.size() < kMostInputsBeside; i--)
            {
                past.push_back(old.samples[i - 1]);
                past.push_back(old.samples[i - 1]);
            }
        }

        const std::size_t count = std::min(past.size(), stream.samples.size());
        for (std::size_t i = 0; i < count; i++)
        {
            stream.samples[stream.samples.size() - 1 - i] = past[i];
        }
    }

    if (have)
    {
        Stream& old = streams_.at(held->second);
        old.closed = true;
        old.holds = !op.init;
        if (old.uses == 0)
        {
            streams_.erase(held->second);
        }
    }

    const uint32_t id = next_stream_++;
    streams_by_state_[op.state] = id;
    streams_.emplace(id, std::move(stream));

    return id;
}

void EnhancedMix::AddInputs(uint32_t id, const float* inputs, uint32_t count, uint64_t serial)
{
    Stream& stream = streams_.at(id);
    stream.samples.insert(stream.samples.end(), inputs, inputs + count);
    stream.last_frame = serial;
}

void EnhancedMix::AddMain(Frame& frame, const AudioResampleOp& op)
{
    MainResample main;
    const auto held = streams_by_state_.find(op.state);
    const uint32_t old_stream = held != streams_by_state_.end() ? held->second : next_stream_;
    main.stream = StreamOf(op);
    Stream& stream = streams_.at(main.stream);
    const float* inputs = frame.ops.inputs.data() + op.inputs;
    if (stream.End() == static_cast<int64_t>(kMostInputsBeside) && op.mode == 0)
    {
        // A voice's first resampler. A voice that the record joins halfway has its state's samples before its inputs.
        std::copy_n(inputs, op.in_tap, stream.samples.end() - op.in_tap);
    }

    const int64_t in_index = stream.End();
    const uint32_t new_inputs = op.input_count > op.in_tap ? op.input_count - op.in_tap : 0;
    const float* first_new = inputs + std::min<uint32_t>(op.in_tap, op.input_count);
    AddInputs(main.stream, first_new, new_inputs, frame.serial);
    main.origin = in_index - op.in_tap + 1;
    main.out = op.out;
    main.samples = op.bytes / 2;
    main.first_fraction = op.first_fraction;
    main.pitch = op.pitch;

    // The old stream's last outputs look ahead into the new inputs, at the old rate.
    if (op.mode != 0 && old_stream != main.stream)
    {
        const auto old = streams_.find(old_stream);
        if (old != streams_.end())
        {
            for (uint32_t i = 0; i < new_inputs; i += op.mode == 2 ? 1 : 2)
            {
                old->second.samples.push_back(first_new[i]);
                if (op.mode == 2)
                {
                    old->second.samples.push_back(first_new[i]);
                }
            }
        }
    }

    stream.uses++;
    AudioOp record;
    record.kind = AudioOpKind::kMainResample;
    record.extra = static_cast<uint32_t>(frame.mains.size());
    frame.mains.push_back(main);
    frame.run.push_back(record);
}

void EnhancedMix::AddUnresampled(Frame& frame, const AudioEnvOp& env)
{
    // A voice without a resampler plays each input as it is.
    AudioResampleOp op;
    op.state = env.state;
    op.init = env.init;
    MainResample main;
    main.stream = StreamOf(op);
    Stream& stream = streams_.at(main.stream);
    main.origin = stream.End();
    AddInputs(main.stream, frame.ops.inputs.data() + env.inputs, env.input_count, frame.serial);
    main.out = env.in;
    main.samples = env.input_count;
    main.pitch = 0x8000;
    stream.uses++;
    AudioOp record;
    record.kind = AudioOpKind::kMainResample;
    record.extra = static_cast<uint32_t>(frame.mains.size());
    frame.mains.push_back(main);
    frame.run.push_back(record);
}

void EnhancedMix::ResolveVoice(Frame& frame, std::size_t begin, std::size_t end)
{
    // A voice can have several envelope mixers in a chunk, where a change of its volume or pitch falls inside the
    // chunk. What comes before each mixer's last resampler only makes that resampler's input.
    const std::vector<AudioOp>& ops = frame.ops.ops;
    std::size_t place = begin;
    while (place < end)
    {
        std::size_t env_at = place;
        while (env_at < end && ops[env_at].kind != AudioOpKind::kEnvMixer)
        {
            env_at++;
        }

        if (env_at == end)
        {
            break;
        }

        std::size_t main_at = env_at;
        while (main_at > place && ops[main_at - 1].kind != AudioOpKind::kResample)
        {
            main_at--;
        }

        std::size_t rest = env_at;
        if (main_at > place)
        {
            AddMain(frame, frame.ops.resamples[ops[main_at - 1].extra]);
            rest = main_at;
        }
        else
        {
            AddUnresampled(frame, frame.ops.envs[ops[env_at].extra]);
        }

        frame.run.insert(frame.run.end(), ops.begin() + static_cast<std::ptrdiff_t>(rest),
                         ops.begin() + static_cast<std::ptrdiff_t>(env_at) + 1);
        place = env_at + 1;
    }

    frame.run.insert(frame.run.end(), ops.begin() + static_cast<std::ptrdiff_t>(place),
                     ops.begin() + static_cast<std::ptrdiff_t>(end));
}

void EnhancedMix::Resolve(Frame& frame)
{
    const std::vector<AudioOp>& ops = frame.ops.ops;
    std::size_t i = 0;
    while (i < ops.size())
    {
        frame.run.push_back(ops[i]);
        if (ops[i].kind != AudioOpKind::kVoice)
        {
            i++;
            continue;
        }

        std::size_t end = i + 1;
        while (end < ops.size() && ops[end].kind != AudioOpKind::kVoice && ops[end].kind != AudioOpKind::kVoicesEnd &&
               ops[end].kind != AudioOpKind::kChunkEnd)
        {
            end++;
        }

        ResolveVoice(frame, i + 1, end);
        i = end;
    }

    // A mark's samples are those of the output that comes after it.
    uint32_t bytes = 0;
    for (auto op = frame.run.rbegin(); op != frame.run.rend(); ++op)
    {
        if (op->kind == AudioOpKind::kInterleave)
        {
            bytes = op->bytes;
        }
        else if (op->kind == AudioOpKind::kVoicesEnd || op->kind == AudioOpKind::kChunkEnd)
        {
            op->bytes = bytes;
        }
    }
}

void EnhancedMix::RunMain(const MainResample& main)
{
    const auto found = streams_.find(main.stream);
    Stream& stream = found->second;
    const int64_t k = factor_;
    const double step = (2.0 * main.pitch) / (static_cast<double>(kFractionOne) * static_cast<double>(k));
    const SincKernel& kernel = SincKernel::ForStep(step);
    const int half = kernel.HalfWidth();
    const std::size_t out = high_.Place(main.out);
    const auto count = static_cast<int64_t>(main.samples) * k;
    float past_end = 0.0F;
    if (!stream.closed ? stream.last_frame == serial_ : stream.holds)
    {
        past_end = stream.samples.back();
    }

    std::array<float, kMostInputsBeside * 2> scratch;
    int64_t last = main.origin;
    for (int64_t j = 0; j < count; j++)
    {
        const int64_t place = (int64_t{main.first_fraction} * k) + (2 * int64_t{main.pitch} * j);
        const int64_t whole = place / (kFractionOne * k);
        const auto fraction = static_cast<uint32_t>((place - (whole * kFractionOne * k)) / k);
        last = main.origin + whole;
        const float* window = Window(stream.samples, stream.base, last - (half - 1), half * 2, past_end, scratch);
        high_.Dmem(out + static_cast<std::size_t>(j)) = kernel.At(window, fraction);
    }

    // What the next outputs can't reach goes.
    const int64_t keep_from = last - static_cast<int64_t>(kMostInputsBeside);
    if (keep_from - stream.base > 8192)
    {
        stream.samples.erase(stream.samples.begin(), stream.samples.begin() + (keep_from - stream.base));
        stream.base = keep_from;
    }

    stream.uses--;
    if (stream.closed && stream.uses == 0)
    {
        streams_.erase(found);
    }
}

void EnhancedMix::StartVoice(int track, uint32_t muted, bool metered)
{
    voice_track_ = track;
    voice_dirty_ = true;
    const bool ours = track >= 0 && track < kTracks;
    const uint32_t bit = ours ? uint32_t{1} << track : 0;
    const float gain = ours ? track_gains_[static_cast<std::size_t>(track)] : 1.0F;
    voice_metered_ = ours && metered;
    voice_followed_ = ours && (metered || (muted & bit) != 0 || gain != 1.0F);
    if (voice_followed_)
    {
        std::size_t at = 0;
        for (const VoiceArea& area : areas_)
        {
            const std::size_t place = high_.Place(area.place);
            const std::size_t count = (area.bytes / 2) * static_cast<std::size_t>(factor_);
            for (std::size_t i = 0; i < count; i++)
            {
                before_[at + i] = high_.Dmem(place + i);
            }

            at += count;
        }
    }
}

void EnhancedMix::EndVoice()
{
    if (!voice_followed_)
    {
        return;
    }

    voice_followed_ = false;
    const auto track = static_cast<std::size_t>(voice_track_);
    const float from = track_gains_[track];
    const float to = (frame_muted_ & (uint32_t{1} << voice_track_)) != 0 ? 0.0F : 1.0F;
    const float each = (to - from) / static_cast<float>(frame_total_);
    const std::size_t most = sums_.size() / (static_cast<std::size_t>(kTracks) * 2);
    std::size_t at = 0;
    for (const VoiceArea& area : areas_)
    {
        const std::size_t place = high_.Place(area.place);
        const std::size_t count = (area.bytes / 2) * static_cast<std::size_t>(factor_);
        float* const sums = sums_.data() + ((track * 2) + static_cast<std::size_t>(area.side)) * most;
        for (std::size_t i = 0; i < count; i++)
        {
            float& sample = high_.Dmem(place + i);
            const float added = sample - before_[at + i];
            if (voice_metered_)
            {
                sums[i] += added;
            }

            const float gain = from + (each * static_cast<float>(frame_done_ + static_cast<int>(i) + 1));
            sample = before_[at + i] + (added * gain);
        }

        at += count;
    }

    if (voice_metered_)
    {
        summed_tracks_ |= uint32_t{1} << voice_track_;
    }
}

void EnhancedMix::FlushSends(uint32_t samples)
{
    if (factor_ == 1)
    {
        return;
    }

    const SincKernel& kernel = SincKernel::ForStep(static_cast<double>(factor_));
    const int half = kernel.HalfWidth();
    const auto window = static_cast<std::size_t>(half) * 2;
    const auto k = static_cast<std::size_t>(factor_);
    const std::size_t inputs = samples * k;
    int area_number = 0;
    for (const VoiceArea& area : areas_)
    {
        area_number++;
        if (!area.wet)
        {
            continue;
        }

        Down& down = downs_[(static_cast<uint64_t>(flush_slot_) << 8) | static_cast<uint64_t>(area_number)];
        if (down.history.empty())
        {
            down.history.resize(window);
        }

        if (!voice_dirty_ && down.idle >= half * 2)
        {
            continue;
        }

        down.idle = voice_dirty_ ? 0 : down.idle + static_cast<int>(inputs);
        const std::size_t high_place = high_.Place(area.place);
        const std::size_t low_place = low_.Place(area.place);
        std::vector<float> all(window + inputs);
        std::ranges::copy(down.history, all.begin());
        for (std::size_t i = 0; i < inputs; i++)
        {
            float& sample = high_.Dmem(high_place + i);
            all[window + i] = sample;
            sample = 0.0F;
        }

        for (std::size_t i = 0; i < samples; i++)
        {
            low_.Dmem(low_place + i) += kernel.At(all.data() + (i * k) + 1, 0);
        }

        std::copy(all.end() - static_cast<std::ptrdiff_t>(window), all.end(), down.history.begin());
    }

    voice_dirty_ = false;
    flush_slot_++;
}

void EnhancedMix::EndChunk(uint32_t samples, Peaks& peaks)
{
    const std::size_t most = sums_.size() / (static_cast<std::size_t>(kTracks) * 2);
    for (std::size_t track = 0; summed_tracks_ != 0; track++, summed_tracks_ >>= 1)
    {
        if ((summed_tracks_ & 1) == 0)
        {
            continue;
        }

        for (std::size_t side = 0; side < 2; side++)
        {
            float* const sums = sums_.data() + ((track * 2) + side) * most;
            float peak = 0.0F;
            for (std::size_t i = 0; i < most; i++)
            {
                peak = std::max(peak, sums[i] < 0.0F ? -sums[i] : sums[i]);
                sums[i] = 0.0F;
            }

            peaks[track][side] = std::max(peaks[track][side], static_cast<int32_t>(peak + 0.5F));
        }
    }

    flush_slot_ = 0;
    frame_done_ += static_cast<int>(samples) * factor_;
}

void EnhancedMix::Emit(int samples, std::vector<float>& out)
{
    const auto k = static_cast<std::size_t>(factor_);
    const auto game_samples = static_cast<std::size_t>(samples);
    const std::size_t count = game_samples * k;
    std::vector<float> mixed(count * 2);
    const uint64_t output_place = output_ >> 1;
    for (std::size_t i = 0; i < mixed.size(); i++)
    {
        mixed[i] = high_.ReadMemory((output_place * k) + i);
    }

    if (factor_ > 1)
    {
        DelayDryPart(mixed);
        AddWetPart(mixed, game_samples);
    }

    if (rate_ == output_rate_)
    {
        const std::size_t skipped = std::min(skip_, count);
        skip_ -= skipped;
        out.insert(out.end(), mixed.begin() + static_cast<std::ptrdiff_t>(skipped * 2), mixed.end());

        return;
    }

    RunFinalInterpolator(mixed, count, out);
}

// The dry part waits for the wet part's two filters.
void EnhancedMix::DelayDryPart(std::vector<float>& mixed)
{
    const std::size_t delay_size = delay_.size();
    std::vector<float> delayed(delay_size + mixed.size());
    std::ranges::copy(delay_, delayed.begin());
    std::ranges::copy(mixed, delayed.begin() + static_cast<std::ptrdiff_t>(delay_size));
    std::copy(delayed.end() - static_cast<std::ptrdiff_t>(delay_size), delayed.end(), delay_.begin());
    std::copy_n(delayed.begin(), mixed.size(), mixed.begin());
}

// The wet part comes back up.
void EnhancedMix::AddWetPart(std::vector<float>& mixed, std::size_t game_samples)
{
    const auto k = static_cast<std::size_t>(factor_);
    const uint64_t output_place = output_ >> 1;
    const SincKernel& kernel = SincKernel::ForStep(1.0);
    const auto window = static_cast<std::size_t>(kernel.HalfWidth()) * 2;
    for (std::size_t side = 0; side < 2; side++)
    {
        std::vector<float> all(window + game_samples);
        std::ranges::copy(ups_[side], all.begin());
        for (std::size_t i = 0; i < game_samples; i++)
        {
            all[window + i] = low_.ReadMemory(output_place + (i * 2) + side);
        }

        for (std::size_t i = 0; i < game_samples; i++)
        {
            for (std::size_t j = 0; j < k; j++)
            {
                const auto fraction = static_cast<uint32_t>((static_cast<uint64_t>(j) << 16) / k);
                mixed[(((i * k) + j) * 2) + side] += kernel.At(all.data() + i + 1, fraction);
            }
        }

        std::copy(all.end() - static_cast<std::ptrdiff_t>(window), all.end(), ups_[side].begin());
    }
}

// The last interpolator. The first output is at the time of the console's first sample.
void EnhancedMix::RunFinalInterpolator(const std::vector<float>& mixed, std::size_t count, std::vector<float>& out)
{
    const SincKernel& kernel = SincKernel::ForStep(static_cast<double>(rate_) / output_rate_);
    const int half = kernel.HalfWidth();
    for (std::size_t side = 0; side < 2; side++)
    {
        std::vector<float>& samples_of_side = final_[side];
        const std::size_t old_size = samples_of_side.size();
        samples_of_side.resize(old_size + count);
        for (std::size_t i = 0; i < count; i++)
        {
            samples_of_side[old_size + i] = mixed[(i * 2) + side];
        }
    }

    const int64_t end = final_base_ + static_cast<int64_t>(final_[0].size());
    int64_t last = final_base_ + half;
    while (true)
    {
        const uint64_t place = final_next_ * static_cast<uint64_t>(rate_);
        const int64_t whole =
            static_cast<int64_t>(lag_) + static_cast<int64_t>(place / static_cast<uint64_t>(output_rate_));
        if (whole + half >= end)
        {
            break;
        }

        const auto fraction = static_cast<uint32_t>(((place % static_cast<uint64_t>(output_rate_)) << 16) /
                                                    static_cast<uint64_t>(output_rate_));
        const auto first = static_cast<std::size_t>(whole - (half - 1) - final_base_);
        out.push_back(kernel.At(final_[0].data() + first, fraction));
        out.push_back(kernel.At(final_[1].data() + first, fraction));
        last = whole;
        final_next_++;
    }

    const int64_t keep_from = last - half;
    if (keep_from - final_base_ > 4096)
    {
        for (std::vector<float>& side : final_)
        {
            side.erase(side.begin(), side.begin() + (keep_from - final_base_));
        }

        final_base_ = keep_from;
    }
}

void EnhancedMix::MixFrame(uint32_t muted, bool metered, const AudioBus& bus, Peaks& peaks, std::vector<float>& out)
{
    const Frame& frame = queue_.front();
    if (!mixed_a_frame_)
    {
        mixed_a_frame_ = true;
        for (std::size_t track = 0; track < track_gains_.size(); track++)
        {
            track_gains_[track] = (muted >> track & 1) != 0 ? 0.0F : 1.0F;
        }
    }

    frame_muted_ = muted;
    frame_done_ = 0;
    frame_total_ = frame.samples * factor_;
    flush_slot_ = 0;
    bool in_voice = false;
    for (const AudioOp& op : frame.run)
    {
        switch (op.kind)
        {
        case AudioOpKind::kVoice:
            EndVoice();
            StartVoice(op.track, muted, metered);
            in_voice = true;
            break;
        case AudioOpKind::kVoicesEnd:
        case AudioOpKind::kChunkEnd:
            EndVoice();
            in_voice = false;
            FlushSends(op.bytes / 2);
            if (op.kind == AudioOpKind::kChunkEnd)
            {
                EndChunk(op.bytes / 2, peaks);
            }

            break;
        case AudioOpKind::kMainResample:
            RunMain(frame.mains[op.extra]);
            break;
        default:
            if (factor_ == 1)
            {
                RunOp(high_, frame, op, bus);
            }
            else if (in_voice)
            {
                RunOp(high_, frame, op, bus);
            }
            else
            {
                RunOp(low_, frame, op, bus);
                RunOp(high_, frame, op, bus);
            }

            break;
        }
    }

    EndVoice();

    // A list with no chunk marks.
    if (summed_tracks_ != 0)
    {
        EndChunk(static_cast<uint32_t>(frame.samples), peaks);
    }

    for (int track = 0; track < kTracks; track++)
    {
        track_gains_[static_cast<std::size_t>(track)] = (muted & (uint32_t{1} << track)) != 0 ? 0.0F : 1.0F;
    }

    Emit(frame.samples, out);
    queue_.pop_front();
}

int EnhancedMix::DropFrame()
{
    Frame& frame = queue_.front();
    for (const MainResample& main : frame.mains)
    {
        const auto found = streams_.find(main.stream);
        if (found != streams_.end())
        {
            found->second.uses--;
            if (found->second.closed && found->second.uses == 0)
            {
                streams_.erase(found);
            }
        }
    }

    const int samples = frame.samples;
    queue_.pop_front();

    return samples;
}

void EnhancedMix::ForgetVoices()
{
    streams_.clear();
    streams_by_state_.clear();
}

} // namespace ntdlib::n64
