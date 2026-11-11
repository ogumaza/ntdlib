// SPDX-License-Identifier: MIT

// The scripts in the model of the early EAD engine: the reading of the sequence bytes, and the commands of the
// players, channels and layers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ead_early_constants.h"
#include "ead_tables.h"
#include "ntdlib/n64/ead_early.h"

namespace ntdlib::n64
{

namespace
{

using ead_early_constants::kMuteStopNotes;
using ead_early_constants::kMuteStopScript;
using ead_early_constants::kStateFadeIn;
using ead_early_constants::kStateFadeOut;
using ead_early_constants::kStateFadeStay;
using ead_early_constants::kTatumsPerBeat;

namespace tables = ead_tables;

constexpr uint8_t PortamentoMode(uint8_t mode)
{
    return mode & 0x7F;
}

} // namespace

uint8_t EadEarlyPlayer::PeekByte(const Player& player, uint32_t place) const
{
    return place < player.bytes.size() ? player.bytes[place] : uint8_t{0};
}

uint8_t EadEarlyPlayer::Read8(const Player& player, Script& script) const
{
    return PeekByte(player, script.pc++);
}

uint16_t EadEarlyPlayer::Read16(const Player& player, Script& script) const
{
    const uint8_t high = Read8(player, script);
    const uint8_t low = Read8(player, script);

    return static_cast<uint16_t>((high << 8) | low);
}

uint16_t EadEarlyPlayer::ReadVariable(const Player& player, Script& script) const
{
    uint16_t value = Read8(player, script);
    if ((value & 0x80) != 0)
    {
        value = static_cast<uint16_t>((value << 8) & 0x7F00);
        value = static_cast<uint16_t>(value | Read8(player, script));
    }

    return value;
}

void EadEarlyPlayer::RunPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (!player.enabled)
    {
        return;
    }

    if (player.muted && (player.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count + player.tempo);
    if (player.tempo_count < tempo_limit_)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count - static_cast<uint16_t>(tempo_limit_));

