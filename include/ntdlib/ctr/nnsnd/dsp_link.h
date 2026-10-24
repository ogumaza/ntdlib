// SPDX-License-Identifier: MIT

// Model of nn::snd's link to the DSP audio firmware (CTR SDK 5.2, as linked into Pokemon X).
//
// This is the ARM11 side of the DSP shared-memory protocol, as worked out from the game's code.bin:
//   0x148764 open: register the audio-pipe interrupt event, SetSemaphoreMask(0x2000)
//   0x1488e8 initialize: audio pipe "Initialize", SetSemaphore(0x4000), read the 15 struct
//            addresses, map both regions (region 1 = region 0 address | 0x10000), commit frame 4
//   0x18b40c receive: re-derive the frame counter and read region, read source statuses
//   0x18b2d4 send:    run aux-bus callbacks and effects in place, write parameters, commit the frame counter,
//            signal the semaphore event (which makes dsp::DSP set semaphore 0x2000)

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include "ntdlib/ctr/dsp/device.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"

namespace ntdlib::ctr::nnsnd
{

class DspLink
{
public:
    explicit DspLink(dsp::Device& dsp);

    // Loads the DSP component and performs nn::snd's initialization handshake. Returns false if the DSP didn't answer.
    bool Initialize(std::span<const uint8_t> component);

    // Runs the DSP until it raises the audio-pipe interrupt that ends a frame, then performs nn::snd's receive step.
    // Returns false if the DSP stopped producing frames.
    bool WaitForFrame();

    // Commits the frame being prepared (nn::snd 0x18b2d4 tail): write the frame counter into the write region, advance
    // it and signal the semaphore event.
    void Commit();

    // The structures in the region the ARM11 is preparing (frame & 1) and in the region the DSP last wrote (the read
    // region). They're host copies of DSP RAM, kept in step with it by Initialize, WaitForFrame and Commit, which are
    // the only places the DSP runs. In the enhanced mode, the device keeps the sources' structures outside DSP RAM
    // (dsp::Device::SourceStructure), and the copies come from there.
    SourceParams& SourceParamsOf(int voice);
    int16_t* AdpcmCoefs(int voice);
    MixerParams& Mixer();
    const SourceReport& SourceReportOf(int voice) const;

    // The four channels of aux bus `bus` (0 or 1) in the read region, where the DSP left the bus's samples of its last
    // frame: AuxSamples() samples each. The send step's aux callbacks change them in place, and the DSP takes them back
    // in its next frame. In the firmware's mixing, they're host copies that go back to DSP RAM before the DSP next
    // runs. In the enhanced mode, the device keeps them outside DSP RAM (dsp::Device::AuxStructure).
    std::array<int32_t*, 4> AuxChannels(int bus);
    std::size_t AuxSamples() const;

    // The device's sources, the most that it can have, and a growth to `count` of them (dsp::Device::GrowSources).
    int SourceCount() const
    {
        return dsp_->SourceCount();
    }

    int MaxSources() const
    {
        return dsp_->MaxSources();
    }

    void GrowSources(int count);

    // The device that the link drives, for a host's meters (dsp::Device::SetMeters).
    dsp::Device& Dsp() const
    {
        return *dsp_;
    }

private:
    // Host copies of one of the firmware's structures, one per region. They're loaded from DSP RAM when first used,
    // written back before the DSP next runs, and reloaded after that. A structure of the sources holds an entry for
    // each of the device's sources.
    template <typename T>
    struct Mirror
    {
        std::array<T, 2> value{};
        std::array<bool, 2> loaded{};
    };

    template <typename T>
    using SourceMirror = Mirror<std::vector<T>>;

    uint8_t* Address(int structure, int region) const;
    uint8_t* SourceAddress(int structure, int region) const;
    uint16_t LoadWord(int structure, int region) const;
    void StoreWord(int structure, int region, uint16_t value);

    template <typename T>
    T& Get(Mirror<T>& mirror, int structure, int region) const
    {
        static_assert(std::is_trivially_copyable_v<T>);

        if (!mirror.loaded[region])
        {
            std::memcpy(&mirror.value[region], Address(structure, region), sizeof(T));
            mirror.loaded[region] = true;
        }

        return mirror.value[region];
    }

    template <typename T>
    void WriteBack(const Mirror<T>& mirror, int structure)
    {
        for (int region = 0; region < 2; region++)
        {
            if (mirror.loaded[region])
            {
                std::memcpy(Address(structure, region), &mirror.value[region], sizeof(T));
            }
        }
    }

    template <typename T>
    std::vector<T>& Get(SourceMirror<T>& mirror, int structure, int region) const
    {
        static_assert(std::is_trivially_copyable_v<T>);

        if (!mirror.loaded[region])
        {
            std::vector<T>& value = mirror.value[region];
            value.resize(static_cast<std::size_t>(dsp_->SourceCount()));
            std::memcpy(value.data(), SourceAddress(structure, region), value.size() * sizeof(T));
            mirror.loaded[region] = true;
        }

        return mirror.value[region];
    }

    template <typename T>
    void WriteBack(const SourceMirror<T>& mirror, int structure)
    {
        for (int region = 0; region < 2; region++)
        {
            if (mirror.loaded[region])
            {
                std::memcpy(SourceAddress(structure, region), mirror.value[region].data(),
                            mirror.value[region].size() * sizeof(T));
            }
        }
    }

    // Runs the DSP in slices of `slice` cycles until it raises the audio pipe's interrupt, and clears the interrupt.
    // The slice in which the interrupt comes runs to its end. Returns false if `limit` cycles pass first.
    bool RunUntilAudioInterrupt(uint64_t limit, uint64_t slice);

    void BeforeDspRuns();
    void AfterDspRuns();

    dsp::Device* dsp_; // a pointer, so that the link can be copied into a snapshot
    std::array<uint16_t, 15> struct_addr_{};
    uint16_t frame_ = 0;   // nn::snd +0x131a
    int read_region_ = 0;  // nn::snd +0x131c
    int write_region_ = 0; // nn::snd +0x131e
    bool audio_irq_ = false;
    mutable SourceMirror<SourceParams> sources_;
    mutable SourceMirror<std::array<int16_t, 16>> adpcm_coefs_;
    mutable Mirror<MixerParams> mixer_;
    mutable SourceMirror<SourceReport> reports_;
    mutable Mirror<AuxBuses> aux_;
};

} // namespace ntdlib::ctr::nnsnd
