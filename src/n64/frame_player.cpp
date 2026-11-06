// SPDX-License-Identifier: MIT

#include "ntdlib/n64/frame_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

// What the mixers' DMEM holds in each microcode: a command's address, plus the microcode's base. The list's sides come
// in the order of dry left, dry right, wet left and wet right.
std::vector<VoiceArea> Areas(uint32_t base, std::array<uint32_t, 4> places, uint32_t bytes)
{
    return {{base + places[0], bytes, 0, false},
            {base + places[1], bytes, 1, false},
            {base + places[2], bytes, 0, true},
            {base + places[3], bytes, 1, true}};
}

constexpr uint32_t kStandardBase = 0x5C0;
constexpr uint32_t kFirstEadBase = 0x450;

// The compact microcode's buffers start at this place, for a command's address of 0x4E0.
constexpr uint32_t kCompactBase = 0x4F0;
constexpr int kCompactStep = 184;
constexpr int kStandardStep = 16;

std::vector<VoiceArea> CompactAreas()
{
    return Areas(kCompactBase, {0x4E0, 0x650, 0x7C0, 0x930}, 0x170);
}

// alAudioFrame's first loop, with one client. The compact form of the driver runs its clients in this way too.
template <typename Model>
void RunClient(AlSynth& synth, Model& player, int32_t& client_samples, int samples)
{
    while (client_samples - synth.CurrentSamples() < samples)
    {
        synth.SetParamSamples(client_samples & ~0xF);
        client_samples += synth.TimeToSamplesNoRound(player.RunFrame());
    }

    synth.SetParamSamples(client_samples & ~0xF);
}

} // namespace

FrameFormat AlFrameFormat(const AlSynthConfig& config)
{
    FrameFormat format;
    format.output_rate = config.output_rate;
    if (config.form == AlSynthForm::kCompact)
    {
        format.microcode = AudioMicrocode::kCompact;
        format.frame_step = kCompactStep;
        format.areas = CompactAreas();
    }
    else
    {
        format.microcode = config.linear_ramps ? AudioMicrocode::kStandardLinear : AudioMicrocode::kStandard;
        format.frame_step = kStandardStep;
        format.areas = Areas(kStandardBase,
                             {static_cast<uint32_t>(kAlMainLeft), static_cast<uint32_t>(kAlMainRight),
                              static_cast<uint32_t>(kAlAuxLeft), static_cast<uint32_t>(kAlAuxRight)},
                             kAlMaxChunkSamples * 2);
    }

    return format;
}

FrameFormat EadEarlyFrameFormat(const EadEarlyConfig& config)
{
    FrameFormat format;
    format.microcode = AudioMicrocode::kStandard;
    format.output_rate = config.ai_frequency;
    format.frame_step = kStandardStep;
    format.areas = Areas(kStandardBase, {0x4C0, 0x600, 0x740, 0x880}, 0x140);

    return format;
}

FrameFormat EadMiddleFrameFormat(const EadMiddleConfig& config)
{
    FrameFormat format;
    format.output_rate = config.frequency;
    format.frame_step = kStandardStep;
    if (config.standard_microcode)
    {
        format.microcode = AudioMicrocode::kStandard;
        format.areas = Areas(kStandardBase, {0x4C0, 0x600, 0x740, 0x880}, 0x140);
    }
    else
    {
        format.microcode = AudioMicrocode::kEadFirst;
        format.areas = Areas(kFirstEadBase, {0x540, 0x6C0, 0x840, 0x9C0}, 0x180);
    }

    return format;
}

FrameFormat EadLaterFrameFormat(const EadLaterConfig& config)
{
    FrameFormat format;
    format.output_rate = config.sampling_frequency;
    format.frame_step = kStandardStep;
    if (config.version == EadLaterVersion::kOcarina)
    {
        format.microcode = AudioMicrocode::kEadSecond;
        format.areas = Areas(0, {0x940, 0xAE0, 0xC80, 0xE20}, 0x1A0);
    }
    else
    {
        format.microcode = AudioMicrocode::kEadThird;
        format.areas = Areas(0, {0x930, 0xAD0, 0xC70, 0xE10}, 0x1A0);
    }

    return format;
}