    Script& script = player.script;
    int32_t value = 0;
    if (player.delay > 1)
    {
        player.delay--;
    }
    else
    {
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
                        uint8_t& loops = script.loops[(script.depth - 1) & 3];
                        loops--;
                        if (loops != 0)
                        {
                            script.pc = script.stack[(script.depth - 1) & 3];
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
                case 0xF2:
                    ClearPool(PlayerPool(index));
                    FillPool(PlayerPool(index), Read8(player, script));
                    break;
                case 0xF1:
                    ClearPool(PlayerPool(index));
                    break;
                case 0xDF:
                    player.transpose = static_cast<int16_t>(static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xDE:
                    player.transpose =
                        static_cast<int16_t>(player.transpose + static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xDD:
                case 0xDC:
                    {
                        const uint8_t amount = Read8(player, script);
                        if (command == 0xDD)
                        {
                            player.tempo = static_cast<uint16_t>(amount * kTatumsPerBeat);
                        }
                        else
                        {
                            player.tempo =
                                static_cast<uint16_t>(player.tempo + static_cast<int8_t>(amount) * kTatumsPerBeat);
                        }

                        if (player.tempo > tempo_limit_)
                        {
                            player.tempo = static_cast<uint16_t>(tempo_limit_);
                        }

                        if (static_cast<int16_t>(player.tempo) <= 0)
                        {
                            player.tempo = 1;
                        }

                        break;
                    }
                case 0xDB:
                    {
                        const float target = static_cast<float>(Read8(player, script)) / 127.0F;
                        switch (player.state)
                        {
                        case kStateFadeIn:
                            if (player.fade_updates != 0)
                            {
                                player.fade_step = (target - player.fade_volume) /
                                                   static_cast<float>(static_cast<int32_t>(player.fade_updates));
                                break;
                            }

                            player.fade_volume = target;
                            break;
                        case 0:
                            player.fade_volume = target;
                            break;
                        case kStateFadeOut:
                        case kStateFadeStay:
                            player.volume = target;
                            break;
                        default:
                            break;
                        }

                        break;
                    }
                case 0xDA:
                    player.fade_volume += static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
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
                    value = Read8(player, script) & value;
                    break;
                case 0xC8:
                    value = value - Read8(player, script);
                    break;
                default:
                    break;
                }
            }
            else
            {
                const int low = command & 0xF;
                switch (command & 0xF0)
                {
                case 0x00:
                    {
                        const int slot = player.channels[static_cast<std::size_t>(low)];
                        if (slot >= 0)
                        {
                            value = channels_[static_cast<std::size_t>(slot)].finished ? 1 : 0;
                        }

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
    }

    for (int i = 0; i < kChannelsPerPlayer; i++)
    {
        const int slot = player.channels[static_cast<std::size_t>(i)];
        if (slot >= 0)
        {
            RunChannel(slot);
        }
    }
}

void EadEarlyPlayer::RunChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.enabled)
    {
        return;
    }

    const auto run_layers = [this, &channel]()
    {
        for (int i = 0; i < kLayersPerChannel; i++)
        {
            const int layer = channel.layers[static_cast<std::size_t>(i)];
            if (layer != -1)
            {
                RunLayer(layer);
            }
        }
    };

    if (channel.script_stopped)
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
    int8_t value = 0;
    if (channel.delay == 0)
    {
        while (true)
        {
            const uint8_t command = Read8(player, script);
            if (command == 0xFF)
            {
                if (script.depth == 0)
                {
                    DisableChannel(index);
                    break;
                }

                script.depth--;
                script.pc = script.stack[script.depth & 3];
            }

            if (command == 0xFE)
            {
                break;
            }

            if (command == 0xFD)
            {
                channel.delay = ReadVariable(player, script);
                break;
            }

            if (command == 0xF3)
            {
                channel.script_stopped = true;
                break;
            }

            if (command > 0xC0)
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
                        uint8_t& loops = script.loops[(script.depth - 1) & 3];
                        loops--;
                        if (loops != 0)
                        {
                            script.pc = script.stack[(script.depth - 1) & 3];
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
                case 0xF2:
                    ClearPool(ChannelPool(index));
                    FillPool(ChannelPool(index), Read8(player, script));
                    break;
                case 0xF1:
                    ClearPool(ChannelPool(index));
                    break;
                case 0xC2:
                    channel.dynamic_table = Read16(player, script);
                    break;
                case 0xC5:
                    if (value != -1)
                    {
                        const uint32_t entry = channel.dynamic_table + static_cast<uint32_t>(int32_t{value} * 2);
                        channel.dynamic_table =
                            static_cast<uint16_t>((PeekByte(player, entry) << 8) + PeekByte(player, entry + 1));
                    }

                    break;
                case 0xC1:
                    SetInstrument(index, Read8(player, script));
                    break;
                case 0xC3:
                    channel.large_notes = false;
                    break;
                case 0xC4:
                    channel.large_notes = true;
                    break;
                case 0xDF:
                    channel.volume = static_cast<float>(Read8(player, script)) / 127.0F;
                    break;
                case 0xE0:
                    channel.volume_scale = static_cast<float>(Read8(player, script)) / 128.0F;
                    break;
                case 0xDE:
                    channel.frequency_scale =
                        static_cast<float>(static_cast<int32_t>(Read16(player, script))) / 32768.0F;
                    break;
                case 0xD3:
                    {
                        const auto bend = static_cast<uint8_t>(Read8(player, script) + 127);
                        channel.frequency_scale =
                            bend < tables::kBendScale.size() ? tables::kBendScale[bend] : tables::kNoteFrequencies[0];
                        break;
                    }
                case 0xDD:
                    channel.pan = static_cast<float>(Read8(player, script)) / 128.0F;
                    break;
                case 0xDC:
                    channel.pan_weight = static_cast<float>(Read8(player, script)) / 128.0F;
                    break;
                case 0xDB:
                    channel.transpose = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xDA:
                    channel.adsr.envelope = {.player = player_index, .offset = Read16(player, script)};
                    break;
                case 0xD9:
                    channel.adsr.release = Read8(player, script);
                    break;
                case 0xD8:
                    channel.vibrato_extent_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_extent_start = 0;
                    channel.vibrato_extent_delay = 0;
                    break;
                case 0xD7:
                    {
                        const auto rate = static_cast<uint16_t>(Read8(player, script) * 32);
                        channel.vibrato_rate_start = rate;
                        channel.vibrato_rate_target = rate;
                        channel.vibrato_rate_delay = 0;
                        break;
                    }
                case 0xE2:
                    channel.vibrato_extent_start = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_extent_target = static_cast<uint16_t>(Read8(player, script) * 8);
                    channel.vibrato_extent_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE1:
                    channel.vibrato_rate_start = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_target = static_cast<uint16_t>(Read8(player, script) * 32);
                    channel.vibrato_rate_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xE3:
                    channel.vibrato_delay = static_cast<uint16_t>(Read8(player, script) * 16);
                    break;
                case 0xD6:
                    Read8(player, script);
                    break;
                case 0xD4:
                    channel.reverb = Read8(player, script);
                    break;
                case 0xC6:
                    {
                        const uint8_t from_back = Read8(player, script);
                        const std::vector<uint8_t>& set = files_->bank_sets[player.sequence];
                        if (from_back < set.size())
                        {
                            channel.bank = set[set.size() - 1 - from_back];
                        }

                        break;
                    }
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
                    value = static_cast<int8_t>(value - static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xC9:
                    value = static_cast<int8_t>(value & static_cast<int8_t>(Read8(player, script)));
                    break;
                case 0xCC:
                    value = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xCA:
                    channel.mute_behavior = Read8(player, script);
                    break;
                case 0xCB:
                    {
                        const int32_t place = int32_t{Read16(player, script)} + value;
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
                    channel.adsr.sustain = static_cast<uint16_t>(Read8(player, script) << 8);
                    break;
                case 0xE4:
                    if (value != -1)
                    {
                        const uint32_t entry = channel.dynamic_table + static_cast<uint32_t>(int32_t{value} * 2);
                        script.stack[script.depth & 3] = script.pc;
                        script.depth++;
                        script.pc = static_cast<uint16_t>((PeekByte(player, entry) << 8) + PeekByte(player, entry + 1));
                    }

                    break;
                default:
                    break;
                }
            }
            else
            {
                const int low = command & 0xF;
                const auto layer_slot = static_cast<std::size_t>(low & 3);
                const bool real_slot = low < kLayersPerChannel;
                switch (command & 0xF0)
                {
                case 0x00:
                    if (real_slot && channel.layers[layer_slot] != -1)
                    {
                        value = layers_[static_cast<std::size_t>(channel.layers[layer_slot])].finished ? 1 : 0;
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
                case 0x90:
                    {
                        const uint16_t target = Read16(player, script);
                        if (real_slot && SetLayer(index, low) == 0)
                        {
                            layers_[static_cast<std::size_t>(channel.layers[layer_slot])].script.pc = target;
                        }

                        break;
                    }
                case 0xA0:
                    if (real_slot)
                    {
                        FreeLayer(index, low);
                    }

                    break;
                case 0xB0:
                    if (value != -1 && real_slot && SetLayer(index, low) != -1)
                    {
                        const uint32_t entry = channel.dynamic_table + static_cast<uint32_t>(int32_t{value} * 2);
                        layers_[static_cast<std::size_t>(channel.layers[layer_slot])].script.pc =
                            static_cast<uint16_t>((PeekByte(player, entry) << 8) + PeekByte(player, entry + 1));
                    }

                    break;
                case 0x60:
                    channel.priority = static_cast<uint8_t>(low);
                    break;
                case 0x10:
                    EnableChannel(player_index, low, Read16(player, script));
                    break;
                case 0x20:
                    {
                        const int other = player.channels[static_cast<std::size_t>(low)];
                        if (other >= 0)
                        {
                            DisableChannel(other);
                        }

                        break;
                    }
                case 0x30:
                    {
                        const uint8_t port = Read8(player, script);
                        const int other = player.channels[static_cast<std::size_t>(low)];
                        if (other >= 0)
                        {
                            channels_[static_cast<std::size_t>(other)].ports[port & 7] = value;
                        }

                        break;
                    }
                case 0x40:
                    {
                        const uint8_t port = Read8(player, script);
                        const int other = player.channels[static_cast<std::size_t>(low)];
                        if (other >= 0)
                        {
                            value = channels_[static_cast<std::size_t>(other)].ports[port & 7];
                        }

                        break;
                    }
                default:
                    break;
                }
            }
        }
    }

    run_layers();
}

void EadEarlyPlayer::RunLayer(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    if (!layer.enabled)
    {
        return;
    }

    if (layer.delay > 1)
    {
        layer.delay--;
        if (!layer.silent && layer.delay <= layer.duration)
        {
            DecayOrRelease(index, AdsrPhase::kDecay);
            layer.silent = true;
        }

        return;
    }

    if (!layer.continuous)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }

    if (PortamentoMode(layer.portamento.mode) == 1 || PortamentoMode(layer.portamento.mode) == 2)
    {
        layer.portamento.mode = 0;
    }

    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    bool same_sound = true;
    uint8_t command = 0;
    uint32_t command_place = 0;
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
                uint8_t& loops = script.loops[(script.depth - 1) & 3];
                loops--;
                if (loops != 0)
                {
                    script.pc = script.stack[(script.depth - 1) & 3];
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
        case 0xC1:
            {
                const uint8_t velocity = Read8(player, script);
                layer.velocity_square = static_cast<float>(int32_t{velocity} * velocity);
                break;
            }
        case 0xCA:
            layer.pan = static_cast<float>(Read8(player, script)) / 128.0F;
            break;
        case 0xC2:
            layer.transpose = static_cast<int16_t>(Read8(player, script));
            break;
        case 0xC9:
            layer.gate = Read8(player, script);
            break;
        case 0xC4:
        case 0xC5:
            layer.continuous = command == 0xC4;
            DecayOrRelease(index, AdsrPhase::kDecay);
            break;
        case 0xC3:
            layer.default_length = static_cast<int16_t>(ReadVariable(player, script));
            break;
        case 0xC6:
            {
                const uint8_t program = Read8(player, script);
                if (program < 127)
                {
                    FindInstrument(channel, program, layer.instrument, layer.adsr);
                }

                break;
            }
        case 0xC7:
            {
                layer.portamento.mode = Read8(player, script);
                auto target = static_cast<uint8_t>(Read8(player, script) + channel.transpose + layer.transpose +
                                                   player.transpose);
                if (target >= 0x80)
                {
                    target = 0;
                }

                layer.portamento_target = target;
                if ((layer.portamento.mode & 0x80) != 0)
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
        default:
            if ((command & 0xF0) == 0xD0)
            {
                const auto from_table = static_cast<std::size_t>(command & 0xF);
                const int velocity = player.short_velocities >= 0
                                         ? PeekByte(player, static_cast<uint32_t>(player.short_velocities) +
                                                                static_cast<uint32_t>(from_table))
                                         : tables::kShortVelocity[from_table];
                layer.velocity_square = static_cast<float>(static_cast<uint16_t>(velocity * velocity));
            }
            else if ((command & 0xF0) == 0xE0)
            {
                const auto from_table = static_cast<std::size_t>(command & 0xF);
                layer.gate = player.short_gates >= 0 ? PeekByte(player, static_cast<uint32_t>(player.short_gates) +
                                                                            static_cast<uint32_t>(from_table))
                                                     : tables::kShortGate[from_table];
            }

            break;
        }
    }

    EadNoteStart start;
    if (command == 0xC0)
    {
        layer.delay = static_cast<int16_t>(ReadVariable(player, script));
        layer.silent = true;
    }
    else
    {
        layer.silent = false;
        uint16_t length = 0;
        if (channel.large_notes)
        {
            int velocity = 0;
            switch (command & 0xC0)
            {
            case 0x00:
                length = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                layer.last_length = static_cast<int16_t>(length);
                break;
            case 0x40:
                length = ReadVariable(player, script);
                velocity = Read8(player, script);
                layer.gate = 0;
                layer.last_length = static_cast<int16_t>(length);
                break;
            default:
                length = static_cast<uint16_t>(layer.last_length);
                velocity = Read8(player, script);
                layer.gate = Read8(player, script);
                break;
            }

            layer.velocity_square = static_cast<float>(velocity * velocity);
        }
        else
        {
            switch (command & 0xC0)
            {
            case 0x00:
                length = ReadVariable(player, script);
                layer.last_length = static_cast<int16_t>(length);
                break;
            case 0x40:
                length = static_cast<uint16_t>(layer.default_length);
                break;
            default:
                length = static_cast<uint16_t>(layer.last_length);
                break;
            }
        }

        auto semitone = static_cast<uint8_t>(command - (command & 0xC0));
        layer.delay = static_cast<int16_t>(length);
        layer.duration = static_cast<int16_t>(layer.gate * int32_t{length} / 256);
        if ((player.muted && (channel.mute_behavior & kMuteStopNotes) != 0) || !channel.has_instrument)
        {
            layer.silent = true;
        }
        else if (channel.program == 0)
        {
            // A drum.
            semitone = static_cast<uint8_t>(semitone + channel.transpose + layer.transpose);
            const EadBank& bank = files_->banks[channel.bank];
            bool have_drum = true;
            if (semitone >= bank.drum_notes.size())
            {
                if (bank.drum_notes.empty())
                {
                    layer.silent = true;
                    have_drum = false;
                }
                else
                {
                    semitone = static_cast<uint8_t>(bank.drum_notes.size() - 1);
                }
            }

            if (have_drum)
            {
                const int object = bank.drum_notes[semitone];
                if (object == kNoObject)
                {
                    layer.silent = true;
                }
                else
                {
                    const EadDrum& drum = bank.drums[static_cast<std::size_t>(object)];
                    layer.adsr.envelope = {};
                    if (drum.envelope != kNoObject)
                    {
                        layer.adsr.envelope.bank = &bank.envelopes[static_cast<std::size_t>(drum.envelope)];
                    }

                    layer.adsr.release = drum.release_rate;
                    layer.pan = static_cast<float>(drum.pan) / 128.0F;
                    layer.sound = &drum.sound;
                    layer.frequency_scale = drum.sound.tuning;
                }
            }

            start.drum = true;
        }
        else
        {
            semitone = static_cast<uint8_t>(semitone + player.transpose + channel.transpose + layer.transpose);
            if (semitone >= 0x80)
            {
                layer.silent = true;
            }
            else
            {
                const EadInstrument* instrument = layer.instrument != nullptr ? layer.instrument : channel.instrument;
                const auto choose_sound = [](const EadInstrument& from, int key) -> const EadTunedSample*
                {
                    if (key < from.normal_range_low)
                    {
                        return &from.low;
                    }

                    return key <= from.normal_range_high ? &from.normal : &from.high;
                };

                if (layer.portamento.mode != 0)
                {
                    const uint8_t higher = std::max(layer.portamento_target, semitone);
                    float tuning = 1.0F;
                    if (instrument != nullptr)
                    {
                        const EadTunedSample* sound = choose_sound(*instrument, higher);
                        same_sound = sound == layer.sound;
                        layer.sound = sound;
                        tuning = sound->tuning;
                    }
                    else
                    {
                        layer.sound = nullptr;
                    }

                    const float from_note = tables::kNoteFrequencies[semitone] * tuning;
                    const float to_note = tables::kNoteFrequencies[layer.portamento_target] * tuning;
                    float base = 0.0F;
                    float other = 0.0F;
                    switch (PortamentoMode(layer.portamento.mode))
                    {
                    case 1:
                    case 3:
                    case 5:
                        other = from_note;
                        base = to_note;
                        break;
                    case 2:
                    case 4:
                        base = from_note;
                        other = to_note;
                        break;
                    default:
                        break;
                    }

                    layer.portamento.extent = other / base - 1.0F;
                    if ((layer.portamento.mode & 0x80) != 0)
                    {
                        layer.portamento.speed = 32512.0F * static_cast<float>(static_cast<int32_t>(player.tempo)) /
                                                 (static_cast<float>(layer.delay) * static_cast<float>(tempo_limit_) *
                                                  static_cast<float>(static_cast<int32_t>(layer.portamento_time)));
                    }
                    else
                    {
                        layer.portamento.speed =
                            127.0F / static_cast<float>(static_cast<int32_t>(layer.portamento_time));
                    }

                    layer.portamento.position = 0.0F;
                    layer.frequency_scale = base;
                    if (PortamentoMode(layer.portamento.mode) == 5)
                    {
                        layer.portamento_target = semitone;
                    }
                }
                else if (instrument != nullptr)
                {
                    const EadTunedSample* sound = choose_sound(*instrument, semitone);
                    same_sound = sound == layer.sound;
                    layer.sound = sound;
                    layer.frequency_scale = tables::kNoteFrequencies[semitone] * sound->tuning;
                }
                else
                {
                    layer.sound = nullptr;
                    layer.frequency_scale = tables::kNoteFrequencies[semitone];
                }
            }
        }

        start.key = semitone;
        start.length = length;
        start.gate = layer.duration;
    }

    if (layer.silent)
    {
        if (layer.voice != -1 || layer.continuous)
        {
            DecayOrRelease(index, AdsrPhase::kDecay);
        }

        return;
    }

    bool allocate = false;
    if (!layer.continuous)
    {
        allocate = true;
    }
    else if (layer.voice == -1 || !layer.loaded)
    {
        allocate = true;
    }
    else if (!same_sound)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
        allocate = true;
    }
    else if (layer.sound == nullptr)
    {
        InitWave(voices_[static_cast<std::size_t>(layer.voice)], layer);
    }

    if (allocate)
    {
        layer.voice = AllocVoice(index);
    }

    if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        InitVibrato(layer.voice);
    }

    if (layer.voice != -1)
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

        start.bank = channel.bank;
        start.program = channel.program;
        start.velocity = layer.velocity_square;
        start.pan = layer.pan;
        start.offset = command_place;
        start.update = updates_;
        start.voice = layer.voice;
        on_note(start);
    }
}

} // namespace ntdlib::n64
