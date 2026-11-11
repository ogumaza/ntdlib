// SPDX-License-Identifier: MIT

// The scripts in the model of the middle EAD engine: the reading of a script's bytes, and the commands of the players,
// channels and layers.

#include <array>
#include <cstddef>
#include <cstdint>

#include "ead_later_tables.h"
#include "ead_middle_constants.h"
#include "ead_tables.h"
#include "ntdlib/n64/ead_middle.h"

namespace ntdlib::n64
{

namespace
{

using ead_middle_constants::kMuteStopNotes;
using ead_middle_constants::kMuteStopScript;
using ead_middle_constants::kTicksPerBeat;
using ead_middle_constants::kWaveSamples;

namespace later_tables = ead_later_tables;

constexpr uint8_t kPortamentoSpecial = 0x80;

// The samples in one period of each harmonic of a wave.
constexpr std::array<int, 4> kSamplesInPeriod = {kWaveSamples, kWaveSamples / 2, kWaveSamples / 4, kWaveSamples / 8};

} // namespace

uint8_t EadMiddlePlayer::PeekByte(const Player& player, uint32_t place) const
{
    return place < player.bytes.size() ? player.bytes[place] : uint8_t{0};
}

uint16_t EadMiddlePlayer::Peek16(const Player& player, uint32_t place) const
{
    return static_cast<uint16_t>((PeekByte(player, place) << 8) | PeekByte(player, place + 1));
}

uint8_t EadMiddlePlayer::Read8(const Player& player, Script& script) const
{
    return PeekByte(player, script.pc++);
}

uint16_t EadMiddlePlayer::Read16(const Player& player, Script& script) const
{
    const uint16_t value = Peek16(player, script.pc);
    script.pc += 2;

    return value;
}

uint16_t EadMiddlePlayer::ReadVariable(const Player& player, Script& script) const
{
    uint16_t value = Read8(player, script);
    if ((value & 0x80) != 0)
    {
        value = static_cast<uint16_t>(((value << 8) & 0x7F00) | Read8(player, script));
    }

    return value;
}

void EadMiddlePlayer::RunPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (player.muted && (player.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count + player.tempo);
    if (static_cast<int32_t>(player.tempo_count) < max_tempo_)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count - static_cast<uint16_t>(max_tempo_));
    if (player.delay > 1)
    {
        player.delay--;
    }
    else
    {
        player.recalculate_volume = true;
        Script& script = player.script;
        int value = 0;
        while (true)
        {
            const uint8_t command = Read8(player, script);
            if (command == 0xFF)
            {
                if (script.depth == 0)
                {
                    DisablePlayer(index);
                    break;
                }

                script.depth--;
                script.pc = script.stack[script.depth & 3];
            }

            if (command == 0xFD)
            {
                player.delay = ReadVariable(player, script);
                break;
            }

            if (command == 0xFE)
            {
                player.delay = 1;
                break;
            }

            if (command >= 0xC0)
            {
                switch (command)
                {
                case 0xFC:
                    {
                        const uint16_t target = Read16(player, script);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = target;
                        break;
                    }
                case 0xF8:
                    script.loops[script.depth & 3] = Read8(player, script);
                    script.stack[script.depth & 3] = script.pc;
                    script.depth++;
                    break;
                case 0xF7:
                    {
                        const std::size_t top = (script.depth - 1U) & 3U;
                        script.loops[top]--;
                        if (script.loops[top] != 0)
                        {
                            script.pc = script.stack[top];
                        }
                        else
                        {
                            script.depth--;
                        }

                        break;
                    }
                case 0xFB:
                case 0xFA:
                case 0xF9:
                case 0xF5:
                    {
                        const uint16_t target = Read16(player, script);
                        if ((command == 0xFA && value != 0) || (command == 0xF9 && value >= 0) ||
                            (command == 0xF5 && value < 0))
                        {
                            break;
                        }

                        script.pc = target;
                        break;
                    }
                case 0xF4:
                case 0xF3:
                case 0xF2:
                    {
                        const auto relative = static_cast<int8_t>(Read8(player, script));
                        if ((command == 0xF3 && value != 0) || (command == 0xF2 && value >= 0))
                        {
                            break;
                        }

                        script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                        break;
                    }
                case 0xF1:
                    FillPool(PlayerPool(index), Read8(player, script));
                    break;
                case 0xF0:
                    ClearPool(PlayerPool(index));
                    break;
                case 0xDF:
                    player.transpose = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xDE:
                    player.transpose =
                        static_cast<int16_t>(player.transpose + static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xDC:
                case 0xDD:
                    {
                        const uint8_t tempo = Read8(player, script);
                        if (command == 0xDD)
                        {
                            player.tempo = static_cast<uint16_t>(tempo * kTicksPerBeat);
                        }
                        else
                        {
                            player.tempo =
                                static_cast<uint16_t>(player.tempo + static_cast<int8_t>(tempo) * kTicksPerBeat);
                        }

                        if (static_cast<int32_t>(player.tempo) > max_tempo_)
                        {
                            player.tempo = static_cast<uint16_t>(max_tempo_);
                        }

                        if (static_cast<int16_t>(player.tempo) <= 0)
                        {
                            player.tempo = 1;
                        }

                        break;
                    }
                case 0xDA:
                    {
                        const uint8_t mode = Read8(player, script);
                        const uint16_t updates = Read16(player, script);
                        if (mode == 0 || mode == 1)
                        {
                            if (player.state != 2)
                            {
                                player.fade_updates_next = updates;
                                player.state = mode;
                            }
                        }
                        else if (mode == 2)
                        {
                            player.fade_updates = updates;
                            player.state = mode;
                            player.fade_step = (0.0F - player.fade_volume) / static_cast<float>(int32_t{updates});
                        }

                        break;
                    }
                case 0xDB:
                    {
                        const int volume = Read8(player, script);
                        if (player.state == 1)
                        {
                            player.state = 0;
                            player.fade_volume = 0.0F;
                        }

                        if (player.state == 0)
                        {
                            player.fade_updates = player.fade_updates_next;
                            if (player.fade_updates_next != 0)
                            {
                                player.fade_step = (static_cast<float>(volume) / 127.0F - player.fade_volume) /
                                                   static_cast<float>(int32_t{player.fade_updates});
                            }
                            else
                            {
                                player.fade_volume = static_cast<float>(volume) / 127.0F;
                            }
                        }

                        break;
                    }
                case 0xD9:
                    player.fade_scale = static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
                    break;
                case 0xD7:
                    InitChannels(index, Read16(player, script));
                    break;
                case 0xD6:
                    DisableChannels(index, Read16(player, script));
                    break;
                case 0xD5:
                    player.mute_scale = static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
                    break;
                case 0xD4:
                    player.muted = true;
                    break;
                case 0xD3:
                    player.mute_behavior = Read8(player, script);
                    break;
                case 0xD2:
                    player.short_velocities = Read16(player, script);
                    break;
                case 0xD1:
                    player.short_gates = Read16(player, script);
                    break;
                case 0xD0:
                    player.policy = Read8(player, script);
                    break;
                case 0xCC:
                    value = Read8(player, script);
                    break;
                case 0xC9:
                    value &= Read8(player, script);
                    break;
                case 0xC8:
                    value -= Read8(player, script);
                    break;
                default:
                    break;
                }

                continue;
            }

            const int low = command & 0xF;
            switch (command & 0xF0)
            {
            case 0x00:
                {
                    const Channel* channel = ChannelOf(index, low);
                    value = channel != nullptr && channel->finished ? 1 : 0;
                    break;
                }
            case 0x50:
                value -= player.variation;
                break;
            case 0x70:
                player.variation = static_cast<int8_t>(value);
                break;
            case 0x80:
                value = player.variation;
                break;
            case 0x90:
                EnableChannel(index, low, Read16(player, script));
                break;
            default:
                break;
            }
        }
    }

    for (const int channel : player.channels)
    {
        if (channel != kNoChannel)
        {
            RunChannel(channel);
        }
    }
}

void EadMiddlePlayer::RunChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.enabled)
    {
        return;
    }

    const auto run_layers = [&]()
    {
        for (const int layer : channel.layers)
        {
            if (layer != kNoLayer)
            {
                RunLayer(layer);
            }
        }
    };

    if (channel.stop_script)
    {
        run_layers();

        return;
    }

    const int player_index = channel.player;
    Player& player = players_[static_cast<std::size_t>(player_index)];
    if (player.muted && (channel.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    if (channel.delay != 0)
    {
        channel.delay--;
    }

    Script& script = channel.script;
    const auto table_entry = [&](int value)
    {
        return Peek16(player, static_cast<uint32_t>(static_cast<int32_t>(channel.dynamic_table) + value * 2));
    };

    if (channel.delay == 0)
    {
        int8_t value = 0;
        bool running = true;
        while (running)
        {
            uint8_t command = Read8(player, script);
            if (command > 0xC0)
            {
                switch (command)
                {
                case 0xFF:
                    if (script.depth == 0)
                    {
                        DisableChannel(index);
                        running = false;
                    }
                    else
                    {
                        script.depth--;
                        script.pc = script.stack[script.depth & 3];
                    }

                    break;
                case 0xFE:
                    running = false;
                    break;
                case 0xFD:
                    channel.delay = ReadVariable(player, script);
                    running = false;
                    break;
                case 0xEA:
                    channel.stop_script = true;
                    running = false;
                    break;
                case 0xFC:
                    {
                        const uint16_t target = Read16(player, script);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = target;
                        break;
                    }
                case 0xF8:
                    script.loops[script.depth & 3] = Read8(player, script);
                    script.stack[script.depth & 3] = script.pc;
                    script.depth++;
                    break;
                case 0xF7:
                    {
                        const std::size_t top = (script.depth - 1U) & 3U;
                        script.loops[top]--;
                        if (script.loops[top] != 0)
                        {
                            script.pc = script.stack[top];
                        }
                        else
                        {
                            script.depth--;
                        }

                        break;
                    }
                case 0xF6:
                    script.depth--;
                    break;
                case 0xF5:
                case 0xF9:
                case 0xFA:
                case 0xFB:
                    {
                        const uint16_t target = Read16(player, script);
                        if ((command == 0xFA && value != 0) || (command == 0xF9 && value >= 0) ||
                            (command == 0xF5 && value < 0))
                        {
                            break;
                        }

                        script.pc = target;
                        break;
                    }
                case 0xF2:
                case 0xF3:
                case 0xF4:
                    {
                        const auto relative = static_cast<int8_t>(Read8(player, script));
                        if ((command == 0xF3 && value != 0) || (command == 0xF2 && value >= 0))
                        {
                            break;
                        }

                        script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                        break;
                    }
                case 0xF1:
                    FillPool(ChannelPool(index), Read8(player, script));
                    break;
                case 0xF0:
                    ClearPool(ChannelPool(index));
                    break;
                case 0xC2:
                    channel.dynamic_table = Read16(player, script);
                    break;
                case 0xC5:
                    if (value != -1)
                    {
                        channel.dynamic_table = table_entry(value);
                    }

                    break;
                case 0xEB:
                    ChangeBank(channel, player, Read8(player, script));
                    SetProgram(index, Read8(player, script));
                    break;
                case 0xC1:
                    SetProgram(index, Read8(player, script));
                    break;
                case 0xC3:
                    channel.large_notes = false;
                    break;
                case 0xC4:
                    channel.large_notes = true;
                    break;
                case 0xDF:
                    channel.volume = static_cast<float>(Read8(player, script)) / 127.0F;
                    channel.change_volume = true;
                    break;
                case 0xE0:
                    channel.volume_scale = static_cast<float>(Read8(player, script)) / 128.0F;
                    channel.change_volume = true;
                    break;
                case 0xDE:
                    channel.frequency_scale = static_cast<float>(int32_t{Read16(player, script)}) / 32768.0F;
                    channel.change_frequency = true;
                    break;
                case 0xD3:
                    channel.frequency_scale =
                        later_tables::kBendOctave[static_cast<uint8_t>(Read8(player, script) + 127)];
                    channel.change_frequency = true;
                    break;
                case 0xDD:
                    channel.new_pan = Read8(player, script);
                    channel.change_pan = true;
                    break;
                case 0xDC:
                    channel.pan_weight = Read8(player, script);
                    channel.change_pan = true;
                    break;
                case 0xDB:
                    channel.transpose = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xDA:
                    channel.adsr.envelope = {};
                    channel.adsr.envelope.player = player_index;
                    channel.adsr.envelope.offset = Read16(player, script);
                    break;
                case 0xD9:
                    channel.adsr.release = Read8(player, script);
                    break;
                case 0xD8:
                    channel.vibrato_depth_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_depth_start = 0;
                    channel.vibrato_depth_delay = 0;
                    break;
                case 0xD7:
                    channel.vibrato_rate_target = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_start = channel.vibrato_rate_target;
                    channel.vibrato_rate_delay = 0;
                    break;
                case 0xE2:
                    channel.vibrato_depth_start = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_depth_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_depth_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE1:
                    channel.vibrato_rate_start = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_target = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE3:
                    channel.vibrato_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xD4:
                    channel.reverb = Read8(player, script);
                    break;
                case 0xC6:
                    ChangeBank(channel, player, Read8(player, script));
                    break;
                case 0xC7:
                    {
                        const uint8_t add = Read8(player, script);
                        const uint16_t place = Read16(player, script);
                        if (place < player.bytes.size())
                        {
                            player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(value) + add);
                        }

                        break;
                    }
                case 0xC8:
                case 0xC9:
                case 0xCC:
                    {
                        const auto operand = static_cast<int8_t>(Read8(player, script));
                        if (command == 0xC8)
                        {
                            value = static_cast<int8_t>(value - operand);
                        }
                        else if (command == 0xCC)
                        {
                            value = operand;
                        }
                        else
                        {
                            value = static_cast<int8_t>(value & operand);
                        }

                        break;
                    }
                case 0xCA:
                    channel.mute_behavior = Read8(player, script);
                    break;
                case 0xCB:
                    {
                        const int place = Read16(player, script) + value;
                        value = static_cast<int8_t>(PeekByte(player, static_cast<uint32_t>(place)));
                        break;
                    }
                case 0xD0:
                    channel.stereo_effects = Read8(player, script) != 0;
                    break;
                case 0xD1:
                    channel.policy = Read8(player, script);
                    break;
                case 0xD2:
                    channel.adsr.sustain = Read8(player, script);
                    break;
                case 0xE5:
                    channel.reverb_index = Read8(player, script);
                    break;
                case 0xE4:
                    if (value != -1)
                    {
                        const uint16_t target = table_entry(value);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = target;
                    }

                    break;
                case 0xE6:
                    channel.book_offset = Read8(player, script);
                    break;
                case 0xE7:
                case 0xE8:
                    {
                        // The settings lie in a table in the sequence, or follow the command.
                        Script from = script;
                        if (command == 0xE7)
                        {
                            from.pc = Read16(player, script);
                        }

                        channel.mute_behavior = Read8(player, from);
                        channel.policy = Read8(player, from);
                        channel.priority = Read8(player, from);
                        channel.transpose = static_cast<int8_t>(Read8(player, from));
                        channel.new_pan = Read8(player, from);
                        channel.pan_weight = Read8(player, from);
                        channel.reverb = Read8(player, from);
                        channel.reverb_index = Read8(player, from);
                        channel.change_pan = true;
                        if (command == 0xE8)
                        {
                            script.pc = from.pc;
                        }

                        break;
                    }
                case 0xEC:
                    channel.vibrato_depth_target = 0;
                    channel.vibrato_depth_start = 0;
                    channel.vibrato_depth_delay = 0;
                    channel.vibrato_rate_target = 0;
                    channel.vibrato_rate_start = 0;
                    channel.vibrato_rate_delay = 0;
                    channel.frequency_scale = 1.0F;
                    break;
                case 0xE9:
                    channel.priority = Read8(player, script);
                    break;
                case 0xEF:
                    Read16(player, script);
                    Read8(player, script);
                    break;
                default:
                    break;
                }

                continue;
            }

            const int low = command & 0xF;
            switch (command & 0xF0)
            {
            case 0x00:
                if (low < kLayersPerChannel && channel.layers[static_cast<std::size_t>(low)] != kNoLayer)
                {
                    value = layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].finished
                                ? int8_t{1}
                                : int8_t{0};
                }
                else
                {
                    value = -1;
                }

                break;
            case 0x70:
                channel.ports[static_cast<std::size_t>(low & 7)] = value;
                break;
            case 0x80:
                value = channel.ports[static_cast<std::size_t>(low & 7)];
                if (low < 4)
                {
                    channel.ports[static_cast<std::size_t>(low)] = -1;
                }

                break;
            case 0x50:
                value = static_cast<int8_t>(value - channel.ports[static_cast<std::size_t>(low & 7)]);
                break;
            case 0x60:
                channel.delay = static_cast<uint16_t>(low);
                running = false;
                break;
            case 0x90:
                {
                    const uint16_t target = Read16(player, script);
                    if (low < kLayersPerChannel && SetLayer(index, low))
                    {
                        layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                            target;
                    }

                    break;
                }
            case 0xA0:
                if (low < kLayersPerChannel)
                {
                    FreeLayer(index, low);
                }

                break;
            case 0xB0:
                if (value != -1 && low < kLayersPerChannel && SetLayer(index, low))
                {
                    layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                        table_entry(value);
                }

                break;
            case 0x10:
                EnableChannel(player_index, low, Read16(player, script));
                break;
            case 0x20:
                if (const int other = player.channels[static_cast<std::size_t>(low)]; other != kNoChannel)
                {
                    DisableChannel(other);
                }

                break;
            case 0x30:
                {
                    const uint8_t port = Read8(player, script);
                    if (Channel* other = ChannelOf(player_index, low); other != nullptr && port < kPorts)
                    {
                        other->ports[port] = value;
                    }

                    break;
                }
            case 0x40:
                {
                    const uint8_t port = Read8(player, script);
                    const Channel* other = ChannelOf(player_index, low);
                    value = other != nullptr && port < kPorts ? other->ports[port] : int8_t{0};
                    break;
                }
            default:
                break;
            }
        }
    }

    run_layers();
}

