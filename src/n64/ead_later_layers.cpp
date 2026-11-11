// SPDX-License-Identifier: MIT

// The scripts of the layers in the model of the later EAD engine: the commands of the layers, their notes, the sounds
// that the notes play and the start of their voices.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ead_later_constants.h"
#include "ead_later_tables.h"
#include "ntdlib/n64/ead_later.h"

namespace ntdlib::n64
{

namespace
{

using ead_later_constants::kMuteStopLayerNotes;
using ead_later_constants::kMuteStopNotes;
using ead_later_constants::kScriptEnd;

namespace tables = ead_later_tables;

constexpr uint8_t kPortamentoSpecial = 0x80;

// Returns the sound of an instrument for a note: its low, normal or high sound.
const EadTunedSample* SoundOfNote(const EadInstrument& held, int note)
{
    if (note < held.normal_range_low)
    {
        return &held.low;
    }

    return note <= held.normal_range_high ? &held.normal : &held.high;
}

} // namespace

void EadLaterPlayer::RunLayer(int index)
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
            DecayOrRelease(index, AdsrPhase::kDecay);
            layer.muted = true;
        }

        return;
    }

    if (!layer.continuous)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }
    else if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].wanted_layer == index)
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }

    const int mode = layer.portamento.mode & ~kPortamentoSpecial;
    if (mode == 1 || mode == 2)
    {
        layer.portamento.mode = 0;
    }

    layer.properties_need_init = true;
    int command = LayerCommands(index);
    if (command == kScriptEnd)
    {
        return;
    }

    command = LayerNote(index, command);
    while (majora_ && command == kScriptEnd && layer.delay == 0)
    {
        command = LayerCommands(index);
        if (command == kScriptEnd)
        {
            return;
        }

        command = LayerNote(index, command);
    }

    int same_sound = kScriptEnd;
    if (command != kScriptEnd)
    {
        same_sound = LayerSound(index, command);
    }

    if (same_sound != kScriptEnd)
    {
        LayerVoice(index, same_sound != 0);
    }

    if (layer.muted && (layer.voice != -1 || layer.continuous))
    {
        DecayOrRelease(index, AdsrPhase::kDecay);
    }
}

int EadLaterPlayer::LayerCommands(int index)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    while (true)
    {
        layer.command_place = script.pc;
        uint8_t command = Read8(player, script);
        if (command <= 0xC0)
        {
            return command;
        }

        if (command >= 0xF2)
        {
            const uint16_t argument = FlowArgument(player, script, command);
            if (FlowCommand(player, script, command, argument) == 0)
            {
                continue;
            }

            DisableLayer(index);

            return kScriptEnd;
        }

        switch (command)
        {
        case 0xC1:
            {
                const int value = Read8(player, script);
                layer.velocity_square = static_cast<float>(value * value) / (127.0F * 127.0F);
                break;
            }
        case 0xCA:
            layer.pan = Read8(player, script);
            break;
        case 0xC9:
            layer.gate = Read8(player, script);
            break;
        case 0xC2:
            layer.transpose = Read8(player, script);
            break;
        case 0xC4:
        case 0xC5:
            layer.continuous = command == 0xC4;
            layer.continuous_started = false;
            DecayOrRelease(index, AdsrPhase::kDecay);
            break;
        case 0xC3:
            layer.default_length = static_cast<int16_t>(ReadVariable(player, script));
            break;
        case 0xC6:
            {
                const uint8_t program = Read8(player, script);
                if (program >= 0x7E)
                {
                    if (program == 0x7E)
                    {
                        layer.program = 1;
                    }
                    else if (program == 0x7F)
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
                auto note = Read8(player, script);
                note = static_cast<uint8_t>(note + channel.transpose);
                note = static_cast<uint8_t>(note + layer.transpose);
                note = static_cast<uint8_t>(note + player.transpose);
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
            layer.adsr.envelope.offset = static_cast<uint16_t>(Read16(player, script));
            layer.adsr.release = Read8(player, script);
            break;
        case 0xCF:
            layer.adsr.release = Read8(player, script);
            break;
        case 0xCC:
            layer.ignore_drum_pan = true;
            break;
        case 0xCD:
            layer.stereo = Read8(player, script);
            break;
        case 0xCE:
            layer.bend = tables::kBendFine[static_cast<uint8_t>(Read8(player, script) + 0x80)];
            break;
        case 0xF0:
            if (majora_)
            {
                layer.flags = static_cast<uint16_t>(layer.flags & ~static_cast<uint16_t>(Read16(player, script)));
            }

            break;
        case 0xF1:
            if (majora_)
            {
                layer.surround = Read8(player, script);
            }

            break;
        default:
            if ((command & 0xF0) == 0xD0)
            {
                const int place = player.short_velocities;
                const int value = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                             : tables::kShortVelocity[command & 0xFU];
                layer.velocity_square = static_cast<float>(value * value) / (127.0F * 127.0F);
            }
            else if ((command & 0xF0) == 0xE0)
            {
                const int place = player.short_gates;
                layer.gate = place >= 0 ? PeekByte(player, static_cast<uint32_t>(place) + (command & 0xFU))
                                        : tables::kShortGate[command & 0xFU];
            }

            break;
        }
    }
}

int EadLaterPlayer::LayerNote(int index, int command)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    Script& script = layer.script;
    if (command == 0xC0)
    {
        layer.delay = static_cast<int16_t>(ReadVariable(player, script));
        layer.muted = true;
        layer.continuous_started = false;

        return kScriptEnd;
    }

    layer.muted = false;
    const uint16_t delay =
        channel.large_notes ? ReadLargeNote(layer, player, command) : ReadSmallNote(layer, player, command);
    command -= command & 0xC0;
    SetVariedVelocity(layer, channel);

    layer.delay = static_cast<int16_t>(delay);
    layer.gate_delay = static_cast<int16_t>((layer.gate * int32_t{delay}) >> 8);
    if (channel.gate_random != 0)
    {
        VaryGate(layer, channel);
    }

    if ((player.muted && (channel.mute_behavior & (kMuteStopNotes | kMuteStopLayerNotes)) != 0) || channel.muted)
    {
        layer.muted = true;

        return kScriptEnd;
    }

    if (player.skip_ticks != 0)
    {
        layer.muted = true;

        return kScriptEnd;
    }

    return command;
}

