// SPDX-License-Identifier: MIT

// Tests of the files and the player of the N64 Music Tools (ntdlib/n64/mus_player.h) and of the compact form of the
// synthesis driver (ntdlib/n64/al_synth.h). Each test builds its bytes by hand in the layouts that the header
// describes. The expected values come by hand from the same layouts.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/mus_player.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace n64 = ntdlib::n64;
using Bytes = std::vector<uint8_t>;

void Put32(Bytes& bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 24));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void PutAt(Bytes& bytes, std::size_t place, uint32_t value)
{
    xsflib::StoreBe32(bytes.data() + place, value);
}

// A bank of two waves. The first has a loop and detunes by +50 cents with a base note of 60. The second has neither,
// and its base note is 48.
Bytes MakeBank()
{
    constexpr uint32_t kBaseNotes = 0x30;
    constexpr uint32_t kDetunes = 0x34;
    constexpr uint32_t kWaves = 0x3C;
    constexpr uint32_t kWaveOne = 0x44;
    constexpr uint32_t kWaveTwo = 0x58;
    constexpr uint32_t kLoop = 0x6C;
    constexpr uint32_t kBook = 0x98;
    Bytes bytes(kBook + 8 + 32, 0);
    PutAt(bytes, 0x20, 2);
    PutAt(bytes, 0x24, kBaseNotes);
    PutAt(bytes, 0x28, kDetunes);
    PutAt(bytes, 0x2C, kWaves);
    bytes[kBaseNotes] = 60;
    bytes[kBaseNotes + 1] = 48;
    bytes[kDetunes] = 50;
    PutAt(bytes, kWaves, kWaveOne);
    PutAt(bytes, kWaves + 4, kWaveTwo);
    PutAt(bytes, kWaveOne, 0x100);
    PutAt(bytes, kWaveOne + 4, 900);
    PutAt(bytes, kWaveOne + 12, kLoop);
    PutAt(bytes, kWaveOne + 16, kBook);
    PutAt(bytes, kWaveTwo, 0x500);
    PutAt(bytes, kWaveTwo + 4, 1800);
    PutAt(bytes, kWaveTwo + 16, kBook);
    PutAt(bytes, kLoop, 16);
    PutAt(bytes, kLoop + 4, 1600);
    PutAt(bytes, kLoop + 8, 0xFFFFFFFF);
    PutAt(bytes, kBook, 2);
    PutAt(bytes, kBook + 4, 1);

    return bytes;
}

void TestBank()
{
    const Bytes bytes = MakeBank();
    const n64::MusBank bank = n64::MusBank::Parse(bytes);
    XSFLIB_CHECK(bank.waves.size() == 2);
    XSFLIB_CHECK(std::abs(bank.pitches[0] - 12.5F) < 0.0001F);
    XSFLIB_CHECK(bank.pitches[1] == 0.0F);
    XSFLIB_CHECK(bank.waves[0].base == 0x100);
    XSFLIB_CHECK(bank.waves[0].length == 900);
    XSFLIB_CHECK(bank.waves[0].has_loop);
    XSFLIB_CHECK(bank.waves[0].loop_start == 16);
    XSFLIB_CHECK(bank.waves[0].loop_end == 1600);
    XSFLIB_CHECK(bank.waves[0].loop_count == 0xFFFFFFFF);
    XSFLIB_CHECK(bank.waves[0].loop_state == 0x6C + 12);
    XSFLIB_CHECK(bank.waves[0].book == 0x98 + 8);
    XSFLIB_CHECK(bank.waves[0].book_size == 32);
    XSFLIB_CHECK(!bank.waves[1].has_loop);

    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([] { n64::MusBank::Parse(Bytes(8, 0)); }).empty());
    Bytes wrong = MakeBank();
    PutAt(wrong, 0x2C, 0xFFFFFF00);
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::MusBank::Parse(wrong); }).empty());
}

// A song of the early form with one channel: `list` for its commands and no lists of volumes or bends.
Bytes MakeSong(const Bytes& list)
{
    constexpr uint32_t kHeader = 0x18;
    Bytes bytes;
    Put32(bytes, 1);
    Put32(bytes, kHeader);
    Put32(bytes, kHeader + 4);
    Put32(bytes, kHeader + 8);
    Put32(bytes, 0);
    Put32(bytes, 0);
    Put32(bytes, kHeader + 12);
    Put32(bytes, 0);
    Put32(bytes, 0);
    bytes.insert(bytes.end(), list.begin(), list.end());

    return bytes;
}

