// SPDX-License-Identifier: MIT

#include "ntdlib/n64/ambrella_player.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ntdlib::n64
{

namespace
{

constexpr uint8_t kStatusNoteOff = 0x80;
constexpr uint8_t kStatusNoteOn = 0x90;
constexpr uint8_t kStatusController = 0xB0;
constexpr uint8_t kStatusProgram = 0xC0;
constexpr uint8_t kStatusBend = 0xE0;
constexpr uint8_t kStatusSystem = 0xF0;
constexpr uint8_t kStatusSystemEnd = 0xF7;
constexpr uint8_t kStatusMeta = 0xFF;
constexpr uint8_t kMetaEnd = 0x2F;
constexpr uint8_t kMetaTempo = 0x51;

constexpr uint8_t kControllerModulation = 1;
constexpr uint8_t kControllerPortamentoTime = 5;
constexpr uint8_t kControllerDataHigh = 6;
constexpr uint8_t kControllerVolume = 7;
constexpr uint8_t kControllerPan = 10;
constexpr uint8_t kControllerExpression = 11;
constexpr uint8_t kControllerDataLow = 38;
constexpr uint8_t kControllerPanLow = 42;
constexpr uint8_t kControllerSustain = 64;
constexpr uint8_t kControllerPortamento = 65;
constexpr uint8_t kControllerPortamentoKey = 84;
constexpr uint8_t kControllerFxMix = 91;
constexpr uint8_t kControllerParameterLow = 100;
constexpr uint8_t kControllerParameterHigh = 101;
constexpr uint8_t kControllerLoopStart = 102;
constexpr uint8_t kControllerLoopEnd = 103;
constexpr uint8_t kControllerSoundOff = 120;
constexpr uint8_t kControllerReset = 121;
constexpr uint8_t kControllerNotesOff = 123;

constexpr uint8_t kPedalDown = 64;
constexpr int16_t kVoicePriority = 5;
constexpr int32_t kEnvelopeFull = 0xFFFF;
constexpr int32_t kEnvelopeEnd = 8193;
constexpr uint32_t kEnvelopeLevelMask = 0xF000;
constexpr double kEnvelopeScale = 65536.0;
constexpr double kMicroseconds = 0.000001;
constexpr double kPerSecond = 1000000.0;
constexpr int kSweepUnits = 768;
constexpr int kPhaseShift = 12;
constexpr uint8_t kMaximum7Bit = 127;
constexpr int32_t kBendCentre = 8192;
constexpr int32_t kCentsPerSemitone = 100;
constexpr uint8_t kCentrePan = 64;
constexpr double kFadeTimeUnits = 16.0;

// The system message that the reader makes for a fade: F0 7F 7F 04 03 time level F7.
constexpr uint8_t kUniversalRealTime = 0x7F;
constexpr uint8_t kDeviceControl = 0x04;
constexpr uint8_t kMasterVolumeMessage = 0x01;
constexpr uint8_t kFadeMessage = 0x03;

// The tables at 0x800635E0 and 0x80063620 in the game: a raised cosine for tremolo, and a sine for vibrato.
constexpr std::array<uint32_t, 16> kTremoloTable = {0x00, 0x09, 0x25, 0x4E, 0x7F, 0xB0, 0xD9, 0xF5,
                                                    0xFF, 0xF5, 0xD9, 0xB0, 0x7F, 0x4E, 0x25, 0x09};
constexpr std::array<int32_t, 16> kVibratoTable = {0, 48,  90,  117,  127,  117,  90,  48,
                                                   0, -48, -90, -117, -127, -117, -90, -48};

uint8_t To7Bit(double value)
{
    const auto whole = static_cast<int32_t>(value);

    return static_cast<uint8_t>(std::clamp(whole, 0, int32_t{kMaximum7Bit}));
}

} // namespace

AmbrellaPlayer::AmbrellaPlayer(const AmbrellaPlayerConfig& config, AlSynth& synth)
    : config_(config),
      voices_(static_cast<std::size_t>(config.voices)),
      players_(static_cast<std::size_t>(config.players))
{
    const int first = synth.AddVoices(config.voices);
    for (int i = 0; i < config.voices; i++)
    {
        Voice& voice = voices_[static_cast<std::size_t>(i)];
        voice.synth_voice = first + i;
        voice.next = i + 1 < config.voices ? i + 1 : -1;
    }

    free_head_ = config.voices > 0 ? 0 : -1;
    for (int i = 0; i < config.players; i++)
    {
        players_[static_cast<std::size_t>(i)].number = i;
    }
}

void AmbrellaPlayer::SetBank(const AlSoundBank& bank)
{
    bank_ = bank;
    const int32_t percussion = bank.file->banks[bank.bank].percussion;
    for (Player& player : players_)
    {
        for (Channel& channel : player.channels)
        {
            channel.instrument = percussion;
        }
    }
}

void AmbrellaPlayer::SetMasterVolume(uint8_t volume)
{
    master_volume_ = volume;
}

int AmbrellaPlayer::StartSong(std::span<const uint8_t> file)
{
    constexpr std::size_t kHeaderSize = 22;
    constexpr std::size_t kFormat = 8;
    constexpr std::size_t kTracks = 10;
    constexpr std::size_t kDivision = 12;
    constexpr std::size_t kTrack = 14;
    constexpr std::size_t kTrackLength = 18;

    const auto word = [&file](std::size_t at)
    {
        return (static_cast<uint32_t>(file[at]) << 24) | (static_cast<uint32_t>(file[at + 1]) << 16) |
               (static_cast<uint32_t>(file[at + 2]) << 8) | file[at + 3];
    };
    const auto half = [&file](std::size_t at)
    {
        return static_cast<uint32_t>((file[at] << 8) | file[at + 1]);
    };

    if (file.size() < kHeaderSize || word(0) != 0x4D546864 || word(4) != 6 || half(kFormat) != 0 ||
        half(kTracks) != 1 || (half(kDivision) & 0x8000) != 0 || word(kTrack) != 0x4D54726B)
    {
        return -1;
    }

    for (Player& player : players_)
    {
        if (!player.stopped)
        {
            continue;
        }

        player.reset_pending = true;
        player.stop_timer = -1.0;
        player.fade_time = -1.0;
        player.file = file;
        player.position = kHeaderSize;
        player.end = kHeaderSize + word(kTrackLength);
        player.running_status = 0;
        player.division = static_cast<int16_t>(half(kDivision));
        player.tempo = 500000;
        player.has_loop = false;
        player.channel_mask = 0xFFFF;
        player.passes = 0;
        player.speed = 1.0;
        player.stopped = false;
        player.level_now = 1.0;

        return player.number;
    }

    return -1;
}

bool AmbrellaPlayer::Playing(int player) const
{
    return !players_[static_cast<std::size_t>(player)].stopped;
}

void AmbrellaPlayer::StopSong(int player, double seconds)
{
    Player& held = players_[static_cast<std::size_t>(player)];
    if (held.stopped)
    {
        return;
    }

    if (seconds == 0.0)
    {
        StopReader(held);
    }
    else if (held.stop_timer < 0.0 || seconds < held.stop_timer)
    {
        held.stop_timer = seconds;
    }
}

void AmbrellaPlayer::FadeTo(int player, double seconds, double level)
{
    Player& held = players_[static_cast<std::size_t>(player)];
    if (seconds == 0.0)
    {
        held.level_now = level;
    }
    else
    {
        held.fade_time = seconds;
        held.fade_target = level;
    }
}

void AmbrellaPlayer::SetSpeed(int player, double speed)
{
    players_[static_cast<std::size_t>(player)].speed = speed;
}

void AmbrellaPlayer::Handle(AlSynth& synth)
{
    // The game's list of players has the newest first.
    for (auto player = players_.rbegin(); player != players_.rend(); ++player)
    {
        RunEvents(synth, *player);
        RunFade(*player, config_.update_seconds);
        UpdateVoices(synth, *player);
    }
}

bool AmbrellaPlayer::AnyPlaying() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return !player.stopped || player.head != -1; });
}