// Reads the arguments of a note in the large form: its length, velocity and gate. Returns the length.
uint16_t EadLaterPlayer::ReadLargeNote(Layer& layer, const Player& player, int command)
{
    Script& script = layer.script;
    uint16_t delay = 0;
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

    if (velocity > 0x7F)
    {
        velocity = 0x7F;
    }

    const auto value = static_cast<float>(velocity);
    layer.velocity_square = value * value / (127.0F * 127.0F);

    return delay;
}

// Returns the length of a note in the small form: read from the script, or the default or last length.
uint16_t EadLaterPlayer::ReadSmallNote(Layer& layer, const Player& player, int command)
{
    Script& script = layer.script;
    uint16_t delay = 0;
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

    return delay;
}

// Sets the varied velocity of a layer's note: its velocity, changed at random within the channel's range.
void EadLaterPlayer::SetVariedVelocity(Layer& layer, const Channel& channel)
{
    if (channel.velocity_random == 0)
    {
        layer.velocity_varied = layer.velocity_square;

        return;
    }

    float change = layer.velocity_square * static_cast<float>(random_ % channel.velocity_random) / 100.0F;
    if ((random_ & 0x8000) != 0)
    {
        change = -change;
    }

    layer.velocity_varied = layer.velocity_square + change;
    if (layer.velocity_varied < 0.0F)
    {
        layer.velocity_varied = 0.0F;
    }
    else if (layer.velocity_varied > 1.0F)
    {
        layer.velocity_varied = 1.0F;
    }
}

// Varies the gate of a layer's note at random. The gate stays within the length of the note.
void EadLaterPlayer::VaryGate(Layer& layer, const Channel& channel)
{
    // The engine takes the velocity's range here. The model avoids a division by zero when a channel has no range.
    const uint32_t range = channel.velocity_random != 0 ? channel.velocity_random : 1U;
    int change = (layer.gate_delay * static_cast<int>(random_ % range)) / 100;
    if ((random_ & 0x4000) != 0)
    {
        change = -change;
    }

    layer.gate_delay = static_cast<int16_t>(layer.gate_delay + change);
    if (layer.gate_delay < 0)
    {
        layer.gate_delay = 0;
    }
    else if (layer.gate_delay > layer.delay)
    {
        layer.gate_delay = layer.delay;
    }
}

int EadLaterPlayer::LayerSound(int index, int command)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    const Player& player = players_[static_cast<std::size_t>(channel.player)];
    bool same_sound = true;
    auto semitone = static_cast<uint8_t>(command);
    int program = layer.program;
    if (program == 0xFF)
    {
        if (!channel.has_instrument)
        {
            return kScriptEnd;
        }

        program = channel.program;
    }

    bool found = false;
    if (program == 0)
    {
        found = SetDrumSound(layer, channel, semitone);
    }
    else if (program == 1)
    {
        found = SetEffectSound(layer, channel, semitone);
    }
    else
    {
        found = SetInstrumentSound(layer, channel, player, semitone, same_sound);
    }

    if (!found)
    {
        return kScriptEnd;
    }

    layer.delay_copy = layer.delay;
    layer.frequency_scale *= layer.bend;
    if (layer.delay == 0)
    {
        SetSampleLength(layer, player);
    }

    return same_sound ? 1 : 0;
}

