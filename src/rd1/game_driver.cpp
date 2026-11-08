// SPDX-License-Identifier: MIT

#include "ntdlib/rd1/game_driver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "ntdlib/agb/thumb.h"
#include "table_bytes.h"

namespace ntdlib::rd1
{
namespace
{

// Thumb's `swi kVoicesSwi`, and a nop (mov r8, r8).
constexpr uint16_t kSwiOpcode = 0xDF00 | kVoicesSwi;
constexpr uint16_t kNopOpcode = 0x46C0;

// The start of the handler of DMA 2's interrupt: it moves the ring's read place on by 4 words.
constexpr const char* kTransferPattern = "B570 B081 48xx 8800 2800 D100 E0xx 2400 49xx 6808 3004 6008";

// The places in the mixer of the loads of its RAM's addresses and of its echo's routine.
constexpr uint32_t kMixerVoicesLoad = 0x34;
constexpr uint32_t kRingWordsLoad = 0x44;
constexpr uint32_t kWritePlaceLoad = 0x4E;
constexpr uint32_t kReadPlaceLoad = 0x50;
constexpr uint32_t kFilterLoad = 0x68;
constexpr uint32_t kEchoRoutineLoad = 0xF4;
constexpr uint32_t kMixerVoiceCountLoad = 0x116;

// The places in the routine that mixes the echo of the loads of its settings.
constexpr uint32_t kEchoOffsetLoad = 0x2C;
constexpr uint32_t kEchoLevelLoad = 0x42;
constexpr uint32_t kEchoShiftLoad = 0x46;
constexpr uint32_t kEchoShift2Load = 0x4A;

// The mixer's gain of a voice through the filter: ldrb r0, [rN, #1]; mov r1, sl; muls r1, r0; ldr r0, =gain;
// ldrb r0, [r0]; muls r0, r1; lsls r0, #9; lsrs r0, #16.
constexpr const char* kFilterGainPattern = "78xx 4651 4341 48xx 7800 4348 0240 0C00";
constexpr uint32_t kFilterGainLoad = 6;

// The test of a revision's flag that sends every voice through the filter: movs r0, #8; ands r0, r1; cmp r0, #0;
// bne; ldr r0, =flag; ldrb r0, [r0]; cmp r0, #0; beq.
constexpr const char* kFilterAllPattern = "2008 4008 2800 D1xx 48xx 7800 2800 D0xx";
constexpr uint32_t kFilterAllLoad = 8;

// The mixer's length that FindGameRoutines() searches for the patterns.
constexpr uint32_t kMaxMixer = 0x300;

// The longest per-frame routine that FindGameRoutines() reads.
constexpr uint32_t kMaxFrameRoutine = 0x400;

// The longest handler of DMA 2's interrupt that FindGameRoutines() reads.
constexpr uint32_t kMaxHandler = 0x200;

// SOUNDCNT_H, whose bit 10 gives FIFO A's timer, and timer 0's reload register.
constexpr uint32_t kRegSoundCntH = 0x04000082;
constexpr uint32_t kRegTimer0Reload = 0x04000100;

// The registers of DMA 1's source, whose store in the handler comes before the loads of the rings' addresses.
constexpr uint32_t kRegDma1Source = 0x040000BC;

bool IsLiteralLoad(const agb::Rom& rom, uint32_t at)
{
    return (rom.U16(at) & 0xF800) == 0x4800;
}

bool InIwram(uint32_t address)
{
    return address >= agb::kIwramBase && address < agb::kIwramBase + agb::kIwramSize;
}

// The literal that the load at `at` reads, or 0 when there's no load there or the literal is outside IWRAM.
uint32_t IwramLiteral(const agb::Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, 2) || !IsLiteralLoad(rom, at))
    {
        return 0;
    }

    const uint32_t value = agb::ThumbLiteral(rom, at);
    return InIwram(value) ? value : 0;
}

// The first literal that a load from `from` on reads, within `length` bytes, or 0.
uint32_t FirstIwramLiteral(const agb::Rom& rom, uint32_t from, uint32_t length)
{
    for (uint32_t at = from; at < from + length; at += 2)
    {
        if (const uint32_t value = IwramLiteral(rom, at))
        {
            return value;
        }
    }

    return 0;
}

// The calls of the routine at `routine`, up to its return through `pop {r0}; bx r0`.
std::vector<uint32_t> CallsUpToReturn(const agb::Rom& rom, uint32_t routine)
{
    std::vector<uint32_t> calls;
    for (uint32_t at = routine; at < routine + kMaxFrameRoutine && rom.Contains(at, 4); at += 2)
    {
        if (agb::BlTarget(rom, at))
        {
            calls.push_back(at);
            at += 2;
        }
        else if (rom.U16(at) == 0xBC01 && rom.U16(at + 2) == 0x4700)
        {
            break;
        }
    }

    return calls;
}

} // namespace

