// SPDX-License-Identifier: MIT

// The layer that Skyward Sword HD puts in place of AX: NW4R SND's AX voices, mixed by nn::audio's renderer. The game
// is the Wii game, recompiled for the Switch. NW4R SND still writes AX's PBs, and the layer turns them into voices of
// the renderer. The model follows the code in the game's `main` (version 0) and its `sdk` (NintendoSDK 11.4.0, renderer
// REV8). Each function names the routine that it follows.
//
// The layer keeps 64 records, one for each AX voice. A record holds the PB and the layer's fields beside it. NW4R SND
// writes the PB as on the Wii. A record is free, acquired, playing, paused or waiting for its release. The PB's state
// word holds the record's state. The game's AX timer runs NW4R SND's frame every 3 ms. The render thread runs once for
// each frame of the renderer, every 5 ms (RenderFrame).
//
// Each record that plays has a voice of the renderer at 32 kHz. The voice's pitch is the PB's ratio. A splitter sends
// the voice to the final mix and to a sub-mix for each of aux buses A and B. The renderer gets the envelope's current
// volume and the mix's volumes, without their ramps. The low-pass filter becomes biquad 0, and the biquad becomes
// biquad 1. A wave buffer covers the wave from a base aligned to 64 bytes. A looping wave's buffer loops with the
// renderer's loop fields. The renderer reads PCM16 as little-endian, and NW4R SND's waves are the Wii's big-endian
// ones. The layer therefore gives the renderer a copy of each wave, with a PCM16 wave's bytes swapped. The layer plays
// no stream voices: a stream voice throws std::logic_error.
//
// Each sub-mix reaches the final mix at the return volume of its aux bus. The return volumes start at 0. The render
// thread reads each bus's 4 channels from the renderer, a frame at a time. A bus with the layer's callback
// (SetAuxCallback) runs it on them in place, and the renderer gets them back. The game's AxManager gives its effects
// that callback (see effects.h). A callback that NW4R SND gives AX for the Wii's buffers throws std::logic_error. So
// does a master volume other than 0x8000.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/dsp/memory.h"
#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"

namespace ntdlib::rvl::nx
{

namespace audio = ntdlib::nx::audio;

class Layer final : public ax::VoiceLayer
{
public:
    static constexpr int kVoiceCount = 64;

    // The samples that each read of an aux bus takes (0x103e2b0): 160 of each of the bus's 4 channels, one channel
    // after the other.
    static constexpr std::size_t kAuxSamples = 0x280;

    // A callback on an aux bus's frame. It processes the samples in place.
    using AuxCallback = std::function<void(std::span<int32_t, kAuxSamples> samples)>;

    // The renderer's priority for an AX priority (0x103ee88): 255 - 255 × priority / 31, in single precision, held to
    // 0 to 255 and truncated.
    static int32_t RendererPriority(uint32_t priority);

    // The renderer's settings (0xf2cc48): 32 kHz in frames of 160 samples, 14 mix buffers, 2 sub-mixes, 64 voices, 64
    // splitters and 192 splitter destinations.
    static audio::RendererParameter Parameter();

    // Gives the layer `ax`'s voices (System::UseLayer) and sets up `renderer` as the game does (0xf2cc48). The final
    // mix has 6 buffers. Sub-mixes A and B have 4 buffers at 32 kHz. They go to the first 4 buffers of the final mix.
    // Each sub-mix has an aux effect on its 4 buffers. The device sink takes buffers 0, 1, 4, 5, 2 and 3. `renderer`
    // must have the settings of Parameter(). `memory` contains the waves that NW4R SND plays. All three must outlive
    // the layer.
    Layer(ax::System& ax, const ntdlib::dsp::WiiMemory& memory, audio::Renderer& renderer);

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    // AXAcquireVoice (0x1027cf4): the first free record by index, at `priority`. With none free, the record of the
    // lowest priority below `priority` is taken, and among equals the one acquired first. Its callback runs first.
    // Its voice then stops and goes back to the renderer at once. Returns nullptr when no record has a lower priority.
    // The record's PB starts as zeros.
    ax::Voice* AcquireVoice(uint32_t priority, ax::Voice::Callback callback) override;

    // AXFreeVoice (0x1027b08). A record that never played is free at once. A record that plays or is paused stops. It
    // goes back to the renderer after the next frame (RenderFrame).
    void FreeVoice(ax::Voice& voice) override;

    // AXSetVoicePriority (0x1032bd0): the record's priority, and the renderer voice's while the record plays or is
    // paused. The order of acquisition stays.
    void SetVoicePriority(ax::Voice& voice, uint32_t priority) override;
    uint32_t VoicePriority(const ax::Voice& voice) const override;

    // AXSetVoiceState (0x103edc0). Running a record sets up its renderer voice the first time, then starts it. A set-up
    // that finds no free voice or splitter leaves the record acquired. Stopping a record that plays pauses its renderer
    // voice.
    void SetVoiceState(ax::Voice& voice, ax::VoiceState state) override;