void EadMiddlePlayer::RunLayer(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (!layer.enabled)
    {
        return;
    }

    if (layer.delay > 1)
    {
        layer.delay--;
        if (!layer.muted && layer.delay <= layer.gate_delay)
        {
            DecayOrRelease(index, false);
            layer.muted = true;
        }

        return;
    }

    if (!layer.continuous)
    {
        DecayOrRelease(index, false);
    }

    const int old_mode = layer.portamento.mode & ~kPortamentoSpecial;
    if (old_mode == 1 || old_mode == 2)
    {
        layer.portamento.mode = 0;
    }

    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    layer.properties_need_init = true;
    uint32_t command_place = 0;
    uint8_t command = 0;
    while (true)
    {
        command_place = script.pc;
        command = Read8(player, script);
        if (command <= 0xC0)
        {
            break;
        }

        switch (command)
        {
        case 0xFF:
            if (script.depth == 0)
            {
                DisableLayer(index);

                return;
            }

            script.depth--;
            script.pc = script.stack[script.depth & 3];
            break;
        case 0xFC:
            {
                const uint16_t target = Read16(player, script);
                script.stack[script.depth & 3] = script.pc;
                script.depth++;
                script.pc = target;
                break;
            }
        case 0xF8:
            script.loops[script.depth & 3] = Read8(player, script);
            script.stack[script.depth & 3] = script.pc;
            script.depth++;
            break;
        case 0xF7:
            {
                const std::size_t top = (script.depth - 1U) & 3U;
                script.loops[top]--;
                if (script.loops[top] != 0)
                {
                    script.pc = script.stack[top];
                }
                else
                {
                    script.depth--;
                }

                break;
            }
        case 0xFB:
            script.pc = Read16(player, script);
            break;
        case 0xF4:
            {
                const auto relative = static_cast<int8_t>(Read8(player, script));
                script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                break;
            }
        case 0xC1:
            {
                const int value = Read8(player, script);
                layer.velocity_square = static_cast<float>(value * value) / VelocityDivisor();
                break;
            }
        case 0xCA:
            layer.pan = Read8(player, script);
            break;
        case 0xC2:
            layer.transpose = Read8(player, script);
            break;
        case 0xC9:
            layer.gate = Read8(player, script);
            break;
        case 0xC4:
        case 0xC5:
            layer.continuous = command == 0xC4;
            DecayOrRelease(index, false);
            break;
        case 0xC3:
            layer.default_length = static_cast<int16_t>(ReadVariable(player, script));
            break;
        case 0xC6:
            {
                const uint8_t program = Read8(player, script);
                if (program >= 0x7F)
                {
                    if (program == 0x7F)
                    {
                        layer.program = 0;
                    }
                    else
                    {
                        layer.program = program;
                        layer.instrument = nullptr;
                    }

                    if (program == 0xFF)
                    {
                        layer.adsr.release = 0;
                    }
                }
                else
                {
                    layer.program = FindProgram(channel, program, layer.instrument, layer.adsr);
                    if (layer.program == 0)
                    {
                        layer.program = 0xFF;
                    }
                }

                break;
            }
        case 0xC7:
            {
                layer.portamento.mode = Read8(player, script);
                auto note = static_cast<uint8_t>(Read8(player, script) + channel.transpose + layer.transpose +
                                                 player.transpose);
                if (note >= 0x80)
                {
                    note = 0;
                }

                layer.portamento_target = note;
                if ((layer.portamento.mode & kPortamentoSpecial) != 0)
                {
                    layer.portamento_time = Read8(player, script);
                }
                else
                {
                    layer.portamento_time = ReadVariable(player, script);
                }

                break;
            }
        case 0xC8:
            layer.portamento.mode = 0;
            break;
        case 0xCB:
            layer.adsr.envelope = {};
            layer.adsr.envelope.player = channel.player;
            layer.adsr.envelope.offset = Read16(player, script);
            layer.adsr.release = Read8(player, script);
            break;
        case 0xCC:
            layer.ignore_drum_pan = true;
            break;
        default:
            if ((command & 0xF0) == 0xD0)
            {
                const int place = player.short_velocities;
                const int value = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                             : later_tables::kShortVelocity[command & 0xFU];
                layer.velocity_square = static_cast<float>(value * value) / VelocityDivisor();
            }
            else if ((command & 0xF0) == 0xE0)
            {
                const int place = player.short_gates;
                layer.gate = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                        : later_tables::kShortGate[command & 0xFU];
            }

            break;
        }
    }

    bool same_sound = true;
    uint8_t note = 0;
    bool is_drum = false;
    if (command == 0xC0)
    {
        layer.delay = static_cast<int16_t>(ReadVariable(player, script));
        layer.muted = true;
    }
    else
    {
        layer.muted = false;
        uint16_t delay = 0;
        if (channel.large_notes)
        {
            int velocity = 0;
            switch (command & 0xC0)
            {
            case 0x00:
                delay = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                layer.last_length = static_cast<int16_t>(delay);
                break;
            case 0x40:
                delay = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = 0;
                layer.last_length = static_cast<int16_t>(delay);
                break;
            default:
                delay = static_cast<uint16_t>(layer.last_length);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                break;
            }

            if (velocity >= 0x80)
            {
                velocity = 0x7F;
            }

            layer.velocity_square = static_cast<float>(velocity) * static_cast<float>(velocity) / VelocityDivisor();
        }
        else
        {
            switch (command & 0xC0)
            {
            case 0x00:
                delay = ReadVariable(player, script);
                layer.last_length = static_cast<int16_t>(delay);
                break;
            case 0x40:
                delay = static_cast<uint16_t>(layer.default_length);
                break;
            default:
                delay = static_cast<uint16_t>(layer.last_length);
                break;
            }
        }

        command = static_cast<uint8_t>(command & 0x3F);
        layer.delay = static_cast<int16_t>(delay);
        layer.gate_delay = static_cast<int16_t>((layer.gate * int32_t{delay}) >> 8);
        if ((player.muted && (channel.mute_behavior & kMuteStopNotes) != 0) || channel.stop_notes)
        {
            layer.muted = true;
        }
        else
        {
            int program = layer.program;
            if (program == 0xFF)
            {
                if (!channel.has_instrument)
                {
                    return;
                }

                program = channel.program;
            }

            if (program == 0)
            {
                is_drum = true;
                note = static_cast<uint8_t>(command + channel.transpose + layer.transpose);
                const EadDrum* drum = Drum(channel.bank, note);
                if (drum == nullptr)
                {
                    layer.muted = true;

                    return;
                }

                layer.adsr.envelope = BankEnvelope(channel.bank, drum->envelope);
                layer.adsr.release = drum->release_rate;
                if (!layer.ignore_drum_pan)
                {
                    layer.pan = drum->pan;
                }

                layer.sound = &drum->sound;
                layer.sound_bank = channel.bank;
                layer.frequency_scale = drum->sound.tuning;
            }
            else
            {
                note = static_cast<uint8_t>(command + player.transpose + channel.transpose + layer.transpose);
                if (note >= 0x80)
                {
                    layer.muted = true;
                }
                else
                {
                    const EadInstrument* instrument = layer.program == 0xFF ? channel.instrument : layer.instrument;
                    const auto sound_of = [](const EadInstrument& held, int key) -> const EadTunedSample*
                    {
                        if (key < held.normal_range_low)
                        {
                            return &held.low;
                        }

                        return key <= held.normal_range_high ? &held.normal : &held.high;
                    };

                    if (layer.portamento.mode != 0)
                    {
                        const int highest = layer.portamento_target < note ? note : layer.portamento_target;
                        float tuning = 1.0F;
                        if (instrument != nullptr)
                        {
                            const EadTunedSample* sound = sound_of(*instrument, highest);
                            same_sound = sound == layer.sound;
                            layer.sound = sound;
                            layer.sound_bank = channel.bank;
                            tuning = sound->tuning;
                        }
                        else
                        {
                            layer.sound = nullptr;
                        }

                        const float from_note = ead_tables::kNoteFrequencies[note] * tuning;
                        const float to_note = ead_tables::kNoteFrequencies[layer.portamento_target] * tuning;
                        const int mode = layer.portamento.mode & ~kPortamentoSpecial;
                        const float base = mode == 1 || mode == 3 || mode == 5 ? to_note : from_note;
                        layer.portamento.extent = from_note / base - 1.0F;
                        if ((layer.portamento.mode & kPortamentoSpecial) != 0)
                        {
                            layer.portamento.speed = 32512.0F * static_cast<float>(player.tempo) /
                                                     (static_cast<float>(layer.delay) * static_cast<float>(max_tempo_) *
                                                      static_cast<float>(layer.portamento_time));
                        }
                        else
                        {
                            layer.portamento.speed = 127.0F / static_cast<float>(layer.portamento_time);
                        }

                        layer.portamento.position = 0.0F;
                        layer.frequency_scale = base;
                        if (mode == 5)
                        {
                            layer.portamento_target = note;
                        }
                    }
                    else if (instrument != nullptr)
                    {
                        const EadTunedSample* sound = sound_of(*instrument, note);
                        same_sound = sound == layer.sound;
                        layer.sound = sound;
                        layer.sound_bank = channel.bank;
                        layer.frequency_scale = ead_tables::kNoteFrequencies[note] * sound->tuning;
                    }
                    else
                    {
                        layer.sound = nullptr;
                        layer.frequency_scale = ead_tables::kNoteFrequencies[note];
                    }
                }
            }
        }
    }

    if (layer.muted)
    {
        if (layer.voice != kNoVoice || layer.continuous)
        {
            DecayOrRelease(index, false);
        }

        return;
    }

    bool take_voice = false;
    if (!layer.continuous || layer.voice == kNoVoice || !layer.loaded)
    {
        take_voice = true;
    }
    else if (!same_sound)
    {
        DecayOrRelease(index, false);
        take_voice = true;
    }
    else if (index != voices_[static_cast<std::size_t>(layer.voice)].layer)
    {
        take_voice = true;
    }
    else if (layer.sound == nullptr)
    {
        Voice& voice = voices_[static_cast<std::size_t>(layer.voice)];
        const int previous = voice.harmonic;
        const int wave = layer.program == 0xFF ? channel.program : layer.program;
        const int current = BuildWave(voice, layer, wave);
        voice.position = voice.position * kSamplesInPeriod[static_cast<std::size_t>(current)] /
                         kSamplesInPeriod[static_cast<std::size_t>(previous)];
    }

    if (take_voice)
    {
        layer.voice = AllocVoice(index);
    }

    if (layer.voice != kNoVoice && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        InitVibrato(layer.voice);
    }

    if (layer.voice != kNoVoice)
    {
        for (int i = 0; i < kChannelsPerPlayer; i++)
        {
            if (player.channels[static_cast<std::size_t>(i)] == layer.channel)
            {
                voices_[static_cast<std::size_t>(layer.voice)].track = channel.player * kChannelsPerPlayer + i;
            }
        }
    }

    if (on_note)
    {
        EadNoteStart start;
        start.player = channel.player;
        for (int i = 0; i < kChannelsPerPlayer; i++)
        {
            if (player.channels[static_cast<std::size_t>(i)] == layer.channel)
            {
                start.channel = i;
            }
        }

        for (int i = 0; i < kLayersPerChannel; i++)
        {
            if (channel.layers[static_cast<std::size_t>(i)] == index)
            {
                start.layer = i;
            }
        }

        start.key = note;
        start.drum = is_drum;
        start.bank = channel.bank;
        start.program = layer.program == 0xFF ? channel.program : layer.program;
        start.velocity = layer.velocity_square;
        start.pan = static_cast<float>(layer.pan) / 127.0F;
        start.length = layer.delay;
        start.gate = layer.gate_delay;
        start.offset = command_place;
        start.update = updates_;
        start.voice = layer.voice;
        on_note(start);
    }
}

} // namespace ntdlib::n64