uint8_t AmbrellaPlayer::NextByte(Player& player)
{
    return player.position < player.file.size() ? player.file[player.position++] : uint8_t{0};
}

uint32_t AmbrellaPlayer::NextLength(Player& player)
{
    uint32_t value = NextByte(player);
    if ((value & 0x80) != 0)
    {
        value &= 0x7F;
        uint8_t byte = 0;
        do
        {
            byte = NextByte(player);
            value = (value << 7) + (byte & 0x7F);
        } while ((byte & 0x80) != 0);
    }

    return value;
}

void AmbrellaPlayer::StopReader(Player& player)
{
    if (!player.stopped)
    {
        player.flush_notes = true;
        player.flush_pedals = true;
        player.stopped = true;
    }
}

// The code at 0x8000530C.
bool AmbrellaPlayer::ReadFromFile(Player& player, double& delta)
{
    auto& event = player.event;
    delta = 0.0;
    for (;;)
    {
        if (player.flush_pedals)
        {
            bool sent = false;
            for (int c = 0; c < kChannels && !sent; c++)
            {
                uint8_t& pedal = player.pedals[static_cast<std::size_t>(c)];
                if (pedal >= kPedalDown)
                {
                    pedal = 0;
                    event[0] = static_cast<uint8_t>(kStatusController | c);
                    event[1] = kControllerSustain;
                    event[2] = 0;
                    sent = true;
                }
            }

            if (sent)
            {
                return true;
            }

            player.flush_pedals = false;
        }

        if (player.flush_notes)
        {
            if (player.held_notes != 0)
            {
                player.held_notes--;
                const auto held = static_cast<std::size_t>(player.held_notes);
                event[0] = static_cast<uint8_t>(kStatusNoteOn | player.held_channels[held]);
                event[1] = player.held_keys[held];
                event[2] = 0;

                return true;
            }

            player.flush_notes = false;
        }

        if (player.stopped)
        {
            delta = -1.0;

            return false;
        }

        if (player.position >= player.end)
        {
            StopReader(player);
            continue;
        }

        const uint32_t ticks = NextLength(player);
        player.event_offset = static_cast<uint32_t>(player.position);
        const uint8_t first = NextByte(player);
        event[0] = first;
        if (first == kStatusSystem || first == kStatusSystemEnd)
        {
            const uint32_t length = NextLength(player);
            player.position += length;
        }
        else if (first == kStatusMeta)
        {
            event[1] = NextByte(player);
            event[2] = NextByte(player);
            player.running_status = 0;
            const std::size_t length = event[2];
            for (std::size_t i = 0; i < length; i++)
            {
                const uint8_t byte = NextByte(player);
                if (i + 3 < event.size())
                {
                    event[i + 3] = byte;
                }
            }
        }
        else
        {
            if ((first & 0x80) != 0)
            {
                event[1] = NextByte(player);
                player.running_status = first;
            }
            else
            {
                event[1] = first;
                event[0] = player.running_status;
            }

            const uint8_t kind = event[0] & 0xF0;
            if (kind != kStatusProgram && kind != 0xD0)
            {
                event[2] = NextByte(player);
            }
        }

        delta += static_cast<double>(ticks) * kMicroseconds * static_cast<double>(player.tempo) /
                 static_cast<double>(player.division) / player.speed;

        const uint8_t status = event[0];
        const uint8_t kind = status & 0xF0;
        const uint8_t channel = status & 0x0F;
        if (kind == kStatusNoteOn && event[2] != 0)
        {
            if (((player.channel_mask >> channel) & 1) == 0 || player.held_notes >= kHeldNotes)
            {
                continue;
            }

            const auto held = static_cast<std::size_t>(player.held_notes);
            player.held_channels[held] = channel;
            player.held_keys[held] = event[1];
            player.held_notes++;

            return true;
        }

        if (kind == kStatusNoteOff || kind == kStatusNoteOn)
        {
            for (int i = 0; i < player.held_notes; i++)
            {
                const auto held = static_cast<std::size_t>(i);
                if (player.held_channels[held] == channel && player.held_keys[held] == event[1])
                {
                    player.held_notes--;
                    const auto last = static_cast<std::size_t>(player.held_notes);
                    player.held_channels[held] = player.held_channels[last];
                    player.held_keys[held] = player.held_keys[last];
                    break;
                }
            }

            return true;
        }

        if (kind == kStatusController)
        {
            if (event[1] == kControllerSustain)
            {
                player.pedals[channel] = event[2];
            }
            else if (event[1] == kControllerLoopStart)
            {
                player.loop_position = player.position;
                player.loop_status = player.running_status;
                player.has_loop = true;
                continue;
            }
            else if (event[1] == kControllerLoopEnd)
            {
                // From the sixth pass on, two passes of every three leave out a random set of the upper channels.
                player.flush_notes = true;
                if (player.passes % 3 > 0 && player.passes >= 4)
                {
                    player.channel_mask = static_cast<uint16_t>((random ? random() : uint32_t{0xFFFF}) | 0xFF);
                }
                else
                {
                    player.channel_mask = 0xFFFF;
                }

                player.passes++;
                if (player.has_loop)
                {
                    player.position = player.loop_position;
                    player.running_status = player.loop_status;
                }

                continue;
            }

            return true;
        }

        if (kind == kStatusSystem)
        {
            if (status == kStatusSystem || status == kStatusSystemEnd)
            {
                continue;
            }

            if (status == kStatusMeta)
            {
                if (event[1] == kMetaEnd)
                {
                    StopReader(player);
                }
                else if (event[1] == kMetaTempo)
                {
                    player.tempo = (event[3] << 16) | (event[4] << 8) | event[5];
                }

                continue;
            }
        }

        return true;
    }
}