    // One frame of the render thread (0x103dfd0): the final mix's volume, the wave buffers of each record that plays,
    // the aux buses, the parameters of each record that plays or is paused, then the renderer's frame
    // (Renderer::RenderFrame in place of RequestUpdateAudioRenderer). The records that wait for their release then go
    // back to the renderer.
    void RenderFrame();

    // The callback that the render thread runs on each frame of aux bus `bus` (0 or 1) that the renderer gives it in
    // full (0x103e2ec and 0x103e460). A bus without one returns what it read. The renderer runs a bus's aux effect only
    // while the bus has a callback.
    void SetAuxCallback(int bus, AuxCallback callback);

    // A snapshot of the records. The renderer and AX have separate snapshots. The wave copies stay while the layer
    // lives. The snapshot can only be restored to this layer.
    struct Snapshot;
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

private:
    // A record's state, in the PB's state word (+ 0x68).
    enum class State : int16_t
    {
        kPaused = 0,
        kPlaying = 1,
        kAcquired = 8,
        kReleasing = -2,
        kFree = -1,
    };

    // The layer's fields of a record. The two wave buffers, the loop's DSP-ADPCM context and the coefficients are in
    // the layer's arrays for the record (0x4cbb010, 0x4ca1000 and 0x4ca9000).
    struct Record
    {
        State state = State::kFree;
        uint32_t priority = 0; // + 0x30
        uint64_t stamp = 0;    // the order of acquisition (0x4cbd050)
        int voice = -1;
        int splitter = audio::Renderer::kNoSplitter;
        uint64_t base = 0;  // + 0x1a0: the wave buffers' base, in bytes
        uint64_t start = 0; // + 0x1a8: the first sample, from the base
        uint64_t end = 0;   // + 0x1b0: the last sample, from the base
        uint16_t slot = 0;  // + 0x1b8: the wave buffer that plays
        std::array<audio::WaveBufferInfo, 2> buffers{};
        audio::AdpcmContext loop_context{};
        audio::AdpcmCoefficients coefficients{};
    };

    // A wave's bytes from the model's memory, and the renderer's copy.
    struct WaveCopy
    {
        std::vector<uint8_t> original;
        std::vector<uint8_t> samples;
    };

    Record& RecordOf(const ax::Voice& voice);
    const Record& RecordOf(const ax::Voice& voice) const;
    void SetState(ax::Voice& voice, Record& record, State state);

    // The record's fields from + 0x10 on cleared, the PB and the callback included (0x1027f50 and 0x1027b5c).
    void ClearFields(ax::Voice& voice, Record& record);

    // The renderer voice and splitter go back to the renderer, and the record becomes free (0x1027fa0, 0x103e6f0).
    void Release(ax::Voice& voice, Record& record);

    // The set-up at a record's first run (0x103ee08 to 0x103f11c). Returns false when the renderer has no voice or
    // splitter for the record.
    bool SetUp(ax::Voice& voice, Record& record);

    // The record's wave buffer in `slot` (0x103e7b0).
    void AppendWaveBuffer(const ax::Voice& voice, Record& record, std::size_t slot);

    // The mix volumes for the bits of `mixer` (0x103eac0).
    void SetMix(const ax::Voice& voice, const Record& record, uint32_t mixer);

    // The record's wave buffers after a frame (0x103e0e8 to 0x103e244), and its parameters (0x103e590 to 0x103e6ec).
    void ServiceWaveBuffers(ax::Voice& voice, Record& record);
    void Convert(ax::Voice& voice, const Record& record);

    // An aux bus's return (0x103e250 for bus A, 0x103e3c4 for bus B).
    void ServiceAux(int bus);

    // The renderer's copy of `size` bytes of a wave at `address`.
    std::span<const uint8_t> Samples(uint32_t address, std::size_t size, ax::SampleFormat format);

    ax::System* ax_;
    const ntdlib::dsp::WiiMemory* memory_;
    audio::Renderer* renderer_;
    std::array<Record, kVoiceCount> records_{};
    uint64_t next_stamp_ = 1; // 0x169ab80
    std::array<int, 2> sub_mixes_{};
    std::array<int, 2> auxes_{};
    std::array<bool, 2> aux_enabled_{};
    std::array<AuxCallback, 2> aux_callbacks_;

    // The wave copies, by address and size. A copy stays while the layer lives: the renderer's wave buffers and its
    // snapshots point into it. A wave whose bytes change gets a new copy.
    std::deque<WaveCopy> copies_;
    std::map<std::pair<uint32_t, std::size_t>, const WaveCopy*> copy_index_;
};

struct Layer::Snapshot
{
    const Layer* layer = nullptr;
    std::array<Record, kVoiceCount> records{};
    uint64_t next_stamp = 1;
    std::array<bool, 2> aux_enabled{};
};

} // namespace ntdlib::rvl::nx
