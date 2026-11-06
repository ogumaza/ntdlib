// SPDX-License-Identifier: MIT

// Plays sounds on a cartridge's sound driver. The cartridge's ARM7 binary runs on Arm7System, with the sound driver
// (SND) that the game's ARM7 runs. The player takes the ARM9's part: it answers the ARM7's start-up, loads files into
// main memory and sends the driver lists of commands. The ARM9's sound library does the same.
//
// The ARM9's sound archive library loads the files from kFiles on. sdat::SoundHeap lays them out as it does.
//
// The driver runs a frame every snd::kFrameClocks cycles, on an alarm. The system's CPU is infinitely fast: each frame
// runs at one clock. The player sets up the shared work first, and each frame reads the channels' state into it. Every
// frame therefore accesses the sound unit's registers.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr
{

// A frame of the driver: its clock, and its accesses in their order.
struct DriverFrame
{
    uint64_t clock = 0;
    std::vector<RegisterAccess> accesses;
};

class DriverPlayer
{
public:
    // Where the player puts the ARM9's data in main memory: the shared work, the command lists, then the files.
    static constexpr uint32_t kSharedWork = 0x02001000;
    static constexpr uint32_t kCommandLists = 0x02002000;
    static constexpr uint32_t kFiles = 0x02010000;

    // Boots `rom`'s ARM7 binary until its sound driver waits for commands, and gives the driver the shared work. The
    // first frame of the driver that takes it is past at the return. Throws std::runtime_error when the binary doesn't
    // start its driver within 2 seconds.
    explicit DriverPlayer(const Rom& rom);

    Arm7System& System();

    // The end of the ARM9's part of main memory: the files go from kFiles up to it. The ARM7's part follows.
    uint32_t FilesEnd() const;

    // Writes `list` into main memory and sends it to the driver. The driver takes it at its next frame.
    void Send(const snd::CommandList& list);

    // Runs the system to the driver's next frame, and returns the frame. Appends the sound unit's output samples to
    // `out`.
    DriverFrame NextFrame(std::vector<SoundOutput>& out);

    // The driver's frames so far, the frames before the shared work included. The driver's random numbers take a step
    // in each frame.
    uint64_t Frames() const;

    // The system clock of the driver's last frame.
    uint64_t Clock() const;

private:
    Arm7System system_;
    uint32_t files_end_;
    uint32_t next_list_ = 0;
    uint64_t next_frame_ = 0;
    uint64_t frames_ = 0;
    std::vector<RegisterAccess> accesses_;
    std::vector<uint64_t> clocks_;
};

} // namespace ntdlib::ntr