// The code at 0x800059E4 and 0x80006068.
bool AmbrellaPlayer::ReadEvent(Player& player)
{
    auto& event = player.event;
    if (player.stop_timer == 0.0)
    {
        StopReader(player);
        player.stop_timer = -1.0;
    }

    const auto fade_event = [&event](double seconds, double level)
    {
        event[0] = kStatusSystem;
        event[1] = kUniversalRealTime;
        event[2] = kUniversalRealTime;
        event[3] = kDeviceControl;
        event[4] = kFadeMessage;
        event[5] = To7Bit(seconds * kFadeTimeUnits);
        event[6] = To7Bit(level * kMaximum7Bit);
        event[7] = kStatusSystemEnd;
    };

    if (player.reset_pending)
    {
        player.delta = 0.0;
        event[0] = kStatusMeta;
        player.reset_pending = false;

        return true;
    }

    if (player.level_now >= 0.0)
    {
        fade_event(0.0, player.level_now);
        player.level_now = -1.0;
        player.delta = 0.0;

        return true;
    }

    if (player.fade_time >= 0.0)
    {
        fade_event(player.fade_time, player.fade_target);
        player.fade_time = -1.0;
        player.delta = 0.0;

        return true;
    }

    const bool read = ReadFromFile(player, player.delta);
    if (player.delta > 0.0 && player.stop_timer >= 0.0)
    {
        player.stop_timer = player.stop_timer < player.delta ? 0.0 : player.stop_timer - player.delta;
    }

    return read;
}

