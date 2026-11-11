// SPDX-License-Identifier: MIT

// The scripts of the players and channels in the model of the later EAD engine: the reading of a script's bytes, the
// flow commands, and the commands of the players and channels.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ead_later_constants.h"
#include "ead_later_tables.h"
#include "ead_lists.h"
#include "ntdlib/n64/ead_later.h"

namespace ntdlib::n64
{

namespace
{

using ead_later_constants::kMuteStopScript;
using ead_later_constants::kScriptEnd;
using ead_later_constants::kTicksPerBeat;
using ead_lists::Align16;

namespace tables = ead_later_tables;

// The number of arguments of each command from 0xB0 on, and which are 16 bits wide. The arguments of a channel's script
// take this form. The top bit is the first argument's width.
constexpr std::array<uint8_t, 80> kChannelArguments = {
    0x81, 0x00, 0x81, 0x01, 0x00, 0x00, 0x00, 0x81, 0x01, 0x01, 0x01, 0x42, 0x81, 0xC2, 0x00, 0x00, //
    0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x81, 0x81, //
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x01, 0x81, 0x01, //
    0x01, 0x03, 0x03, 0x01, 0x00, 0x01, 0x01, 0x81, 0x03, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82, //
    0x00, 0x01, 0x01, 0x01, 0x01, 0x81, 0x00, 0x00, 0x01, 0x81, 0x81, 0x81, 0x81, 0x00, 0x00, 0x00, //
};

} // namespace

uint8_t EadLaterPlayer::PeekByte(const Player& player, uint32_t place) const
{
    return place < player.bytes.size() ? player.bytes[place] : uint8_t{0};
}

uint16_t EadLaterPlayer::Peek16(const Player& player, uint32_t place) const
{
    return static_cast<uint16_t>((uint32_t{PeekByte(player, place)} << 8) | PeekByte(player, place + 1));
}

uint8_t EadLaterPlayer::Read8(const Player& player, Script& script) const
{
    return PeekByte(player, script.pc++);
}

int16_t EadLaterPlayer::Read16(const Player& player, Script& script) const
{
    const uint16_t value = Peek16(player, script.pc);
    script.pc += 2;

    return static_cast<int16_t>(value);
}

uint16_t EadLaterPlayer::ReadVariable(const Player& player, Script& script) const
{
    const uint8_t first = Read8(player, script);
    if ((first & 0x80) == 0)
    {
        return first;
    }

    return static_cast<uint16_t>(((first & 0x7F) << 8) | Read8(player, script));
}

const EadInstrument* EadLaterPlayer::Instrument(int font, int program) const
{
    if (font == 0xFF || static_cast<std::size_t>(font) >= font_loaded_.size() ||
        font_loaded_[static_cast<std::size_t>(font)] == 0)
    {
        return nullptr;
    }

    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (static_cast<std::size_t>(program) >= bank.program_instruments.size())
    {
        return nullptr;
    }

    const int32_t object = bank.program_instruments[static_cast<std::size_t>(program)];

    return object == kNoObject ? nullptr : &bank.instruments[static_cast<std::size_t>(object)];
}

const EadDrum* EadLaterPlayer::Drum(int font, int note) const
{
    if (font == 0xFF || static_cast<std::size_t>(font) >= font_loaded_.size() ||
        font_loaded_[static_cast<std::size_t>(font)] == 0)
    {
        return nullptr;
    }

    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (static_cast<std::size_t>(note) >= bank.drum_notes.size())
    {
        return nullptr;
    }

    const int32_t object = bank.drum_notes[static_cast<std::size_t>(note)];

    return object == kNoObject ? nullptr : &bank.drums[static_cast<std::size_t>(object)];
}

const EadSoundEffect* EadLaterPlayer::SoundEffect(int font, int number) const
{
    if (font == 0xFF || static_cast<std::size_t>(font) >= font_loaded_.size() ||
        font_loaded_[static_cast<std::size_t>(font)] == 0)
    {
        return nullptr;
    }

    const EadBank& bank = files_->banks[static_cast<std::size_t>(font)];
    if (static_cast<std::size_t>(number) >= bank.effects.size())
    {
        return nullptr;
    }

    const EadSoundEffect& effect = bank.effects[static_cast<std::size_t>(number)];

    return effect.sound.sample == kNoObject ? nullptr : &effect;
}

const EadSample* EadLaterPlayer::SampleOf(int font, const EadTunedSample* sound) const
{
    if (sound == nullptr || sound->sample == kNoObject)
    {
        return nullptr;
    }

    return &files_->banks[static_cast<std::size_t>(font)].samples[static_cast<std::size_t>(sound->sample)];
}

uint8_t EadLaterPlayer::ChannelForm(uint8_t command) const
{
    if (majora_)
    {
        static constexpr std::array<uint8_t, 16> kEarlyForms = {0x81, 0x00, 0x81, 0x00, 0x01, 0x00, 0x42, 0x01,
                                                                0xC2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (command < 0xB0)
        {
            return kEarlyForms[static_cast<std::size_t>(command - 0xA0)];
        }

        if (command == 0xBD)
        {
            return 0x81;
        }

        if (command == 0xBE)
        {
            return 0x01;
        }
    }

    return kChannelArguments[static_cast<std::size_t>(command - 0xB0)];
}

uint16_t EadLaterPlayer::FlowArgument(const Player& player, Script& script, uint8_t command) const
{
    const uint8_t form = kChannelArguments[static_cast<std::size_t>(command - 0xB0)];
    if ((form & 3) != 1)
    {
        return 0;
    }

    return (form & 0x80) == 0 ? Read8(player, script) : static_cast<uint16_t>(Read16(player, script));
}

int EadLaterPlayer::FlowCommand(Player& player, Script& script, uint8_t command, uint16_t argument)
{
    switch (command)
    {
    case 0xFF:
        if (script.depth == 0)
        {
            return kScriptEnd;
        }

        script.depth--;
        script.pc = script.stack[script.depth & 3];
        break;
    case 0xFD:
        return ReadVariable(player, script);
    case 0xFE:
        return 1;
    case 0xFC:
        script.stack[script.depth & 3] = script.pc;
        script.depth++;
        script.pc = argument;
        break;
    case 0xF8:
        script.loops[script.depth & 3] = static_cast<uint8_t>(argument);
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
    case 0xF5:
    case 0xF9:
    case 0xFA:
    case 0xFB:
        if ((command == 0xFA && script.value != 0) || (command == 0xF9 && script.value >= 0) ||
            (command == 0xF5 && script.value < 0))
        {
            break;
        }

        script.pc = argument;
        break;
    case 0xF2:
    case 0xF3:
    case 0xF4:
        if ((command == 0xF3 && script.value != 0) || (command == 0xF2 && script.value >= 0))
        {
            break;
        }

        script.pc = static_cast<uint32_t>(static_cast<int32_t>(script.pc) + static_cast<int8_t>(argument & 0xFF));
        break;
    default:
        break;
    }

    return 0;
}

void EadLaterPlayer::RunPlayer(int index)
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

    player.script_counter++;
    if (majora_)
    {
        const int step = std::min<int>(player.tempo + static_cast<int16_t>(player.tempo_change), max_tempo_);
        player.tempo_count = static_cast<uint16_t>(player.tempo_count + step);
    }
    else
    {
        player.tempo_count = static_cast<uint16_t>(player.tempo_count + player.tempo);
        player.tempo_count = static_cast<uint16_t>(player.tempo_count + static_cast<int16_t>(player.tempo_change));
    }

    if (static_cast<int32_t>(player.tempo_count) < max_tempo_)
    {
        return;
    }

    player.tempo_count = static_cast<uint16_t>(player.tempo_count - static_cast<uint16_t>(max_tempo_));
    if (player.stop_script)
    {
        return;
    }

    if (player.delay > 1)
    {
        player.delay--;
    }
    else
    {
        player.recalculate_volume = true;
        Script& script = player.script;
        bool running = true;
        while (running)
        {
            uint8_t command = Read8(player, script);
            if (command >= 0xF2)
            {
                const uint16_t argument = FlowArgument(player, script, command);
                const int delay = FlowCommand(player, script, command, argument);
                if (delay != 0)
                {
                    if (delay == kScriptEnd)
                    {
                        DisablePlayer(index);
                    }
                    else
                    {
                        player.delay = static_cast<uint16_t>(delay);
                    }

                    running = false;
                }

                continue;
            }

            if (command >= 0xC0)
            {
                switch (command)
                {
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
                case 0xDD:
                    player.tempo = static_cast<uint16_t>(Read8(player, script) * kTicksPerBeat);
                    if (player.tempo > static_cast<uint16_t>(max_tempo_))
                    {
                        player.tempo = static_cast<uint16_t>(max_tempo_);
                    }

                    if (static_cast<int16_t>(player.tempo) <= 0)
                    {
                        player.tempo = 1;
                    }

                    break;
                case 0xDC:
                    player.tempo_change =
                        static_cast<uint16_t>(static_cast<int8_t>(Read8(player, script)) * kTicksPerBeat);
                    break;
                case 0xDA:
                    {
                        const uint8_t mode = Read8(player, script);
                        const auto updates = static_cast<uint16_t>(Read16(player, script));
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
                        const int value = Read8(player, script);
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
                                player.fade_step = (static_cast<float>(value) / 127.0F - player.fade_volume) /
                                                   static_cast<float>(int32_t{player.fade_updates_next});
                            }
                            else
                            {
                                player.fade_volume = static_cast<float>(value) / 127.0F;
                            }
                        }

                        break;
                    }
                case 0xD9:
                    player.fade_scale = static_cast<float>(static_cast<int8_t>(Read8(player, script))) / 127.0F;
                    break;
                case 0xD7:
                    {
                        uint32_t mask = static_cast<uint16_t>(Read16(player, script));
                        for (int slot = 0; slot < kChannelsPerPlayer; slot++)
                        {
                            if ((mask & 1) != 0)
                            {
                                Channel& channel =
                                    channels_[static_cast<std::size_t>(index * kChannelsPerPlayer + slot)];
                                channel.font = player.default_font;
                                channel.mute_behavior = player.mute_behavior;
                                channel.policy = player.policy;
                            }

                            mask >>= 1;
                        }

                        break;
                    }
                case 0xD6:
                    Read16(player, script);
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
                case 0xD1:
                    player.short_gates = static_cast<uint16_t>(Read16(player, script));
                    break;
                case 0xD2:
                    player.short_velocities = static_cast<uint16_t>(Read16(player, script));
                    break;
                case 0xD0:
                    player.policy = Read8(player, script);
                    break;
                case 0xCE:
                    {
                        const uint8_t range = Read8(player, script);
                        script.value = range == 0 ? static_cast<int8_t>((random_ >> 2) & 0xFF)
                                                  : static_cast<int8_t>((random_ >> 2) % range);
                        break;
                    }
                case 0xCD:
                    {
                        const auto table = static_cast<uint16_t>(Read16(player, script));
                        if (script.value != -1 && script.depth != 3)
                        {
                            const uint32_t place = static_cast<uint32_t>(table + (script.value << 1));
                            script.stack[script.depth & 3] = script.pc;
                            script.depth++;
                            script.pc = Peek16(player, place);
                        }

                        break;
                    }
                case 0xCC:
                    script.value = static_cast<int8_t>(Read8(player, script));
                    break;
                case 0xC9:
                    script.value = static_cast<int8_t>(script.value & Read8(player, script));
                    break;
                case 0xC8:
                    script.value = static_cast<int8_t>(script.value - Read8(player, script));
                    break;
                case 0xC7:
                    {
                        const uint8_t add = Read8(player, script);
                        const auto place = static_cast<uint16_t>(Read16(player, script));
                        if (place < player.bytes.size())
                        {
                            player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(script.value) + add);
                        }

                        break;
                    }
                case 0xC6:
                    player.stop_script = true;

                    return;
                case 0xC5:
                    player.script_counter = static_cast<uint16_t>(Read16(player, script));
                    break;
                case 0xEF:
                    Read16(player, script);
                    Read8(player, script);
                    break;
                case 0xC2:
                    if (majora_)
                    {
                        const auto table = static_cast<uint16_t>(Read16(player, script));
                        if (script.value != -1)
                        {
                            script.pc = Peek16(player, table + static_cast<uint32_t>(script.value * 2));
                        }
                    }

                    break;
                case 0xC3:
                    if (majora_)
                    {
                        const auto table = static_cast<uint16_t>(Read16(player, script));
                        if (script.value != -1)
                        {
                            uint32_t bits = Peek16(player, table + static_cast<uint32_t>(script.value * 2));
                            for (int slot = 0; slot < kChannelsPerPlayer; slot++)
                            {
                                channels_[static_cast<std::size_t>(index * kChannelsPerPlayer + slot)].muted =
                                    (bits & 1) != 0;
                                bits >>= 1;
                            }
                        }
                    }

                    break;
                case 0xC4:
                    {
                        uint8_t target = Read8(player, script);
                        if (target == 0xFF)
                        {
                            target = static_cast<uint8_t>(index);
                            if (majora_ && player.state == 2)
                            {
                                break;
                            }
                        }

                        const uint8_t sequence = Read8(player, script);
                        Start(target, sequence);
                        if (target == static_cast<uint8_t>(index))
                        {
                            return;
                        }

                        break;
                    }
                default:
                    break;
                }

                continue;
            }