void TestSong()
{
    const Bytes bytes = MakeSong({0x80});
    const n64::MusSong song = n64::MusSong::Parse(bytes, n64::MusSongForm::kEarly);
    XSFLIB_CHECK(song.data_lists == std::vector<uint32_t>{0x24});
    XSFLIB_CHECK(song.volume_lists == std::vector<uint32_t>{0});
    XSFLIB_CHECK(song.bend_lists == std::vector<uint32_t>{0});

    XSFLIB_CHECK(
        !ErrorOf<xsflib::FormatError>([] { n64::MusSong::Parse(Bytes(4, 0), n64::MusSongForm::kEarly); }).empty());
    Bytes wrong = bytes;
    PutAt(wrong, 0, 0x7FFFFFFF);
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { n64::MusSong::Parse(wrong, n64::MusSongForm::kEarly); }).empty());
}

void TestPlayerTiming()
{
    // A channel's default tempo is 0x6000 / 60 = 409 of 256 to a tick in each frame. Two notes of 10 ticks: a tick
    // count of 2560 passes in frame 7, and the second note follows the first exactly 10 ticks later on the channel's
    // clock.
    const Bytes bank_bytes = MakeBank();
    const n64::MusBank bank = n64::MusBank::Parse(bank_bytes);
    const Bytes song_bytes = MakeSong({0x81, 1, 40, 10, 41, 10, 0x80});
    const n64::MusSong song = n64::MusSong::Parse(song_bytes, n64::MusSongForm::kEarly);

    const Bytes image(0x1000, 0);
    n64::AudioMemory memory(image, 0x100000);
    n64::AlSynthConfig synth_config;
    synth_config.form = n64::AlSynthForm::kCompact;
    synth_config.voices = 4;
    n64::AlSynth synth(synth_config, memory);

    n64::MusPlayerConfig config;
    config.channels = 2;
    config.voices = 4;
    n64::MusPlayer player(config, synth);
    std::vector<n64::MusNoteStart> notes;
    player.on_note = [&notes](const n64::MusNoteStart& note)
    {
        notes.push_back(note);
    };

    n64::MusSoundBank sound_bank;
    sound_bank.bank = &bank;
    sound_bank.bank_address = memory.Allocate(bank_bytes.size());
    sound_bank.sample_address = n64::AudioMemory::kImageBase;
    player.StartSong(song, sound_bank);

    for (int frame = 0; frame < 6; frame++)
    {
        player.RunFrame();
    }

    XSFLIB_CHECK(notes.size() == 1);
    XSFLIB_CHECK(notes[0].key == 40);
    XSFLIB_CHECK(notes[0].wave == 1);
    XSFLIB_CHECK(notes[0].offset == 0x24 + 2);
    XSFLIB_CHECK(notes[0].time == 0);

    player.RunFrame();
    XSFLIB_CHECK(notes.size() == 2);
    XSFLIB_CHECK(notes[1].key == 41);
    XSFLIB_CHECK(notes[1].time == 2560);
    XSFLIB_CHECK(notes[1].offset == 0x24 + 4);
}

// A late form song of one channel: the header, then the lists, the wave table and the master list. The channel's list
// comes last. Wave number 2 of the table holds no wave.
Bytes MakeLateSong(const Bytes& list)
{
    constexpr uint32_t kHeader = 0x2C;
    constexpr uint32_t kDataList = kHeader;
    constexpr uint32_t kVolumeList = kDataList + 4;
    constexpr uint32_t kBendList = kVolumeList + 4;
    constexpr uint32_t kWaveTable = kBendList + 4;
    constexpr uint32_t kMasterList = kWaveTable + 6;
    constexpr uint32_t kList = kMasterList + 1;
    Bytes bytes(kList, 0);
    PutAt(bytes, 0x04, 1);
    PutAt(bytes, 0x0C, kDataList);
    PutAt(bytes, 0x10, kVolumeList);
    PutAt(bytes, 0x14, kBendList);
    PutAt(bytes, 0x20, kWaveTable);
    PutAt(bytes, 0x24, kMasterList);
    PutAt(bytes, kDataList, kList);
    bytes[kWaveTable + 3] = 1;
    bytes[kWaveTable + 4] = 0xFF;
    bytes[kWaveTable + 5] = 0xFF;
    bytes[kMasterList] = 0x80;
    bytes.insert(bytes.end(), list.begin(), list.end());

    return bytes;
}

