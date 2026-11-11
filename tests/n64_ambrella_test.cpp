// SPDX-License-Identifier: MIT

// Tests the model of the player in Hey You, Pikachu!. The expected values are worked by hand from the game's code at
// the addresses in src/n64/ambrella_player.cpp.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/ambrella_player.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/bank_file.h"

namespace
{

using Bytes = std::vector<uint8_t>;

namespace n64 = ntdlib::n64;

constexpr int kFrameSamples = n64::kAlCompactChunkSamples;
constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kController = 0xB0;

// A bank of one program and one sound. The percussion instrument is the same program.
n64::BankFile MakeBank(int32_t decay_time, uint8_t decay_volume, int32_t release_time, int8_t detune)
{
    n64::BankFile file;
    n64::BankEnvelope envelope;
    envelope.decay_time = decay_time;
    envelope.decay_volume = decay_volume;
    envelope.release_time = release_time;
    file.envelopes.push_back(envelope);

    n64::BankKeyMap map;
    map.velocity_max = 127;
    map.key_max = 127;
    map.key_base = 60;
    map.detune = detune;
    file.key_maps.push_back(map);

    n64::BankWave wave;
    wave.base = 0;
    wave.length = 0x100000;
    file.waves.push_back(wave);

    n64::BankSound sound;
    sound.envelope = 0;
    sound.key_map = 0;
    sound.wave = 0;
    file.sounds.push_back(sound);

    n64::BankInstrument instrument;
    instrument.sounds = {0};
    file.instruments.push_back(instrument);

    n64::Bank bank;
    bank.percussion = 0;
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
    explicit Rig(const n64::BankFile& file) : memory(image, 0x100000), synth(Config(), memory), player({}, synth)
    {
        output = memory.Allocate(kFrameSamples * 4 * 4);
        n64::AlSoundBank bank;
        bank.file = &file;
        bank.sample_address = n64::AudioMemory::kImageBase;
        player.SetBank(bank);
    }

    static n64::AlSynthConfig Config()
    {
        n64::AlSynthConfig config;
        config.form = n64::AlSynthForm::kCompact;
        config.voices = 8;

        return config;
    }

    std::vector<n64::AudioCommand> Frame()
    {
        n64::AudioList list;
        n64::BuildAmbrellaFrame(synth, player, list, memory, output, kFrameSamples);

        return list.commands;
    }

    Bytes image = Bytes(0x1000, 0);
    n64::AudioMemory memory;
    n64::AlSynth synth;
    n64::AmbrellaPlayer player;
    uint32_t output = 0;
};

n64::AudioCommand CommandWith(const std::vector<n64::AudioCommand>& commands, uint32_t high_half)
{
    const auto found = std::ranges::find_if(
        commands, [high_half](const n64::AudioCommand& command) { return (command.w0 >> 16) == high_half; });
    XSFLIB_CHECK(found != commands.end());

    return *found;
}

std::ptrdiff_t CountVoices(const std::vector<n64::AudioCommand>& commands)
{
    return std::ranges::count_if(
        commands, [](const n64::AudioCommand& command) { return command.Opcode() == n64::kAudioEnvMixer; });
}

// A voice's level at the top of every factor but the channel's volume of 100.
//
// 128 * 32767 * 127 >> 14 = 32511, the player's master volume and the velocity.
// 32511 * 100 * 127 >> 14 = 25200, the channel's volume and the sound's.
// 25200 * 16256 >> 14 = 25003, the expression that a channel starts with.
// 0xF000 * 25003 >> 16 = 23440, the envelope's top bits.
//
// The driver squares the level: 23440 * 23440 >> 15 = 16767. The pan of 64 takes 23026 from the driver's table on the
// left, and 23313 on the right. 16767 * 23026 >> 15 = 11782, and 16767 * 23313 >> 15 = 11928.
void TestLevel()
{
    const n64::BankFile file = MakeBank(0, 127, 40000, 0);
    const Bytes song = MakeSong({0, kNoteOn, 60, 127, 0x60, kNoteOn, 60, 0, 0, 0xFF, 0x2F, 0});
    Rig rig(file);
    XSFLIB_CHECK(rig.player.StartSong(song) == 0);

    const std::vector<n64::AudioCommand> first = rig.Frame();
    XSFLIB_CHECK(CountVoices(first) == 1);
    constexpr uint32_t kSetCurrentLeft = 0x0906;
    constexpr uint32_t kSetTargetRight = 0x0904;
    XSFLIB_CHECK((CommandWith(first, kSetCurrentLeft).w0 & 0xFFFF) == 11782);
    XSFLIB_CHECK((CommandWith(first, kSetTargetRight).w0 & 0xFFFF) == 11928);
}

// The key map's detune comes off the pitch. 10 cents down is a ratio of 0.99424, and 0.99424 * 32768 is 32579.
void TestDetune()
{
    const n64::BankFile file = MakeBank(0, 127, 40000, 10);
    const Bytes song = MakeSong({0, kNoteOn, 60, 127, 0x60, kNoteOn, 60, 0, 0, 0xFF, 0x2F, 0});
    Rig rig(file);
    XSFLIB_CHECK(rig.player.StartSong(song) == 0);

    const std::vector<n64::AudioCommand> first = rig.Frame();
    const auto found = std::ranges::find_if(
        first, [](const n64::AudioCommand& command) { return command.Opcode() == n64::kAudioResample; });
    XSFLIB_CHECK(found != first.end());
    const uint32_t increment = (found->w1 >> 14) & 0xFFFF;
    XSFLIB_CHECK(increment >= 32578 && increment <= 32580);
}

// A decay to nothing is one ramp. Over 100 ms, the envelope's rate is -65535 / 0.1 = -655350 for each second. Each call
// takes 376 / 65536 of that: -3759 by the game's multiply, shifted. From 65535 the envelope passes under 8193 on the
// sixteenth call: 65535 - 15 * 3759 = 9150 stays over, and 65535 - 16 * 3759 = 5391 doesn't.
void TestDecayEndsVoice()
{
    const n64::BankFile file = MakeBank(100000, 0, 40000, 0);
    const Bytes song = MakeSong({0, kNoteOn, 60, 127, 0x7F, kNoteOn, 60, 0, 0, 0xFF, 0x2F, 0});
    Rig rig(file);
    XSFLIB_CHECK(rig.player.StartSong(song) == 0);

    int with_voice = 0;
    for (int frame = 0; frame < 40; frame++)
    {
        if (CountVoices(rig.Frame()) != 0)
        {
            with_voice++;
        }
    }

    // The voice's chunks: the first call starts it, the sixteenth ends it before the list.
    XSFLIB_CHECK(with_voice == 15);
}

// Passes of a loop. The fifth CC 103 and the sixth take the random set of channels, the seventh takes them all, and the
// pattern goes on with a period of three. The set always holds channels 0 to 7.
void TestLoopPasses()
{
    const n64::BankFile file = MakeBank(0, 127, 1000, 0);
    const Bytes song = MakeSong({0,  kController, 102, 0, 0, kNoteOn,     60, 127, 0, kNoteOn | 9, 62,  127,
                                 10, kNoteOn,     60,  0, 0, kNoteOn | 9, 62, 0,   0, kController, 103, 0});
    Rig rig(file);
    rig.player.random = []
    {
        return uint32_t{0x12340000};
    };

    std::vector<int> channels;
    rig.player.on_note = [&channels](const n64::AmbrellaNoteStart& note)
    {
        channels.push_back(note.channel);
    };

    XSFLIB_CHECK(rig.player.StartSong(song) == 0);

    while (channels.size() < 18)
    {
        rig.Frame();
    }

    // The first five passes play both channels: 10 notes. The sixth and seventh play channel 0 alone. The eighth plays
    // both. The ninth and tenth play channel 0 alone.
    const std::vector<int> expected = {0, 9, 0, 9, 0, 9, 0, 9, 0, 9, 0, 0, 0, 9, 0, 0};
    XSFLIB_CHECK(std::equal(expected.begin(), expected.end(), channels.begin()));
}

// A sweep starts at the portamento's key. 768 / (time + 1) comes off each call.
void TestPortamento()
{
    const n64::BankFile file = MakeBank(0, 127, 1000, 0);
    const Bytes song = MakeSong({0, kController, 65, 127, 0,    kController, 84, 72, 0, kController, 5,    7,
                                 0, kNoteOn,     60, 127, 0x7F, kNoteOn,     60, 0,  0, 0xFF,        0x2F, 0});
    Rig rig(file);
    XSFLIB_CHECK(rig.player.StartSong(song) == 0);

    const std::vector<n64::AudioCommand> first = rig.Frame();
    const auto increment = [](const std::vector<n64::AudioCommand>& commands)
    {
        const auto found = std::ranges::find_if(
            commands, [](const n64::AudioCommand& command) { return command.Opcode() == n64::kAudioResample; });
        XSFLIB_CHECK(found != commands.end());

        return (found->w1 >> 14) & 0xFFFF;
    };

    // The call that starts the note also updates the voice. The sweep starts 1200 cents up, and 96 have come off:
    // 2^(1104 / 1200) * 32768 is 62001.
    const uint32_t first_increment = increment(first);
    XSFLIB_CHECK(first_increment >= 61990 && first_increment <= 62010);

    // After the next call, 192 have come off: 2^(1008 / 1200) * 32768 is 58657.
    const uint32_t second_increment = increment(rig.Frame());
    XSFLIB_CHECK(second_increment >= 58645 && second_increment <= 58665);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_ambrella",
                                    []
                                    {
                                        TestLevel();
                                        TestDetune();
                                        TestDecayEndsVoice();
                                        TestLoopPasses();
                                        TestPortamento();
                                    });
}
