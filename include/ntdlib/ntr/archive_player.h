// SPDX-License-Identifier: MIT

// Plays a sound archive's sequences on the model of the DS's sound driver (snd::Driver) and on the sound unit, with the
// ARM9's part on the host. The ARM9's part is the SDK's sound archive player. It loads a sequence's files into main
// memory, and it sends the driver the commands that start the sequence.
//
// The player starts the driver, sets up the shared work and gives it to the driver in its first frame. The ARM9's
// SND_Init does the same. Each frame of the driver comes snd::kFrameClocks system clocks after the last. The sound unit
// plays the samples between frames.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/heap.h"

namespace ntdlib::ntr
{

// A sequence of a sound archive: a sequence with a file of its own in the archive, or a sequence of a sequence
// archive.
struct SequenceRef
{
    friend bool operator==(const SequenceRef&, const SequenceRef&) = default;

    uint32_t index = 0;                    // the sequence, or the sequence archive
    std::optional<uint32_t> archived = {}; // the sequence's index in the sequence archive
};

// The values that a start gives the variables before the sequence plays. A game's code can write them the same way. The
// others keep their values: the -1 that the ARM9's SND_Init gives them, or what the sequences have written since.
struct Variables
{
    std::array<std::optional<int16_t>, 16> local;  // the player's
    std::array<std::optional<int16_t>, 16> global; // every player's
};

// The SDK's reverb on the capture units (NNS_SndCaptureStartReverb). Channels 1 and 3 play the two halves of a buffer
// in a loop, at pans 0 and 127, while captures 0 and 1 record the mixer's left and right sides into them with the same
// timers. Each side therefore comes back one half's length later, at the channels' volume over 128 (SoundUnit's
// captures).
struct Reverb
{
    uint32_t size = 0x1000; // the buffer's bytes: two halves, each a multiple of 4 bytes
    bool pcm8 = false;      // the halves' format: PCM8, or PCM16
    int sample_rate = 16000;
    int volume = 0; // the channels' volume, 0 to 127
};

// The commands that NNS_SndCaptureStartReverb sends the driver for `reverb`, with its buffer at `buffer`. They lock
// channels 1 and 3, set up each channel and its capture, and start the four together. The channels' timer is 0xFFB0FF
// over the rate, as the SDK works it out. The SDK clears the buffer before it sends them.
snd::CommandList ReverbCommands(const Reverb& reverb, uint32_t buffer);

// The channels that a game gives some of a sequence's tracks (NNS_SndPlayerSetTrackAllocatableChannel).
struct TrackChannels
{
    uint16_t tracks = 0; // a bit for each track
    uint16_t channels = 0;
};

// What a game's code sets as it starts a sequence: the values of the variables, the channels that the archive's
// players give their sequences' tracks, other channels for some tracks, and the capture units' reverb. A game's profile
// gives them. The default plays the sequence as the archive gives it.
struct GameSettings
{
    Variables variables = {};

    // The channels of the archive's players, by the player's number, in place of the archive's. The SDK takes the
    // archive's in NNS_SndArcPlayerSetup, and a game changes them with NNS_SndPlayerSetAllocatableChannel. As with the
    // archive's, 0 leaves the tracks with the channels that the sequence's preparation gives them.
    std::map<uint32_t, uint16_t> channels = {};

    // The channels that the game gives some tracks once it has started the sequence, in this order.
    std::vector<TrackChannels> track_channels = {};