void TestLatePlayer()
{
    // A channel's default tempo is 0x6000 / 60 = 409 of 256 to a tick in each frame. Each note gives a velocity. The
    // third note's wave number holds no wave. The channel rests.
    const Bytes bank_bytes = MakeBank();
    const n64::MusBank bank = n64::MusBank::Parse(bank_bytes);
    const Bytes song_bytes = MakeLateSong({0x81, 1, 40, 100, 10, 41, 100, 10, 0x81, 2, 42, 100, 10, 0x80});
    const n64::MusSong song = n64::MusSong::Parse(song_bytes, n64::MusSongForm::kLate);
    XSFLIB_CHECK(song.wave_table == 0x2C + 12);
    XSFLIB_CHECK(song.master_list == 0x2C + 18);

    const Bytes image(0x1000, 0);
    n64::AudioMemory memory(image, 0x100000);
    n64::AlSynthConfig synth_config;
    synth_config.voices = 4;
    n64::AlSynth synth(synth_config, memory);

    n64::MusLatePlayerConfig config;
    config.channels = 2;
    n64::MusLatePlayer player(config, synth);
    std::vector<n64::MusNoteStart> notes;
    player.on_note = [&notes](const n64::MusNoteStart& note)
    {
        notes.push_back(note);
    };

    n64::MusSoundBank sound_bank;
    sound_bank.bank = &bank;
    sound_bank.bank_address = memory.Allocate(bank_bytes.size());
    sound_bank.sample_address = n64::AudioMemory::kImageBase;
    player.StartSong(song, sound_bank);
    XSFLIB_CHECK(!player.Finished());

    for (int frame = 0; frame < 6; frame++)
    {
        player.RunFrame();
    }

    XSFLIB_CHECK(notes.size() == 1);
    XSFLIB_CHECK(notes[0].key == 40);
    XSFLIB_CHECK(notes[0].velocity == 100);
    XSFLIB_CHECK(notes[0].wave == 1);
    XSFLIB_CHECK(notes[0].channel == 4);
    XSFLIB_CHECK(notes[0].time == 0);

    player.RunFrame();
    XSFLIB_CHECK(notes.size() == 2);
    XSFLIB_CHECK(notes[1].key == 41);
    XSFLIB_CHECK(notes[1].time == 2560);

    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame();
    }

    XSFLIB_CHECK(notes.size() == 2);
    XSFLIB_CHECK(player.Finished());
}

// The commands of the first `frames` frames of a late song on the compact driver.
std::vector<std::pair<uint32_t, uint32_t>> LateCommands(const Bytes& list, n64::MusLatePlayerConfig config, int frames)
{
    const Bytes bank_bytes = MakeBank();
    const n64::MusBank bank = n64::MusBank::Parse(bank_bytes);
    const Bytes song_bytes = MakeLateSong(list);
    const n64::MusSong song = n64::MusSong::Parse(song_bytes, n64::MusSongForm::kLate);

    const Bytes image(0x1000, 0);
    n64::AudioMemory memory(image, 0x100000);
    n64::AlSynthConfig synth_config;
    synth_config.form = n64::AlSynthForm::kCompact;
    synth_config.voices = 4;
    n64::AlSynth synth(synth_config, memory);

    config.channels = 2;
    n64::MusLatePlayer player(config, synth);
    n64::MusSoundBank sound_bank;
    sound_bank.bank = &bank;
    sound_bank.bank_address = memory.Allocate(bank_bytes.size());
    sound_bank.sample_address = n64::AudioMemory::kImageBase;
    player.StartSong(song, sound_bank);

    const uint32_t output = memory.Allocate(n64::kAlCompactChunkSamples * 4 * 4);
    std::vector<std::pair<uint32_t, uint32_t>> commands;
    for (int frame = 0; frame < frames; frame++)
    {
        synth.SetParamSamples(0);
        player.RunFrame();
        n64::AudioList audio;
        synth.BuildCommands(audio, memory, output, n64::kAlCompactChunkSamples * 4);
        for (const n64::AudioCommand& command : audio.commands)
        {
            commands.emplace_back(command.w0, command.w1);
        }
    }

    return commands;
}

void TestLateRestVelocity()
{
    // A note, then a rest. Mario Tennis's build at 0x802EFDF0 skips a rest's velocity byte. Puzzle League's at
    // 0x8008C524 stores it, and the fading note then plays at the new velocity.
    constexpr int kFrames = 12;
    const Bytes quiet_rest = {0x81, 1, 5, 100, 4, 96, 1, 40};
    const Bytes loud_rest = {0x81, 1, 5, 100, 4, 96, 100, 40};
    n64::MusLatePlayerConfig stores;
    XSFLIB_CHECK(LateCommands(quiet_rest, stores, kFrames) != LateCommands(loud_rest, stores, kFrames));

    n64::MusLatePlayerConfig keeps;
    keeps.rest_keeps_velocity = true;
    XSFLIB_CHECK(LateCommands(quiet_rest, keeps, kFrames) == LateCommands(loud_rest, keeps, kFrames));
}