FrameFormat SbnFrameFormat(const SbnPlayerConfig& config)
{
    FrameFormat format;
    format.microcode = AudioMicrocode::kCompact;
    format.output_rate = config.output_rate;
    format.frame_step = kCompactStep;
    format.areas = CompactAreas();

    return format;
}

AlFrames::AlFrames(const AlSynthConfig& synth, const AlPlayerConfig& player, AudioMemory& memory)
    : synth_(synth, memory), player_(player, synth_), format_(AlFrameFormat(synth))
{
}

AlPlayer& AlFrames::Player()
{
    return player_;
}

AlSynth& AlFrames::Synth()
{
    return synth_;
}

std::unique_ptr<FrameModel> AlFrames::Clone() const
{
    return std::make_unique<AlFrames>(*this);
}

void AlFrames::TakeState(const FrameModel& other)
{
    const auto& held = dynamic_cast<const AlFrames&>(other);
    auto hook = std::move(player_.on_note);
    synth_ = held.synth_;
    player_ = held.player_;
    player_.on_note = std::move(hook);
}

void AlFrames::BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    BuildAlFrame(synth_, player_, list, memory, output, samples);
}

bool AlFrames::Busy() const
{
    return player_.State() == AlPlayerState::kPlaying || synth_.AnyVoicePlaying();
}

const FrameFormat& AlFrames::Format() const
{
    return format_;
}

AmbrellaFrames::AmbrellaFrames(const AlSynthConfig& synth, const AmbrellaPlayerConfig& player, AudioMemory& memory)
    : synth_(synth, memory), player_(player, synth_), format_(AlFrameFormat(synth))
{
}

AmbrellaPlayer& AmbrellaFrames::Player()
{
    return player_;
}

AlSynth& AmbrellaFrames::Synth()
{
    return synth_;
}

std::unique_ptr<FrameModel> AmbrellaFrames::Clone() const
{
    return std::make_unique<AmbrellaFrames>(*this);
}

void AmbrellaFrames::TakeState(const FrameModel& other)
{
    const auto& held = dynamic_cast<const AmbrellaFrames&>(other);
    auto note_hook = std::move(player_.on_note);
    auto random_hook = std::move(player_.random);
    synth_ = held.synth_;
    player_ = held.player_;
    player_.on_note = std::move(note_hook);
    player_.random = std::move(random_hook);
}

void AmbrellaFrames::BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    BuildAmbrellaFrame(synth_, player_, list, memory, output, samples);
}

bool AmbrellaFrames::Busy() const
{
    return player_.AnyPlaying() || synth_.AnyVoicePlaying();
}

const FrameFormat& AmbrellaFrames::Format() const
{
    return format_;
}

template <typename Model, typename Config>
MusFrames<Model, Config>::MusFrames(const AlSynthConfig& synth, const Config& player, AudioMemory& memory)
    : synth_(synth, memory), player_(player, synth_), format_(AlFrameFormat(synth))
{
}

template <typename Model, typename Config>
MusFrames<Model, Config>::MusFrames(const MusFrames& other)
    : FrameModel(other),
      synth_(other.synth_),
      player_(other.player_),
      client_samples_(other.client_samples_),
      format_(other.format_)
{
    player_.UseSynth(synth_);
}

template <typename Model, typename Config>
Model& MusFrames<Model, Config>::Player()
{
    return player_;
}

template <typename Model, typename Config>
AlSynth& MusFrames<Model, Config>::Synth()
{
    return synth_;
}

template <typename Model, typename Config>
std::unique_ptr<FrameModel> MusFrames<Model, Config>::Clone() const
{
    return std::make_unique<MusFrames>(*this);
}

