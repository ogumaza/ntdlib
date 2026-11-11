// SPDX-License-Identifier: MIT

#include "ntdlib/konami/output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/wave/sinc.h"
#include "output_impl.h"

namespace ntdlib::konami
{
Output::Impl::Impl(const agb::Rom& rom_in, const DriverInfo& info_in) : rom(rom_in), info(info_in)
{
    if (info.revision == Revision::kDungeonDiceMonsters)
    {
        stage = Stage::kFifo;
        voice_size = 12;
        voice_count = 4;
        rules.has_echo = false;
        wave_gate = false;
        wave_keeps_restart = WaveKeepsRestart();
        echo_rules.first_delay_blocks = 0;
        voices.assign(size_t(voice_count), OutputVoice());
        block_cycles = kBlock;
        return;
    }

    const uint32_t mixer = FindMixer();
    FindPitchTable();
    voice_size = VoiceSizeNear(mixer);
    voice_count = 12;
    rules = MixRules();
    SetRevisionRules(LevelTableNear(mixer));

    // Voice v's routing is bit (11 - v) * 2: the mask keeps voices 5 to 11.
    if (MixesBus0FromVoice5(info.revision))
    {
        echo_rules.bus0_voices = 0x1FFF;
    }

    echo_rules.first_delay_blocks = FirstDelayBlocks();
    voices.assign(size_t(voice_count), OutputVoice());
    work.assign(kWorkBytes, 0);
    const double rate = info.mix_rate > 0 ? info.mix_rate : 21024.0;
    block_cycles = kBlock * uint32_t(std::lround(double(agb::kSecondCycles) / rate));
}

uint32_t Output::Impl::FindMixer()
{
    // The sample table's loop code in the mixer: ldrsh r0, [r7, #12] / movs r0, r0, lsl #17.
    constexpr uint32_t kLoadKey = 0xE1D700FC;
    constexpr uint32_t kShiftKey = 0xE1B00880;
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 8); at += 4)
    {
        if (rom.U32(at) != kLoadKey || rom.U32(at + 4) != kShiftKey)
        {
            continue;
        }

        const auto literal = [this](uint32_t where, uint32_t opcode, uint32_t mask) -> uint32_t
        {
            const uint32_t word = rom.U32(where);
            return (word & mask) == opcode ? rom.U32(where + 8 + (word & 0xFFF)) : 0;
        };

        // ldrlo r5, =table / ldrhs r5, =second after the shift, or ldr r5, =table before the load and ldrhs r5, =second
        // after the shift.
        uint32_t main_table = literal(at + 8, 0x359F5000, 0xFFFFF000);
        uint32_t other = literal(at + 12, 0x259F5000, 0xFFFFF000);
        if (!main_table)
        {
            main_table = literal(at - 4, 0xE59F5000, 0xFFFFF000);
            other = literal(at + 8, 0x259F5000, 0xFFFFF000);
        }

        if (main_table == info.sample_table)
        {
            second_table = other;
            return at;
        }
    }

    throw std::runtime_error("the driver's mixer wasn't found");
}

void Output::Impl::FindPitchTable()
{
    // The pitch table: 4096 * 2 ^ (pitch / 384), round its centre.
    constexpr std::array<uint16_t, 5> kPitchCentre = {4081, 4089, 4096, 4103, 4111};
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 10) && !pitch_table; at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < kPitchCentre.size() && match; i++)
        {
            match = rom.U16(at + uint32_t(2 * i)) == kPitchCentre[i];
        }
        if (match)
        {
            pitch_table = at + 4;
        }
    }

    if (!pitch_table)
    {
        throw std::runtime_error("the driver's pitch table wasn't found");
    }
}

int Output::Impl::VoiceSizeNear(uint32_t mixer) const
{
    // The build's records: sub r7, r7, #size in the mixers that run from the last voice, and add r7, r7, #size in the
    // others.
    const auto holds = [&](uint32_t word)
    {
        for (uint32_t at = mixer - std::min(mixer - agb::kRomBase, 0x400u); at < mixer + 0x400; at += 4)
        {
            if (rom.U32(at) == word)
            {
                return true;
            }
        }
        return false;
    };

    const bool ring = info.revision == Revision::kEternalDuelist || info.revision == Revision::kCaptainTsubasa;
    return ring                                     ? 16
           : holds(0xE247701C) || holds(0xE287701C) ? 28
           : holds(0xE2477018) || holds(0xE2877018) ? 24
                                                    : 20;
}