    std::optional<Reverb> reverb = {};
};

// Loads into `heap` what `sequence` of `archive` requires, and returns the commands that start it on the driver's
// player `player`. The SDK's sound archive player does the same in NNS_SndArcPlayerStartSeq and
// NNS_SndArcPlayerStartSeqArc, and in the NNS_SndMain that follows.
//
// A sequence's bank goes in first, then its file. A sequence archive's file goes in before the bank: the SDK's player
// requires it loaded already. The commands prepare the sequence, give its tracks the channels of the archive's player
// when the player names some, and set its channel priority. `settings` can give the archive's players other channels.
// Then the commands set the variables, set the player's volume to the sequence's when that isn't 127, and start the
// sequence. The track channels of `settings` come last. The reverb isn't among the commands: ReverbCommands gives it.
// Throws xsflib::FormatError when the archive lacks the sequence, its file or its bank, and std::runtime_error when the
// files don't fit in the heap.
snd::CommandList StartCommands(sdat::SoundHeap& heap, const sdat::Archive& archive, const SequenceRef& sequence,
                               const GameSettings& settings = {}, int player = 0);

// The commands that give the variables the values in `variables`: the local variables of the driver's player `player`,
// then the global variables. StartCommands gives the same commands. A host sends them while a sequence plays to change
// its variables (ArchivePlayer::Send).
snd::CommandList VariableCommands(const Variables& variables, int player = 0);

// A sequence's label: its name in the archive's SYMB block. Without a name, a sequence's label is SEQ_ and its number,
// and a sequence archive's sequence takes the archive's label, an underscore and its number. A sequence archive's label
// is its name, or SEQARC_ and its number.
std::string Label(const sdat::Archive& archive, const SequenceRef& sequence);

// The archive's sequences, then each sequence of each sequence archive. A sequence archive whose file can't be read
// gives none.
std::vector<SequenceRef> Sequences(const sdat::Archive& archive);

struct ArchivePlayerOptions
{
    snd::Build build = {};

    // Where the driver's frames lie: the system clock of the frame that takes the shared work, and the frames before
    // it, each of which steps the driver's random numbers. A player that follows an anchor takes the anchor's values.
    // One that stands alone takes 0 for both.
    uint64_t clock = 0;
    uint64_t frames = 0;

    // The state of the driver's random numbers before those frames (snd::Driver::SetRandomState).
    uint32_t random_state = 0x12345678;

    // Mixes in the enhanced mode (SoundUnit::StartEnhanced), with a driver that grows its channels in place of taking a
    // channel whose note sounds before its release (snd::Driver::GrowChannels). RunFrame(std::vector<float>&) then
    // gives each frame's samples.
    bool enhanced = false;
};

class ArchivePlayer
{
public:
    // Where the player keeps the ARM9's data in main memory: the shared work, then the files.
    static constexpr uint32_t kMainMemory = 0x02000000;
    static constexpr uint32_t kMainMemorySize = 0x400000;
    static constexpr uint32_t kSharedWork = 0x02001000;
    static constexpr uint32_t kReverbBuffer = 0x02002000;
    static constexpr uint32_t kFiles = 0x02010000;

    // A player of `archive`'s sequences, whose driver has taken the shared work. The archive must outlive the player.
    explicit ArchivePlayer(const sdat::Archive& archive, const ArchivePlayerOptions& options = {});
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;

    // Loads `sequence`'s files and starts it on the driver's player `player` at the next frame, with what a game's code
    // sets before it (see StartCommands). A reverb in `settings` starts in the same frame, before the sequence, with
    // its buffer at kReverbBuffer. A reverb that runs already stops first, as NNS_SndCaptureStartReverb stops it.
    // Throws std::invalid_argument for a reverb whose buffer doesn't fit before kFiles or whose halves aren't
    // multiples of 4 bytes.
    void Start(const SequenceRef& sequence, const GameSettings& settings = {}, int player = 0);

    // Sends the driver `list`. The driver runs it at the next frame.
    void Send(snd::CommandList list);

    // Plays the samples up to the driver's next frame, appends them to `out`, and runs the frame. Throws
    // std::logic_error in the enhanced mode.
    void RunFrame(std::vector<SoundOutput>& out);

    // Plays the enhanced mode's kEnhancedFrameSamples samples up to the driver's next frame in the same way, the left
    // and the right side in turn. Throws std::logic_error outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs the frame in the same way, without mixing the samples (SoundUnit::Run). The player ends the frame in the
    // state that RunFrame(out) leaves. The meters then give 0. A host runs such frames to reach a place in a sequence
    // quickly.
    void RunFrame();

    // Whether commands wait for the next frame, a sequence plays or a channel sounds.
    bool Busy() const;