std::optional<GameRoutines> FindGameRoutines(const agb::Rom& rom, const DriverInfo& info)
{
    GameRoutines r;
    r.init = info.init_routine;
    r.echo = info.echo_routine;
    for (size_t i = 0; i < r.echo_settings.size(); i++)
    {
        r.echo_settings[i] = uint32_t(info.echo[i]);
    }
    r.play = info.play_routine;
    r.start = info.start_routine;
    r.player_table = info.player_table;
    r.frame = info.frame_routine;

    // The per-frame routine ends with its calls of the voices' routine, the echo's settings and the mixer.
    const std::vector<uint32_t> calls = CallsUpToReturn(rom, r.frame);
    if (calls.size() < 3)
    {
        return std::nullopt;
    }

    r.voices_call = calls[calls.size() - 3];
    const uint32_t mixer = agb::BlTarget(rom, calls.back());
    r.mixer_voices = IwramLiteral(rom, mixer + kMixerVoicesLoad);
    r.ring_words = IwramLiteral(rom, mixer + kRingWordsLoad);
    r.write_place = IwramLiteral(rom, mixer + kWritePlaceLoad);
    r.read_place = IwramLiteral(rom, mixer + kReadPlaceLoad);
    r.mixer_voice_count = IwramLiteral(rom, mixer + kMixerVoiceCountLoad);
    r.filter = IwramLiteral(rom, mixer + kFilterLoad);
    for (const auto& [pattern, load, out] : {std::tuple{kFilterGainPattern, kFilterGainLoad, &r.filter_gain},
                                             std::tuple{kFilterAllPattern, kFilterAllLoad, &r.filter_all}})
    {
        for (uint32_t at : agb::FindThumb(rom, agb::ParseThumbPattern(pattern)))
        {
            if (at >= mixer && at < mixer + kMaxMixer)
            {
                *out = IwramLiteral(rom, at + load);
                break;
            }
        }
    }

    const uint32_t echo_mix = rom.Contains(mixer + kEchoRoutineLoad, 2) && IsLiteralLoad(rom, mixer + kEchoRoutineLoad)
                                  ? agb::ThumbLiteral(rom, mixer + kEchoRoutineLoad) & ~1u
                                  : 0;
    if (echo_mix)
    {
        r.echo_offset = IwramLiteral(rom, echo_mix + kEchoOffsetLoad);
        r.echo_level = IwramLiteral(rom, echo_mix + kEchoLevelLoad);
        r.echo_shift = IwramLiteral(rom, echo_mix + kEchoShiftLoad);
        r.echo_shift_2 = IwramLiteral(rom, echo_mix + kEchoShift2Load);
    }

    // The voices' routine calls the routine of each sample voice, then the PSG's output.
    const std::vector<uint32_t> voice_calls = CallsUpToReturn(rom, agb::BlTarget(rom, r.voices_call));
    if (voice_calls.size() < 2)
    {
        return std::nullopt;
    }

    r.sample_voices = FirstIwramLiteral(rom, agb::BlTarget(rom, voice_calls[0]), 0x10);
    r.psg_voices = FirstIwramLiteral(rom, agb::BlTarget(rom, voice_calls[1]), 0x10);

    // The handler of DMA 2's interrupt names the read place, and then the rings' addresses after DMA 1's source.
    for (uint32_t handler : agb::FindThumb(rom, agb::ParseThumbPattern(kTransferPattern)))
    {
        if (FirstIwramLiteral(rom, handler + 0x10, 2) != r.read_place)
        {
            continue;
        }

        for (uint32_t at = handler; at < handler + kMaxHandler; at += 2)
        {
            if (IsLiteralLoad(rom, at) && agb::ThumbLiteral(rom, at) == kRegDma1Source)
            {
                r.transfer = handler;
                r.fifo_a = FirstIwramLiteral(rom, at + 2, 0x10);
                for (uint32_t b = at + 4; b < at + 0x20 && !r.fifo_b; b += 2)
                {
                    const uint32_t value = IwramLiteral(rom, b);
                    r.fifo_b = value != r.fifo_a && value != r.read_place ? value : 0;
                }
                break;
            }
        }
        break;
    }

    const bool found = r.voices_call && r.mixer_voices && r.ring_words && r.write_place && r.read_place &&
                       r.mixer_voice_count && r.sample_voices && r.psg_voices && r.transfer && r.fifo_a && r.fifo_b &&
                       r.filter && r.filter_gain && r.echo_offset && r.echo_level && r.echo_shift && r.echo_shift_2;
    if (!found)
    {
        return std::nullopt;
    }

    return r;
}

