// SPDX-License-Identifier: MIT

// System's frames (see system.h): the render of a frame and the steps without mixing, the host's mutes and meters, the
// snapshots, and the sync of each voice's changes into the renderer's copy.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "cafe/ax/system_internal.h"
#include "ntdlib/ax/ax.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/render.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/ax/voice.h"

namespace ntdlib::cafe::ax
{

void System::RenderFrame()
{
    in_frame_ = true;
    const int n = samples_per_frame_;
    TakeVoices();
    if (render_observer_)
    {
        render_observer_(order_);
    }
    if (!mixing_)
    {
        AdvanceWithoutMixing();
        return;
    }
    silent_ = false;
    MixVoices();
    MeasureMeters(n);

    // The aux buses go out and come back. Then the game's callbacks run.
    if (enhanced_ != nullptr)
    {
        enhanced_->main = enhanced_->buses;
        ReturnAuxEnhanced(frame_);
    }
    else
    {
        main_ = buses_;
        ReturnAux(frame_);
    }
    RunAuxCallbacks(frame_);
    if (frame_callback_)
    {
        frame_callback_();
    }
    if (enhanced_ != nullptr)
    {
        FinalMixStageEnhanced();
        ++frame_;
        in_frame_ = false;
        return;
    }

    // Each device's main bus becomes the device's output at unity volume.
    const auto samples = static_cast<std::size_t>(n);
    for (int channel = 0; channel < kTvChannels; ++channel)
    {
        OutputBus({main_.tv[0][channel].data(), samples}, kUnityVolume, kUnityVolume,
                  {output_.tv[channel].data(), samples});
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int channel = 0; channel < kDrcChannels; ++channel)
        {
            OutputBus({main_.drc[drc][0][channel].data(), samples}, kUnityVolume, kUnityVolume,
                      {output_.drc[drc][channel].data(), samples});
        }
    }

    // The final mix stage, then the TV's stereo output, clamped to 16 bits (sndcore2 0x02008078 and 0x020092BC in
    // version 15702 of the OS). Between the two the console also runs the post-final-mix callback, the GamePad's
    // virtual surround, the OS's transition fade and the compressor. None of those is modelled.
    FinalMixStage();
    for (int i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        stereo_[2 * i] = ClampToInt16(output48_.tv[0][i]);
        stereo_[2 * i + 1] = ClampToInt16(output48_.tv[1][i]);
    }

    ++frame_;
    in_frame_ = false;
}

void System::TakeVoices()
{
    // The voices to render are those that are playing or have changes waiting. They're taken by priority from the
    // highest, oldest first within each priority. Each voice takes its changes now. The voices are rendered in the
    // opposite order.
    order_.clear();
    const auto take = [this](uint32_t index)
    {
        Voice* voice = &voices_[index];
        if (voice->game.state == VoiceState::kRun || voice->sync)
        {
            Sync(*voice);
            order_.push_back(voice);
        }
    };
    for (uint32_t priority = kPriorityMax; priority >= 1; --priority)
    {
        allocation_.ForEachOldestFirst(priority, take);
    }

    // A free voice is stopped in both copies.
    const auto stop = [this](uint32_t index)
    {
        Voice& voice = voices_[index];
        voice.state = VoiceState::kStop;
        voice.game.state = VoiceState::kStop;
        voice.renderer.state = VoiceState::kStop;
    };
    allocation_.ForEachNewestFirst(kPriorityFree, stop);
}

void System::MixVoices()
{
    // The host's mutes and meters take part in the voices' mixes.
    if (enhanced_ != nullptr)
    {
        EnhancedState& enhanced = *enhanced_;
        enhanced.buses = {};
        for (std::array<FloatBuses::Channel, 2>& sums : enhanced.meter_sums)
        {
            sums = {};
        }
        for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
        {
            Voice& each = **voice;
            RenderVoiceEnhanced(each.renderer, enhanced.voices[each.index], *memory_, enhanced.buses,
                                TakeHostMix(each));
        }
    }
    else
    {
        buses_ = {};
        for (std::array<Buses::Channel, 2>& sums : meter_sums_)
        {
            sums = {};
        }
        for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
        {
            RenderVoice((*voice)->renderer, *memory_, samples_per_frame_, buses_, cemu_, TakeHostMix(**voice));
        }
    }
    mutes_at_once_ = false;
}