// The code at 0x80006FD4.
void AmbrellaPlayer::RunEvents(AlSynth& synth, Player& player)
{
    double left = config_.update_seconds;
    if (player.delta <= left)
    {
        do
        {
            if (player.delta >= 0.0)
            {
                left -= player.delta;
            }

            Dispatch(synth, player);
            player.has_event = ReadEvent(player);
        } while (player.delta >= 0.0 && player.delta <= left);
    }

    if (player.delta >= 0.0)
    {
        player.delta -= left;
    }
}

// The code at 0x80006E08.
void AmbrellaPlayer::Dispatch(AlSynth& synth, Player& player)
{
    if (!player.has_event)
    {
        return;
    }

    const auto& event = player.event;
    const uint8_t kind = event[0] & 0xF0;
    const int channel = event[0] & 0x0F;
    switch (kind)
    {
    case kStatusNoteOn:
        if (event[2] != 0)
        {
            NoteOn(synth, player, channel, event[1], event[2]);
        }
        else
        {
            NoteOff(synth, player, channel, event[1]);
        }

        break;

    case kStatusNoteOff:
        NoteOff(synth, player, channel, event[1]);
        break;

    case kStatusController:
        Controller(synth, player, channel, event[1], event[2]);
        break;

    case kStatusProgram:
        {
            const Bank& bank = bank_.file->banks[bank_.bank];
            if (event[1] < bank.instruments.size())
            {
                player.channels[static_cast<std::size_t>(channel)].instrument = bank.instruments[event[1]];
            }

            break;
        }

    case kStatusBend:
        player.channels[static_cast<std::size_t>(channel)].bend = static_cast<uint16_t>((event[2] << 7) | event[1]);
        break;

    case kStatusSystem:
        if (event[0] == kStatusMeta)
        {
            ResetPlayer(synth, player);
        }
        else if (event[0] == kStatusSystem)
        {
            SystemMessage(player);
        }

        break;

    default:
        break;
    }
}

// The code at 0x80006CC0.
void AmbrellaPlayer::SystemMessage(Player& player)
{
    const auto& event = player.event;
    if (event[1] != kUniversalRealTime || event[3] != kDeviceControl)
    {
        return;
    }

    if (event[4] == kMasterVolumeMessage)
    {
        player.master = static_cast<uint16_t>(event[5] | (event[6] << 7));
    }
    else if (event[4] == kFadeMessage)
    {
        const double seconds = static_cast<double>(event[5]) * (1.0 / kFadeTimeUnits);
        const double target = static_cast<double>(event[6]) / kMaximum7Bit;
        if (seconds > 0.0)
        {
            player.fade_remaining = seconds;
            player.fade_rate = (target - player.fade_level) / seconds;
        }
        else
        {
            player.fade_level = target;
        }
    }
}

// The code at 0x80006F2C.
void AmbrellaPlayer::RunFade(Player& player, double seconds)
{
    if (player.fade_rate == 0.0)
    {
        return;
    }

    if (player.fade_remaining < seconds)
    {
        player.fade_level += player.fade_remaining * player.fade_rate;
        player.fade_rate = 0.0;
    }
    else
    {
        player.fade_level += seconds * player.fade_rate;
        player.fade_remaining -= seconds;
    }

    player.fade_level = std::clamp(player.fade_level, 0.0, 1.0);
}