GameDriver::GameDriver(const agb::Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    voices_routine_ = agb::BlTarget(rom, routines_.voices_call);
    if (!voices_routine_)
    {
        throw std::invalid_argument("the per-frame routine's call of the voices' routine isn't a call");
    }

    system_.Store(routines_.voices_call, 2, kSwiOpcode);
    system_.Store(routines_.voices_call + 2, 2, kNopOpcode);
    system_.on_swi = [this](uint32_t number)
    {
        if (number != kVoicesSwi)
        {
            return false;
        }

        system_.Cpu().Call(voices_routine_ | 1, {}, agb::System::kCallCycles);
        if (before_mix)
        {
            before_mix();
        }

        return true;
    };

    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }
    };

    system_.Call(routines_.init | 1);
    system_.Call(routines_.echo | 1, routines_.echo_settings);
}

void GameDriver::Play(int song)
{
    sound_writes_.clear();
    const std::array<uint32_t, 1> args = {uint32_t(song)};
    system_.Call(routines_.play | 1, args);
}

void GameDriver::Start(int player, uint32_t record)
{
    sound_writes_.clear();
    const std::array<uint32_t, 2> args = {
        system_.Load(routines_.player_table + kPlayerEntryBytes * uint32_t(player), 4), record};
    system_.Call(routines_.start | 1, args);
}

void GameDriver::Transfer()
{
    system_.Call(routines_.transfer | 1);
}

void GameDriver::Frame()
{
    sound_writes_.clear();
    fifo_a_.clear();
    fifo_b_.clear();
    const uint32_t from = system_.Load(routines_.write_place, 4);
    system_.Call(routines_.frame | 1);
    const uint32_t to = system_.Load(routines_.write_place, 4);
    ReadRing(system_.Load(routines_.fifo_a, 4), from, to, fifo_a_);
    ReadRing(system_.Load(routines_.fifo_b, 4), from, to, fifo_b_);
}

uint32_t GameDriver::TimerPeriod()
{
    const uint32_t timer = system_.Load(kRegSoundCntH, 2) >> 10 & 1;

    return 0x10000 - system_.Load(kRegTimer0Reload + 4 * timer, 2);
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

const std::vector<int8_t>& GameDriver::FifoA() const
{
    return fifo_a_;
}

const std::vector<int8_t>& GameDriver::FifoB() const
{
    return fifo_b_;
}

std::vector<int8_t> GameDriver::PendingA()
{
    std::vector<int8_t> bytes;
    ReadRing(system_.Load(routines_.fifo_a, 4), system_.Load(routines_.read_place, 4),
             system_.Load(routines_.write_place, 4), bytes);

    return bytes;
}

std::vector<int8_t> GameDriver::PendingB()
{
    std::vector<int8_t> bytes;
    ReadRing(system_.Load(routines_.fifo_b, 4), system_.Load(routines_.read_place, 4),
             system_.Load(routines_.write_place, 4), bytes);

    return bytes;
}

std::vector<uint8_t> GameDriver::SampleVoiceBytes(int voice)
{
    return RecordBytes(system_.Load(routines_.sample_voices, 4) + uint32_t(voice * kVoiceRecordBytes));
}

std::vector<uint8_t> GameDriver::MixerVoiceBytes(int voice)
{
    return RecordBytes(system_.Load(routines_.mixer_voices, 4) + uint32_t(voice * kVoiceRecordBytes));
}

std::vector<uint8_t> GameDriver::PsgVoiceBytes(int voice)
{
    return RecordBytes(routines_.psg_voices + uint32_t(voice * kVoiceRecordBytes));
}

MixSettings GameDriver::Settings()
{
    MixSettings s;
    s.filter = uint8_t(system_.Load(routines_.filter, 1));
    s.filter_gain = uint8_t(system_.Load(routines_.filter_gain, 1));
    s.filter_all = routines_.filter_all && system_.Load(routines_.filter_all, 1);
    s.echo_level = system_.Load(routines_.echo_level, 4);
    s.echo_shift = system_.Load(routines_.echo_shift, 4);
    s.echo_shift_2 = system_.Load(routines_.echo_shift_2, 4);
    s.ring_bytes = 4 * system_.Load(routines_.ring_words, 4);
    s.echo_offset = 4 * system_.Load(routines_.echo_offset, 4);

    return s;
}

const GameRoutines& GameDriver::Routines() const
{
    return routines_;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

const agb::System& GameDriver::Gba() const
{
    return system_;
}

void GameDriver::ReadRing(uint32_t ring, uint32_t from, uint32_t to, std::vector<int8_t>& out)
{
    const uint32_t words = system_.Load(routines_.ring_words, 4);
    for (uint32_t w = from; w != to && words; w = (w + 1) % words)
    {
        for (uint32_t b = 0; b < 4; b++)
        {
            out.push_back(int8_t(system_.Load(ring + 4 * w + b, 1)));
        }
    }
}

std::vector<uint8_t> GameDriver::RecordBytes(uint32_t at)
{
    std::vector<uint8_t> bytes(kVoiceRecordBytes);
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = uint8_t(system_.Load(at + i, 1));
    }

    return bytes;
}

} // namespace ntdlib::rd1