uint32_t Output::Impl::LevelTableNear(uint32_t mixer) const
{
    // The table of levels' load: ldr r0, =table before ldrb r8, [r7, #16].
    uint32_t level_table = 0;
    for (uint32_t at = mixer - std::min(mixer - agb::kRomBase, 0x400u); at < mixer + 0x400 && !level_table; at += 4)
    {
        if (rom.U32(at) == 0xE5D78010 && (rom.U32(at - 4) & 0xFFFFF000) == 0xE59F0000)
        {
            level_table = rom.U32(at - 4 + 8 + (rom.U32(at - 4) & 0xFFF));
        }
    }

    return level_table;
}

void Output::Impl::SetRevisionRules(uint32_t level_table)
{
    switch (info.revision)
    {
    case Revision::kUltimateMasters:
    case Revision::kLiveStart:
    case Revision::kPrinceOfTennis:
        if (voice_size == 28)
        {
            rules.unequal = rules.equal = rules.reverse = Scale::kRoundUp7;
            rules.coded = Coded::kAdpcm;
            rules.wet_rounds_up = true;
            clears_held = true;
        }

        pan_sets_levels = info.revision != Revision::kPrinceOfTennis;
        if (info.revision != Revision::kUltimateMasters)
        {
            echo_rules.feedback_ramp = 6;
            echo_rules.delay_ramp = 4;
            echo_rules.countdown = false;
        }
        break;
    case Revision::kWct2004:
    case Revision::kMinnaNoOujisama:
        stage = Stage::kWct;
        levels_each_frame = true;
        echo_rules.feedback_ramp = 4;
        echo_rules.delay_ramp = 4;
        echo_rules.countdown = false;
        echo_rules.shares_settings = false;
        echo_rules.clears_routed = false;
        echo_rules.stop_clears_feedback = voice_size == 24;
        rules.unequal = rules.reverse = Scale::kUp4;
        rules.reverse_two_levels = true;
        if (voice_size == 28)
        {
            rules.equal = Scale::kUp4;
            rules.keeps_path = true;
            rules.double_loop = false;
        }
        else
        {
            rules.equal = Scale::kDown4;
            rules.reverse_crosses = info.revision == Revision::kWct2004;
            rules.reverse_two_levels = info.revision == Revision::kWct2004;
        }
        break;
    case Revision::kKitaro:
        stage = Stage::kWct;
        levels_each_frame = true;
        echo_rules.feedback_ramp = 4;
        echo_rules.delay_ramp = 4;
        echo_rules.countdown = false;
        echo_rules.shares_settings = false;
        echo_rules.clears_routed = false;
        echo_rules.runs_with_feedback = false;
        echo_rules.stop_clears_feedback = false;
        echo_rules.delay_sets_present = false;
        rules.unequal = rules.equal = Scale::kTable;
        rules.reverse = Scale::kTableSigned;
        rules.reverse_two_levels = true;
        rules.keeps_path = true;
        rules.single_loop = true;
        rules.double_loop = false;
        rules.mask_each_block = true;
        rules.level_table = level_table;
        break;
    case Revision::kRaveMaster:
    case Revision::kHunterXHunter:
        stage = Stage::kWct;
        voice_count = 8;
        rules.unequal = rules.equal = Scale::kTable;
        rules.reverse = Scale::kTableSigned;
        rules.reverse_two_levels = true;
        rules.single_loop = true;
        rules.double_loop = false;
        rules.has_echo = false;
        rules.level_table = level_table;
        if (info.revision == Revision::kHunterXHunter)
        {
            rules.keeps_path = true;
        }
        else
        {
            rules.skips_silent = true;
            if (voice_size == 20)
            {
                rules.coded = Coded::kPackedRuns;
                rules.loop_from_header = true;
                wave_zero_check = false;
                wave_gate = false;
                nr51 = 0xFF;
            }
        }
        break;
    case Revision::kEternalDuelist:
    case Revision::kCaptainTsubasa:
        stage = Stage::kRing;
        voice_count = 6;
        rules.has_echo = false;
        wave_zero_check = false;
        wave_second_bank = false;
        wave_gate = false;
        nr51 = 0xFF;
        pitch_on_any_flag = info.revision == Revision::kEternalDuelist;
        nr51_twice = RingStoresNr51Twice();
        break;
    default:
        break;
    }
}