void TestLatePitchBeforeVolume()
{
    // Wave 0 with note 0 is 12.5 semitones up, past the synthesizer's octave, so the player silences it by its
    // velocity. Mario Tennis's build sends the pitch first, at 0x802F0A60: the voice's first volume is already 0.
    // Puzzle League's sends the volume first.
    const auto first_target = [](const n64::MusLatePlayerConfig& config)
    {
        for (const auto& [high, low] : LateCommands({0x81, 0, 0, 100, 40}, config, 1))
        {
            if ((high >> 24) == n64::kAudioSetVolume)
            {
                return high & 0xFFFF;
            }
        }

        return uint32_t{0xFFFFFFFF};
    };

    n64::MusLatePlayerConfig volume_first;
    XSFLIB_CHECK(first_target(volume_first) != 0 && first_target(volume_first) != 0xFFFFFFFF);

    n64::MusLatePlayerConfig pitch_first;
    pitch_first.pitch_before_volume = true;
    XSFLIB_CHECK(first_target(pitch_first) == 0);
}

void TestCompactList()
{
    // One voice in one chunk: the buses' clears, then the voice's filters with the first chunk's four commands of the
    // envelope mixer, then the mixes to the main bus, the interleave and the save.
    const Bytes bank_bytes = MakeBank();
    const n64::MusBank bank = n64::MusBank::Parse(bank_bytes);
    const Bytes song_bytes = MakeSong({0x81, 1, 0, 100, 0x80});
    const n64::MusSong song = n64::MusSong::Parse(song_bytes, n64::MusSongForm::kEarly);

    const Bytes image(0x1000, 0);
    n64::AudioMemory memory(image, 0x100000);
    n64::AlSynthConfig synth_config;
    synth_config.form = n64::AlSynthForm::kCompact;
    synth_config.voices = 4;
    n64::AlSynth synth(synth_config, memory);

    n64::MusPlayerConfig config;
    config.channels = 2;
    config.voices = 4;
    n64::MusPlayer player(config, synth);
    n64::MusSoundBank sound_bank;
    sound_bank.bank = &bank;
    sound_bank.bank_address = memory.Allocate(bank_bytes.size());
    sound_bank.sample_address = n64::AudioMemory::kImageBase;
    player.StartSong(song, sound_bank);

    synth.SetParamSamples(0);
    player.RunFrame();
    const uint32_t output = memory.Allocate(n64::kAlCompactChunkSamples * 4);
    n64::AudioList list;
    synth.BuildCommands(list, memory, output, n64::kAlCompactChunkSamples);

    std::vector<uint8_t> opcodes;
    for (const n64::AudioCommand& command : list.commands)
    {
        opcodes.push_back(command.Opcode());
    }

    const std::vector<uint8_t> expected = {n64::kAudioClearBuffer, n64::kAudioClearBuffer, n64::kAudioLoadAdpcm,
                                           n64::kAudioLoadBuffer,  n64::kAudioAdpcm,       n64::kAudioResample,
                                           n64::kAudioSetVolume,   n64::kAudioSetVolume,   n64::kAudioSetVolume,
                                           n64::kAudioEnvMixer,    n64::kAudioMixer,       n64::kAudioMixer,
                                           n64::kAudioInterleave,  n64::kAudioSaveBuffer};
    XSFLIB_CHECK(opcodes == expected);
    XSFLIB_CHECK(list.commands[0].w0 == 0x020004E0);
    XSFLIB_CHECK(list.commands[0].w1 == 736);
    XSFLIB_CHECK(list.commands[1].w0 == 0x020007C0);
    XSFLIB_CHECK(list.commands[1].w1 == 736);
    XSFLIB_CHECK(list.commands[10].w0 == 0x0C007FFF);
    XSFLIB_CHECK(list.commands[10].w1 == 0x07C004E0);
    XSFLIB_CHECK(list.commands[13].w0 == 0x062E0000);
    XSFLIB_CHECK(list.commands[13].w1 == output);

    // With no new update, the next chunk's mixer has no setup.
    list.Clear();
    synth.BuildCommands(list, memory, output, n64::kAlCompactChunkSamples);
    const auto mixers = std::ranges::count_if(
        list.commands, [](const n64::AudioCommand& command) { return command.Opcode() == n64::kAudioEnvMixer; });
    const auto volumes = std::ranges::count_if(
        list.commands, [](const n64::AudioCommand& command) { return command.Opcode() == n64::kAudioSetVolume; });
    XSFLIB_CHECK(mixers == 1);
    XSFLIB_CHECK(volumes == 0);
}

} // namespace