// The code at 0x8000658C.
void AmbrellaPlayer::NoteOn(AlSynth& synth, Player& player, int channel, uint8_t key, uint8_t velocity)
{
    Channel& held = player.channels[static_cast<std::size_t>(channel)];
    const int32_t sound = FindSound(held.instrument, key, velocity);
    if (sound == kNoObject)
    {
        return;
    }

    const int index = TakeVoice(player);
    if (index < 0)
    {
        return;
    }

    Voice& voice = voices_[static_cast<std::size_t>(index)];
    const BankFile& file = *bank_.file;
    const BankSound& object = file.sounds[static_cast<std::size_t>(sound)];
    const BankEnvelope& envelope = file.envelopes[static_cast<std::size_t>(object.envelope)];
    voice.sustained = false;
    voice.on = true;
    voice.sound = sound;
    voice.envelope = kEnvelopeFull;
    voice.envelope_rate = 0;
    if (envelope.decay_time != 0 && envelope.decay_volume == 0)
    {
        voice.envelope_rate = static_cast<int32_t>(static_cast<double>(-kEnvelopeFull) /
                                                   (kMicroseconds * static_cast<double>(envelope.decay_time)));
    }

    voice.instrument = held.instrument;
    voice.channel = channel;
    voice.key = key;
    voice.vibrato_phase = 0;
    voice.tremolo_phase = 0;
    voice.velocity = velocity;
    voice.level = VoiceLevel(player, voice);
    voice.sweep = 0;
    if (held.portamento != 0)
    {
        voice.sweep = static_cast<int16_t>((held.last_key - key) * kCentsPerSemitone);
    }

    held.last_key = key;
    voice.pitch = VoicePitch(player, voice);
    voice.pan = VoicePan(player, voice);

    synth.AllocVoice(voice.synth_voice, kVoicePriority, false);
    synth.SetVoiceTag(voice.synth_voice, TrackOf(player.number, channel));
    synth.StartVoiceParams(voice.synth_voice, WaveOfSound(bank_, object), voice.pitch,
                           static_cast<int16_t>(voice.level), voice.pan, held.fx_mix, 0);
    if (voice.envelope_rate != 0)
    {
        synth.SetVolume(voice.synth_voice, 0, envelope.decay_time);
    }

    if (on_note)
    {
        on_note({player.number, channel, key, velocity, player.event_offset});
    }
}

// The code at 0x800067BC.
void AmbrellaPlayer::NoteOff(AlSynth& synth, Player& player, int channel, uint8_t key)
{
    // The walk goes on from a freed voice's next pointer, as the game's does. That pointer leads into the free list.
    for (int index = player.head; index != -1;)
    {
        Voice& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.key == key && voice.channel == channel && voice.on)
        {
            voice.on = false;
            if (player.channels[static_cast<std::size_t>(channel)].sustain >= kPedalDown)
            {
                voice.sustained = true;
            }
            else
            {
                ReleaseVoice(synth, player, index);
            }
        }

        index = voice.next;
    }
}

// The jump table at 0x800A0250.
void AmbrellaPlayer::Controller(AlSynth& synth, Player& player, int channel, uint8_t controller, uint8_t value)
{
    Channel& held = player.channels[static_cast<std::size_t>(channel)];
    const auto update_pans = [&]
    {
        for (int index = player.head; index != -1; index = voices_[static_cast<std::size_t>(index)].next)
        {
            Voice& voice = voices_[static_cast<std::size_t>(index)];
            if (voice.channel == channel)
            {
                UpdatePan(synth, player, voice);
            }
        }
    };

    const auto set_data = [&held](bool low, uint8_t data)
    {
        if (held.parameter_high != 0 || held.parameter_low != 0)
        {
            return;
        }

        held.bend_range =
            low ? static_cast<uint16_t>((held.bend_range & 0x3F80) | data) : static_cast<uint16_t>(data << 7);
    };

    switch (controller)
    {
    case kControllerModulation:
        held.vibrato_depth = value;
        held.tremolo_depth = value;
        break;

    case kControllerPortamentoTime:
        held.portamento_time = value;
        break;

    case kControllerDataHigh:
        set_data(false, value);
        break;

    case kControllerDataLow:
        set_data(true, value);
        break;

    case kControllerVolume:
        held.volume = value;
        break;

    case kControllerPan:
        held.pan = static_cast<uint16_t>(value << 7);
        update_pans();
        break;

    case kControllerPanLow:
        held.pan = static_cast<uint16_t>((held.pan & 0x3F80) | value);
        update_pans();
        break;

    case kControllerExpression:
        held.expression = static_cast<uint16_t>(value << 7);
        break;

    case kControllerSustain:
        SetSustain(synth, player, channel, value);
        break;

    case kControllerPortamento:
        held.portamento = value;
        break;

    case kControllerPortamentoKey:
        held.last_key = value;
        break;

    case kControllerFxMix:
        held.fx_mix = value;
        for (int index = player.head; index != -1; index = voices_[static_cast<std::size_t>(index)].next)
        {
            const Voice& voice = voices_[static_cast<std::size_t>(index)];
            if (voice.channel == channel)
            {
                synth.SetFxMix(voice.synth_voice, held.fx_mix);
            }
        }

        break;

    case kControllerParameterLow:
        held.parameter_low = value;
        break;

    case kControllerParameterHigh:
        held.parameter_high = value;
        break;

    case kControllerSoundOff:
        for (int index = player.head; index != -1;)
        {
            const int next = voices_[static_cast<std::size_t>(index)].next;
            if (voices_[static_cast<std::size_t>(index)].channel == channel)
            {
                EndVoice(synth, player, index);
            }

            index = next;
        }

        break;

    case kControllerReset:
        ResetControllers(synth, player, channel);
        break;

    case kControllerNotesOff:
        for (int index = player.head; index != -1;)
        {
            const int next = voices_[static_cast<std::size_t>(index)].next;
            if (voices_[static_cast<std::size_t>(index)].channel == channel)
            {
                ReleaseVoice(synth, player, index);
            }

            index = next;
        }

        break;

    default:
        break;
    }
}