bool Output::Impl::RingStoresNr51Twice() const
{
    // The output stage opens with the store and the setting's test: ldr r3, =NR51 / movs r2, #0xE0 / lsls r2, r2, #1 /
    // add r2, r8 / ldrb r0, [r2] / strb r0, [r3]. The other builds load the byte just before the one store.
    constexpr std::array<uint16_t, 5> kOpening = {0x22E0, 0x0052, 0x4442, 0x7810, 0x7018};
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 10); at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < kOpening.size() && match; i++)
        {
            match = rom.U16(at + uint32_t(2 * i)) == kOpening[i];
        }
        if (match)
        {
            return true;
        }
    }

    return false;
}

bool Output::Impl::WaveKeepsRestart() const
{
    // The later build's output stage stores the entry as it stands: movs r3, #0x18 / ldrsh r0, [r6, r3] /
    // lsls r0, r0, #1 / adds r0, r0, r1 / ldrh r0, [r0] / strh r0, [r2]. The others take bit 15 off it first.
    constexpr std::array<uint16_t, 6> kStore = {0x2318, 0x5EF0, 0x0040, 0x1840, 0x8800, 0x8010};
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 12); at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < kStore.size() && match; i++)
        {
            match = rom.U16(at + uint32_t(2 * i)) == kStore[i];
        }
        if (match)
        {
            return true;
        }
    }

    return false;
}

uint8_t Output::Impl::FirstDelayBlocks() const
{
    // The init sets the mixer's rate, and the rate's routine works out the delay of each bus in blocks from a table of
    // scales. Some builds' routine has already put the timer's setting in the register that held the rate, and reads
    // the table at twice that setting: at whatever data lies there.
    constexpr uint32_t kFullDelay = 2 * 0x7F;
    constexpr std::array<uint16_t, 5> kScales = {4096, 3537, 3072, 2606, 2048};
    // ldr r3, [r0] / movs r0, #0x80 / lsls r0, r0, #16 / orrs r0, r3 / str r0, [r2], then lsls r0, r3, #1 after the two
    // literals' loads.
    constexpr std::array<uint16_t, 5> kStray = {0x6803, 0x2080, 0x0400, 0x4318, 0x6010};
    // The later revisions' init leaves the rate to the game's code, and the delays at 0.
    if (stage != Stage::kUltimate)
    {
        return 0;
    }

    if (!info.timer_table)
    {
        return uint8_t(kFullDelay);
    }

    const auto find = [this](const auto& words) -> uint32_t
    {
        for (uint32_t at = agb::kRomBase; rom.Contains(at, uint32_t(2 * words.size())); at += 2)
        {
            bool match = true;
            for (size_t i = 0; i < words.size() && match; i++)
            {
                match = rom.U16(at + uint32_t(2 * i)) == words[i];
            }
            if (match)
            {
                return at;
            }
        }
        return 0;
    };

    const uint32_t stray = find(kStray);
    const uint32_t scales = find(kScales);
    if (!stray || !scales || rom.U16(stray + 14) != 0x0058)
    {
        return uint8_t(kFullDelay);
    }

    const uint32_t scale = rom.U16(scales + rom.U32(info.timer_table) * 2);
    return uint8_t(((scale * 0x7F) >> 12) * 2);
}

void Output::Impl::Play(int song, const Request& request)
{
    sequencer = Sequencer::Create(rom, info, song, request);
    stop_pending = false;
    idle = false;
    first_frame = true;
    wave_started = false;
}