void System::AdvanceWithoutMixing()
{
    // The voices move on in the same order. Each takes its host's mute as in a mixed frame.
    for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
    {
        Voice& each = **voice;
        TakeHostMix(each);
        if (enhanced_ != nullptr)
        {
            AdvanceVoiceEnhanced(each.renderer, enhanced_->voices[each.index], *memory_);
        }
        else
        {
            AdvanceVoice(each.renderer, *memory_, samples_per_frame_, cemu_);
        }
    }
    mutes_at_once_ = false;

    // The frame's buses, output and meters are silent. The buses and the output stay so through the frames that follow
    // without mixing. Each aux bus with a callback takes its return volume as the DSP's.
    if (!silent_)
    {
        buses_ = {};
        main_ = {};
        output_ = {};
        output48_ = {};
        stereo_ = {};
        if (enhanced_ != nullptr)
        {
            enhanced_->buses = {};
            enhanced_->main = {};
            enhanced_->output = {};
        }
        silent_ = true;
    }
    for (std::array<int32_t, 2>& peaks : meter_peaks_)
    {
        peaks = {0, 0};
    }
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        for (std::size_t bus = 0; bus < 3; ++bus)
        {
            aux_peaks_[device][bus] = {0, 0};
            AuxBus& aux = aux_[device][bus];
            if (aux.callback)
            {
                aux.dsp_return_volume = aux.return_volume;
            }
        }
    }

    if (frame_callback_)
    {
        frame_callback_();
    }
    ++frame_;
    in_frame_ = false;
}

void System::SetVoiceMuted(const Voice& voice, bool muted)
{
    host_[voice.index].muted = muted;
}

void System::SetMeters(int count)
{
    const auto meters = static_cast<std::size_t>(std::max(count, 0));
    meter_sums_.assign(meters, {});
    meter_peaks_.assign(meters, {0, 0});
    if (enhanced_ != nullptr)
    {
        enhanced_->meter_sums.assign(meters, {});
    }
}

void System::SetVoiceMeter(const Voice& voice, int meter)
{
    host_[voice.index].meter = meter;
}

std::array<int32_t, 2> System::MeterPeaks(int meter) const
{
    if (meter < 0 || static_cast<std::size_t>(meter) >= meter_peaks_.size())
    {
        return {0, 0};
    }

    return meter_peaks_[static_cast<std::size_t>(meter)];
}

std::array<int32_t, 2> System::AuxReturnPeaks(Device device, uint32_t index, int bus) const
{
    if (bus < 0 || bus > 2)
    {
        return {0, 0};
    }
    if (device == Device::kTv && index == 0)
    {
        return aux_peaks_[0][static_cast<std::size_t>(bus)];
    }
    if (device == Device::kDrc && index < kDrcCount)
    {
        return aux_peaks_[1 + index][static_cast<std::size_t>(bus)];
    }

    return {0, 0};
}

HostMix System::TakeHostMix(const Voice& voice)
{
    HostVoice& host = host_[voice.index];
    HostMix mix;
    mix.was_muted = host.rendered && !mutes_at_once_ ? host.was_muted : host.muted;
    mix.muted = host.muted;
    if (host.meter >= 0 && static_cast<std::size_t>(host.meter) < meter_sums_.size())
    {
        const auto meter = static_cast<std::size_t>(host.meter);
        if (enhanced_ != nullptr)
        {
            mix.float_meter = &enhanced_->meter_sums[meter];
        }
        else
        {
            mix.meter = &meter_sums_[meter];
        }
    }
    host.was_muted = host.muted;
    host.rendered = true;

    return mix;
}

System::Snapshot System::Save() const
{
    const auto first = static_cast<std::ptrdiff_t>(first_taken_);
    Snapshot snapshot{this, frame_,      user_count_, first_taken_, {voices_.begin() + first, voices_.end()},
                      {},   allocation_, aux_,        mixers_};
    if (enhanced_ != nullptr)
    {
        snapshot.enhanced.assign(enhanced_->voices.begin() + first, enhanced_->voices.end());
    }

    return snapshot;
}

