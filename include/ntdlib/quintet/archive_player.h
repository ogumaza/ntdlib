// SPDX-License-Identifier: MIT

// Plays a song or a sound effect of a GBA ROM with Quintet's driver on ntdlib's models: the driver's per-frame routine
// (quintet::Sequencer) on the GBA's sound hardware (agb::Apu), frame by frame. The song's 6 channels are the tracks:
// square 1, square 2, the wave channel, the noise channel, then FIFO A and FIFO B.
//
// The driver plays a PCM note from the ROM. It empties the note's FIFO, stores the sample's first 32 bytes in it and
// points the FIFO's DMA at the rest. The DMA then moves 16 bytes each time the FIFO runs down to 16, and the FIFO's
// timer plays a byte at each overflow. SoundBus models that. The driver's stores of a frame all take effect at the
// frame's start: the model doesn't time them within the frame.
//
// The hardware starts as the game's sound init leaves it. The output's resolution comes from the sound bias register,
// which the play routine sets. Rate() gives it.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate. The APU plays the PSG's channels in its enhanced mode, and each FIFO's bytes reach it at that
// rate, interpolated between the timer's overflows. The mode has no anchor: no game ever ran it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/sequencer.h"

namespace ntdlib::quintet
{

// The GBA's sound hardware as Quintet's driver uses it: the APU, and each FIFO's DMA from the ROM at the rate of the
// FIFO's timer. Stores take effect at the start of the frame that RunFrame() runs next.
class SoundBus
{
public:
    // The registers from agb::kRegSound1CntL up to the FIFOs as `registers` holds them (agb::Apu's constructor). With
    // `fills`, the start of a FIFO's DMA puts the 32 bytes before the DMA's source in the FIFO, as the driver's stores
    // to the FIFO do. Without it, only stores to the FIFOs' registers put bytes in them.
    SoundBus(const agb::Rom& rom, std::span<const uint8_t> registers, bool fills);

    // Makes a store to the sound registers, to the FIFOs, to DMA 1's or DMA 2's registers, or to timer 0's or timer
    // 1's. The bus ignores others.
    void Store(const agb::RegisterWrite& write);

    // Runs agb::kFrameCycles of the hardware, and adds the samples to `out` or to `enhanced_out` (at most one of them),
    // left then right.
    void RunFrame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);

    // Puts the APU in the enhanced mixing mode.
    void StartEnhanced();

    // Returns true while FIFO `fifo` (0 or 1) has its timer running and its DMA on.
    bool FifoRunning(int fifo) const;

    agb::Apu& Apu()
    {
        return apu_;
    }
    const agb::Apu& Apu() const
    {
        return apu_;
    }

private:
    struct Timer
    {
        std::array<uint8_t, 4> registers = {}; // the reload, then the control
        uint32_t period = 0;                   // the cycles between overflows, or 0 while stopped
        uint64_t next = 0;                     // the cycles from the frame's start to the next overflow
    };

    struct Fifo
    {
        std::array<uint8_t, 12> registers = {}; // the DMA's source, destination, count and control
        std::deque<int8_t> bytes;               // what the FIFO holds
        int8_t output = 0;                      // the byte that the FIFO plays
        uint32_t source = 0;                    // the DMA's next byte
        bool dma = false;
    };

    // Empties each FIFO that a store to the high byte of SOUNDCNT_H resets.
    void ResetFifos(const agb::RegisterWrite& write);

    void StoreTimer(int timer, uint32_t offset, int size, uint32_t value);
    void StoreDma(int fifo, uint32_t offset, int size, uint32_t value);

    // Plays the bytes of FIFO `f` for a frame at each overflow of its timer, and refills it by DMA. In the enhanced
    // mode, adds the samples between the bytes to `enhanced`.
    void PlayFifo(int f, std::vector<float>& enhanced);

    // Puts `count` bytes from `address` in FIFO `fifo`, as the DMA or the CPU does.
    void Fill(int fifo, uint32_t address, uint32_t count);

    // Returns the timer that SOUNDCNT_H gives FIFO `fifo`.
    int TimerOf(int fifo) const;

    const agb::Rom* rom_ = nullptr;
    bool fills_ = true;
    agb::Apu apu_;
    std::array<Timer, 2> timers_;
    std::array<Fifo, 2> fifos_;
    std::array<std::vector<int8_t>, 2> moved_; // the bytes that a frame moves to each FIFO, for the APU
    bool enhanced_ = false;
};

// The registers from agb::kRegSound1CntL up to the FIFOs as the games' sound init leaves them in revision `revision`.
std::vector<uint8_t> InitRegisters(Revision revision);

// The most tracks that the player mutes and meters: the channels.
constexpr int kPlayerTracks = kChannels;

// What the driver and the hardware start from, beyond the song.
struct ArchivePlayerOptions
{
    // Plays in the enhanced mixing mode.
    bool enhanced = false;
};