            const int low = command & 0xF;
            switch (command & 0xF0)
            {
            case 0x00:
                script.value = static_cast<int8_t>(
                    channels_[static_cast<std::size_t>(index * kChannelsPerPlayer + low)].enabled ? 0 : 1);
                break;
            case 0x50:
                script.value = static_cast<int8_t>(script.value - player.ports[static_cast<std::size_t>(low & 7)]);
                break;
            case 0x70:
                player.ports[static_cast<std::size_t>(low & 7)] = script.value;
                break;
            case 0x80:
                script.value = player.ports[static_cast<std::size_t>(low & 7)];
                if (low < 2)
                {
                    player.ports[static_cast<std::size_t>(low)] = -1;
                }

                break;
            case 0x40:
                DisableChannel(index * kChannelsPerPlayer + low);
                break;
            case 0x90:
                EnableChannel(index, low, static_cast<uint16_t>(Read16(player, script)));
                break;
            case 0xA0:
                {
                    const int16_t relative = Read16(player, script);
                    EnableChannel(index, low, static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative));
                    break;
                }
            case 0xB0:
                {
                    const uint8_t sequence = Read8(player, script);
                    const auto place = static_cast<uint16_t>(Read16(player, script));
                    if (sequence >= files_->sequences.size())
                    {
                        player.ports[static_cast<std::size_t>(low & 7)] = 0;
                        break;
                    }

                    SlowLoad load;
                    load.bytes_left = Align16(static_cast<int>(files_->sequences[sequence].size()));
                    load.player = index;
                    load.port = low & 7;
                    load.sequence = sequence;
                    load.copy_to = place;
                    StartSlowLoad(load);
                    break;
                }
            case 0x60:
                {
                    const uint8_t table = Read8(player, script);
                    const uint8_t id = Read8(player, script);
                    if (table == 1 && id < font_loaded_.size())
                    {
                        font_loaded_[id] = 1;
                    }

                    pending_script_loads_.push_back({static_cast<int8_t>(index), static_cast<int8_t>(low & 7)});
                    break;
                }
            default:
                break;
            }
        }
    }

    if (!player.enabled)
    {
        return;
    }

    for (int slot = 0; slot < kChannelsPerPlayer; slot++)
    {
        const int channel = index * kChannelsPerPlayer + slot;
        if (channels_[static_cast<std::size_t>(channel)].enabled)
        {
            RunChannel(channel);
        }
    }
}