template <typename Model, typename Config>
void MusFrames<Model, Config>::TakeState(const FrameModel& other)
{
    const auto& held = dynamic_cast<const MusFrames&>(other);
    auto hook = std::move(player_.on_note);
    synth_ = held.synth_;
    player_ = held.player_;
    player_.UseSynth(synth_);
    player_.on_note = std::move(hook);
    client_samples_ = held.client_samples_;
}

template <typename Model, typename Config>
void MusFrames<Model, Config>::BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    RunClient(synth_, player_, client_samples_, samples);
    synth_.BuildCommands(list, memory, output, samples);
}

template <typename Model, typename Config>
bool MusFrames<Model, Config>::Busy() const
{
    return !player_.Finished() || synth_.AnyVoicePlaying();
}

template <typename Model, typename Config>
const FrameFormat& MusFrames<Model, Config>::Format() const
{
    return format_;
}

template class MusFrames<MusPlayer, MusPlayerConfig>;
template class MusFrames<MusLatePlayer, MusLatePlayerConfig>;

template <typename Model>
Model& EngineFrames<Model>::Player()
{
    return player_;
}

template <typename Model>
std::unique_ptr<FrameModel> EngineFrames<Model>::Clone() const
{
    return std::make_unique<EngineFrames>(*this);
}

template <typename Model>
void EngineFrames<Model>::TakeState(const FrameModel& other)
{
    const auto& held = dynamic_cast<const EngineFrames&>(other);
    auto hook = std::move(player_.on_note);
    player_ = held.player_;
    player_.on_note = std::move(hook);
}

template <typename Model>
void EngineFrames<Model>::BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    player_.RunFrame(list, memory, output, samples);
}

template <typename Model>
bool EngineFrames<Model>::Busy() const
{
    return player_.Busy();
}

template <typename Model>
const FrameFormat& EngineFrames<Model>::Format() const
{
    return format_;
}

template class EngineFrames<EadEarlyPlayer>;
template class EngineFrames<EadMiddlePlayer>;
template class EngineFrames<EadLaterPlayer>;
template class EngineFrames<SbnPlayer>;

FramePlayer::FramePlayer(std::unique_ptr<FrameModel> model, AudioMemory& memory, double frame_samples,
                         const FramePlayerOptions& options)
    : model_(std::move(model)), memory_(&memory), frame_samples_(frame_samples), rsp_(model_->Format().microcode)
{
    const FrameFormat& format = model_->Format();
    if (!(frame_samples_ >= format.frame_step))
    {
        throw std::invalid_argument("a frame's samples are fewer than the format's step");
    }

    const auto most =
        static_cast<std::size_t>(std::ceil(frame_samples_)) + static_cast<std::size_t>(format.frame_step) * 2;
    output_ = memory_->Allocate(most * 4);
    std::size_t bytes = 0;
    for (const VoiceArea& area : format.areas)
    {
        bytes += area.bytes;
        area_samples_ = std::max<std::size_t>(area_samples_, area.bytes / 2);
    }

    before_.resize(bytes);
    sums_.resize(static_cast<std::size_t>(kHostTracks) * 2 * area_samples_);
    if (options.enhanced)
    {
        enhanced_.emplace(format.output_rate, format.areas, output_, options.output_rate);
    }
    else if (options.output_rate != 0)
    {
        throw std::invalid_argument("an output rate without the enhanced mode");
    }
}

int FramePlayer::OutputRate() const
{
    return enhanced_ ? enhanced_->OutputRate() : model_->Format().output_rate;
}

FrameModel& FramePlayer::Model()
{
    return *model_;
}

const FrameModel& FramePlayer::Model() const
{
    return *model_;
}