// The code at 0x80005D6C. Lifting a pedal releases the sustained voices of every channel, not only the pedal's. The
// flag stays set on those voices.
void AmbrellaPlayer::SetSustain(AlSynth& synth, Player& player, int channel, uint8_t value)
{
    Channel& held = player.channels[static_cast<std::size_t>(channel)];
    if (held.sustain >= kPedalDown && value < kPedalDown)
    {
        for (int index = player.head; index != -1;)
        {
            const Voice& voice = voices_[static_cast<std::size_t>(index)];
            if (voice.sustained)
            {
                ReleaseVoice(synth, player, index);
            }

            index = voice.next;
        }
    }

    held.sustain = value;
}

// The code at 0x80005EC4.
void AmbrellaPlayer::ResetControllers(AlSynth& synth, Player& player, int channel)
{
    SetSustain(synth, player, channel, 0);
    Channel& held = player.channels[static_cast<std::size_t>(channel)];
    held.bend = static_cast<uint16_t>(kBendCentre);
    held.portamento = 0;
    held.vibrato_depth = 0;
    held.tremolo_depth = 0;
    held.parameter_low = kMaximum7Bit;
    held.parameter_high = kMaximum7Bit;
    held.expression = 16256;
}

// The code at 0x80005F30.
void AmbrellaPlayer::ResetPlayer(AlSynth& synth, Player& player)
{
    player.master = 16384;
    player.fade_rate = 0.0;
    player.fade_level = 1.0;
    const int32_t percussion = bank_.file->banks[bank_.bank].percussion;
    for (int c = 0; c < kChannels; c++)
    {
        ResetControllers(synth, player, c);
        Channel& held = player.channels[static_cast<std::size_t>(c)];
        held.instrument = percussion;
        held.fx_mix = 0;
        held.volume = 100;
        held.bend_range = 256;
        held.portamento_time = 0;
        held.last_key = 64;
        held.pan = static_cast<uint16_t>(kBendCentre);
    }

    for (int index = player.head; index != -1; index = voices_[static_cast<std::size_t>(index)].next)
    {
        Voice& voice = voices_[static_cast<std::size_t>(index)];
        UpdatePan(synth, player, voice);
        synth.SetFxMix(voice.synth_voice, player.channels[static_cast<std::size_t>(voice.channel)].fx_mix);
    }
}

// The code at 0x80006140: a search through the instrument's sounds, as they lie in the bank.
int32_t AmbrellaPlayer::FindSound(int32_t instrument, uint8_t key, uint8_t velocity) const
{
    if (instrument == kNoObject)
    {
        return kNoObject;
    }

    const BankFile& file = *bank_.file;
    const std::vector<int32_t>& sounds = file.instruments[static_cast<std::size_t>(instrument)].sounds;
    int low = 0;
    int high = static_cast<int>(sounds.size()) - 1;
    while (high >= low)
    {
        const int middle = (low + high) / 2;
        const int32_t sound = sounds[static_cast<std::size_t>(middle)];
        const BankKeyMap& map =
            file.key_maps[static_cast<std::size_t>(file.sounds[static_cast<std::size_t>(sound)].key_map)];
        if (key >= map.key_min && key <= map.key_max && velocity >= map.velocity_min && velocity <= map.velocity_max)
        {
            return sound;
        }

        if (key < map.key_min)
        {
            high = middle - 1;
        }
        else if (velocity >= map.velocity_min || map.key_max < key)
        {
            low = middle + 1;
        }
        else
        {
            high = middle - 1;
        }
    }

    return kNoObject;
}

