// SPDX-License-Identifier: MIT

// Tests of NW4R SND's wave sounds on the model of the Wii (rvl/snd/wave_sound.h), on small archives built here. A wave
// sound plays its first note's wave once, with the wave sound's pitch, pan, surround pan and sends, and with the
// volume, the channel priority and the release priority fix of its entry. The note's key, volume, pan and pitch stay
// unread. The builds from March 2008 take the note's hold. The build of October 2006 starts the main send at 1 and sets
// no release priority fix. A wave sound that lacks its note or its wave ends in its first update. A wave that doesn't
// loop ends the sound in the frame after its voice ends, and a wave that loops plays on. A snapshot keeps the wave
// sound's player. The expected values are the ones that NW4R SND's WsdPlayer and WsdFileReader give in the code of Wii
// Sports, Mario Kart Wii and WarioWare: Smooth Moves (see wave_sound.h).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/wave_sound.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_files.h"
#include "xsflib/testing/check.h"

namespace
{

using namespace ntdlib;
using namespace ntdlib::testing::rsar_files;
namespace snd = ntdlib::rvl::snd;
using VoiceParam = ntdlib::nwsnd::RevolutionPolicy::ChannelVoiceParam;
using xsflib::testing::Check;

// The builds: Wii Sports' of 2007, Mario Kart Wii's of 2008 and the build of October 2006.
constexpr SndBuild k2007{20070227, false};
constexpr SndBuild k2008{20080620, false};
constexpr SndBuild kOctober2006{0, true};

// A wave of `count` samples of a ramp, mono PCM16 at 32 kHz.
Bytes RampWave(uint32_t count, bool loop = false)
{
    std::vector<int16_t> samples(count);
    for (uint32_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(static_cast<int>(i % 200) * 100 - 10000);
    }

    return MakePcm16Rwav(count, samples, 1, loop);
}

// An archive of wave sounds of an RWSD file of version 1.3. The file's waves: 9600 samples that don't loop, then 960
// that loop.
struct TestArchive
{
    TestArchive(const std::vector<WaveSoundSpec>& sounds, const std::vector<ArchiveWaveSound>& entries)
        : bytes(MakeWaveSoundArchive(MakeWaveSoundFile(0x0103, sounds),
                                     MakeRwarOf({RampWave(9600), RampWave(960, true)}), entries)),
          archive(rsar::SoundArchive::Load(bytes, snd::kRsarOptions))
    {
    }

