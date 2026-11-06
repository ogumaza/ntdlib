// SPDX-License-Identifier: MIT

// Tests of the model of the DS's sound driver (ntdlib/ntr/snd/driver.h), on a sequence, a bank and a wave archive laid
// out in main memory as the ARM9 lays them out. The expected values follow the driver's rules, worked out by hand.

#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"

namespace
{

namespace snd = ntdlib::ntr::snd;
using ntdlib::ntr::SoundOutput;
using ntdlib::ntr::SoundUnit;

constexpr uint32_t kMemory = 0x02000000;
constexpr uint32_t kSharedWork = kMemory + 0x100;
constexpr uint32_t kSequence = kMemory + 0x1000;
constexpr uint32_t kBank = kMemory + 0x2000;
constexpr uint32_t kWaveArchive = kMemory + 0x3000;
constexpr uint32_t kWave = kWaveArchive + 0x40;
constexpr uint16_t kWaveTimer = 0x200;

struct Write
{
    friend bool operator==(const Write&, const Write&) = default;

    uint32_t address = 0;
    int size = 0;
    uint32_t value = 0;
};

// The sound unit's registers, with a log of the writes.
class LoggedRegisters : public snd::Registers
{
public:
    explicit LoggedRegisters(SoundUnit& unit) : unit_(unit)
    {
    }

    uint32_t Read(uint32_t address, int size) override
    {
        return unit_.Read(address, size);
    }

    void Write(uint32_t address, int size, uint32_t value) override
    {
        writes.push_back({address, size, value});
        unit_.Write(address, size, value);
    }

    std::vector<::Write> writes;

private:
    SoundUnit& unit_;
};

void Put16(std::vector<uint8_t>& memory, uint32_t address, uint16_t value)
{
    memory[address - kMemory] = static_cast<uint8_t>(value);
    memory[address - kMemory + 1] = static_cast<uint8_t>(value >> 8);
}

void Put32(std::vector<uint8_t>& memory, uint32_t address, uint32_t value)
{
    Put16(memory, address, static_cast<uint16_t>(value));
    Put16(memory, address + 2, static_cast<uint16_t>(value >> 16));
}

// Main memory with a sequence, a bank of one PCM instrument (program 0, root key 60, attack, decay, sustain and release
// 127, centre pan) and a wave archive of one PCM16 wave of 8 samples, one-shot.
std::vector<uint8_t> Memory(const std::vector<uint8_t>& sequence)
{
    std::vector<uint8_t> memory(0x10000);
    for (std::size_t i = 0; i < sequence.size(); i++)
    {
        memory[kSequence - kMemory + i] = sequence[i];
    }

    Put32(memory, kBank + 0x18, kWaveArchive);
    Put32(memory, kBank + 0x38, 1);
    Put32(memory, kBank + 0x3C, 0x40 << 8 | 1);
    const std::vector<uint8_t> instrument = {0, 0, 0, 0, 60, 127, 127, 127, 127, 64};
    for (std::size_t i = 0; i < instrument.size(); i++)
    {
        memory[kBank + 0x40 - kMemory + i] = instrument[i];
    }

    Put32(memory, kWaveArchive + 0x38, 1);
    Put32(memory, kWaveArchive + 0x3C, kWave - kWaveArchive);
    memory[kWave - kMemory] = 1; // PCM16
    memory[kWave - kMemory + 1] = 0;
    Put16(memory, kWave + 4, kWaveTimer);
    Put32(memory, kWave + 8, 4);
    for (uint32_t i = 0; i < 8; i++)
    {
        Put16(memory, kWave + 12 + 2 * i, 0x1000);
    }

    return memory;
}

struct Driver
{
    explicit Driver(const std::vector<uint8_t>& sequence)
        : memory(Memory(sequence)), unit(memory), registers(unit), driver(memory, registers)
    {
        unit.Write(0x04000504, 4, 0x200);
        driver.Start();
    }

    // A frame of the alarm, and the sound unit's samples to the next frame.
    void Frame(std::vector<snd::CommandList> lists = {})
    {
        driver.Frame(true, lists);
        std::vector<SoundOutput> out;
        unit.Run(snd::kFrameClocks / 2, out);
    }