void System::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this)
    {
        throw std::invalid_argument("a snapshot of another AX");
    }

    frame_ = snapshot.frame;
    user_count_ = snapshot.user_count;
    allocation_ = snapshot.allocation;

    // The voices taken first since the snapshot go back to how Init left them. The others take the snapshot's copies.
    for (uint32_t index = first_taken_; index < snapshot.first_taken; ++index)
    {
        voices_[index] = Voice{};
        voices_[index].index = index;
        if (enhanced_ != nullptr)
        {
            enhanced_->voices[index] = EnhancedVoice{};
        }
    }
    const auto first = static_cast<std::ptrdiff_t>(snapshot.first_taken);
    std::copy(snapshot.voices.begin(), snapshot.voices.end(), voices_.begin() + first);
    if (enhanced_ != nullptr)
    {
        std::copy(snapshot.enhanced.begin(), snapshot.enhanced.end(), enhanced_->voices.begin() + first);
    }
    first_taken_ = snapshot.first_taken;

    // The buses and the final mix stages take back their state, and keep their callbacks.
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        for (std::size_t bus = 0; bus < aux_[device].size(); ++bus)
        {
            AuxBus& aux = aux_[device][bus];
            const AuxBus& saved = snapshot.aux[device][bus];
            aux.return_volume = saved.return_volume;
            aux.dsp_return_volume = saved.dsp_return_volume;
            aux.buffers = saved.buffers;
            aux.sends = saved.sends;
            aux.returns = saved.returns;
        }
    }
    for (std::size_t i = 0; i < mixers_.size(); ++i)
    {
        FinalMixCallback callback = std::move(mixers_[i].callback);
        mixers_[i] = snapshot.mixers[i];
        mixers_[i].callback = std::move(callback);
    }

    // The next frame takes the voices' mutes at once.
    mutes_at_once_ = true;
}

void System::ResetVoice(Voice& voice)
{
    // What every acquired voice starts with. The rest of the game's copy (the addresses, the ADPCM state, the envelope,
    // the mix volumes, the ratio, the filters' coefficients) keeps the last owner's values until the game sets it. The
    // renderer's copy keeps all of the last owner's values until the changes reach it.
    voice.state = VoiceState::kStop;
    voice.sync = sync::kState | sync::kItd | sync::kSrc | sync::kLpf | sync::kBiquad | sync::kRmtOn | sync::kRmtSrc |
                 sync::kRmtIir;
    voice.last_position = 0;
    voice.loop_count = 0;

    Pb& game = voice.game;
    game.state = VoiceState::kStop;
    game.lpf.on = 0;
    game.biquad.on = 0;
    game.mixer_select = static_cast<uint16_t>(default_mixer_select_);
    game.src_mode = SrcMode::kFourTap;
    game.src.frac = 0;
    game.src.last = {};
    game.tv_mask = {};
    game.drc_mask = {};
}

void System::Touch(Voice& voice)
{
    // While the game has begun protection, the voices it changes are protected too.
    if (user_count_ > 0 && !in_frame_)
    {
        voice.is_protected = true;
    }
}

void System::Sync(Voice& voice)
{
    // The renderer's progress reaches the game's copy. If the game has changed the same thing since, the game's change
    // stays.
    TakeRendererProgress(voice);

    // A protected voice's changes wait.
    if (voice.is_protected)
    {
        return;
    }

    // In the enhanced mode, a voice that the game starts takes its gains at once.
    const uint32_t bits = voice.sync;
    if (enhanced_ != nullptr && (bits & sync::kState) && voice.renderer.state != VoiceState::kRun &&
        voice.game.state == VoiceState::kRun)
    {
        enhanced_->voices[voice.index].starts = true;
    }

    // The game's changes reach the renderer's copy.
    ApplyGameChanges(voice.game, voice.renderer, bits, cemu_.stale_history);

    // In the enhanced mode, a new SRC, low-pass or biquad sets the past values that the voice keeps in that mode.
    if (enhanced_ != nullptr)
    {
        ResetEnhancedPast(enhanced_->voices[voice.index], voice.renderer, bits);
    }

    voice.sync = 0;
}

} // namespace ntdlib::cafe::ax