void Output::Impl::WaveLoad(int wave, int volume)
{
    if (volume != 0 || !wave_zero_check)
    {
        uint32_t table = info.wave_table;
        if (wave_second_bank && (wave & 0x80))
        {
            table = info.sfx_wave_table;
            wave &= 0x7F;
        }

        const uint32_t image = table + uint32_t((wave << 4) + volume) * 16;
        for (uint32_t i = 0; i < 4; i++)
        {
            Store(kWaveRam + 4 * i, 4, rom.U32(image + 4 * i));
        }
    }
    else
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            Store(kWaveRam + 4 * i, 4, 0);
        }
    }

    wave_bank ^= 1;
    Store(kSound3CntL, 2, wave_bank ? 0x80 : 0xC0);
}

void Output::Impl::WaveLoadFifo(int wave)
{
    Store(kSound3CntL, 2, 0x40);
    for (uint32_t i = 0; i < 8; i++)
    {
        Store(kWaveRam + 2 * i, 2, rom.U16(info.wave_table + uint32_t(wave) * 16 + 2 * i));
    }

    Store(kSound3CntL, 2, 0x80);
    if (!wave_keeps_restart)
    {
        Store(kSound3CntX, 2, 0x8000);
    }
}

void Output::Impl::PlainReset()
{
    if (stage == Stage::kFifo)
    {
        Store(kSound1CntL, 2, 0);
        WaveLoadFifo(0);
        return;
    }

    if (stage == Stage::kRing)
    {
        Store(kNr51, 1, 0xFF);
        Store(kSoundCntH, 2, 0x330E);
    }

    Store(kSound3CntH, 2, 0);
    WaveLoad(0, 0);
}

void Output::Impl::ClearLine(int bus)
{
    const OutputEcho& e = echo[size_t(bus)];
    if (e.has_line)
    {
        std::fill_n(work.begin() + e.line, kLineBytes, int8_t(0));
        if (enhanced)
        {
            std::vector<float>& line = enhanced->lines[e.line == 0 ? 0 : 1];
            std::fill(line.begin(), line.end(), 0.0f);
        }
    }
}

void Output::Impl::RouteUltimate(int bus, int voice, bool on)
{
    if (bus > 2 || voice > 11)
    {
        return;
    }

    OutputEcho& e = echo[size_t(bus)];
    if (!e.has_line || voice < 0)
    {
        e.routing = 0;
        e.routed = 0;
        return;
    }

    const uint32_t bit = 1u << ((11 - voice) * 2);
    uint32_t routed = (e.routed << 2) >> 2;
    routed |= bit;
    if (!on)
    {
        routed &= ~bit;
    }
    if (routed)
    {
        routed |= 0x40000000;
    }

    e.routed = routed;
}

void Output::Impl::FeedbackUltimate(int bus, int value, bool now)
{
    OutputEcho& e = echo[size_t(bus)];
    if (!e.has_line)
    {
        e.feedback = 0;
        e.feedback_goal = 0;
        return;
    }

    e.feedback_goal = uint8_t(value);
    if (now)
    {
        e.feedback = uint8_t(value);
        if (bus == 2)
        {
            echo[1].feedback = uint8_t(value);
        }
    }
}

void Output::Impl::DelayUltimate(int bus, int value, bool now)
{
    OutputEcho& e = echo[size_t(bus)];
    if (!e.has_line)
    {
        e.delay = 0x7F;
        e.delay_goal = 0x7F;
        return;
    }

    if (now)
    {
        e.delay_goal = uint8_t(value);
        if (echo_rules.delay_sets_present)
        {
            e.delay = uint8_t(value);
        }
    }

    e.delay_blocks = uint8_t(2 * value);
}

