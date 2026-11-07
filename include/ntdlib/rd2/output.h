// SPDX-License-Identifier: MIT

// A model of Nintendo R&D2's driver and its mixer, frame by frame: the sequencer (rd2::Sequencer), the stores to the
// PSG's registers, the echo and the 8-bit bytes that the mixer leaves for the FIFOs. The model's bytes and stores
// follow the game's code bit for bit (rd2::GameDriver runs that code).
//
// The mixer of each frame:
//
// - It starts from a mix of 176 16-bit points on each side, all 0. While the echo's level is 15 or less, the echo's
//   buffer starts from the echo of 18 frames before. The Super Mario Advance 2 revision's echo always runs.
// - It adds each sample voice in the sequencer's order: to the echo's buffer with an echo send while the echo runs, and
//   to the mix without one. A sum wraps in 16 bits.
// - It moves the echo's level by 1 towards the level that the game asks for.
// - It adds the echo's buffer to the mix, and keeps the buffer shifted right by the level for 18 frames later.
// - It brings the mix to 8 bits and saturates it. The Super Mario Advance 2 revision has shifted each voice's points
//   already, and only saturates.
//
// The enhanced mixing mode (StartEnhancedMixing()) mixes the same voices in floating point: agb::kEnhancedFrameSamples
// samples a frame, through wave::SincKernel in place of the console's nearest point, with each voice's levels moving in
// a straight line across a frame. A note's first frame takes its levels at once. No sum wraps or saturates. The echo's
// line holds floating-point samples for the same 18 frames. The sequencer grows its voices in the mode
// (Sequencer::GrowVoices). The mode has no anchor: no game ever ran it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/sequencer.h"

namespace ntdlib::rd2
{

// The cycles of timer 0's period. Each period takes a byte from each FIFO. A frame holds kFrameSamples of them.
constexpr uint32_t kFifoCycles = 1596;

// The bytes that a FIFO plays between the VBlank at which a frame's mixing starts and the first byte of that mix. The
// frame's VBlank starts the DMAs on the last frame's mix, the next VBlank starts them on this frame's, and the FIFO
// still holds 17 bytes then.
constexpr int kFifoLead = int(kFrameSamples) + 17;

// The echo's delay, in frames.
constexpr int kEchoFrames = 18;

class Output
{
public:
    // Keeps references to `rom` and `info`. Both must outlive the model.
    Output(const Rom& rom, const DriverInfo& info);

    // Plays version `version` of song `song`, from the state that the game's sound init leaves. The sequencer takes the
    // request in the next Frame().
    void Play(int song, int version = 0);

    // The song that plays, or null before Play().
    const Sequencer* Song() const
    {
        return sequencer_ ? &*sequencer_ : nullptr;
    }

    // Asks for the echo's level `level`, up to 31, as the game's request does. The level moves towards it by 1 a frame.
    void SetEchoGoal(uint8_t level)
    {
        echo_goal_ = level < 31 ? level : 31;
    }

    uint8_t EchoLevel() const
    {
        return echo_level_;
    }

    // Runs a frame: the sequencer, the PSG's voices and the mixer.
    void Frame();

    // The frame's stores to the sound registers, in order.
    const std::vector<agb::RegisterWrite>& SoundWrites() const
    {
        return sequencer_->Writes();
    }

    // The bytes that the frame's mix leaves for FIFO A (the left side) and FIFO B (the right side).
    const std::array<int8_t, kFrameSamples>& FifoA() const
    {
        return fifo_a_;
    }

    const std::array<int8_t, kFrameSamples>& FifoB() const
    {
        return fifo_b_;
    }

    // The sound registers from agb::kRegSound1CntL up to agb::kRegFifoA, as the driver's init leaves them.
    std::vector<uint8_t> SoundRegisters() const;

