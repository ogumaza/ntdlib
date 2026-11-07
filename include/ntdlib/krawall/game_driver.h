// SPDX-License-Identifier: MIT

// A game's build of Krawall, run on the GBA's CPU (agb::System). It copies the library's code and data to RAM as the
// game's start-up code does, and makes the calls of the game's start-up code: the game's sound init, or the library's
// set-up routines. It starts a piece as the game does, and each frame runs the game's routine for the frame, if it has
// one, and the mixer's worker. It reads what the library leaves in memory: the records of the player, its channels and
// the mixer's channels, the mixer's variables and the frame's output. It's the reference for ntdlib's model of Krawall
// (krawall::Player).
//
// The worker asks getDmaAddress() for the buffers to mix into and the samples to mix. The game's routine works that
// out from the DMA's position in a double buffer. A stand-in for it hands the worker two buffers in EWRAM and a frame's
// samples at each call. The video memory isn't modelled: a build that keeps code there doesn't run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/krawall/driver.h"

namespace ntdlib::krawall
{

// The buffers that the stand-in for getDmaAddress() hands the worker, left and right.
constexpr uint32_t kGameLeftBuffer = 0x02030000;
constexpr uint32_t kGameRightBuffer = 0x02031000;

// The routines and the RAM of a game's build of Krawall.
struct GameRoutines
{
    // Bytes that the start-up code copies from the ROM to RAM.
    struct Copy
    {
        uint32_t source = 0;
        uint32_t size = 0;
        uint32_t destination = 0;
    };

    // A call of a routine with up to 4 arguments. Thumb code unless `arm`.
    struct Call
    {
        uint32_t routine = 0;
        std::vector<uint32_t> args;
        bool arm = false;
    };

    // A store of `size` bytes.
    struct Store
    {
        uint32_t address = 0;
        uint32_t value = 0;
        int size = 1;
    };

    std::vector<Copy> copies;  // what the start-up code copies, in order
    std::vector<Store> stores; // what the game stores before the first call: its settings and its heap's pointer
    std::vector<Call> init;    // the calls of the start-up code, in order

    // How the game starts a piece. krapPlay(module, mode, song) and krapSetMusicVol(volume, 0), and what the game's
    // routine that calls krapPlay() stores after it. A game that loads its modules from music files plays one with its
    // routine at `play_file`, (file, loop), and keeps the address of the file's unpacked data at `unpacked`. A game
    // that plays entry n of its table of modules does so with its routine at `play_entry`, (n).
    uint32_t play = 0;
    uint32_t set_music_vol = 0;
    std::vector<Store> after_play;
    uint32_t play_file = 0;
    uint32_t unpacked = 0;
    uint32_t play_entry = 0;
    uint32_t module_table = 0;

    // What runs each frame: a routine of the game's or 0, then the worker, in ARM, with the values that it reads stored
    // first. getDmaAddress() (Thumb, 4-byte aligned) or 0 if the worker has none, and the buffers that the worker mixes
    // into then.
    uint32_t frame_routine = 0;
    uint32_t worker = 0;
    std::vector<Store> worker_state;
    uint32_t dma_address = 0;
    uint32_t out_left = kGameLeftBuffer;
    uint32_t out_right = kGameRightBuffer;
    int frame_samples = 276;
    bool vram = false; // the start-up code copies code to the video memory: the game can't run

    // The records: the player's, which its channels follow, and the mixer's channels. The mixer's variables: the
    // master volumes of the music and the sound effects with hqMode and hqRamp, the timer's step and count, the music
    // volume and the count of plays.
    uint32_t player = 0;
    uint32_t mixer = 0;
    uint32_t master = 0;
    uint32_t timer = 0;
    uint32_t music_volume = 0;
    uint32_t plays = 0;
};

class GameDriver
{
public:
    // Loads the cartridge, copies the library to RAM and makes the start-up code's calls. Throws std::invalid_argument
    // for a game that keeps code in the video memory, and std::runtime_error when a routine fails.
    GameDriver(const agb::Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts `song`, a piece of `info.songs`, as the game does. Returns the address of the module that plays: in a game
    // that loads its modules from music files, the address of the module in the file's unpacked data.
    uint32_t Play(const DriverInfo& info, const ModuleSong& song);

    // Runs the game's routine for the frame and the worker.
    void Frame();

    // Runs the game's routine for the frame alone, as in a frame in which getDmaAddress() hands the worker no buffer:
    // the worker then returns at once.
    void SkipFrame();

    // The frame's output, as signed 8-bit points.
    std::vector<int8_t> Left();
    std::vector<int8_t> Right();

    // Returns true while the player plays.
    bool Playing();

    // Returns `size` bytes from `address`.
    std::vector<uint8_t> Bytes(uint32_t address, uint32_t size);

    // Moves each word of `record` at `offsets` that points into the unpacked data of the music file that the last
    // Play() loaded, to the address of the same byte in ntdlib's copy of it (DriverInfo::unpacked). Leaves the record
    // alone in a game without music files.
    void Translate(std::vector<uint8_t>& record, std::span<const uint32_t> offsets) const;

    const GameRoutines& Routines() const
    {
        return routines_;
    }

    agb::System& Gba();

private:
    uint32_t Call(const GameRoutines::Call& call);

    GameRoutines routines_;
    agb::System system_;
    uint32_t unpacked_low_ = 0;
    uint32_t unpacked_high_ = 0;
    uint32_t translation_ = 0;
};

} // namespace ntdlib::krawall