void Output::Impl::UpdateEchoUltimate()
{
    if (!rules.has_echo)
    {
        return;
    }

    const EchoRules& r = echo_rules;
    for (int i = 0; i < 2; i++)
    {
        OutputEcho& e = echo[size_t(i)];
        const OutputEcho* settings = &e;
        uint32_t routing = e.routed;
        if (i == 1 && echo[2].routed != 0)
        {
            routing |= echo[2].routed;
            settings = &echo[2];
            if (r.shares_settings)
            {
                e.feedback = echo[2].feedback;
                e.delay_blocks = echo[2].delay_blocks;
            }
        }

        e.routing = routing;
        uint8_t feedback = e.feedback;
        const uint8_t goal = settings->feedback_goal;
        if (feedback != goal)
        {
            if (feedback > goal)
            {
                feedback = uint8_t(std::max(feedback - r.feedback_ramp, int(goal)));
            }
            else
            {
                feedback = uint8_t(std::min(feedback + r.feedback_ramp, int(goal)));
            }

            e.feedback = feedback;
            if (feedback == 0)
            {
                e.countdown = r.countdown ? 0x3C : e.countdown;
                if (r.clears_routed)
                {
                    e.routed = 0;
                }
                e.routing = 0;
            }
            else
            {
                e.countdown = r.countdown ? 0 : e.countdown;
                e.routing = routing;
            }
        }

        if (r.countdown)
        {
            if (feedback == 0 && e.countdown >= 0)
            {
                e.routing |= 0x40000000;
                e.countdown = int8_t(e.countdown - 1);
                if (e.countdown < 0)
                {
                    ClearLine(i);
                }
            }
        }
        else if (r.runs_with_feedback && e.feedback != 0 && e.routing == 0)
        {
            e.routing = 0x40000000;
        }

        if (e.delay != settings->delay_goal)
        {
            // The routine sets the blocks from the new value, and keeps the present value.
            DelayUltimate(i, std::max(e.delay - r.delay_ramp, int(settings->delay_goal)), false);
        }
    }

    uint32_t mask = echo[0].routing & ~echo[1].routing & r.bus0_voices;
    if (mask != 0 || (voice_size == 28 && r.countdown && echo[0].countdown != 0))
    {
        mask |= 0x40000000;
    }

    echo_mask = (echo[1].routing << 1) | mask;
}

void Output::Impl::RunEchoCommandsUltimate()
{
    for (const StageCommand& c : sequencer->StageCommands())
    {
        switch (c.op)
        {
        case Op::kEchoRoute:
            {
                const int voice = c.value >> 4;
                const int bus = (c.value & 0x0F) - 1;
                if (bus < 0)
                {
                    RouteUltimate(0, voice, false);
                    RouteUltimate(1, voice, false);
                }
                else
                {
                    RouteUltimate(bus, voice, true);
                }
                break;
            }
        case Op::kEchoDelay:
            DelayUltimate(c.value > 0x7F ? 1 : 0, c.value > 0x7F ? c.value - 0x80 : c.value, true);
            break;
        case Op::kEchoFeedback:
            if (stage == Stage::kWct)
            {
                FeedbackUltimate(0, c.value & 0x7F, false);
            }
            else
            {
                FeedbackUltimate(c.value > 0x7F ? 1 : 0, c.value > 0x7F ? c.value - 0x80 : c.value, false);
            }
            break;
        case Op::kWave:
            WaveLoad(c.value, c.volume);
            break;
        default:
            break;
        }
    }
}