int FramePlayer::BuildNextFrame()
{
    const int step = model_->Format().frame_step;
    frame_debt_ += frame_samples_;
    const int samples = std::max(static_cast<int>(frame_debt_ / step) * step, step);
    frame_debt_ -= samples;
    list_.Clear();
    model_->BuildFrame(list_, *memory_, output_, samples);

    return samples;
}

void FramePlayer::RecordFrame()
{
    const int samples = BuildNextFrame();
    rsp_.StartTask();
    rsp_.RecordOps(&enhanced_->Recording());
    const std::span<const AudioCommand> commands = list_.commands;
    std::size_t done = 0;
    for (const AudioMark& mark : list_.marks)
    {
        rsp_.Run(commands.subspan(done, mark.command - done), *memory_);
        done = mark.command;
        if (mark.voice)
        {
            rsp_.RecordMark(AudioOpKind::kVoice, mark.track);
        }
        else
        {
            rsp_.RecordMark(mark.chunk_end ? AudioOpKind::kChunkEnd : AudioOpKind::kVoicesEnd, AudioMark::kNoTrack);
        }
    }

    rsp_.Run(commands.subspan(done), *memory_);
    rsp_.RecordOps(nullptr);
    enhanced_->EndRecord(samples, model_->Busy());
}

void FramePlayer::MixNextFrame()
{
    while (enhanced_->Queued() <= EnhancedMix::kLookAheadFrames)
    {
        RecordFrame();
    }

    frame_peaks_ = {};
    float_pending_.clear();
    pending_read_ = 0;
    enhanced_->MixFrame(muted_, metered_, *memory_, frame_peaks_, float_pending_);
    for (std::size_t track = 0; track < peaks_.size(); track++)
    {
        for (std::size_t side = 0; side < 2; side++)
        {
            peaks_[track][side] = std::max(peaks_[track][side], frame_peaks_[track][side]);
        }
    }
}

void FramePlayer::RunEnhancedWithoutMixing(std::size_t samples)
{
    peaks_ = {};
    const std::size_t held = std::min(samples, (float_pending_.size() - pending_read_) / 2);
    pending_read_ += held * 2;
    position_ += samples;
    if (samples == held)
    {
        return;
    }

    const double game_per_output =
        static_cast<double>(model_->Format().output_rate) / static_cast<double>(enhanced_->OutputRate());
    double needed = static_cast<double>(samples - held) * game_per_output;
    bool built = false;
    while (needed > 0.0)
    {
        if (enhanced_->Queued() > 0)
        {
            needed -= enhanced_->DropFrame();
        }
        else
        {
            needed -= BuildNextFrame();
            built = true;
        }
    }

    if (built)
    {
        enhanced_->ForgetVoices();
    }

    // The rest of the last frame is silent.
    float_pending_.assign(static_cast<std::size_t>(-needed / game_per_output) * 2, 0.0F);
    pending_read_ = 0;
}

void FramePlayer::StartFrame(bool mix)
{
    const int samples = BuildNextFrame();
    frame_peaks_ = {};
    pending_.clear();
    pending_read_ = 0;
    if (!mix)
    {
        pending_.resize(static_cast<std::size_t>(samples) * 2, 0);

        return;
    }

    rsp_.StartTask();
    if (muted_ != 0 || metered_)
    {
        RunMarked();
    }
    else
    {
        rsp_.Run(list_.commands, *memory_);
    }

    pending_.resize(static_cast<std::size_t>(samples) * 2);
    const std::span<const uint8_t> work = std::as_const(*memory_).Work();
    for (std::size_t i = 0; i < pending_.size(); i++)
    {
        pending_[i] = static_cast<int16_t>(xsflib::LoadBe16(work.data() + output_ + i * 2));
    }

    for (std::size_t track = 0; track < peaks_.size(); track++)
    {
        for (std::size_t side = 0; side < 2; side++)
        {
            peaks_[track][side] = std::max(peaks_[track][side], frame_peaks_[track][side]);
        }
    }
}

