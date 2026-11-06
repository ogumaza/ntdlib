// SPDX-License-Identifier: MIT

// Tests the model of libultra's sequence players (ntdlib/n64/al_player.h) on what its lists give: the player's scales
// in the form that Pokémon Snap links, and the oscillators of Nintendo's example code. The expected values are worked
// by hand from the formulas in src/n64/al_player.cpp.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/n64/al_player.h"
#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/bank_file.h"

namespace
{

using Bytes = std::vector<uint8_t>;

namespace n64 = ntdlib::n64;

constexpr int kFrameSamples = 1024;
constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kController = 0xB0;
constexpr uint8_t kControllerVolume = 7;
constexpr uint8_t kControllerChannelScale = 8;
constexpr uint8_t kControllerPlayerScale = 21;

// A bank of one program and one sound that holds its level.
n64::BankFile MakeBank(uint8_t sound_volume)
{
    n64::BankFile file;
    n64::BankEnvelope envelope;
    envelope.attack_volume = 127;
    envelope.decay_volume = 127;
    envelope.decay_time = 1000000;
    envelope.release_time = 1000;
    file.envelopes.push_back(envelope);

    n64::BankKeyMap map;
    map.velocity_max = 127;
    map.key_max = 127;
    map.key_base = 60;
    file.key_maps.push_back(map);

    n64::BankWave wave;
    wave.length = 0x100000;
    file.waves.push_back(wave);

    n64::BankSound sound;
    sound.envelope = 0;
    sound.key_map = 0;
    sound.wave = 0;
    sound.volume = sound_volume;
    file.sounds.push_back(sound);

    n64::BankInstrument instrument;
    instrument.sounds = {0};
    file.instruments.push_back(instrument);

    n64::Bank bank;
    bank.instruments = {0};
    file.banks.push_back(bank);

    return file;
}

// A standard MIDI file of format 0, with 480 ticks to a quarter note.
Bytes MakeSong(std::initializer_list<uint8_t> track)
{
    Bytes file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
    const auto size = static_cast<uint32_t>(track.size());
    file.push_back(static_cast<uint8_t>(size >> 24));
    file.push_back(static_cast<uint8_t>(size >> 16));
    file.push_back(static_cast<uint8_t>(size >> 8));
    file.push_back(static_cast<uint8_t>(size));
    file.insert(file.end(), track.begin(), track.end());

    return file;
}

struct Rig
{
    Rig(const n64::BankFile& file, const n64::AlPlayerConfig& config)
        : memory(image, 0x100000), synth(n64::AlSynthConfig{}, memory), player(config, synth)
    {
        output = memory.Allocate(kFrameSamples * 4);
        n64::AlSoundBank bank;
        bank.file = &file;
        bank.sample_address = n64::AudioMemory::kImageBase;
        player.SetBank(bank);
    }

    void Start(const Bytes& song)
    {
        player.SetSequence(n64::AlSequenceForm::kStandard, song);
        player.Play();
    }

    std::vector<n64::AudioCommand> Frame()
    {
        n64::AudioList list;
        n64::BuildAlFrame(synth, player, list, memory, output, kFrameSamples);

        return list.commands;
    }