bool Output::Impl::Frame()
{
    writes.clear();
    for (Fifo& fifo : fifos)
    {
        fifo.restarted = false;
    }

    UpdateEchoUltimate();
    if (idle || !sequencer || !sequencer->Valid())
    {
        return false;
    }

    if (stop_pending)
    {
        for (TrackOutput& r : records)
        {
            // Without a mixer, a record's stop leaves the envelope that the frame before gave the same place.
            const uint16_t envelope = r.key;
            r = TrackOutput();
            r.trig = 1;
            r.flags = stage == Stage::kFifo ? kOutDungeonDiceNote : kOutStop;
            r.pan_r = 0x3F;
            r.pan_l = 0x3F;
            if (stage == Stage::kFifo)
            {
                r.key = envelope;
            }
        }

        if (echo_rules.stop_clears_feedback)
        {
            FeedbackUltimate(0, 0, false);
            FeedbackUltimate(1, 0, false);
        }

        RunStage();
        if (stage == Stage::kFifo)
        {
            StartNotesFifo();
        }

        stop_pending = false;
        idle = true;
        return true;
    }

    if (first_frame)
    {
        first_frame = false;
        RouteUltimate(0, -1, false);
        RouteUltimate(1, -1, false);
        FeedbackUltimate(0, 0, false);
        FeedbackUltimate(1, 0, false);
        PlainReset();
    }

    records = sequencer->Step();
    if (stage == Stage::kFifo)
    {
        // The wave loads and a loop's reset come in the tracks' order.
        for (const StageCommand& c : sequencer->StageCommands())
        {
            if (c.op == Op::kWave)
            {
                WaveLoadFifo(c.value);
            }
            else if (c.op == Op::kJump)
            {
                PlainReset();
            }
        }

        RunStage();
        StartNotesFifo();
        if (sequencer->Stopped())
        {
            stop_pending = true;
        }

        return true;
    }

    if (sequencer->LoopedLastFrame())
    {
        PlainReset();
    }

    RunEchoCommandsUltimate();
    if (stage == Stage::kRing)
    {
        // The frame ends with NR51's value: the OR of what the PSG tracks ask for.
        nr51 = uint32_t(records[0].pan | records[1].pan | records[2].pan | records[3].pan);
    }

    RunStage();

    if (sequencer->Stopped())
    {
        stop_pending = true;
    }

    return true;
}

Output::Output(const agb::Rom& rom, const DriverInfo& info) : impl_(std::make_unique<Impl>(rom, info))
{
    // The init gives bus 0 the work buffer's first 8 KB, and buses 1 and 2 the rest.
    impl_->echo[0].has_line = true;
    impl_->echo[1].has_line = true;
    impl_->echo[2].has_line = true;
    impl_->echo[1].line = kLineBytes;
    impl_->echo[2].line = kLineBytes;
    for (OutputEcho& e : impl_->echo)
    {
        e.delay = 0x7F;
        e.delay_goal = 0x7F;
        e.delay_blocks = impl_->echo_rules.first_delay_blocks;
    }
}

Output::~Output() = default;

Output::Output(const Output& other) : impl_(std::make_unique<Impl>(*other.impl_))
{
}

Output& Output::operator=(const Output& other)
{
    impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

void Output::Play(int song, const Request& request)
{
    impl_->Play(song, request);
}

bool Output::Frame()
{
    return impl_->Frame();
}

const std::vector<agb::RegisterWrite>& Output::SoundWrites() const
{
    return impl_->writes;
}

void Output::Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    impl_->Mix(fifo_a, fifo_b);
}

const std::array<TrackOutput, kTracks>& Output::Records() const
{
    return impl_->records;
}

const std::vector<OutputVoice>& Output::Voices() const
{
    return impl_->voices;
}

const OutputEcho& Output::Echo(int bus) const
{
    return impl_->echo[size_t(bus)];
}

uint32_t Output::EchoMask() const
{
    return impl_->echo_mask;
}

double Output::Rate() const
{
    return impl_->stage == Stage::kFifo ? 0.0 : 16.0 * double(agb::kSecondCycles) / impl_->block_cycles;
}

uint32_t Output::FifoCycles(int fifo) const
{
    return impl_->stage == Stage::kFifo ? impl_->FifoCycles(fifo) : impl_->block_cycles / kBlock;
}

const Sequencer* Output::Song() const
{
    return impl_->sequencer.get();
}

bool Output::FifoRestarted(int fifo) const
{
    return impl_->fifos[size_t(fifo)].restarted;
}

const std::vector<FifoTimerChange>& Output::TimerChanges() const
{
    return impl_->timer_changes;
}

uint32_t Output::FifoLead() const
{
    switch (impl_->stage)
    {
    case Stage::kFifo:
        return 0;
    case Stage::kRing:
        return uint32_t(kRingBytes) - uint32_t(agb::kFrameCycles * kBlock / impl_->block_cycles);
    default:
        return 3 * kBlock;
    }
}

int Output::PsgChannelTrack(int channel) const
{
    return impl_->channel_tracks[size_t(channel)];
}

