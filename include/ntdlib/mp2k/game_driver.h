// SPDX-License-Identifier: MIT

// A game's MP2K driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the init,
// the routine that starts a song, and each frame the routine that restarts the sound DMA and the driver's main
// routine. It reads what the driver leaves in memory: the mixer's output and the sound channels. It's the reference
// for ntdlib's model of the driver.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/mp2k/driver.h"

namespace ntdlib::mp2k
{

using ntdlib::agb::Rom;

// The routines of a game's driver that the game calls. The addresses are of Thumb code.
struct GameRoutines
{
    uint32_t init = 0;       // m4aSoundInit
    uint32_t song_start = 0; // m4aSongNumStart
    uint32_t main = 0;       // SoundMain: the routine that m4aSoundMain calls in the game's VBlank interrupt
    uint32_t vsync = 0;      // m4aSoundVSync: the routine that restarts the sound DMA at the buffer's end

    // Camelot's version of the driver, and 0 for the SDK's. The game's boot code copies `copy_size` bytes from
    // `copy_source` to `copy_destination` in IWRAM: Camelot's mixer among them. The game's VBlank interrupt copies the
    // DMA counter to `counter_copy` after m4aSoundVSync. After SoundMain, it calls the mixer's output stage `output`
    // with `output_argument` in r0. The output stage writes DriverInfo::camelot_output_points points for each of the
    // mixer's into the buffers at `pcm_buffer`: the right side's, then the left side's, each as long as the output
    // points of kPcmBufferSize of the mixer's points.
    uint32_t copy_source = 0;
    uint32_t copy_destination = 0;
    uint32_t copy_size = 0;
    uint32_t counter_copy = 0;
    uint32_t output = 0;
    uint32_t output_argument = 0;
    uint32_t pcm_buffer = 0;
};

// Finds the routines by their shapes. SoundMain and m4aSoundVSync open by loading kSoundInfoPointer and checking the
// SoundInfo's ident. m4aSoundVSync then counts down the SoundInfo's DMA counter. The init is the routine that calls
// m4aSoundMode with the mode that detection found, or else the routine before m4aSoundMain. A game can hold more than
// one copy of the driver: the copy nearest to the routine that starts a song counts. Returns false and sets `error`
// when a routine has no copy of its shape.
//
// Camelot's version of the driver keeps its SoundMain, its m4aSoundVSync and its mixer in the game's code. Its
// SoundMain loads the stack's limit, the ident and kSoundInfoPointer from a literal pool with adr and ldm, and jumps to
// the mixer in IWRAM with ldr r3, =mixer and bx r3. A DMA3 transfer of the boot code copies the mixer there: its
// literal pool holds the source, the destination and the control. The VBlank interrupt calls SoundMain, and then the
// output stage with mov lr, r3 and the second half of a BL alone. The game's init calls m4aSoundInit first.
bool FindGameRoutines(const Rom& rom, const DriverInfo& info, GameRoutines& routines, std::string& error);

// The offsets in the driver's structures that the reference reads.
constexpr uint32_t kSoundInfoPointer = 0x03007FF0; // the driver keeps its SoundInfo's address here
constexpr uint32_t kSoundInfoDmaCounter = 0x04;    // the frames left before the sound DMA starts the buffer again
constexpr uint32_t kSoundInfoMaxChannels = 0x06;
constexpr uint32_t kSoundInfoDmaPeriod = 0x0B;   // the frames that the buffer holds
constexpr uint32_t kSoundInfoFramePoints = 0x10; // the mixer's points a frame
constexpr uint32_t kSoundInfoCgbChannels = 0x1C; // the four PSG channels' structures
constexpr uint32_t kSoundInfoChannels = 0x50;    // the DirectSound channels' structures
constexpr uint32_t kSoundInfoPcmBuffer = 0x350;  // the right side's buffer, then the left side's
constexpr uint32_t kChannelSize = 0x40;
constexpr uint32_t kTrackSize = 0x50;

// A sound channel's structure as the driver leaves it after a frame: kChannelSize bytes.
struct GameChannel
{
    uint8_t status = 0;
    uint8_t type = 0;
    uint8_t right = 0; // the channel's volumes
    uint8_t left = 0;
    uint8_t key = 0;
    uint8_t envelope = 0;
    uint8_t goal = 0;    // a PSG channel's envelope goal
    uint8_t counter = 0; // a PSG channel's envelope counter
    uint8_t midi_key = 0;
    uint8_t velocity = 0;
    uint8_t priority = 0;
    uint8_t sustain_goal = 0; // a PSG channel's
    uint8_t pan = 0;          // a PSG channel's sides, in the sound control register's form
    int32_t count = 0;        // a DirectSound channel's points left before its sample's end
    uint32_t fraction = 0;    // its position between two points, in 2^-23 points
    uint32_t frequency = 0;
    uint32_t wave = 0;
    uint32_t position = 0;
    uint32_t track = 0; // the address of the track that plays the channel, or 0
};

class GameDriver
{
public:
    // Loads the cartridge and runs the init. `info` gives the song table and the music players. Throws
    // std::runtime_error when the init fails or leaves no SoundInfo.
    GameDriver(const Rom& rom, const DriverInfo& info, const GameRoutines& routines);

    // Starts `song` on its music player. The song plays from the next frame. Throws std::runtime_error when the song or
    // its player is past its table, or the routine fails.
    void Start(int song);

    // Whether the song's player has tracks that haven't ended, or any channel still sounds.
    bool Playing();

    // The number of the track at `address` among the song's player's tracks, or -1.
    int TrackOf(uint32_t address) const;

    // Runs a frame as the game's interrupts do: the routine that restarts the sound DMA, then the main routine, with
    // the vertical count at the start of the vertical blank. Camelot's driver also gets its copy of the DMA counter and
    // its output stage. Puts the mixer's output for the frame in `right` and `left`, a signed 8-bit point for each of
    // the frame's points, or DriverInfo::camelot_output_points for each in Camelot's driver. The mono mixer's left side
    // is its right side.
    void Frame(std::vector<int8_t>& right, std::vector<int8_t>& left);

    // The last frame's stores to the sound registers before the FIFOs, in order: the PSG's registers, the sound control
    // registers and the wave RAM, from agb::kRegSound1CntL up to agb::kRegFifoA.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // Whether the init set up the mono mixer: one output, sent to both sides.
    bool Mono() const;

    // The rate of the DMA's points in Hz, from the timer that the init started. Camelot's driver can play two points
    // for each of its mixer's.
    double Rate();

    // The DirectSound channels up to the driver's limit, then the four PSG channels. Each holds what the last frame
    // left in it.
    std::vector<GameChannel> Channels();

    agb::System& Gba();

    // The address of the driver's SoundInfo.
    uint32_t SoundInfo() const;

private:
    GameChannel ReadChannel(uint32_t address);

    const Rom& rom_;
    DriverInfo info_;
    GameRoutines routines_;
    agb::System system_;
    uint32_t sound_info_ = 0;
    bool mono_ = false;
    bool recording_ = false; // Frame() records the stores to the sound registers
    std::vector<agb::RegisterWrite> sound_writes_;
    uint32_t player_ = 0; // the song's music player, and its tracks
    uint32_t tracks_ = 0;
    int track_count_ = 0;
};

} // namespace ntdlib::mp2k