// The commands of the second chunk of a voice that starts at full volume with no attack. `update` runs after the
// first chunk.
template <typename Update>
std::vector<n64::AudioCommand> SecondChunk(bool later_compact, const Update& update)
{
    const Bytes image(0x1000, 0);
    n64::AudioMemory memory(image, 0x100000);
    n64::AlSynthConfig config;
    config.form = n64::AlSynthForm::kCompact;
    config.voices = 1;
    config.later_compact = later_compact;
    n64::AlSynth synth(config, memory);
    const int voice = synth.AddVoices(1);
    XSFLIB_CHECK(synth.AllocVoice(voice, 1, false));

    n64::AlWave wave;
    wave.base = n64::AudioMemory::kImageBase;
    wave.length = 0x800;
    wave.book = n64::AudioMemory::kImageBase;
    wave.book_size = 0x80;
    synth.StartVoiceParams(voice, wave, 1.0F, 0x7FFF, 64, 0, 0);

    const uint32_t output = memory.Allocate(n64::kAlCompactChunkSamples * 4 * 4);
    n64::AudioList first;
    synth.BuildCommands(first, memory, output, n64::kAlCompactChunkSamples);

    update(synth, voice);
    n64::AudioList second;
    synth.BuildCommands(second, memory, output, n64::kAlCompactChunkSamples);

    return second.commands;
}

n64::AudioCommand CommandWith(const std::vector<n64::AudioCommand>& commands, uint32_t high_half)
{
    const auto found = std::find_if(commands.begin(), commands.end(), [high_half](const n64::AudioCommand& command)
                                    { return (command.w0 >> 16) == high_half; });
    XSFLIB_CHECK(found != commands.end());

    return *found;
}

// The rules of the later compact library, read at 0x800CF930, 0x800CFD68 and 0x800CFF38 in Chameleon Twist 2.
void TestLaterCompact()
{
    constexpr uint32_t kSetTargetLeft = 0x0900;
    constexpr uint32_t kSetCurrentLeft = 0x0906;
    constexpr uint32_t kEnvMixer = 3;

    // Two updates at one time. The second takes the first's target of nothing at once. The earlier form ramps from
    // where the voice is, over no samples.
    const auto two_updates = [](n64::AlSynth& synth, int voice)
    {
        synth.SetParamSamples(synth.CurrentSamples());
        synth.SetVolume(voice, 0, 1000000);
        synth.SetVolume(voice, 0, 1000000);
    };
    XSFLIB_CHECK((CommandWith(SecondChunk(true, two_updates), kSetCurrentLeft).w0 & 0xFFFF) == 1);
    XSFLIB_CHECK((CommandWith(SecondChunk(false, two_updates), kSetCurrentLeft).w0 & 0xFFFF) > 1);

    // A falling ramp with no time holds. The earlier form falls at the fastest rate.
    const auto no_time = [](n64::AlSynth& synth, int voice)
    {
        synth.SetParamSamples(synth.CurrentSamples());
        synth.SetVolume(voice, 0, 0);
    };
    XSFLIB_CHECK(CommandWith(SecondChunk(true, no_time), kSetTargetLeft).w1 == 0);
    XSFLIB_CHECK(CommandWith(SecondChunk(false, no_time), kSetTargetLeft).w1 == 0x80000000);

    // An update behind the chunk's start. Its time rounds to one chunk before the start. The voice then makes a further
    // chunk. The earlier form rounds the distance to nothing.
    const auto behind = [](n64::AlSynth& synth, int voice)
    {
        synth.SetParamSamples(synth.CurrentSamples() - 100);
        synth.SetPitch(voice, 1.0F);
    };
    const auto mixers = [](const std::vector<n64::AudioCommand>& commands)
    {
        return std::count_if(commands.begin(), commands.end(),
                             [](const n64::AudioCommand& command) { return (command.w0 >> 24) == kEnvMixer; });
    };
    XSFLIB_CHECK(mixers(SecondChunk(true, behind)) == 2);
    XSFLIB_CHECK(mixers(SecondChunk(false, behind)) == 1);
}

int main()
{
    return xsflib::testing::RunTest("n64_mus",
                                    []
                                    {
                                        TestBank();
                                        TestSong();
                                        TestPlayerTiming();
                                        TestLatePlayer();
                                        TestLateRestVelocity();
                                        TestLatePitchBeforeVolume();
                                        TestCompactList();
                                        TestLaterCompact();
                                    });
}
