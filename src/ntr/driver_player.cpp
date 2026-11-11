// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/driver_player.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr
{

namespace
{

constexpr uint32_t kMainMemory = 0x02000000;

// The ARM7's part of main memory starts at its binary's address when the binary lies in main memory. Otherwise the
// ARM7 keeps its data from 0x02380000 on.
constexpr uint32_t kArm7MainMemory = 0x02380000;

// The IPC's tags that the start-up and the player use, and the word of the ARM7's tags whose handlers are set.
constexpr uint32_t kTagSound = 7;
constexpr uint32_t kTagCartridgeSlot = 13;
constexpr uint32_t kArm7ReadyTags = 0x027FFF8C;

// The ARM9's message to the ARM7's GBA slot library at start-up: subcommand 1, with the address of a copy of the slot's
// header in units of 32 bytes from main memory's start. The ARM7 checks the logo in it. The player gives it a header of
// zeros. An empty slot gives the same.
constexpr uint32_t kSlotHeader = 0x02000000;
constexpr uint32_t kModuleInfo = 1;

constexpr std::size_t kCommandSize = 24;
constexpr uint32_t kListSlotSize = 0x800;
constexpr uint32_t kListSlots = (DriverPlayer::kFiles - DriverPlayer::kCommandLists) / kListSlotSize;

constexpr uint64_t kStartLimit = 2 * kSystemClockHz;

// The driver's thread starts its alarm after its writes at the start, and the alarm's first frame comes 0x10000 of the
// OS's ticks later. A tick is 64 cycles.
constexpr uint64_t kFirstFrameDelay = 0x10000 * 64;

uint32_t PxiWord(uint32_t tag, uint32_t data)
{
    return tag | data << 6;
}

} // namespace

DriverPlayer::DriverPlayer(const Rom& rom)
    : system_(rom),
      files_end_(rom.Header().arm7.ram_address >> 24 == 0x02 ? std::min(rom.Header().arm7.ram_address, kArm7MainMemory)
                                                             : kArm7MainMemory)
{
    system_.on_sound_access = [this](uint32_t address, int size, uint32_t value, bool write)
    {
        accesses_.push_back({address, size, value, write});
        clocks_.push_back(system_.Clock());
    };

    // Run until the driver's handler of the sound tag is set and the ARM7 idles.
    const auto ready = [this]
    {
        const std::span<const uint8_t> memory = system_.MainMemory();
        const uint32_t at = (kArm7ReadyTags - kMainMemory) & static_cast<uint32_t>(memory.size() - 1);
        return xsflib::LoadLe32(&memory[at]);
    };
    std::vector<SoundOutput> out;
    bool slot_answered = false;
    while ((ready() & 1u << kTagSound) == 0 || !system_.Halted())
    {
        if (system_.Clock() >= kStartLimit)
        {
            throw std::runtime_error("the ARM7 binary doesn't start its sound driver");
        }

        system_.Run(system_.Clock() + snd::kFrameClocks, out);
        if (!slot_answered && (ready() & 1u << kTagCartridgeSlot) != 0)
        {
            slot_answered = true;
            system_.SendToArm7(PxiWord(kTagCartridgeSlot, kModuleInfo | (kSlotHeader - kMainMemory) / 32 << 6));
        }
        system_.TakeFromArm7();
    }

    // The player sets the shared work up as the ARM9's SND_Init does. The frame that takes it reads the channels'
    // state: its clock gives the frames' times. The frames before it count from the driver's writes at the start.
    if (clocks_.empty())
    {
        throw std::runtime_error("the ARM7's sound driver doesn't start the sound unit");
    }
    const uint64_t start = clocks_.front();
    accesses_.clear();
    clocks_.clear();
    snd::InitSharedWork(system_.MainMemory().subspan(kSharedWork - kMainMemory, snd::kSharedWorkSize));
    Send({{snd::CommandId::kSetSharedWork, {kSharedWork, 0, 0, 0}}});
    while (accesses_.empty())
    {
        if (system_.Clock() >= kStartLimit + 4 * snd::kFrameClocks)
        {
            throw std::runtime_error("the ARM7's sound driver doesn't take the shared work");
        }
        system_.Run(system_.Clock() + snd::kFrameClocks, out);
        system_.TakeFromArm7();
    }
    next_frame_ = clocks_.front() + snd::kFrameClocks;
    frames_ = (clocks_.front() - start - kFirstFrameDelay + snd::kFrameClocks / 2) / snd::kFrameClocks + 1;
    accesses_.clear();
    clocks_.clear();
}

Arm7System& DriverPlayer::System()
{
    return system_;
}

uint32_t DriverPlayer::FilesEnd() const
{
    return files_end_;
}

void DriverPlayer::Send(const snd::CommandList& list)
{
    if (list.empty())
    {
        return;
    }
    if (list.size() * kCommandSize > kListSlotSize)
    {
        throw std::runtime_error("a list of commands is too long for its place in main memory");
    }

    // The lists take places in turn. The driver takes each before the player sends kListSlots more.
    const uint32_t base = kCommandLists + next_list_ * kListSlotSize;
    next_list_ = (next_list_ + 1) % kListSlots;
    const std::span<uint8_t> memory = system_.MainMemory();
    const auto put32 = [&memory](uint32_t address, uint32_t value)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            memory[address - kMainMemory + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    for (std::size_t i = 0; i < list.size(); i++)
    {
        const auto at = static_cast<uint32_t>(base + i * kCommandSize);
        put32(at, i + 1 < list.size() ? static_cast<uint32_t>(at + kCommandSize) : 0);
        put32(at + 4, static_cast<uint32_t>(list[i].id));
        for (uint32_t arg = 0; arg < 4; arg++)
        {
            put32(at + 8 + 4 * arg, list[i].args[arg]);
        }
    }
    system_.SendToArm7(PxiWord(kTagSound, base));
}

DriverFrame DriverPlayer::NextFrame(std::vector<SoundOutput>& out)
{
    system_.Run(next_frame_ + 1, out);
    system_.TakeFromArm7();

    if (accesses_.empty())
    {
        throw std::runtime_error("the driver's frame didn't come at its time");
    }

    DriverFrame frame{next_frame_, {}};
    for (std::size_t i = 0; i < accesses_.size(); i++)
    {
        if (clocks_[i] != next_frame_)
        {
            throw std::runtime_error("the sound unit's registers were accessed outside the driver's frames");
        }
    }
    frame.accesses = std::move(accesses_);
    accesses_.clear();
    clocks_.clear();
    next_frame_ += snd::kFrameClocks;
    frames_++;
    return frame;
}

uint64_t DriverPlayer::Frames() const
{
    return frames_;
}

uint64_t DriverPlayer::Clock() const
{
    return next_frame_ - snd::kFrameClocks;
}

} // namespace ntdlib::ntr
