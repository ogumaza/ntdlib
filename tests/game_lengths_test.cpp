// SPDX-License-Identifier: MIT

// Tests of the length analysis of a JAudio2 or JAudio1 sound in game mode (jaudio/game_lengths.h), on a made-up
// machine: a CPU whose watched functions the test runs, words and bytes of memory, and an output that the test fills.

#include "ntdlib/jaudio/game_lengths.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

namespace jaudio = ntdlib::jaudio;

namespace
{

// The driver's word for the sound's JAISound, the JAISound, its vtable and its root track.
constexpr uint32_t kHandleAddress = 0x81700020;
constexpr uint32_t kHandle = 0x80400000;
constexpr uint32_t kSequenceVtable = 0x80500000;
constexpr uint32_t kTrackOffset = 0x10;
constexpr uint32_t kTrack = kHandle + kTrackOffset;
constexpr uint32_t kTrackData = 0x80600000;

// The parser's functions, the place where it takes a conditional jump, a wave lookup and the table of wave banks.
constexpr uint32_t kParse = 0x80010000;
constexpr uint32_t kJump = 0x80010100;
constexpr uint32_t kConditionalJump = 0x80010200;
constexpr uint32_t kWaveLookup = 0x80020000;
constexpr uint32_t kWaveBankTable = 0x80700000;

// Commands by their first byte.
constexpr uint8_t kNote = 0x3c;
constexpr uint8_t kWait = 0xf0;

// A CPU that only records the watch, with the registers that the watch reads.
struct FakeCpu
{
    void Watch(std::vector<uint32_t> pcs, std::function<void(uint32_t pc)> callback)
    {
        watched = std::move(pcs);
        on_watch = std::move(callback);
    }

    struct
    {
        uint32_t gpr[32] = {};
    } state_;

    std::vector<uint32_t> watched;
    std::function<void(uint32_t pc)> on_watch;
};

// A machine whose output and CPU the test drives: before each block, `step` gets the block's number and fills it.
class FakeMachine
{
public:
    std::size_t Render(int16_t* out, std::size_t frames)
    {
        std::fill(out, out + 2 * frames, int16_t{0});
        step(*this, blocks_++, out, frames);

        return frames;
    }

    uint32_t Word(uint32_t address) const
    {
        const auto it = words.find(address);

        return it == words.end() ? 0 : it->second;
    }

    std::optional<uint8_t> Byte(uint32_t address) const
    {
        const auto it = bytes.find(address);

        return it == bytes.end() ? std::nullopt : std::optional<uint8_t>(it->second);
    }

    FakeCpu& Cpu()
    {
        return cpu;
    }

    // The game has started the sound: a sequence with the root track at kTrack.
    void StartSound()
    {
        words[kHandleAddress] = kHandle;
        words[kHandle] = kSequenceVtable;
        words[kTrack] = kTrackData;
    }

    // The root track runs the command at `at`.
    void Command(uint32_t at, uint8_t command)
    {
        words[kTrack + 4] = kTrackData + at;
        bytes[kTrackData + at] = command;
        cpu.state_.gpr[4] = kTrack;
        cpu.on_watch(kParse);
    }

    // The root track jumps from the command that ends at `at` to `to`.
    void Jump(uint32_t at, uint32_t to)
    {
        words[kTrack + 4] = kTrackData + at;
        words[0x80800000] = to;
        cpu.state_.gpr[4] = kTrack;
        cpu.state_.gpr[5] = 0x80800000;
        cpu.on_watch(kJump);
    }

    // The root track takes a conditional jump from the command that ends at `at` to `to`. The jump's arguments are the
    // condition and the target. The plain jump's registers hold no track and no target. The CPU calls the watch only if
    // the analysis watches the place.
    void ConditionalJump(uint32_t at, uint32_t to)
    {
        words[kTrack + 4] = kTrackData + at;
        words[0x80800100] = 1;
        words[0x80800104] = to;
        cpu.state_.gpr[4] = 0;
        cpu.state_.gpr[5] = 0;
        cpu.state_.gpr[30] = kTrack;
        cpu.state_.gpr[31] = 0x80800100;
        if (std::find(cpu.watched.begin(), cpu.watched.end(), kConditionalJump) != cpu.watched.end())
        {
            cpu.on_watch(kConditionalJump);
        }
    }