// Sets the sound of a layer to the drum of its note. Returns false when the bank has no such drum.
bool EadLaterPlayer::SetDrumSound(Layer& layer, const Channel& channel, uint8_t semitone)
{
    semitone = static_cast<uint8_t>(semitone + channel.transpose + layer.transpose);
    layer.semitone = semitone;
    const EadDrum* drum = Drum(channel.font, semitone);
    if (drum == nullptr)
    {
        layer.muted = true;
        layer.delay_copy = layer.delay;

        return false;
    }

    layer.adsr.envelope = {};
    layer.adsr.envelope.bank = drum->envelope == kNoObject
                                   ? nullptr
                                   : &files_->banks[channel.font].envelopes[static_cast<std::size_t>(drum->envelope)];
    layer.adsr.release = drum->release_rate;
    if (!layer.ignore_drum_pan)
    {
        layer.pan = drum->pan;
    }

    layer.sound = &drum->sound;
    layer.sound_font = channel.font;
    layer.frequency_scale = drum->sound.tuning;

    return true;
}

// Sets the sound of a layer to a sound effect. Returns false when the bank has no such effect.
bool EadLaterPlayer::SetEffectSound(Layer& layer, const Channel& channel, uint8_t semitone)
{
    layer.semitone = semitone;
    const int number = (layer.transpose << 6) + semitone;
    const EadSoundEffect* effect = SoundEffect(channel.font, number);
    if (effect == nullptr)
    {
        layer.muted = true;
        layer.delay_copy = static_cast<int16_t>(layer.delay + 1);

        return false;
    }

    layer.sound = &effect->sound;
    layer.sound_font = channel.font;
    layer.frequency_scale = effect->sound.tuning;

    return true;
}

// Sets the sound and frequency of a layer for an instrument's note and starts the layer's portamento. Returns false
// for a note above 127. Sets `same_sound` to false when the layer's sound changes.
bool EadLaterPlayer::SetInstrumentSound(Layer& layer, const Channel& channel, const Player& player, uint8_t semitone,
                                        bool& same_sound)
{
    semitone = static_cast<uint8_t>(semitone + player.transpose + channel.transpose + layer.transpose);
    layer.semitone = semitone;
    if (semitone >= 0x80)
    {
        layer.muted = true;

        return false;
    }

    const EadInstrument* instrument = layer.program == 0xFF ? channel.instrument : layer.instrument;
    if (layer.portamento.mode != 0)
    {
        same_sound = StartPortamento(layer, channel, player, instrument, semitone);
    }
    else if (instrument != nullptr)
    {
        const EadTunedSample* sound = SoundOfNote(*instrument, semitone);
        same_sound = sound == layer.sound;
        layer.sound = sound;
        layer.sound_font = channel.font;
        layer.frequency_scale = tables::kPitchFrequencies[semitone] * sound->tuning;
    }
    else
    {
        layer.sound = nullptr;
        layer.frequency_scale = tables::kPitchFrequencies[semitone];
    }

    return true;
}

// Starts the portamento of a layer's note. The layer plays the sound of the higher of the two notes. Returns whether
// the layer keeps its sound.
bool EadLaterPlayer::StartPortamento(Layer& layer, const Channel& channel, const Player& player,
                                     const EadInstrument* instrument, uint8_t semitone)
{
    bool same_sound = true;
    const int note_mode = layer.portamento.mode & ~kPortamentoSpecial;
    const int highest = semitone > layer.portamento_target ? semitone : layer.portamento_target;
    float tuning = 1.0F;
    if (instrument != nullptr)
    {
        const EadTunedSample* sound = SoundOfNote(*instrument, highest);
        same_sound = layer.sound == sound;
        layer.sound = sound;
        layer.sound_font = channel.font;
        tuning = sound->tuning;
    }
    else
    {
        layer.sound = nullptr;
    }

    const float from_note = tables::kPitchFrequencies[semitone] * tuning;
    const float to_note = tables::kPitchFrequencies[layer.portamento_target] * tuning;
    float base = from_note;
    float other = from_note;
    switch (note_mode)
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

    layer.portamento.extent = (other / base) - 1.0F;
    int speed = 0;
    if ((layer.portamento.mode & kPortamentoSpecial) != 0)
    {
        speed = int32_t{player.tempo} * 0x8000 / max_tempo_;
        if (layer.delay != 0)
        {
            speed = speed * 0x100 / (layer.delay * int32_t{layer.portamento_time});
        }
    }
    else
    {
        const int denominator = int32_t{layer.portamento_time} * updates_per_frame_ / 3;
        speed = denominator != 0 ? 0x20000 / denominator : 0x7FFF;
    }

    speed = std::clamp(speed, 1, 0x7FFF);
    layer.portamento.speed = static_cast<uint16_t>(speed);
    layer.portamento.position = 0;
    layer.frequency_scale = base;
    if (note_mode == 5)
    {
        layer.portamento_target = semitone;
    }

    return same_sound;
}

