// SPDX-License-Identifier: MIT

// Plays DSE's songs and sound effects as a DS game plays them: the model of DSE's driver (dse::Driver) on the ARM9's
// side, the model of the DS's sound driver (ntr::snd::Driver) on the ARM7's, and the sound unit. The player keeps the
// banks' samples in main memory, where the sound unit reads them.
//
// DSE's start-up locks every channel of the ARM7's driver against the driver's sequences, and sets up an alarm of the
// ARM7's driver: 5,236 ticks of the OS timer, 64 system clocks each, about 10 ms. Each alarm wakes DSE's thread. The
// thread reads the channels' status in the shared work, runs a tick of DSE's driver and sends the ARM7's driver its
// commands. The ARM7's driver runs them in its next frame, and writes the channels' status at the frame's end. Its
// frames come every snd::kFrameClocks. A channel stops when its wave ends without a loop, and DSE's next tick after the
// next frame then frees the voice.
//
// The player starts the ARM7's driver, sets up the shared work and gives it to the driver in its first frame, with
// DSE's start-up commands. That frame's clock is 0. DSE's ticks come every kTickClocks after it.
//
// A host can mute the followed sequence's tracks and meter them. DSE has neither. A voice belongs to the track whose
// note last started it, and its hardware channel goes on belonging to that track until another voice of DSE's starts
// there.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::dse
{

// What a song or a sound effect plays: the song, or effect `effect` of a bank of sound effects, the banks that the game
// loads, in their order, and the main bank that holds the waves of the banks without samples. The files must outlive
// what uses them.
struct SequenceFiles
{
    const Song* song = nullptr;
    const SoundEffects* effects = nullptr; // in place of a song
    uint16_t effect = 0;
    std::vector<const WaveBank*> banks = {};
    const WaveBank* main_bank = nullptr;
};

struct PlayerOptions
{
    // The clock of the first frame of the ARM7's driver after the start-up's frame, from 1 to snd::kFrameClocks. The
    // driver's frames come every snd::kFrameClocks from then on. In a game, the phase of the driver's alarm against
    // DSE's depends on when each started.
    uint64_t first_frame = ntr::snd::kFrameClocks;
    Build build = Build::kLater; // the build of DSE's driver

    // Mixes in the sound unit's enhanced mode (ntr::SoundUnit::StartEnhanced), from clock 0. Run(clocks,
    // std::vector<float>&) then gives the samples. DSE's driver keeps its 16 voices.
    bool enhanced = false;
};

class Player
{
public:
    // Where the player keeps the ARM9's data in main memory: the shared work, then the banks' samples.
    static constexpr uint32_t kMainMemory = 0x02000000;
    static constexpr uint32_t kMainMemorySize = 0x400000;
    static constexpr uint32_t kSharedWork = 0x02001000;
    static constexpr uint32_t kSamples = 0x02010000;

    // The time between DSE's ticks, in system clocks.
    static constexpr uint64_t kTickClocks = 5236 * 64;

    // The tracks of the followed sequence that a host can mute and meter.
    static constexpr int kHostTracks = 32;

    explicit Player(const PlayerOptions& options = {});
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Adds `bank` to DSE's banks (Driver::AddBank), with its samples in main memory when it holds them, and returns
    // the bank's id. DSE refuses a bank whose id it holds already, and the function then returns nothing. The bank must
    // outlive the player. Throws std::runtime_error when the samples don't fit in main memory.
    std::optional<uint16_t> AddBank(const WaveBank& bank);

    // Places the waves of bank `id` that `main_bank` holds in main memory, as DSE loads them (Driver::PlaceWaves).
    // Throws std::runtime_error when DSE holds no bank of that id, or when the waves don't fit in main memory.
    void PlaceWaves(uint16_t id, const WaveBank& main_bank);

    // Adds `effects` to DSE's banks of sound effects (Driver::AddEffects).
    std::optional<uint16_t> AddEffects(const SoundEffects& effects);

    // Loads the banks of `files` as a game does: each bank (AddBank), then the waves of each bank without samples that
    // the main bank holds (PlaceWaves). Returns the ids of the banks that DSE refused. DSE then plays the bank of that
    // id that it loaded first.
    std::vector<uint16_t> LoadBanks(const SequenceFiles& files);

    // Starts the song of `files`, or its sound effect, and follows its sequence (Driver::Follow). A player that plays
    // no sound effect gives one the first sequence of the pool. Returns false when DSE refuses the sound effect. A
    // start of a song stops the song that plays. Throws std::invalid_argument for files with neither.
    bool Start(const SequenceFiles& files);

    // DSE's driver, to play and stop songs and sound effects, and for a performer's hooks and views.
    dse::Driver& Driver();
    const dse::Driver& Driver() const;

    // Plays the samples up to DSE's next tick, appends them to `out`, and runs the ARM7's frames that come before it,
    // then the tick. Throws std::logic_error in the enhanced mode.
    void RunTick(std::vector<ntr::SoundOutput>& out);

    // Runs the tick in the same way, without mixing the samples (SoundUnit::Run). The meters then give 0.
    void RunTick();

    // Plays `clocks` system clocks of samples from Clock() on, and appends them to `out`. The ARM7's frames and DSE's
    // ticks that come in that time run in their order, those at its last clock included. A frame runs before a tick of
    // the same clock. Throws std::logic_error in the enhanced mode.
    void Run(uint64_t clocks, std::vector<ntr::SoundOutput>& out);

    // The same in the enhanced mode, with its samples in floating point, the left and the right side in turn. Throws
    // std::logic_error outside the enhanced mode.
    void Run(uint64_t clocks, std::vector<float>& out);

    // Runs the clocks in the same way, without mixing the samples. The meters then give 0.
    void Run(uint64_t clocks);

    // The system clock that the player has run to. After RunTick, that is the clock of DSE's last tick.
    uint64_t Clock() const;

    // The system clock of DSE's next tick.
    uint64_t NextTick() const;

    // Whether commands wait for the ARM7's next frame, DSE plays a song or a sound effect, or a channel sounds.
    bool Busy() const;

    // A host's mute of track `track` of the followed sequence, from 0 to kHostTracks - 1. A muted track plays on as
    // before, and none of the voices of its notes reach the output, their releases included. A change takes effect
    // with the next samples. The mutes aren't part of a snapshot. They stay through a restore. Throws
    // std::out_of_range for another track.
    void MuteTrack(int track, bool mute);
    bool IsTrackMuted(int track) const;

    // A host's meters of the followed sequence's tracks. A track's meter adds up its channels' output as the mixer adds
    // them (ntr::SoundUnit::SetMeters). TrackPeaks gives the peaks of track `track`'s meter in the last run, left and
    // right, on the scale of 16-bit samples (see ntr::AppendPcm16). Without meters, and for another track, it gives 0.
    // The meters change nothing in the output. They aren't part of a snapshot, and stay through a restore.
    void MeterTracks(bool meter);
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The channels that play, a bit for each: the channels' busy bits.
    uint16_t BusyChannels() const;

    // The channels' status that DSE's last tick read in the shared work.
    uint16_t LastStatus() const;

    const ntr::snd::Driver& SoundDriver() const;
    const ntr::SoundUnit& Unit() const;
    std::span<const uint8_t> MainMemory() const;

    // The player's state between ticks, with DSE's banks and the samples in main memory.
    class Snapshot
    {
    private:
        friend class Player;

        const Player* owner_ = nullptr;
        std::optional<dse::Driver> dse_;
        std::optional<ntr::snd::Driver> driver_;
        std::optional<ntr::SoundUnit> unit_;
        std::array<uint8_t, ntr::snd::kSharedWorkSize> shared_work_ = {};
        std::shared_ptr<const std::vector<uint8_t>> samples_;
        uint32_t free_ = 0;
        std::map<uint16_t, const WaveBank*> banks_;
        uint64_t clock_ = 0;
        uint64_t next_frame_ = 0;
        uint64_t next_tick_ = 0;
        uint16_t status_ = 0;
        std::vector<ntr::snd::CommandList> pending_;
        std::array<int8_t, ntr::snd::kChannelCount> channel_tracks_ = {};
    };

    // Takes a snapshot between ticks. DSE's hooks aren't part of it.
    Snapshot Save() const;

    // Returns the player to `snapshot`. DSE's hooks stay. Throws std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    class UnitRegisters final : public ntr::snd::Registers
    {
    public:
        explicit UnitRegisters(ntr::SoundUnit& unit);

        uint32_t Read(uint32_t address, int size) override;
        void Write(uint32_t address, int size, uint32_t value) override;

    private:
        ntr::SoundUnit& unit_;
    };

    // Copies `bytes` to the next free place in main memory and returns its address.
    uint32_t Store(std::span<const uint8_t> bytes);

    // Runs the clocks up to `end`, as Run does.
    void Play(uint64_t end, std::vector<ntr::SoundOutput>* out, std::vector<float>* enhanced_out);

    // Plays the samples up to `clock`, with the host's mutes and meters.
    void Advance(uint64_t clock, std::vector<ntr::SoundOutput>* out, std::vector<float>* enhanced_out);

    // The track of the followed sequence that each hardware channel plays for, from DSE's voices.
    void FollowVoices();

    // Runs DSE's tick.
    void Tick();

    std::vector<uint8_t> memory_;
    ntr::SoundUnit unit_;
    UnitRegisters registers_;
    ntr::snd::Driver driver_;
    dse::Driver dse_;
    uint32_t free_ = kSamples;
    std::map<uint16_t, const WaveBank*> banks_; // the files of DSE's banks, by their ids
    uint64_t clock_ = 0;
    uint64_t next_frame_ = 0;
    uint64_t next_tick_ = kTickClocks;
    uint16_t status_ = 0;
    std::vector<ntr::snd::CommandList> pending_; // the commands that wait for the ARM7's next frame

    // The track of the followed sequence that each hardware channel plays for, or -1.
    std::array<int8_t, ntr::snd::kChannelCount> channel_tracks_ = {};
    uint32_t muted_ = 0;
    bool metered_ = false;
    std::array<std::array<int32_t, 2>, kHostTracks> peaks_ = {};

    // The samples of the last snapshot, while no load has changed them.
    mutable std::shared_ptr<const std::vector<uint8_t>> saved_samples_;
};

} // namespace ntdlib::dse