    FakeCpu cpu;
    std::map<uint32_t, uint32_t> words;
    std::map<uint32_t, uint8_t> bytes;
    std::function<void(FakeMachine&, uint64_t block, int16_t* out, std::size_t frames)> step;

private:
    uint64_t blocks_ = 0;
};

jaudio::GameWatch Watch()
{
    jaudio::GameWatch watch;
    watch.parse_command = kParse;
    watch.jump_handler = kJump;
    watch.sequence_vtable = kSequenceVtable;
    watch.sequence_track_offset = kTrackOffset;
    watch.wave_handle_functions[0] = kWaveLookup;
    watch.wave_bank_table = kWaveBankTable;
    watch.wave_bank_table_offset = 0xf4;

    return watch;
}

const jaudio::GameLengthRules kRules{.sample_rate = 32000, .block_frames = 32};

// A block of output with sound in every frame.
void Sound(int16_t* out, std::size_t frames)
{
    for (std::size_t i = 0; i < frames; i++)
    {
        out[2 * i] = 1000;
    }
}

void TestRelease()
{
    // The sound plays for 100 blocks of 1 ms, then the game releases it. It ends 0.5 s after its last sound.
    FakeMachine machine;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        if (block == 0)
        {
            m.StartSound();
        }
        if (block < 100)
        {
            Sound(out, frames);
        }
        if (block == 100)
        {
            m.words[kHandleAddress] = 0;
        }
    };

    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, Watch(), kRules);

    XSFLIB_CHECK(timing && timing->length_ms == 100 + 500 && timing->fade_ms == 0 && !timing->silent);

    // The wave lookup is watched only for wave discovery.
    XSFLIB_CHECK(machine.cpu.watched == (std::vector<uint32_t>{kParse, kJump}));
}

void TestSilent()
{
    // A sound that the game releases without a sound is silent.
    FakeMachine machine;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t*, std::size_t)
    {
        if (block == 0)
        {
            m.StartSound();
        }
        if (block == 10)
        {
            m.words[kHandleAddress] = 0;
        }
    };

    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, Watch(), kRules);

    XSFLIB_CHECK(timing && timing->silent && timing->length_ms == 500);
}

void TestLoop()
{
    // A note and a wait, then the jump back to the note, twice. The second jump back is the loop's point.
    FakeMachine machine;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        Sound(out, frames);
        if (block == 0)
        {
            m.StartSound();
            return;
        }

        const uint64_t pass_block = (block - 1) % 3;
        if (block > 6)
        {
            return;
        }

        if (pass_block == 0)
        {
            m.Command(0x10, kNote);
        }
        else if (pass_block == 1)
        {
            m.Command(0x12, kWait);
        }
        else
        {
            m.Jump(0x18, 0x10);
        }
    };

    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, Watch(), kRules);

    // The second jump runs in block 6. That block starts at frame 192: 6 ms.
    XSFLIB_CHECK(timing && timing->length_ms == 6 && timing->fade_ms == 10'000 && !timing->silent);
}

void TestWaveDiscovery()
{
    // A note looks up wave 42 of the bank at the table's fourth entry. With lookups_sound, the lookup counts as sound.
    FakeMachine machine;
    machine.words[kWaveBankTable] = 0x80710000;
    machine.words[0x80710000 + 0xf4 + 4 * 3] = 0x80720000;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t*, std::size_t)
    {
        if (block == 0)
        {
            m.StartSound();
        }
        if (block == 2)
        {
            m.cpu.state_.gpr[3] = 0x80720000;
            m.cpu.state_.gpr[4] = 42;
            m.cpu.on_watch(kWaveLookup);
        }
        if (block == 5)
        {
            m.words[kHandleAddress] = 0;
        }
    };

    std::set<jaudio::WaveUse> found;
    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, Watch(), kRules, &found, true);

    XSFLIB_CHECK(found == (std::set<jaudio::WaveUse>{{3, 42}}));
    XSFLIB_CHECK(timing && !timing->silent);
    XSFLIB_CHECK(machine.cpu.watched == (std::vector<uint32_t>{kParse, kJump, kWaveLookup}));

    // With the table's count at +0xf8, the walk stops at the count: a table of 3 banks has no fourth entry.
    FakeMachine counted = machine;
    counted.words[0x80710000 + 0xf8] = 3;
    jaudio::GameWatch watch = Watch();
    watch.wave_bank_count_offset = 0xf8;

    std::set<jaudio::WaveUse> none;
    jaudio::AnalyzeGameSound(counted, kHandleAddress, watch, kRules, &none, true);

    XSFLIB_CHECK(none.empty());
}