    Bytes bytes;
    rsar::SoundArchive archive;
};

// What a wave sound did while it played: each frame's values of its channel, its channel's callbacks with the frame of
// each, the frame after which the player had finished, and the output.
struct Played
{
    std::vector<VoiceParam> params;
    std::vector<int> callback_frames;
    int finished_frame = -1;
    std::vector<int16_t> output;
};

// Plays wave sound `index` on the renderer's model in `build` for `frames` frames. `each` runs after each frame.
Played Play(const rsar::SoundArchive& archive, uint32_t index, SndBuild build, int frames,
            const std::function<void(snd::ArchiveModel&, snd::WsdPlayer&)>& each = {})
{
    Played played;
    auto model = std::make_unique<snd::ArchiveModel>();
    model->build = build;
    model->StartWithRenderer(archive);
    int frame = 0;
    model->engine->SetVoiceParamHook([&](snd::ChannelId, const VoiceParam& param) { played.params.push_back(param); });
    model->engine->SetChannelEventHook([&](const snd::Channel&, snd::Channel::CallbackStatus)
                                       { played.callback_frames.push_back(frame); });
    snd::WsdPlayer* player = model->engine->StartWaveSound(index);
    Check(player != nullptr, "wave sound " + std::to_string(index) + " starts");
    if (player == nullptr)
    {
        return played;
    }

    for (; frame < frames; frame++)
    {
        model->RunFrame(played.output);
        if (played.finished_frame < 0 && player->IsFinished())
        {
            played.finished_frame = frame;
        }
        if (each)
        {
            each(*model, *player);
        }
    }

    return played;
}

// A wave sound with a value in every field, and a note whose key, volume, pan and pitch the player doesn't read.
WaveSoundSpec FullSound()
{
    WaveSoundNote note;
    note.original_key = 62;
    note.volume = 80;
    note.pan = 70;
    note.surround_pan = 3;
    note.pitch = 0.5f;

    WaveSoundSpec sound;
    sound.pitch = 1.5f;
    sound.pan = 32;
    sound.surround_pan = 10;
    sound.fx_sends[0] = 20;
    sound.fx_sends[1] = 40;
    sound.fx_sends[2] = 60;
    sound.main_send = 100;
    sound.notes = {note};

    return sound;
}

// A wave sound that plays wave `wave` with `pan` and `surround_pan`.
WaveSoundSpec SoundWith(int32_t wave, uint8_t pan = 64, uint8_t surround_pan = 0)
{
    WaveSoundNote note;
    note.wave = wave;

    WaveSoundSpec sound;
    sound.pan = pan;
    sound.surround_pan = surround_pan;
    sound.notes = {note};

    return sound;
}

// The values that the player gives its channel in every frame (WsdPlayer::UpdateChannel): the volume of the entry, the
// wave sound's pitch, its pan from 64 in 63rds, its surround pan in 63rds plus 64ths, its main send less 1 and its
// sends. The note's values don't reach the channel.
void CheckValues()
{
    const TestArchive test({FullSound()}, {{90, 0, 64, true, 1, 2}});
    const Played played = Play(test.archive, 0, k2007, 10);
    Check(played.params.size() == 10,
          "the channel takes values in each frame (" + std::to_string(played.params.size()) + ")");
    if (played.params.empty())
    {
        return;
    }

    const VoiceParam& param = played.params.back();
    const float span = 0.0f + 10.0f / 63.0f + 10.0f * (1.0f / 64.0f);
    Check(param.volume == 90.0f / 127.0f && param.pitch == 1.5f, "the entry's volume and the wave sound's pitch");
    Check(param.pan == -32.0f / 63.0f && param.surround_pan == span, "the wave sound's pan and surround pan");
    Check(param.main_send == 100.0f / 127.0f - 1.0f && param.fx_send[0] == 20.0f / 127.0f &&
              param.fx_send[1] == 40.0f / 127.0f && param.fx_send[2] == 60.0f / 127.0f,
          "the wave sound's main send and sends");
    Check(param.pan_mode == 1 && param.pan_curve == 2 && param.lpf_freq == 1.0f,
          "the entry's pan mode and curve, and the filter left open");

    // The volume of the entry, held to full volume.
    const TestArchive loud({FullSound()}, {{200, 0, 64, false, 0, 0}});
    const Played loud_played = Play(loud.archive, 0, k2007, 2);
    Check(!loud_played.params.empty() && loud_played.params.back().volume == 1.0f,
          "a volume above 127 plays at full volume");

    // The pans of 0 and 1 count from 63, as do the surround pans of 0 and 1 in their first term.
    const TestArchive edges({SoundWith(0, 0, 0), SoundWith(0, 1, 1), SoundWith(0, 127, 127)},
                            {{127, 0, 64, false, 0, 0}, {127, 1, 64, false, 0, 0}, {127, 2, 64, false, 0, 0}});
    const float spans[] = {1.0f / 63.0f, 0.0f + 2.0f / 63.0f + 1.0f / 64.0f,
                           0.0f + 127.0f / 63.0f + 127.0f * (1.0f / 64.0f)};
    const float pans[] = {-63.0f / 63.0f, -62.0f / 63.0f, 63.0f / 63.0f};
    for (uint32_t i = 0; i < 3; i++)
    {
        const Played edge = Play(edges.archive, i, k2007, 2);
        Check(!edge.params.empty() && edge.params.back().pan == pans[i] && edge.params.back().surround_pan == spans[i],
              "the pan and the surround pan of wave sound " + std::to_string(i));
    }
}

// The channel's priority is the entry's channel priority plus 64. The channel takes the entry's release priority fix,
// except in the build of October 2006. That build also starts the main send at 1.
void CheckPriorities()
{
    const TestArchive test({FullSound()}, {{127, 0, 70, true, 0, 0}});
    for (const SndBuild build : {k2007, kOctober2006})
    {
        const bool october = build.October2006();
        const std::string name = october ? "the build of October 2006" : "the build of 2007";
        int priority = -1;
        bool fix = !october;
        const Played played = Play(test.archive, 0, build, 2,
                                   [&](snd::ArchiveModel& model, snd::WsdPlayer& player)
                                   {
                                       if (player.ChannelPlace() == snd::kNoChannel)
                                       {
                                           return;
                                       }

                                       const snd::Channel& channel = model.engine->Channels().At(player.ChannelPlace());
                                       priority = model.engine->Voices().At(channel.voice).Priority();
                                       fix = channel.release_priority_fix;
                                   });
        Check(priority == 134, name + ": the channel's priority (" + std::to_string(priority) + ")");
        Check(fix == !october, name + ": the release priority fix");
        const float main_send = october ? 1.0f + (100.0f / 127.0f - 1.0f) : 100.0f / 127.0f - 1.0f;
        Check(!played.params.empty() && played.params.back().main_send == main_send, name + ": the main send");
    }
}

// The builds from March 2008 take the note's hold. After an attack that takes no time, the envelope holds.
void CheckHold()
{
    WaveSoundSpec sound = SoundWith(0);
    sound.notes[0].hold = 50;
    const TestArchive test({sound}, {{127, 0, 64, false, 0, 0}});
    for (const SndBuild build : {k2007, k2008})
    {
        ntdlib::nwsnd::AdshrStatus status = ntdlib::nwsnd::AdshrStatus::kAttack;
        Play(test.archive, 0, build, 3,
             [&](snd::ArchiveModel& model, snd::WsdPlayer& player)
             {
                 if (player.ChannelPlace() != snd::kNoChannel)
                 {
                     status = model.engine->Channels().At(player.ChannelPlace()).envelope.GetStatus();
                 }
             });
        const bool holds = build.SinceMarch2008();
        Check((status == ntdlib::nwsnd::AdshrStatus::kHold) == holds,
              std::string(holds ? "the build of 2008" : "the build of 2007") + ": the hold");
    }
}

// A wave that doesn't loop ends the sound in the frame after its voice ends. A wave of 9600 samples at a pitch of 1
// plays for 100 frames, and at a pitch of 1.5 for 66.7. A wave that loops plays on.
void CheckEnds()
{
    WaveSoundSpec faster = SoundWith(0);
    faster.pitch = 1.5f;
    const TestArchive test({SoundWith(0), faster, SoundWith(1)},
                           {{127, 0, 64, false, 0, 0}, {127, 1, 64, false, 0, 0}, {127, 2, 64, false, 0, 0}});
    const int lengths[] = {100, 67};
    for (uint32_t i = 0; i < 2; i++)
    {
        const Played played = Play(test.archive, i, k2007, 150);
        const std::string name = "wave sound " + std::to_string(i);
        Check(played.callback_frames.size() == 1, name + ": its channel calls back once");
        if (played.callback_frames.size() != 1)
        {
            continue;
        }

        const int ended = played.callback_frames[0];
        Check(ended >= lengths[i] && ended <= lengths[i] + 3,
              name + ": the voice ends after its wave (frame " + std::to_string(ended) + ")");
        Check(played.finished_frame == ended + 1,
              name + ": the player finishes in the next frame (" + std::to_string(played.finished_frame) + ")");

        // The output: the wave's ramp, the same on both sides, and silence after the voice ends.
        bool sounds = false;
        bool centred = true;
        bool silent = true;
        for (std::size_t s = 0; s + 1 < played.output.size(); s += 2)
        {
            sounds = sounds || played.output[s] != 0;
            centred = centred && played.output[s] == played.output[s + 1];
            if (s / 2 >= static_cast<std::size_t>(ended + 2) * 96)
            {
                silent = silent && played.output[s] == 0 && played.output[s + 1] == 0;
            }
        }

        Check(sounds && centred && silent, name + ": the output");
    }

    const Played looped = Play(test.archive, 2, k2007, 200);
    Check(looped.callback_frames.empty() && looped.finished_frame < 0, "a wave that loops plays on");
}

// A wave sound that the file lacks, a note that the wave sound lacks, and a wave that the file lacks each give a player
// that ends in its first update.
void CheckMissing()
{
    WaveSoundSpec without_notes = SoundWith(0);
    without_notes.notes.clear();
    const TestArchive test({without_notes, SoundWith(5)},
                           {{127, 0, 64, false, 0, 0}, {127, 1, 64, false, 0, 0}, {127, 7, 64, false, 0, 0}});
    for (uint32_t i = 0; i < 3; i++)
    {
        const Played played = Play(test.archive, i, k2007, 3);
        Check(played.finished_frame == 0 && played.params.empty(),
              "wave sound " + std::to_string(i) + " ends in its first update");
    }

    auto model = std::make_unique<snd::ArchiveModel>();
    model->StartWithRenderer(test.archive);
    Check(model->engine->StartWaveSound(3) == nullptr, "a sound that the archive lacks doesn't start");

    const Bytes sequence_bytes = MakeArchive(MakeSequence({0xFF}), 0, 1);
    const rsar::SoundArchive sequences = rsar::SoundArchive::Load(sequence_bytes, snd::kRsarOptions);
    auto other = std::make_unique<snd::ArchiveModel>();
    other->StartWithRenderer(sequences);
    Check(other->engine->StartWaveSound(0) == nullptr, "a sequence sound doesn't start as a wave sound");
}

// A snapshot keeps the wave sound's player. A restore gives the same output again. The game's sends reach the wave
// sound's player. A restore after a later start is refused.
void CheckSnapshots()
{
    const TestArchive test({SoundWith(0)}, {{127, 0, 64, false, 0, 0}});
    auto model = std::make_unique<snd::ArchiveModel>();
    model->StartWithRenderer(test.archive);
    model->engine->SetEffects({}, {{0, 1, 0.25f}}, 0);
    snd::WsdPlayer* player = model->engine->StartWaveSound(0);
    Check(player != nullptr && model->engine->SoundSends(player->Id())[1] == 0.25f, "the game's sends");
    std::vector<int16_t> before;
    for (int frame = 0; frame < 10; frame++)
    {
        model->RunFrame(before);
    }

    const snd::ArchiveModel::Snapshot snapshot = model->Save();
    std::vector<int16_t> first;
    for (int frame = 0; frame < 120; frame++)
    {
        model->RunFrame(first);
    }

    model->Restore(snapshot);
    std::vector<int16_t> second;
    for (int frame = 0; frame < 120; frame++)
    {
        model->RunFrame(second);
    }

    Check(first == second && player->IsFinished(), "a restore gives the same output again");
    model->engine->StartWaveSound(0);
    bool refused = false;
    try
    {
        model->Restore(snapshot);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }

    Check(refused, "a restore after another start is refused");
}

} // namespace

int main()
{
    const auto tests = []
    {
        CheckValues();
        CheckPriorities();
        CheckHold();
        CheckEnds();
        CheckMissing();
        CheckSnapshots();
    };

    return xsflib::testing::RunTest("rvl_snd", tests);
}