    // Leaves each voice of the tracks in `tracks`, a bit for each of the player's tracks, out of the frame's mix and of
    // the echo. The sums then hold no wrap of those voices. The tracks go on as before.
    void SetMutedTracks(uint16_t tracks)
    {
        muted_ = tracks;
    }

    // The player's track that last started a note on PSG unit `unit`'s channel `channel` (0 to 3), or -1 for none.
    int PsgChannelTrack(int unit, int channel) const;

    // The PSG units that the voices have used: the console's, and the grown ones.
    int PsgUnits() const
    {
        return int(psg_tracks_.size());
    }

    // From the next Frame() on, keeps the largest part of each track's voices on each side when `on`.
    void MeterVoices(bool on)
    {
        metering_ = on;
    }

    // The last frame's largest part of `track`'s sample voices on the left and the right, on the scale of 8-bit
    // samples. The echo stays out.
    const std::array<float, 2>& VoicePeaks(int track) const
    {
        return peaks_[size_t(track)];
    }

    // Puts the model in the enhanced mixing mode from its next Frame() on, before the song's first. The console's
    // mixer goes on beside it. Throws std::logic_error before Play() and once the song has run a frame.
    void StartEnhancedMixing();

    // In the enhanced mode, whether the next frames give samples. Without them, the voices move on without mixing. An
    // echo's input is silent there.
    void SetEnhancedFrames(bool on)
    {
        enhanced_frames_ = on;
    }

    // The enhanced mode's samples of the last frame for each FIFO, on the scale of 8-bit samples.
    const std::vector<float>& EnhancedFifoA() const
    {
        return enhanced_->out[0];
    }

    const std::vector<float>& EnhancedFifoB() const
    {
        return enhanced_->out[1];
    }

    // Whether the mixer has fallen silent: no sample voice plays, the frame's mix holds nothing and the echo's history
    // holds nothing.
    bool MixerSilent() const;

private:
    struct EnhancedVoice
    {
        uint64_t position = 0; // in 2^-32 points from the sample's first point
        std::array<float, 2> levels = {};
    };

    struct Enhanced
    {
        std::vector<EnhancedVoice> voices;
        std::array<std::vector<float>, 2> out;
        std::array<std::vector<float>, 2> wet;  // the voices with an echo send, in the frame
        std::array<std::vector<float>, 2> line; // the echo's line: kEchoFrames frames on each side
        size_t line_position = 0;
    };

    void FollowEvents();
    void MixConsole();
    void MixVoice(const VoiceMix& mix, bool to_echo);
    void RunEcho();
    void MakeBytes();
    void MixEnhanced(bool echo_runs);
    void MixEnhancedVoice(const VoiceMix& mix, bool to_echo);
    bool Muted(int voice) const;

    const Rom& rom_;
    const DriverInfo& info_;
    const RevisionTraits& traits_;
    std::optional<Sequencer> sequencer_;
    std::array<int16_t, 2 * 2 * kFrameSamples> buffer_ = {}; // the mix's two sides, then the echo buffer's
    std::vector<int16_t> history_;                           // the echo's frames, each laid out as the echo buffer
    uint8_t echo_level_ = 16;
    uint8_t echo_goal_ = 16;
    uint8_t echo_counter_ = 0;
    uint8_t echo_index_ = 0;
    std::array<int8_t, kFrameSamples> fifo_a_ = {};
    std::array<int8_t, kFrameSamples> fifo_b_ = {};
    bool bytes_silent_ = true;
    uint16_t muted_ = 0;
    std::vector<int> voice_tracks_;              // the player's track behind each voice's last note, or -1
    std::vector<std::array<int, 4>> psg_tracks_; // the same for each PSG unit's channels
    bool metering_ = false;
    std::array<std::array<float, 2>, kPlayerTracks> peaks_ = {};
    std::optional<Enhanced> enhanced_;
    bool enhanced_frames_ = true;
    bool ran_ = false;
};

} // namespace ntdlib::rd2