    std::vector<uint8_t> memory;
    SoundUnit unit;
    LoggedRegisters registers;
    snd::Driver driver;
};

snd::CommandList StartSequence()
{
    return {{snd::CommandId::kSetSharedWork, {kSharedWork, 0, 0, 0}},
            {snd::CommandId::kStartSequence, {0, kSequence, 0, kBank}}};
}

void TestStart()
{
    Driver driver({0xFF});

    // SND_Enable sets SOUNDCNT's master enable, the output selector keeps it, and the master volume is 127.
    XSFLIB_CHECK(driver.registers.writes ==
                 (std::vector<Write>{{0x04000501, 1, 0x80}, {0x04000501, 1, 0x80}, {0x04000500, 1, 0x7F}}));
}

void TestNote()
{
    // Key 60 at velocity 127 for 48 ticks, then the end.
    Driver driver({0x3C, 0x7F, 0x30, 0xFF});
    driver.registers.writes.clear();

    driver.Frame({StartSequence()});

    // The first tick plays the note. The channel order starts at channel 4. The channel starts in the next frame.
    XSFLIB_CHECK(driver.registers.writes.empty());
    XSFLIB_CHECK(driver.driver.GetChannel(4).active);
    XSFLIB_CHECK(driver.driver.GetPlayer(0).active);
    XSFLIB_CHECK(driver.driver.GetTrack(0).wait == 48);

    driver.Frame();

    // The allocation marks the channel to stop first: the frame clears SOUNDxCNT's busy bit. The attack of 127 reaches
    // full level in one step. The volume is 127, the pan 64, and the timer the wave's at the root key. The wave is
    // one-shot: repeat mode 2.
    const std::vector<Write> expected = {
        {0x04000440, 4, 0},
        {0x04000440, 4, 1u << 29 | 2u << 27 | 64u << 16 | 127},
        {0x04000448, 2, 0x10000 - kWaveTimer},
        {0x0400044A, 2, 0},
        {0x0400044C, 4, 4},
        {0x04000444, 4, kWave + 12},
        {0x04000443, 1, 0xB0},
    };
    XSFLIB_CHECK(driver.registers.writes == expected);
}

void TestOneShotEnd()
{
    Driver driver({0x3C, 0x7F, 0x30, 0xFF});
    driver.Frame({StartSequence()});
    driver.Frame();
    XSFLIB_CHECK(driver.driver.GetTrack(0).channels == 4);

    // The wave's 8 samples end within the frame. The next frame finds the channel stopped and frees it.
    driver.Frame();

    XSFLIB_CHECK(!driver.unit.Busy(4));
    XSFLIB_CHECK(!driver.driver.GetChannel(4).active);
    XSFLIB_CHECK(driver.driver.GetChannel(4).owner == -1);
    XSFLIB_CHECK(driver.driver.GetTrack(0).channels == -1);
}

void TestTempo()
{
    Driver driver({0x3C, 0x7F, 0x30, 0xFF});
    driver.Frame({StartSequence()});

    // At 120 beats a minute, the counter of 240 gains 120 a frame: a tick each second frame. The note's wait of 48 ends
    // at the 49th tick, in frame 97. The end stops the player there.
    for (int frame = 2; frame <= 96; frame++)
    {
        driver.Frame();
    }
    XSFLIB_CHECK(driver.driver.GetPlayer(0).active);
    XSFLIB_CHECK(driver.driver.GetTrack(0).wait == 1);

    driver.Frame();

    XSFLIB_CHECK(!driver.driver.GetPlayer(0).active);
    XSFLIB_CHECK(!driver.driver.GetTrack(0).active);
    // The shared work counts the ticks that the player played: 48. The tick that ends it doesn't count.
    XSFLIB_CHECK(driver.memory[kSharedWork + 0x20 + 0x20 - kMemory] == 48);
}

void TestVariables()
{
    // Variable 0 = 5, variable 0 += 3, then compare variable 0 with 8, and set variable 1 to 7 only if equal, and
    // variable 2 to 9 only if not. Global variable 16 = -2.
    Driver driver({0xB0, 0x00, 0x05, 0x00, 0xB1, 0x00, 0x03, 0x00, 0xB8, 0x00, 0x08, 0x00, 0xA2, 0xB0, 0x01, 0x07,
                   0x00, 0xB8, 0x00, 0x09, 0x00, 0xA2, 0xB0, 0x02, 0x09, 0x00, 0xB0, 0x10, 0xFE, 0xFF, 0xFF});

    driver.Frame({StartSequence()});

    const auto variable = [&driver](uint32_t offset)
    {
        const uint32_t at = kSharedWork + offset - kMemory;
        return static_cast<int16_t>(driver.memory[at] | driver.memory[at + 1] << 8);
    };
    XSFLIB_CHECK(variable(0x20) == 8);
    XSFLIB_CHECK(variable(0x22) == 7);
    XSFLIB_CHECK(variable(0x24) == -1);
    XSFLIB_CHECK(variable(0x260) == -2);
    // Each command list counts in the shared work.
    XSFLIB_CHECK(driver.memory[kSharedWork - kMemory] == 1);
}

void TestRandom()
{
    // A wait of a random 0 to 9 ticks: the random number's high 16 bits times 10, over 65536. The frame that starts
    // the sequence draws the first number. The seed is 0x12345678, and each step multiplies by 1664525 and adds
    // 1013904223: 0x75432777 (mod 2^32), whose high half is 0x7543 = 30019. 30019 * 10 / 65536 = 4.58: a wait of 4.
    Driver driver({0xA0, 0x80, 0x00, 0x00, 0x09, 0x00, 0xFF});

    driver.Frame({StartSequence()});

    XSFLIB_CHECK(driver.driver.GetTrack(0).wait == 4);
}

void TestGrownChannels()
{
    // Poly mode, then 17 notes in one tick, each for 48 ticks. The console's driver has 16 channels: the 17th note
    // takes one of them from a note of the same priority. A driver that grows its channels gives it channel 16
    // instead, a twin of the channel that it would have taken, with registers past the console's.
    std::vector<uint8_t> sequence = {0xC7, 0x00};
    for (uint8_t key = 60; key < 77; key++)
    {
        sequence.insert(sequence.end(), {key, 0x7F, 0x30});
    }
    sequence.insert(sequence.end(), {0x80, 0x30, 0xFF});

    for (const bool growing : {false, true})
    {
        Driver driver(sequence);
        driver.driver.GrowChannels(growing);
        driver.Frame({StartSequence()});

        int notes = 0;
        for (int channel = driver.driver.GetTrack(0).channels; channel >= 0;
             channel = driver.driver.GetChannel(channel).next)
        {
            notes++;
        }
        XSFLIB_CHECK(notes == (growing ? 17 : 16));
        XSFLIB_CHECK(driver.driver.ChannelCount() == (growing ? 17 : 16));
        if (!growing)
        {
            continue;
        }

        const snd::Channel& grown = driver.driver.GetChannel(16);
        XSFLIB_CHECK(grown.active);
        XSFLIB_CHECK(grown.id == 16);
        XSFLIB_CHECK(grown.twin < 16);
        XSFLIB_CHECK(driver.unit.Read(ntdlib::ntr::kGrownChannelRegisters + ntdlib::ntr::kGrownChannelTwin, 4) ==
                     grown.twin);

        driver.registers.writes.clear();
        driver.Frame();
        XSFLIB_CHECK(driver.unit.ChannelCount() == 17);
        bool started = false;
        for (const Write& write : driver.registers.writes)
        {
            started = started || (write.address == ntdlib::ntr::kGrownChannelRegisters + 3 && (write.value & 0x80));
        }
        XSFLIB_CHECK(started);
    }
}

void TestReleaseGivesWay()
{
    // Poly mode, a release of 32, then 16 notes of a tick each and, two ticks later, a 17th note. The wave loops, and
    // each note's release lasts about 1,400 frames: 92,544 steps down to silence at 65 a frame. On the console, the
    // 17th note takes a channel from a note in its release. A driver that grows its channels does the same: it grows
    // none.
    std::vector<uint8_t> sequence = {0xC7, 0x00, 0xD3, 0x20};
    for (uint8_t key = 60; key < 76; key++)
    {
        sequence.insert(sequence.end(), {key, 0x7F, 0x01});
    }
    sequence.insert(sequence.end(), {0x80, 0x02, 77, 0x7F, 0x30, 0x80, 0x30, 0xFF});

    for (const bool growing : {false, true})
    {
        Driver driver(sequence);
        driver.memory[kWave - kMemory + 1] = 1; // the wave's loop
        driver.driver.GrowChannels(growing);
        driver.Frame({StartSequence()});
        for (int frame = 0; frame < 8; frame++)
        {
            driver.Frame();
        }

        XSFLIB_CHECK(driver.driver.ChannelCount() == 16);
        int released = 0;
        int played = 0;
        for (int channel = 0; channel < 16; channel++)
        {
            const snd::Channel& c = driver.driver.GetChannel(channel);
            released += c.active && c.envelope_stage == 3 ? 1 : 0;
            played += c.active && c.key == 77 && c.envelope_stage != 3 ? 1 : 0;
        }
        XSFLIB_CHECK(released == 15);
        XSFLIB_CHECK(played == 1);
    }
}

} // namespace

int main()
{
    TestStart();
    TestNote();
    TestOneShotEnd();
    TestTempo();
    TestVariables();
    TestRandom();
    TestGrownChannels();
    TestReleaseGivesWay();

    return xsflib::testing::Summary("DS sound driver");
}