// The code at 0x80005BE0.
int AmbrellaPlayer::TakeVoice(Player& player)
{
    const int index = free_head_;
    if (index < 0)
    {
        return -1;
    }

    Voice& voice = voices_[static_cast<std::size_t>(index)];
    free_head_ = voice.next;
    voice.previous = -1;
    voice.next = player.head;
    if (player.head != -1)
    {
        voices_[static_cast<std::size_t>(player.head)].previous = index;
    }

    player.head = index;

    return index;
}

// The code at 0x80005C64 and 0x80005C1C.
void AmbrellaPlayer::EndVoice(AlSynth& synth, Player& player, int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    synth.StopVoice(voice.synth_voice);
    synth.FreeVoice(voice.synth_voice);
    if (voice.previous != -1)
    {
        voices_[static_cast<std::size_t>(voice.previous)].next = voice.next;
    }
    else
    {
        player.head = voice.next;
    }

    if (voice.next != -1)
    {
        voices_[static_cast<std::size_t>(voice.next)].previous = voice.previous;
    }

    voice.next = free_head_;
    free_head_ = index;
}

// The code at 0x80005CAC.
void AmbrellaPlayer::ReleaseVoice(AlSynth& synth, Player& player, int index)
{
    Voice& voice = voices_[static_cast<std::size_t>(index)];
    const BankFile& file = *bank_.file;
    const BankSound& sound = file.sounds[static_cast<std::size_t>(voice.sound)];
    const BankEnvelope& envelope = file.envelopes[static_cast<std::size_t>(sound.envelope)];
    const double seconds = static_cast<double>(envelope.release_time) * kMicroseconds;
    if (seconds > 0.0)
    {
        voice.envelope_rate = static_cast<int32_t>(static_cast<double>(-voice.envelope) / seconds);
        if (voice.envelope_rate == 0)
        {
            voice.envelope_rate = -1;
        }

        synth.SetVolume(voice.synth_voice, 0, static_cast<int32_t>(seconds * kPerSecond));
    }
    else
    {
        EndVoice(synth, player, index);
    }
}

// The code at 0x80006254.
uint16_t AmbrellaPlayer::VoiceLevel(const Player& player, const Voice& voice) const
{
    const Channel& held = player.channels[static_cast<std::size_t>(voice.channel)];
    const BankFile& file = *bank_.file;
    uint32_t tremolo = 256;
    if (held.tremolo_depth != 0)
    {
        const BankInstrument& instrument = file.instruments[static_cast<std::size_t>(voice.instrument)];
        const uint32_t product = held.tremolo_depth *
                                 kTremoloTable[static_cast<std::size_t>(voice.tremolo_phase >> kPhaseShift)] *
                                 instrument.tremolo_depth;
        tremolo = 256 - ((product >> 8) >> 7);
    }

    const BankSound& sound = file.sounds[static_cast<std::size_t>(voice.sound)];
    uint32_t level = (uint32_t{master_volume_} * 32767 * voice.velocity) >> 14;
    level = (level * held.volume * sound.volume) >> 14;
    level = (level * tremolo) >> 8;
    level = (level * held.expression) >> 14;
    level = static_cast<uint32_t>(static_cast<double>(level) * player.fade_level);
    level = (uint32_t{player.master} * level) >> 14;
    level = ((static_cast<uint32_t>(voice.envelope) & kEnvelopeLevelMask) * level) >> 16;

    return level >= 0x8000 ? uint16_t{32767} : static_cast<uint16_t>(level);
}

// The code at 0x80005E14.
uint8_t AmbrellaPlayer::VoicePan(const Player& player, const Voice& voice) const
{
    if (config_.mono)
    {
        return kCentrePan;
    }

    const Channel& held = player.channels[static_cast<std::size_t>(voice.channel)];
    const BankSound& sound = bank_.file->sounds[static_cast<std::size_t>(voice.sound)];
    const int pan = (held.pan >> 7) + sound.pan - kCentrePan;

    return static_cast<uint8_t>(std::clamp(pan, 0, int{kMaximum7Bit}));
}