void TestVtableOffset()
{
    // A JAISound whose vtable pointer follows its members, at +0x94. Its first word is the handle's back-pointer. The
    // loop of TestLoop is found through the vtable at +0x94.
    FakeMachine machine;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        Sound(out, frames);
        if (block == 0)
        {
            m.StartSound();
            m.words[kHandle] = kHandleAddress;
            m.words[kHandle + 0x94] = kSequenceVtable;
            return;
        }

        const uint64_t pass_block = (block - 1) % 3;
        if (block > 6)
        {
            return;
        }

        if (pass_block == 0)
        {
            m.Command(0x10, kNote);
        }
        else if (pass_block == 1)
        {
            m.Command(0x12, kWait);
        }
        else
        {
            m.Jump(0x18, 0x10);
        }
    };

    jaudio::GameWatch watch = Watch();
    watch.vtable_offset = 0x94;

    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, watch, kRules);

    XSFLIB_CHECK(timing && timing->length_ms == 6 && timing->fade_ms == 10'000);
}

void TestConditionalJump()
{
    // The root track loops once through a conditional jump back to 0x10. Then the condition fails, and the track goes
    // on to a second loop at 0x20. It takes that loop's conditional jump back twice. The second of those jumps is the
    // loop's point, in block 11: 11 ms. A read of the condition as the target would find the loop at the second jump.
    const auto step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        Sound(out, frames);
        switch (block)
        {
        case 0:
            m.StartSound();
            break;
        case 1:
        case 4:
            m.Command(0x10, kNote);
            break;
        case 2:
        case 5:
            m.Command(0x12, kWait);
            break;
        case 3:
            m.ConditionalJump(0x18, 0x10);
            break;
        case 6:
        case 9:
            m.Command(0x20, kNote);
            break;
        case 7:
        case 10:
            m.Command(0x22, kWait);
            break;
        case 8:
        case 11:
            m.ConditionalJump(0x28, 0x20);
            break;
        default:
            break;
        }
    };

    FakeMachine machine;
    machine.step = step;
    jaudio::GameWatch watch = Watch();
    watch.conditional_jump = kConditionalJump;

    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, watch, kRules);

    XSFLIB_CHECK(timing && timing->length_ms == 11 && timing->fade_ms == 10'000);
    XSFLIB_CHECK(machine.cpu.watched == (std::vector<uint32_t>{kParse, kJump, kConditionalJump}));

    // Without the watch, no jump back reaches the analysis. The sound plays to the limit.
    FakeMachine unwatched;
    unwatched.step = step;
    jaudio::GameLengthRules rules = kRules;
    rules.limit_ms = 100;

    const auto limit = jaudio::AnalyzeGameSound(unwatched, kHandleAddress, Watch(), rules);

    XSFLIB_CHECK(limit && limit->length_ms == 100 && limit->fade_ms == 10'000);
}

// JAudio1's objects, laid out as in The Wind Waker HD: the parameters of a sequence's JAISound at +0x38, with the root
// track at +0x1360, and a track's reader at +0xc, its parent at +0x2f8 and its children from +0x2fc. The driver's word
// for the effects sequence's JAISound, that JAISound and its parameters, and the tracks of an effect's slots.
constexpr uint32_t kJAudio1Parameters = 0x80410000;
constexpr uint32_t kJAudio1Root = kJAudio1Parameters + 0x1360;
constexpr uint32_t kEffectsHandleAddress = 0x81700040;
constexpr uint32_t kEffectsSound = 0x80420000;
constexpr uint32_t kEffectsParameters = 0x80430000;
constexpr uint32_t kEffectsRoot = kEffectsParameters + 0x1360;
constexpr uint32_t kSlotParent = 0x80440000;
constexpr uint32_t kSlotTrack = 0x80450000;
constexpr uint32_t kOtherSlotTrack = 0x80460000;
constexpr uint32_t kChildTrack = 0x80470000;

// JAudio1's commands to wait and to end a track. The wait doesn't change the sound.
constexpr uint8_t kJAudio1Wait = 0x80;
constexpr uint8_t kJAudio1End = 0xff;

// The registers that hold the track at the parser's watched places, and the jump's target.
constexpr int kCommandTrack = 31;
constexpr int kJumpTrack = 29;
constexpr int kJumpTarget = 9;

jaudio::GameWatch JAudio1Watch()
{
    jaudio::GameWatch watch = Watch();
    watch.sequence_vtable = 0;
    watch.sequence_track_offset = 0;
    watch.jaudio1 = true;
    watch.command_track_register = kCommandTrack;
    watch.jump_track_register = kJumpTrack;
    watch.jump_target_register = kJumpTarget;
    watch.reader_offset = 0xc;
    watch.depth_offset = 0x18;
    watch.wait_offset = 0x14;
    watch.parameters_offset = 0x38;
    watch.parameters_track_offset = 0x1360;
    watch.effects_sequence = kEffectsHandleAddress;
    watch.effect_slot_offset = 0;
    watch.parent_offset = 0x2f8;
    watch.children_offset = 0x2fc;

    return watch;
}

// The game has started a JAudio1 sound, with the effects sequence playing: the sound's JAISound at kHandle, and the
// effects sequence's root track with a child of slot 1 whose children of slots 2 and 3 are kSlotTrack and
// kOtherSlotTrack. kChildTrack is a child of the sequence's root track.
void StartJAudio1Sound(FakeMachine& m)
{
    m.words[kHandleAddress] = kHandle;
    m.words[kHandle + 0x38] = kJAudio1Parameters;
    m.words[kEffectsHandleAddress] = kEffectsSound;
    m.words[kEffectsSound + 0x38] = kEffectsParameters;
    m.words[kEffectsRoot + 0x2fc + 1 * 4] = kSlotParent;
    m.words[kSlotParent + 0x2f8] = kEffectsRoot;
    m.words[kSlotParent + 0x2fc + 2 * 4] = kSlotTrack;
    m.words[kSlotParent + 0x2fc + 3 * 4] = kOtherSlotTrack;
    m.words[kSlotTrack + 0x2f8] = kSlotParent;
    m.words[kOtherSlotTrack + 0x2f8] = kSlotParent;
    m.words[kChildTrack + 0x2f8] = kJAudio1Root;
}

// A JAudio1 track runs the command `command` at `at`, followed by the byte `next`. The track's reader is at +0xc.
void JAudio1Command(FakeMachine& m, uint32_t track, uint32_t at, uint8_t command, uint8_t next = 0)
{
    m.words[track + 0xc] = kTrackData;
    m.words[track + 0x10] = kTrackData + at;
    m.bytes[kTrackData + at] = command;
    m.bytes[kTrackData + at + 1] = next;
    m.cpu.state_.gpr[kCommandTrack] = track;
    m.cpu.on_watch(kParse);
}

// A JAudio1 track jumps from the command that ends at `at` to `to`. The register holds the target itself.
void JAudio1Jump(FakeMachine& m, uint32_t track, uint32_t at, uint32_t to)
{
    m.words[track + 0xc] = kTrackData;
    m.words[track + 0x10] = kTrackData + at;
    m.cpu.state_.gpr[kJumpTrack] = track;
    m.cpu.state_.gpr[kJumpTarget] = to;
    m.cpu.on_watch(kJump);
}

// One pass of TestLoop's loop on `track`, in blocks 1 to 6: a note, a wait, and the jump back to the note. The note
// comes after a command from 0xb0 to 0xbf. Such a command runs the command after it. Only the note changes the sound.
void JAudio1Pass(FakeMachine& m, uint32_t track, uint64_t block)
{
    const uint64_t pass_block = (block - 1) % 3;
    if (block == 0 || block > 6)
    {
        return;
    }

    if (pass_block == 0)
    {
        JAudio1Command(m, track, 0x10, 0xb3, kNote);
    }
    else if (pass_block == 1)
    {
        JAudio1Command(m, track, 0x12, kJAudio1Wait);
    }
    else
    {
        JAudio1Jump(m, track, 0x18, 0x10);
    }
}

// Another track's loop: a note and the jump back, in blocks `first` and `first` + 1.
void OtherLoop(FakeMachine& m, uint32_t track, uint64_t block, uint64_t first = 1)
{
    if (block == first || block == first + 1)
    {
        JAudio1Command(m, track, 0x20, kNote);
        JAudio1Jump(m, track, 0x22, 0x20);
    }
}

void TestJAudio1Sequence()
{
    // The sequence's root track loops as in TestLoop. The effects sequence's track loops sooner. That track isn't part
    // of the sound.
    FakeMachine machine;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        Sound(out, frames);
        if (block == 0)
        {
            StartJAudio1Sound(m);
        }
        JAudio1Pass(m, kJAudio1Root, block);
        OtherLoop(m, kSlotTrack, block);
    };

    const auto timing =
        jaudio::AnalyzeGameSound(machine, kHandleAddress, JAudio1Watch(), kRules, nullptr, false, 0x80000001);

    XSFLIB_CHECK(timing && timing->length_ms == 6 && timing->fade_ms == 10'000);

    // The root track finishes. Then the effects sequence's track loops, and the sound doesn't. It sounds for 10 blocks,
    // and the game releases it.
    machine = FakeMachine{};
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        if (block < 10)
        {
            Sound(out, frames);
        }
        if (block == 0)
        {
            StartJAudio1Sound(m);
        }
        if (block == 1)
        {
            JAudio1Command(m, kJAudio1Root, 0x08, kJAudio1End);
        }
        if (block == 10)
        {
            m.words[kHandleAddress] = 0;
        }
        OtherLoop(m, kSlotTrack, block, 2);
    };

    const auto released =
        jaudio::AnalyzeGameSound(machine, kHandleAddress, JAudio1Watch(), kRules, nullptr, false, 0x80000001);

    XSFLIB_CHECK(released && released->length_ms == 10 + 500 && released->fade_ms == 0);

    // A child of the root track is part of the sound. Once the root track has finished, the child's loop is the sound's
    // loop.
    machine = FakeMachine{};
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        Sound(out, frames);
        if (block == 0)
        {
            StartJAudio1Sound(m);
        }
        if (block == 1)
        {
            JAudio1Command(m, kJAudio1Root, 0x08, kJAudio1End);
        }
        OtherLoop(m, kChildTrack, block);
    };

    const auto child =
        jaudio::AnalyzeGameSound(machine, kHandleAddress, JAudio1Watch(), kRules, nullptr, false, 0x80000001);

    XSFLIB_CHECK(child && child->length_ms == 2 && child->fade_ms == 10'000);
}