void EadLaterPlayer::RunChannel(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    const auto run_layers = [&]
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

    if (player.muted && (channel.mute_behavior & kMuteStopScript) != 0)
    {
        return;
    }

    if (channel.delay >= 2)
    {
        channel.delay--;
        run_layers();

        return;
    }

    Script& script = channel.script;
    const auto slot_channel = [&](int slot) -> Channel&
    {
        return channels_[static_cast<std::size_t>(channel.player * kChannelsPerPlayer + slot)];
    };
    while (true)
    {
        uint8_t command = Read8(player, script);
        if (command >= (majora_ ? 0xA0 : 0xB0))
        {
            const uint8_t form = ChannelForm(command);
            const int count = form & 3;
            std::array<uint32_t, 3> args = {};
            uint8_t widths = form;
            for (int i = 0; i < count; i++)
            {
                args[static_cast<std::size_t>(i)] = (widths & 0x80) == 0
                                                        ? uint32_t{Read8(player, script)}
                                                        : static_cast<uint32_t>(int32_t{Read16(player, script)});
                widths = static_cast<uint8_t>(widths << 1);
            }

            if (command >= 0xF2)
            {
                const int delay = FlowCommand(player, script, command, static_cast<uint16_t>(args[0]));
                if (delay != 0)
                {
                    if (delay == kScriptEnd)
                    {
                        DisableChannel(index);
                    }
                    else
                    {
                        channel.delay = static_cast<uint16_t>(delay);
                    }

                    break;
                }

                continue;
            }

            const auto arg8 = static_cast<uint8_t>(args[0]);
            const auto arg16 = static_cast<uint16_t>(args[0]);
            const auto arg_signed = static_cast<int8_t>(args[0]);
            const auto random_pointer = [&]()
            {
                const uint32_t value = args[0] == 0 ? random_ & 0xFFFF : random_ % args[0];
                const auto sum = static_cast<uint16_t>(value + args[1]);
                channel.pointer = static_cast<uint16_t>(((sum / 0x100 + 0x80) << 8) | (sum % 0x100));
            };

            bool leave = false;
            switch (command)
            {
            case 0xEA:
                channel.stop_script = true;
                leave = true;
                break;
            case 0xF1:
                ClearPool(ChannelPool(index));
                FillPool(ChannelPool(index), arg8);
                break;
            case 0xF0:
                ClearPool(ChannelPool(index));
                break;
            case 0xC2:
                channel.dynamic_table = arg16;
                break;
            case 0xC5:
                if (script.value != -1)
                {
                    const uint32_t place = channel.dynamic_table + static_cast<uint32_t>(script.value * 2);
                    if (majora_)
                    {
                        script.pc = Peek16(player, place);
                    }
                    else
                    {
                        channel.dynamic_table = Peek16(player, place);
                    }
                }

                break;
            case 0xEB:
            case 0xC6:
                {
                    uint8_t font = arg8;
                    if (player.default_font != 0xFF &&
                        static_cast<std::size_t>(player.sequence) < files_->bank_sets.size())
                    {
                        // The number counts back from the end of the sequence's list of banks.
                        const std::vector<uint8_t>& set = files_->bank_sets[player.sequence];
                        const int place = static_cast<int>(set.size()) - 1 - arg8;
                        font = place >= 0 && static_cast<std::size_t>(place) < set.size()
                                   ? set[static_cast<std::size_t>(place)]
                                   : uint8_t{0};
                    }

                    if (font < font_loaded_.size() && font_loaded_[font] != 0)
                    {
                        channel.font = font;
                    }

                    if (command == 0xEB)
                    {
                        SetProgram(index, static_cast<uint8_t>(args[1]));
                    }

                    break;
                }
            case 0xC1:
                SetProgram(index, arg8);
                break;
            case 0xC3:
                channel.large_notes = false;
                break;
            case 0xC4:
                channel.large_notes = true;
                break;
            case 0xDF:
                channel.volume = static_cast<float>(int32_t{arg8}) / 127.0F;
                channel.change_volume = true;
                break;
            case 0xE0:
                channel.volume_scale = static_cast<float>(int32_t{arg8}) / 128.0F;
                channel.change_volume = true;
                break;
            case 0xDE:
                channel.frequency_scale = static_cast<float>(int32_t{arg16}) / 32768.0F;
                channel.change_frequency = true;
                break;
            case 0xD3:
                channel.frequency_scale = tables::kBendOctave[static_cast<uint8_t>(arg8 + 0x80)];
                channel.change_frequency = true;
                break;
            case 0xEE:
                channel.frequency_scale = tables::kBendFine[static_cast<uint8_t>(arg8 + 0x80)];
                channel.change_frequency = true;
                break;
            case 0xDD:
                channel.new_pan = arg8;
                channel.change_pan = true;
                break;
            case 0xDC:
                channel.pan_weight = arg8;
                channel.change_pan = true;
                break;
            case 0xDB:
                channel.transpose = arg_signed;
                break;
            case 0xDA:
                channel.adsr.envelope = {};
                channel.adsr.envelope.player = channel.player;
                channel.adsr.envelope.offset = arg16;
                break;
            case 0xD9:
                channel.adsr.release = arg8;
                break;
            case 0xD8:
                channel.vibrato_depth_target = static_cast<uint16_t>(arg8 * 8);
                channel.vibrato_depth_start = 0;
                channel.vibrato_depth_delay = 0;
                break;
            case 0xD7:
                channel.vibrato_rate_delay = 0;
                channel.vibrato_rate_target = static_cast<uint16_t>(arg8 * 32);
                channel.vibrato_rate_start = static_cast<uint16_t>(arg8 * 32);
                break;
            case 0xE2:
                channel.vibrato_depth_start = static_cast<uint16_t>(arg8 * 8);
                channel.vibrato_depth_target = static_cast<uint16_t>((args[1] & 0xFF) * 8);
                channel.vibrato_depth_delay = static_cast<uint16_t>((args[2] & 0xFF) * 16);
                break;
            case 0xE1:
                channel.vibrato_rate_start = static_cast<uint16_t>(arg8 * 32);
                channel.vibrato_rate_target = static_cast<uint16_t>((args[1] & 0xFF) * 32);
                channel.vibrato_rate_delay = static_cast<uint16_t>((args[2] & 0xFF) * 16);
                break;
            case 0xE3:
                channel.vibrato_delay = static_cast<uint16_t>(arg8 * 16);
                break;
            case 0xD4:
                channel.reverb = arg8;
                break;
            case 0xC7:
                {
                    const auto place = static_cast<uint16_t>(args[1]);
                    if (place < player.bytes.size())
                    {
                        player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(script.value) + arg8);
                    }

                    break;
                }
            case 0xC8:
                script.value = static_cast<int8_t>(script.value - arg_signed);
                break;
            case 0xCC:
                script.value = arg_signed;
                break;
            case 0xC9:
                script.value = static_cast<int8_t>(script.value & arg_signed);
                break;
            case 0xCD:
                DisableChannel(channel.player * kChannelsPerPlayer + (arg8 & 0xF));
                break;
            case 0xCA:
                channel.mute_behavior = arg8;
                channel.change_volume = true;
                break;
            case 0xCB:
                script.value = static_cast<int8_t>(
                    PeekByte(player, static_cast<uint32_t>(static_cast<int32_t>(arg16) + script.value)));
                break;
            case 0xCE:
                channel.pointer = arg16;
                break;
            case 0xCF:
                if (uint32_t{arg16} + 1 < player.bytes.size())
                {
                    player.bytes[arg16] = static_cast<uint8_t>(channel.pointer >> 8);
                    player.bytes[arg16 + 1U] = static_cast<uint8_t>(channel.pointer);
                }

                break;
            case 0xD0:
                channel.stereo_effects = (arg8 & 0x80) != 0;
                channel.stereo = arg8 & 0x7F;
                break;
            case 0xD1:
                channel.policy = arg8;
                break;
            case 0xD2:
                channel.adsr.sustain = arg8;
                break;
            case 0xE5:
                channel.reverb_index = arg8;
                break;
            case 0xE4:
                if (script.value != -1)
                {
                    const uint32_t place = channel.dynamic_table + static_cast<uint32_t>(script.value * 2);
                    script.stack[script.depth & 3] = script.pc;
                    script.depth++;
                    script.pc = Peek16(player, place);
                }

                break;
            case 0xE6:
                channel.book_offset = arg8;
                break;
            case 0xE7:
                channel.mute_behavior = PeekByte(player, arg16);
                channel.policy = PeekByte(player, arg16 + 1U);
                SetPriorities(channel, PeekByte(player, arg16 + 2U));
                channel.transpose = static_cast<int8_t>(PeekByte(player, arg16 + 3U));
                channel.new_pan = PeekByte(player, arg16 + 4U);
                channel.pan_weight = PeekByte(player, arg16 + 5U);
                channel.reverb = PeekByte(player, arg16 + 6U);
                channel.reverb_index = PeekByte(player, arg16 + 7U);
                channel.change_pan = true;
                break;
            case 0xE8:
                channel.mute_behavior = arg8;
                channel.policy = static_cast<uint8_t>(args[1]);
                SetPriorities(channel, static_cast<uint8_t>(args[2]));
                channel.transpose = static_cast<int8_t>(Read8(player, script));
                channel.new_pan = Read8(player, script);
                channel.pan_weight = Read8(player, script);
                channel.reverb = Read8(player, script);
                channel.reverb_index = Read8(player, script);
                channel.change_pan = true;
                break;
            case 0xA0:
            case 0xA1:
            case 0xA2:
            case 0xA3:
                // These read and write a state that the game's logic gives only its channels of sound effects.
                break;
            case 0xA4:
                channel.surround = arg8;
                break;
            case 0xA5:
                script.value = static_cast<int8_t>(script.value + index % kChannelsPerPlayer);
                break;
            case 0xA6:
                {
                    const uint32_t place =
                        static_cast<uint16_t>(args[1]) + static_cast<uint32_t>(index % kChannelsPerPlayer);
                    if (place < player.bytes.size())
                    {
                        player.bytes[place] = static_cast<uint8_t>(static_cast<uint8_t>(script.value) + arg8);
                    }

                    break;
                }
            case 0xA7:
                {
                    const int value = script.value;
                    auto shifted =
                        static_cast<uint8_t>((arg8 & 0x80) == 0 ? value << (arg8 & 0xF) : value >> (arg8 & 0xF));
                    if ((arg8 & 0x40) != 0)
                    {
                        shifted = static_cast<uint8_t>((shifted & 0x7F) | (value & 0x80));
                    }

                    script.value = static_cast<int8_t>(shifted);
                    break;
                }
            case 0xA8:
                random_pointer();
                break;
            case 0xBE:
                break;
            case 0xEC:
                channel.start_sample = 0;
                channel.vibrato_depth_target = 0;
                channel.vibrato_depth_start = 0;
                channel.vibrato_depth_delay = 0;
                channel.vibrato_rate_target = 0;
                channel.vibrato_rate_start = 0;
                channel.vibrato_rate_delay = 0;
                channel.filter = -1;
                channel.gain = 0;
                channel.adsr.sustain = 0;
                channel.velocity_random = 0;
                channel.gate_random = 0;
                channel.comb_size = 0;
                channel.comb_gain = 0;
                channel.book_offset = 0;
                channel.frequency_scale = 1.0F;
                break;
            case 0xE9:
                SetPriorities(channel, arg8);
                break;
            case 0xED:
                channel.gain = arg8;
                break;
            case 0xB0:
                channel.filter = arg16;
                break;
            case 0xB1:
                channel.filter = -1;
                break;
            case 0xB3:
                if (channel.filter != -1)
                {
                    LoadFilter(channel.player, static_cast<uint32_t>(channel.filter), (arg8 >> 4) & 0xF, arg8 & 0xF);
                }

                break;
            case 0xB2:
                channel.pointer = Peek16(player, static_cast<uint32_t>(static_cast<int32_t>(arg16) + script.value));
                break;
            case 0xB4:
                channel.dynamic_table = channel.pointer;
                break;
            case 0xB5:
                channel.pointer =
                    Peek16(player, channel.dynamic_table + static_cast<uint32_t>(int32_t{script.value} * 2));
                break;
            case 0xB6:
                script.value = static_cast<int8_t>(
                    PeekByte(player, channel.dynamic_table + static_cast<uint32_t>(int32_t{script.value})));
                break;
            case 0xB7:
                channel.pointer = static_cast<uint16_t>(args[0] == 0 ? random_ & 0xFFFF : random_ % args[0]);
                break;
            case 0xB8:
                script.value = static_cast<int8_t>(args[0] == 0 ? random_ & 0xFFFF : random_ % args[0]);
                break;
            case 0xBD:
                if (majora_)
                {
                    channel.start_sample = arg16;
                }
                else
                {
                    random_pointer();
                }

                break;
            case 0xB9:
                channel.velocity_random = arg8;
                break;
            case 0xBA:
                channel.gate_random = arg8;
                break;
            case 0xBB:
                channel.comb_size = arg8;
                channel.comb_gain = static_cast<uint16_t>(args[1]);
                break;
            case 0xBC:
                channel.pointer = static_cast<uint16_t>(channel.pointer + args[0]);
                break;
            default:
                break;
            }

            if (leave)
            {
                break;
            }

            continue;
        }

        if (command >= 0x70)
        {
            int low = command & 0x7;
            const int group = command & 0xF8;
            if (group != 0x70 && low >= (stadium_2_ ? kLayersPerChannel : 4))
            {
                low = 0;
            }

            switch (group)
            {
            case 0x80:
                {
                    const int layer = channel.layers[static_cast<std::size_t>(low)];
                    script.value = layer != kNoLayer
                                       ? static_cast<int8_t>(layers_[static_cast<std::size_t>(layer)].finished)
                                       : int8_t{-1};
                    break;
                }
            case 0x88:
                {
                    const auto place = static_cast<uint16_t>(Read16(player, script));
                    if (SetLayer(index, low))
                    {
                        layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                            place;
                    }

                    break;
                }
            case 0x90:
                FreeLayer(index, low);
                break;
            case 0x98:
                if (script.value != -1 && SetLayer(index, low))
                {
                    const uint32_t place = channel.dynamic_table + static_cast<uint32_t>(script.value * 2);
                    layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                        Peek16(player, place);
                }

                break;
            case 0x70:
                channel.ports[static_cast<std::size_t>(low)] = script.value;
                break;
            case 0x78:
                {
                    const int16_t relative = Read16(player, script);
                    if (SetLayer(index, low))
                    {
                        layers_[static_cast<std::size_t>(channel.layers[static_cast<std::size_t>(low)])].script.pc =
                            static_cast<uint32_t>(static_cast<int32_t>(script.pc) + relative);
                    }

                    break;
                }
            default:
                break;
            }

            continue;
        }

        const int low = command & 0xF;
        bool leave = false;
        switch (command & 0xF0)
        {
        case 0x00:
            channel.delay = static_cast<uint16_t>(low);
            leave = true;
            break;
        case 0x10:
            if (low < 8)
            {
                channel.ports[static_cast<std::size_t>(low)] = -1;
                StartSampleLoad(channel.player, index, low, channel.font, script.value);
            }
            else
            {
                channel.ports[static_cast<std::size_t>(low - 8)] = -1;
                StartSampleLoad(channel.player, index, low - 8, channel.font, channel.pointer + 0x100);
            }

            break;
        case 0x60:
            script.value = channel.ports[static_cast<std::size_t>(low & 7)];
            if (low < 2)
            {
                channel.ports[static_cast<std::size_t>(low)] = -1;
            }

            break;
        case 0x50:
            script.value = static_cast<int8_t>(script.value - channel.ports[static_cast<std::size_t>(low & 7)]);
            break;
        case 0x20:
            EnableChannel(channel.player, low, static_cast<uint16_t>(Read16(player, script)));
            break;
        case 0x30:
            {
                const uint8_t port = Read8(player, script);
                slot_channel(low).ports[port & 7U] = script.value;
                break;
            }
        case 0x40:
            {
                const uint8_t port = Read8(player, script);
                script.value = slot_channel(low).ports[port & 7U];
                break;
            }
        default:
            break;
        }

        if (leave)
        {
            break;
        }
    }

    run_layers();
}

} // namespace ntdlib::n64