void FramePlayer::RunMarked()
{
    const std::span<const AudioCommand> commands = list_.commands;
    std::size_t done = 0;
    int track = AudioMark::kNoTrack;
    for (const AudioMark& mark : list_.marks)
    {
        rsp_.Run(commands.subspan(done, mark.command - done), *memory_);
        done = mark.command;
        if (track != AudioMark::kNoTrack)
        {
            EndVoice(track);
        }

        if (mark.chunk_end)
        {
            EndChunk();
        }

        const bool followed =
            mark.track >= 0 && mark.track < kHostTracks && (metered_ || (muted_ & (uint32_t{1} << mark.track)) != 0);
        track = followed ? mark.track : AudioMark::kNoTrack;
        if (followed)
        {
            std::size_t at = 0;
            for (const VoiceArea& area : model_->Format().areas)
            {
                std::memcpy(before_.data() + at, rsp_.Dmem().data() + area.place, area.bytes);
                at += area.bytes;
            }
        }
    }

    rsp_.Run(commands.subspan(done), *memory_);
    if (metered_)
    {
        EndChunk();
    }
}

void FramePlayer::EndVoice(int track)
{
    const bool muted = (muted_ & (uint32_t{1} << track)) != 0;
    std::size_t at = 0;
    for (const VoiceArea& area : model_->Format().areas)
    {
        uint8_t* const now = rsp_.Dmem().data() + area.place;
        if (metered_)
        {
            int32_t* const sums =
                sums_.data() +
                (static_cast<std::size_t>(track) * 2 + static_cast<std::size_t>(area.side)) * area_samples_;
            for (uint32_t i = 0; i < area.bytes / 2; i++)
            {
                const auto after = static_cast<int16_t>(xsflib::LoadBe16(now + i * 2));
                const auto earlier = static_cast<int16_t>(xsflib::LoadBe16(before_.data() + at + i * 2));
                sums[i] += after - earlier;
            }
        }

        if (muted)
        {
            std::memcpy(now, before_.data() + at, area.bytes);
        }

        at += area.bytes;
    }

    if (metered_)
    {
        summed_tracks_ |= uint32_t{1} << track;
    }
}

void FramePlayer::EndChunk()
{
    for (int track = 0; summed_tracks_ != 0; track++, summed_tracks_ >>= 1)
    {
        if ((summed_tracks_ & 1) == 0)
        {
            continue;
        }

        for (std::size_t side = 0; side < 2; side++)
        {
            int32_t* const sums = sums_.data() + (static_cast<std::size_t>(track) * 2 + side) * area_samples_;
            int32_t& peak = frame_peaks_[static_cast<std::size_t>(track)][side];
            for (std::size_t i = 0; i < area_samples_; i++)
            {
                peak = std::max(peak, std::abs(sums[i]));
                sums[i] = 0;
            }
        }
    }
}

void FramePlayer::ClearPeaks()
{
    const std::size_t pending = enhanced_ ? float_pending_.size() : pending_.size();
    peaks_ = pending_read_ < pending ? frame_peaks_ : Peaks{};
}

void FramePlayer::Run(std::size_t samples, std::vector<float>& out)
{
    if (!enhanced_)
    {
        std::vector<int16_t> whole;
        whole.reserve(samples * 2);
        Run(samples, whole);
        for (const int16_t sample : whole)
        {
            out.push_back(static_cast<float>(sample));
        }

        return;
    }

    ClearPeaks();
    while (samples > 0)
    {
        if (pending_read_ == float_pending_.size())
        {
            MixNextFrame();
            continue;
        }

        const std::size_t count = std::min(samples * 2, float_pending_.size() - pending_read_);
        const auto first = float_pending_.begin() + static_cast<std::ptrdiff_t>(pending_read_);
        out.insert(out.end(), first, first + static_cast<std::ptrdiff_t>(count));
        pending_read_ += count;
        samples -= count / 2;
        position_ += count / 2;
    }
}