    // The system clock of the last frame.
    uint64_t Clock() const;

    const snd::Driver& Driver() const;
    const SoundUnit& Unit() const;
    std::span<const uint8_t> MainMemory() const;

    // Called with each access of the driver to the sound unit's registers.
    std::function<void(const RegisterAccess&)> on_sound_access;

    // Called with each note that a track plays (snd::Driver::on_note).
    std::function<void(const snd::NoteEvent&)> on_note;

    // Called at the start of each tick of a driver's player (snd::Driver::on_tick).
    std::function<void(int player)> on_tick;

    // Sets the driver's hooks for a rip tool's length analysis (snd::Driver::FollowLengths).
    void FollowLengths(snd::LengthHooks hooks, std::set<snd::LoopPlace> endless_loops = {});

    // A host's mute of track `track` (0 to 15) of the driver's player `player`. The driver has no such mute. Its mute
    // (SND_CMD_MUTE_TRACK, or a sequence's 0xD7) changes what the track plays. A muted track plays on as before, and
    // none of the channels of its notes reach the output, their releases included. A channel stays with the track
    // whose note last started it, until another track's note or the ARM9's command sets it up again. A change takes
    // effect at the next frame. The mutes aren't part of a snapshot. They stay through a restore.
    void MuteTrack(int player, int track, bool mute);
    bool IsTrackMuted(int player, int track) const;

    // A host's meters of the 16 tracks of the driver's player `player`, or no meters for -1. The driver has no such
    // meters. A track's meter adds up its channels' output as the mixer adds them (SoundUnit::SetMeters). TrackPeaks
    // gives the peaks of track `track`'s meter in the last frame, left and right, on the scale of 16-bit samples (see
    // AppendPcm16 in sound.h). Without meters, it gives 0. The meters change nothing in the output. They
    // aren't part of a snapshot, and stay through a restore.
    void MeterTracks(int player);
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The player's state between frames. A snapshot shares the pages of main memory that haven't changed with the
    // snapshot before it.
    class Snapshot
    {
    private:
        friend class ArchivePlayer;

        static constexpr uint32_t kPageSize = 0x1000;
        using Page = std::array<uint8_t, kPageSize>;

        const ArchivePlayer* owner_ = nullptr;
        std::vector<std::shared_ptr<const Page>> pages_;
        std::optional<snd::Driver> driver_;
        std::optional<SoundUnit> unit_;
        std::optional<sdat::SoundHeap> heap_;
        uint64_t clock_ = 0;
        std::vector<snd::CommandList> pending_;
        std::vector<int16_t> channel_tracks_;
    };

    // Takes a snapshot between frames. `previous` is null or an earlier snapshot of this player.
    Snapshot Save(const Snapshot* previous) const;

    // Returns the player to `snapshot`. Throws std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    // The sound unit's registers for the driver.
    class UnitRegisters final : public snd::Registers
    {
    public:
        explicit UnitRegisters(ArchivePlayer& player);

        uint32_t Read(uint32_t address, int size) override;
        void Write(uint32_t address, int size, uint32_t value) override;

    private:
        ArchivePlayer& player_;
    };

    void Play(std::vector<SoundOutput>* out, std::vector<float>* enhanced_out);
    void OnNote(const snd::NoteEvent& note);

    const sdat::Archive& archive_;
    std::vector<uint8_t> memory_;
    SoundUnit unit_;
    UnitRegisters registers_;
    snd::Driver driver_;
    sdat::SoundHeap heap_;
    uint64_t clock_ = 0;
    std::vector<snd::CommandList> pending_;

    // The track whose note last started each channel: the player's number times 16 plus the track's, or -1.
    std::vector<int16_t> channel_tracks_ = std::vector<int16_t>(snd::kChannelCount, -1);

    std::array<uint16_t, snd::kPlayerCount> muted_ = {}; // each player's muted tracks
    int metered_player_ = -1;
    std::array<std::array<int32_t, 2>, snd::kTracksPerPlayer> peaks_ = {};
};

} // namespace ntdlib::ntr