void TestJAudio1Effect()
{
    // An effect plays in the effects sequence's track of its slot, 0x12: child 2 of child 1. It loops as in TestLoop.
    // The track of slot 0x13 loops sooner. That track isn't part of the effect. The effect has no slot until block 1.
    FakeMachine machine;
    machine.step = [](FakeMachine& m, uint64_t block, int16_t* out, std::size_t frames)
    {
        Sound(out, frames);
        if (block == 0)
        {
            StartJAudio1Sound(m);
            m.bytes[kHandle] = 0xff;
        }
        if (block == 1)
        {
            m.bytes[kHandle] = 0x12;
        }
        JAudio1Pass(m, kSlotTrack, block);
        OtherLoop(m, kOtherSlotTrack, block);
    };

    const auto timing = jaudio::AnalyzeGameSound(machine, kHandleAddress, JAudio1Watch(), kRules, nullptr, false,
                                                 jaudio::kAafToEnd | 0x5000);

    XSFLIB_CHECK(timing && timing->length_ms == 6 && timing->fade_ms == 10'000);
}

} // namespace

int main()
{
    TestRelease();
    TestSilent();
    TestLoop();
    TestWaveDiscovery();
    TestVtableOffset();
    TestConditionalJump();
    TestJAudio1Sequence();
    TestJAudio1Effect();

    return xsflib::testing::Summary("game lengths");
}