// The code at 0x8000645C. The key map's detune comes off the pitch, where the SDK's player adds it.
float AmbrellaPlayer::VoicePitch(const Player& player, const Voice& voice) const
{
    const Channel& held = player.channels[static_cast<std::size_t>(voice.channel)];
    const BankFile& file = *bank_.file;
    int32_t vibrato = 0;
    if (held.vibrato_depth != 0)
    {
        const BankInstrument& instrument = file.instruments[static_cast<std::size_t>(voice.instrument)];
        const int32_t product = kVibratoTable[static_cast<std::size_t>(voice.vibrato_phase >> kPhaseShift)] *
                                held.vibrato_depth * instrument.vibrato_depth;
        vibrato = (product / 128) / 128;
    }

    const BankSound& sound = file.sounds[static_cast<std::size_t>(voice.sound)];
    const BankKeyMap& map = file.key_maps[static_cast<std::size_t>(sound.key_map)];
    const int32_t bend = (((held.bend - kBendCentre) * held.bend_range) / kBendCentre) * kCentsPerSemitone / 128;
    const int32_t cents = ((voice.key - map.key_base) * kCentsPerSemitone) - map.detune + bend + voice.sweep + vibrato;

    return CentsToRatio(cents);
}

// The code at 0x80005E80.
void AmbrellaPlayer::UpdatePan(AlSynth& synth, const Player& player, Voice& voice)
{
    const uint8_t pan = VoicePan(player, voice);
    if (pan != voice.pan)
    {
        voice.pan = pan;
        synth.SetPan(voice.synth_voice, pan);
    }
}

// The code at 0x800070A4. The walk stops at a voice that ends. The voices after it wait for the next call.
void AmbrellaPlayer::UpdateVoices(AlSynth& synth, Player& player)
{
    const auto step = static_cast<int32_t>(config_.update_seconds * kEnvelopeScale);
    const auto ramp_time = static_cast<int32_t>(config_.update_seconds * kPerSecond);
    const BankFile& file = *bank_.file;
    for (int index = player.head; index != -1; index = voices_[static_cast<std::size_t>(index)].next)
    {
        Voice& voice = voices_[static_cast<std::size_t>(index)];
        voice.envelope +=
            static_cast<int32_t>(static_cast<uint32_t>(voice.envelope_rate) * static_cast<uint32_t>(step)) / 65536;
        if (synth.PhysicalVoice(voice.synth_voice) < 0 || synth.VoiceFinished(voice.synth_voice) ||
            voice.envelope < kEnvelopeEnd)
        {
            EndVoice(synth, player, index);

            return;
        }

        const Channel& held = player.channels[static_cast<std::size_t>(voice.channel)];
        const BankInstrument& instrument = file.instruments[static_cast<std::size_t>(voice.instrument)];
        if (held.vibrato_depth != 0)
        {
            voice.vibrato_phase = static_cast<uint16_t>(voice.vibrato_phase + ((step * instrument.vibrato_rate) / 16));
        }

        if (held.tremolo_depth != 0)
        {
            voice.tremolo_phase = static_cast<uint16_t>(voice.tremolo_phase + ((step * instrument.tremolo_rate) / 16));
        }

        if (voice.sweep > 0)
        {
            const int sweep = voice.sweep - (kSweepUnits / (held.portamento_time + 1));
            voice.sweep = static_cast<int16_t>(std::max(sweep, 0));
        }
        else if (voice.sweep < 0)
        {
            const int sweep = voice.sweep + (kSweepUnits / (held.portamento_time + 1));
            voice.sweep = static_cast<int16_t>(std::min(sweep, 0));
        }

        if (voice.envelope_rate == 0)
        {
            const uint16_t level = VoiceLevel(player, voice);
            if (level != voice.level)
            {
                voice.level = level;
                synth.SetVolume(voice.synth_voice, static_cast<int16_t>(level), ramp_time);
            }
        }

        const float pitch = VoicePitch(player, voice);
        if (pitch != voice.pitch)
        {
            voice.pitch = pitch;
            synth.SetPitch(voice.synth_voice, pitch);
        }
    }
}

void BuildAmbrellaFrame(AlSynth& synth, AmbrellaPlayer& player, AudioList& list, AudioMemory& memory, uint32_t output,
                        int samples)
{
    constexpr uint32_t kBytesPerFrame = 4;

    for (int done = 0; done < samples; done += kAlCompactChunkSamples)
    {
        const int chunk = std::min(samples - done, kAlCompactChunkSamples);
        synth.SetParamSamples(synth.CurrentSamples() & ~0xF);
        player.Handle(synth);
        synth.SetParamSamples((synth.CurrentSamples() + chunk) & ~0xF);
        synth.BuildCommands(list, memory, output + (static_cast<uint32_t>(done) * kBytesPerFrame), chunk);
    }
}

} // namespace ntdlib::n64