class ArchivePlayer
{
public:
    // Keeps a reference to `rom`, which must outlive the player, and a copy of `files`: the music files of the game in
    // the numbering of ReadMusicFiles(). Throws std::invalid_argument when `files` is empty.
    ArchivePlayer(const agb::Rom& rom, std::span<const DriverInfo> files);
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;

    // Starts song `song` (a song or a sound effect, in the numbering of ReadMusicFiles()) as the game's play routine
    // does after the sound init. Throws std::invalid_argument when the song can't be played.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Starts an audition: one note on channel `channel` of song `song`, as Sequencer::Audition() plays it with
    // `instrument`, `index` and `volume`. The song's channels stay idle. The note sounds until ReleaseNote(). Throws
    // std::invalid_argument when the song can't be played, and std::out_of_range for a channel or an index that
    // Sequencer::Audition() refuses.
    void StartNote(int song, int channel, uint8_t instrument, int index, uint8_t volume,
                   const ArchivePlayerOptions& options = {});

    // Ends the audition's note at the next frame. Throws std::logic_error before a start.
    void ReleaseNote();

    // Runs one frame of the driver and of the hardware, and adds the frame's samples to `out` at Rate(), left then
    // right. A frame takes agb::kFrameCycles of the CPU's cycles. Throws std::logic_error before a start, and in the
    // enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // In the enhanced mode, runs one frame and adds its samples, left then right, with full scale at 1. Throws
    // std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs one frame without its samples, to reach a place in a song quickly. It ends in the state that a frame with
    // samples leaves.
    void RunFrame();

    // Whether the song still sounds: until its channels have ended and the driver's last stores have taken effect, and
    // while a FIFO runs. An end silences a channel. A note that a channel holds at its loop point sounds for ever, and
    // so does the song.
    bool Busy() const;

    uint64_t Frames() const
    {
        return frames_;
    }

    // The song's tick: the largest tick count of its channels, which counts every pass through a loop.
    uint64_t Tick() const;

    // The output's rate in Hz: agb::kEnhancedRate in the enhanced mode, or else from the resolution in the sound bias
    // register.
    int Rate() const
    {
        return enhanced_ ? agb::kEnhancedRate : bus_.Apu().Rate();
    }

    bool Enhanced() const
    {
        return enhanced_;
    }

    // The music file of the song that the last Start() started.
    const DriverInfo& Info() const;

    // The model of the driver. Throws std::logic_error before a start.
    const Sequencer& Driver() const;

    const SoundBus& Hardware() const
    {
        return bus_;
    }

    // Leaves track `track`'s channel out of the output from the next frame on. The channel goes on as before. Mutes
    // aren't part of a snapshot, and stay through a restore. Throws std::out_of_range for a track past kPlayerTracks.
    void MuteTrack(int track, bool mute);
    bool IsTrackMuted(int track) const
    {
        return muted_ >> track & 1;
    }

    // While any track is soloed, leaves every track that isn't out of the output, in the same way as a mute. A mute
    // still holds on a soloed track.
    void SoloTrack(int track, bool solo);
    bool IsTrackSoloed(int track) const
    {
        return soloed_ >> track & 1;
    }

    // From the next frame with samples on, keeps the tracks' and the output's peaks when `on`.
    void MeterTracks(bool on);

    // The last frame's largest part of track `track`'s channel on each side, left then right, on the scale of 16-bit
    // samples (agb::Apu::ChannelPeaks). 0 without meters, after a frame without samples and for a muted track.
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The last frame's largest output on each side, left then right, on the same scale. 0 without meters and after a
    // frame without samples.
    std::array<int32_t, 2> OutputPeaks() const
    {
        return output_peaks_;
    }

    // What Restore() takes to go back to a place in the song: the driver's whole state and the hardware's.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::shared_ptr<const Sequencer> sequencer_;
        std::shared_ptr<const SoundBus> bus_;
        size_t file_ = 0;
        uint64_t frames_ = 0;
        int tail_ = 0;
        bool enhanced_ = false;
    };

    // Takes a snapshot. `previous` is there for the interface that ntdlib's archive players share: a snapshot stands
    // alone. Throws std::logic_error before a start.
    Snapshot Save(const Snapshot* previous = nullptr) const;

    // Goes back to `snapshot`. The next frames give what they gave after the save. The last frame's meters go. Throws
    // std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    // Returns the sequencer that plays song `song` of the game's files.
    Sequencer Load(int song) const;

    // Starts the hardware and plays `sequencer`, of song `song`, from its start.
    void Begin(int song, const Sequencer& sequencer, const ArchivePlayerOptions& options);

    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void ApplyMutes();

    const agb::Rom& rom_;
    std::vector<DriverInfo> files_;
    size_t file_ = 0;
    std::optional<Sequencer> sequencer_;
    SoundBus bus_;
    uint64_t frames_ = 0;
    int tail_ = 0;
    bool enhanced_ = false;
    uint32_t muted_ = 0;
    uint32_t soloed_ = 0;
    bool metered_ = false;
    std::array<std::array<int32_t, 2>, kPlayerTracks> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::quintet