// Sets the length of a note without a length from its sample: the end of the sample's loop at the player's tempo. A
// portamento in the special mode then gets a speed for that length.
void EadLaterPlayer::SetSampleLength(Layer& layer, const Player& player)
{
    float time = 0.0F;
    const EadSample* sample = SampleOf(layer.sound_font, layer.sound);
    if (sample != nullptr && sample->loop != kNoObject)
    {
        time = static_cast<float>(files_->banks[layer.sound_font].loops[static_cast<std::size_t>(sample->loop)].end);
    }

    time *= static_cast<float>(player.tempo);
    time *= note_length_scale_;
    time /= layer.frequency_scale;
    if (time > 32766.0F)
    {
        time = 32766.0F;
    }

    layer.gate_delay = 0;
    layer.delay = static_cast<int16_t>(static_cast<uint16_t>(static_cast<int32_t>(time)) + 1);
    if (layer.portamento.mode != 0 && (layer.portamento.mode & kPortamentoSpecial) != 0)
    {
        int speed = int32_t{player.tempo} * 0x8000 / max_tempo_;
        const int denominator = layer.delay * int32_t{layer.portamento_time};
        speed = denominator != 0 ? speed * 0x100 / denominator : 0x7FFF;
        layer.portamento.speed = static_cast<uint16_t>(std::clamp(speed, 1, 0x7FFF));
    }
}

void EadLaterPlayer::LayerVoice(int index, bool same_sound)
{
    Layer& layer = layers_[static_cast<std::size_t>(index)];
    const EadSample* sample = SampleOf(layer.sound_font, layer.sound);
    if (!majora_ && !layer.muted && sample != nullptr && sample->codec == kEadCodecS16InMemory)
    {
        layer.muted = true;

        return;
    }

    if (layer.continuous && layer.continuous_started)
    {
        return;
    }

    bool started = false;
    if (layer.continuous && layer.voice != -1 && layer.loaded && same_sound &&
        voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        if (layer.sound == nullptr)
        {
            InitWave(voices_[static_cast<std::size_t>(layer.voice)], layer);
        }
    }
    else
    {
        if (!same_sound)
        {
            DecayOrRelease(index, AdsrPhase::kDecay);
        }

        layer.voice = AllocVoice(index);
        if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
        {
            InitVibrato(layer.voice);
        }

        started = true;
    }

    if (layer.voice != -1 && voices_[static_cast<std::size_t>(layer.voice)].layer == index)
    {
        Voice& voice = voices_[static_cast<std::size_t>(layer.voice)];
        voice.portamento_scale = 1.0F;
        voice.portamento = layer.portamento;
    }

    if (layer.voice != -1)
    {
        voices_[static_cast<std::size_t>(layer.voice)].track = layer.channel;
    }

    if (started && on_note)
    {
        ReportNoteStart(index);
    }
}

// Reports the start of a layer's note to `on_note`.
void EadLaterPlayer::ReportNoteStart(int index)
{
    const Layer& layer = layers_[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(layer.channel)];
    EadNoteStart start;
    start.player = channel.player;
    start.channel = layer.channel - channel.player * kChannelsPerPlayer;
    for (int i = 0; i < kLayersPerChannel; i++)
    {
        if (channel.layers[static_cast<std::size_t>(i)] == index)
        {
            start.layer = i;
        }
    }

    start.key = layer.semitone;
    const int program = layer.program == 0xFF ? channel.program : layer.program;
    start.drum = program == 0;
    start.bank = channel.font;
    start.program = static_cast<int16_t>(program);
    start.velocity = layer.velocity_varied;
    start.pan = static_cast<float>(layer.pan) / 128.0F;
    start.length = layer.delay;
    start.gate = layer.gate_delay;
    start.offset = layer.command_place;
    start.update = updates_;
    start.voice = layer.voice;
    on_note(start);
}

} // namespace ntdlib::n64