    Bytes image = Bytes(0x1000, 0);
    n64::AudioMemory memory;
    n64::AlSynth synth;
    n64::AlPlayer player;
    uint32_t output = 0;
};

// The form's player starts at 0x7800. A library's player takes the same from SetVolume.
//
// The form takes the player's scale for the sound's volume: a scale of 100 on a sound of 50 gives the commands of a
// library's sound of 100.
void TestPlayerScaleTakesTheSoundsPlace()
{
    const n64::BankFile library_bank = MakeBank(100);
    n64::AlPlayerConfig library_config;
    Rig library(library_bank, library_config);
    library.player.SetVolume(0x7800);
    const Bytes library_song = MakeSong(
        {0, kController, kControllerVolume, 100, 0, kNoteOn, 60, 127, 0x87, 0x40, kNoteOn, 60, 0, 0, 0xFF, 0x2F, 0});
    library.Start(library_song);

    const n64::BankFile scaled_bank = MakeBank(50);
    n64::AlPlayerConfig scaled_config;
    scaled_config.form = n64::AlPlayerForm::kPlayerScale;
    Rig scaled(scaled_bank, scaled_config);
    const Bytes scaled_song = MakeSong({0,
                                        kController,
                                        kControllerPlayerScale,
                                        100,
                                        0,
                                        kController,
                                        kControllerVolume,
                                        100,
                                        0,
                                        kNoteOn,
                                        60,
                                        127,
                                        0x87,
                                        0x40,
                                        kNoteOn,
                                        60,
                                        0,
                                        0,
                                        0xFF,
                                        0x2F,
                                        0});
    scaled.Start(scaled_song);

    const std::vector<n64::AudioCommand> first = library.Frame();
    XSFLIB_CHECK(!first.empty());
    XSFLIB_CHECK(first == scaled.Frame());
}

// A channel's scale of 64 halves its channel's part of a level. 100 * 0x7800 * 100 >> 14 is 18750, and 64 * 18750 >> 7
// is 9375. A player's scale of 50 gives 50 * 0x7800 * 100 >> 14, also 9375.
void TestChannelScale()
{
    const n64::BankFile bank = MakeBank(127);
    n64::AlPlayerConfig config;
    config.form = n64::AlPlayerForm::kPlayerScale;

    Rig channel(bank, config);
    const Bytes channel_song = MakeSong({0,
                                         kController,
                                         kControllerPlayerScale,
                                         100,
                                         0,
                                         kController,
                                         kControllerChannelScale,
                                         64,
                                         0,
                                         kController,
                                         kControllerVolume,
                                         100,
                                         0,
                                         kNoteOn,
                                         60,
                                         127,
                                         0x87,
                                         0x40,
                                         kNoteOn,
                                         60,
                                         0,
                                         0,
                                         0xFF,
                                         0x2F,
                                         0});
    channel.Start(channel_song);

    Rig player(bank, config);
    const Bytes player_song = MakeSong({0,
                                        kController,
                                        kControllerPlayerScale,
                                        50,
                                        0,
                                        kController,
                                        kControllerVolume,
                                        100,
                                        0,
                                        kNoteOn,
                                        60,
                                        127,
                                        0x87,
                                        0x40,
                                        kNoteOn,
                                        60,
                                        0,
                                        0,
                                        0xFF,
                                        0x2F,
                                        0});
    player.Start(player_song);

    const std::vector<n64::AudioCommand> first = channel.Frame();
    XSFLIB_CHECK(!first.empty());
    XSFLIB_CHECK(first == player.Frame());
}

// The pitches of the voice's resampler in a frame.
std::vector<uint32_t> Pitches(const std::vector<n64::AudioCommand>& commands)
{
    std::vector<uint32_t> pitches;
    for (const n64::AudioCommand& command : commands)
    {
        if (command.Opcode() == n64::kAudioResample)
        {
            pitches.push_back(command.w0 & 0xFFFF);
        }
    }

    return pitches;
}

// A square vibrato. A depth of 128 is 1.0309929847717285^128 = 49.76 cents, and the example code takes the whole part.
// 2^(49 / 1200) * 32768 is 33709, and 2^(-49 / 1200) * 32768 is 31854.
//
// The voice starts on the high side. The first change comes after the delay: 1 << 14 microseconds, 524 samples. Each
// later one comes (256 - rate) * 16000 microseconds on: 160 ms, 5120 samples, with a rate of 246. The driver takes a
// change on a multiple of 16 samples.
void TestSquareVibrato()
{
    n64::BankFile bank = MakeBank(127);
    bank.instruments[0].vibrato_type = 129;
    bank.instruments[0].vibrato_rate = 246;
    bank.instruments[0].vibrato_depth = 128;
    bank.instruments[0].vibrato_delay = 1;

    n64::AlPlayerConfig config;
    config.oscillators = true;
    Rig rig(bank, config);
    const Bytes song = MakeSong({0, kNoteOn, 60, 127, 0x87, 0x40, kNoteOn, 60, 0, 0, 0xFF, 0x2F, 0});
    rig.Start(song);

    const auto near = [](uint32_t pitch, uint32_t expected)
    {
        return std::abs(static_cast<int64_t>(pitch) - static_cast<int64_t>(expected)) <= 2;
    };

    std::vector<std::pair<int, uint32_t>> changes;
    for (int frame = 0; frame < 12; frame++)
    {
        for (const uint32_t pitch : Pitches(rig.Frame()))
        {
            if (changes.empty() || changes.back().second != pitch)
            {
                changes.emplace_back(frame, pitch);
            }
        }
    }

    constexpr uint32_t kHigh = 33709;
    constexpr uint32_t kLow = 31854;
    XSFLIB_CHECK(changes.size() == 4);
    XSFLIB_CHECK(changes[0].first == 0 && near(changes[0].second, kHigh));
    XSFLIB_CHECK(changes[1].first == 0 && near(changes[1].second, kLow));
    XSFLIB_CHECK(changes[2].first == 5 && near(changes[2].second, kHigh));
    XSFLIB_CHECK(changes[3].first == 10 && near(changes[3].second, kLow));
}

// GoldenEye 007's library ramps in a straight line, in steps of 8 samples. A ramp to nothing from 2553 over 32 samples
// takes 4 steps of -638.25. The library holds that as -639 and a fraction of 0.75, scaled by 65535 from the negative
// side: the game's list has 0xFD81C001. A later update starts from where the ramp has reached.
void TestLinearRamps()
{
    Bytes image(0x1000, 0);
    n64::AudioMemory memory(image, 0x100000);
    n64::AlSynthConfig config;
    config.linear_ramps = true;
    n64::AlSynth synth(config, memory);
    const uint32_t output = memory.Allocate(kFrameSamples * 4);
    const int voice = synth.AddVoices(1);
    XSFLIB_CHECK(synth.AllocVoice(voice, 1, false));

    n64::AlWave wave;
    wave.raw = true;
    wave.base = n64::AudioMemory::kImageBase;
    wave.length = 0x100000;
    synth.SetParamSamples(0);
    synth.StartVoiceParams(voice, wave, 1.0F, 0x7FFF, 64, 0, 0);
    n64::AudioList first;
    synth.BuildCommands(first, memory, output, kFrameSamples);

    // A ramp of 1000 microseconds is 32 samples at the driver's rate.
    synth.SetParamSamples(kFrameSamples);
    synth.SetVolume(voice, 0, 1000);
    synth.SetParamSamples(kFrameSamples + 16);
    synth.SetVolume(voice, 0, 1000);
    n64::AudioList second;
    synth.BuildCommands(second, memory, output, kFrameSamples);

    std::vector<uint32_t> currents;
    std::vector<uint32_t> rates;
    for (const n64::AudioCommand& command : second.commands)
    {
        if ((command.w0 >> 16) == ((uint32_t{n64::kAudioSetVolume} << 8) | n64::kAudioFlagLeft | n64::kAudioFlagVolume))
        {
            currents.push_back(command.w0 & 0xFFFF);
        }
        else if ((command.w0 >> 16) == ((uint32_t{n64::kAudioSetVolume} << 8) | n64::kAudioFlagLeft))
        {
            rates.push_back(command.w1);
        }
    }

    XSFLIB_CHECK(currents.size() >= 2 && rates.size() >= 2);
    const auto expected_rate = [](uint32_t current)
    {
        const float step = -static_cast<float>(current) / 4.0F;
        const float below = step - 1.0F;
        const auto high = static_cast<int16_t>(static_cast<int32_t>(below));
        const auto low = static_cast<uint16_t>(static_cast<int32_t>((below - static_cast<float>(high)) * 65535.0F));

        return (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16) | low;
    };
    XSFLIB_CHECK(expected_rate(2553) == 0xFD81C001);
    XSFLIB_CHECK(rates[0] == expected_rate(currents[0]));

    // 16 samples on, two of those steps have passed. The first rate here is exactly -5757: 23024 / 4 and the 1 under
    // it.
    XSFLIB_CHECK(currents[0] == 23024);
    XSFLIB_CHECK(rates[0] == 0xE9830000);
    XSFLIB_CHECK(currents[1] == 23024 - (2 * 5757));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_al_player",
                                    []
                                    {
                                        TestPlayerScaleTakesTheSoundsPlace();
                                        TestChannelScale();
                                        TestSquareVibrato();
                                        TestLinearRamps();
                                    });
}