void FramePlayer::Run(std::size_t samples, std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        std::vector<float> whole;
        whole.reserve(samples * 2);
        Run(samples, whole);
        for (const float sample : whole)
        {
            const float rounded = std::floor(sample + 0.5F);
            out.push_back(static_cast<int16_t>(std::clamp(rounded, -32768.0F, 32767.0F)));
        }

        return;
    }

    ClearPeaks();
    while (samples > 0)
    {
        if (pending_read_ == pending_.size())
        {
            StartFrame(true);
        }

        const std::size_t count = std::min(samples * 2, pending_.size() - pending_read_);
        const auto first = pending_.begin() + static_cast<std::ptrdiff_t>(pending_read_);
        out.insert(out.end(), first, first + static_cast<std::ptrdiff_t>(count));
        pending_read_ += count;
        samples -= count / 2;
        position_ += count / 2;
    }
}

void FramePlayer::Run(std::size_t samples)
{
    if (enhanced_)
    {
        RunEnhancedWithoutMixing(samples);

        return;
    }

    peaks_ = {};
    while (samples > 0)
    {
        if (pending_read_ == pending_.size())
        {
            StartFrame(false);
        }

        const std::size_t count = std::min(samples * 2, pending_.size() - pending_read_);
        pending_read_ += count;
        samples -= count / 2;
        position_ += count / 2;
    }
}

uint64_t FramePlayer::Position() const
{
    return position_;
}

bool FramePlayer::Busy() const
{
    if (enhanced_ && enhanced_->Queued() > 0)
    {
        return enhanced_->OldestBusy();
    }

    return model_->Busy();
}

void FramePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kHostTracks)
    {
        throw std::out_of_range("no such track");
    }

    const uint32_t bit = uint32_t{1} << track;
    muted_ = mute ? muted_ | bit : muted_ & ~bit;
}

bool FramePlayer::IsTrackMuted(int track) const
{
    return track >= 0 && track < kHostTracks && (muted_ & (uint32_t{1} << track)) != 0;
}

void FramePlayer::MeterTracks(bool meter)
{
    metered_ = meter;
    frame_peaks_ = {};
    peaks_ = {};
}

std::array<int32_t, 2> FramePlayer::TrackPeaks(int track) const
{
    if (!metered_ || track < 0 || track >= kHostTracks)
    {
        return {0, 0};
    }

    return peaks_[static_cast<std::size_t>(track)];
}

FramePlayer::Snapshot FramePlayer::Save() const
{
    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.model_ = model_->Clone();
    const std::span<const uint8_t> work = std::as_const(*memory_).Work();
    snapshot.memory_.assign(work.begin(), work.begin() + static_cast<std::ptrdiff_t>(memory_->UsedBytes()));
    snapshot.rsp_ = rsp_;
    snapshot.pending_ = pending_;
    snapshot.pending_read_ = pending_read_;
    snapshot.frame_debt_ = frame_debt_;
    snapshot.position_ = position_;
    snapshot.enhanced_ = enhanced_;
    snapshot.float_pending_ = float_pending_;

    return snapshot;
}

void FramePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this)
    {
        throw std::invalid_argument("a snapshot of another player");
    }

    model_->TakeState(*snapshot.model_);
    std::ranges::copy(snapshot.memory_, memory_->Work().begin());
    memory_->GiveBackTo(snapshot.memory_.size());
    rsp_ = snapshot.rsp_;
    pending_ = snapshot.pending_;
    pending_read_ = snapshot.pending_read_;
    frame_debt_ = snapshot.frame_debt_;
    position_ = snapshot.position_;
    enhanced_ = snapshot.enhanced_;
    float_pending_ = snapshot.float_pending_;
    frame_peaks_ = {};
    peaks_ = {};
    std::ranges::fill(sums_, 0);
    summed_tracks_ = 0;
}

} // namespace ntdlib::n64