std::array<uint8_t, agb::kSoundRegisterBytes> Output::SoundRegisters() const
{
    const Impl& m = *impl_;
    std::array<uint8_t, agb::kSoundRegisterBytes> registers{};
    const auto set = [&registers](uint32_t address, uint8_t value)
    {
        registers[address - agb::kRegSound1CntL] = value;
    };

    // The init restarts the squares and the wave channel, without a level. The revision without a mixer restarts the
    // noise too, and loads the wave channel's first wave. A ring mixer's revision gives the wave channel full volume.
    const bool fifo = m.stage == Stage::kFifo;
    const bool ring = m.stage == Stage::kRing;
    set(kSound1CntX + 1, 0x80);
    set(kSound2CntH + 1, 0x80);
    set(kSound3CntL, fifo ? 0x80 : 0xC0);
    set(kSound3CntH + 1, ring ? 0x20 : 0x00);
    set(kSound3CntX + 1, 0x80);
    set(kSound4CntH + 1, fifo ? 0x80 : 0x00);
    set(kNr50, 0x77);
    set(kNr51, fifo || ring || m.voice_size == 20 ? 0xFF : 0xBB);
    set(kSoundCntH, 0x0E);
    set(kSoundCntHHigh, fifo || ring ? 0x33 : 0x21);
    set(kSoundCntX, 0x80);
    set(kSoundBias + 1, fifo ? 0x02 : 0x42);
    if (fifo)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            set(kWaveRam + i, m.rom.U8(m.info.wave_table + i));
        }
    }

    return registers;
}

void Output::SetMutedTracks(uint16_t tracks)
{
    impl_->muted_tracks = tracks;
}

void Output::SetEcho(bool on)
{
    // The revisions without an echo have no lines: the revision without a mixer has no work buffer.
    impl_->echo_heard = on;
    if (!on && impl_->rules.has_echo)
    {
        impl_->ClearLine(0);
        impl_->ClearLine(1);
    }
}

void Output::MeterVoices(bool on)
{
    impl_->metering = on;
    if (!on)
    {
        impl_->voice_peaks = {};
    }
}

std::array<float, 2> Output::VoicePeaks(int track) const
{
    return impl_->voice_peaks[size_t(track)];
}

void Output::StartEnhancedMixing()
{
    Impl& m = *impl_;
    Enhanced& e = m.enhanced.emplace();
    const size_t samples = agb::kEnhancedFrameSamples;
    e.voices.assign(m.voices.size(), EnhancedVoice());
    e.out.assign(2 * samples, 0.0f);
    if (m.rules.has_echo && m.stage != Stage::kFifo && m.stage != Stage::kRing)
    {
        // A line holds the console's 256 blocks, and a frame's samples more.
        e.line_samples = size_t(uint64_t(256) * m.block_cycles * samples / agb::kFrameCycles) + samples + 1;
        for (size_t bus = 0; bus < 2; bus++)
        {
            e.inputs[bus].assign(2 * samples, 0.0f);
            e.lines[bus].assign(2 * e.line_samples, 0.0f);
        }
    }
}

void Output::SetEnhancedFrames(bool on)
{
    if (impl_->enhanced)
    {
        impl_->enhanced->frames = on;
    }
}

std::span<const float> Output::EnhancedFifoA() const
{
    if (!impl_->enhanced)
    {
        return {};
    }

    return std::span<const float>(impl_->enhanced->out).first(agb::kEnhancedFrameSamples);
}

std::span<const float> Output::EnhancedFifoB() const
{
    if (!impl_->enhanced)
    {
        return {};
    }

    return std::span<const float>(impl_->enhanced->out).last(agb::kEnhancedFrameSamples);
}

bool Output::MixerSilent() const
{
    const Impl& m = *impl_;
    for (const OutputVoice& v : m.voices)
    {
        if (v.flags & kVoiceOn)
        {
            return false;
        }
    }

    return std::all_of(m.work.begin(), m.work.end(), [](int8_t byte) { return byte == 0; });
}

} // namespace ntdlib::konami
