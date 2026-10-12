// SPDX-License-Identifier: MIT

// A test that plays a sound of any sound archive on a model of a console's sound library. The sound must start, and it
// must sound within the time given. With --wav, the model's output is written to a file, to listen to. With --channels,
// each frame's channels are written to a log (see xsflib/testing/channel_log.h). The log gives each channel by its
// place in the model's pool. With --voices, the model gets that many voices and channels. A console's options follow
// these.
//
// usage: archive_play_test <archive> <sound label> <seconds> [--wav <file>] [--channels <file>] [--voices <count>]
//                          [the console's options]
//
// A test's main returns ArchivePlay<Console>(argc, argv). The console supplies:
//
//   Archive                       the sound archive. FindSound(label) gives a sound's index in it, or nothing.
//   Load(bytes)                   the archive in a file's bytes. It may throw.
//   Options                       the model's options, with voice_count
//   Model                         the model of one archive: Model(archive, options). Its engine plays the archive.
//   Frames(seconds)               the number of the model's frames in `seconds`
//   Output(model)                 the stereo samples of the frame that the model played last
//   kSends                        whether the log gives each channel's sends
//   kUsage                        the console's options for the usage line, each after a space, or ""
//   Outputs(channel, state)       sets the pan and span of a channel's state, and its sends with kSends
//   Option(name, value, options)  applies one of the console's options to `options`. Returns false for an unknown
//                                 option or a bad value.
//
// The model's `engine` points to the engine. The engine supplies StartSequence(index), RunFrame(), Rng().State(),
// Channels() and Voices(). StartSequence gives the sound's player, or nullptr. Channels() and Voices() are those that
// the channels' model requires (see ntdlib/nwsnd/channel.h). The player gives its tracks by Track(index). Each track
// gives its newest channel by ChannelList() (see ntdlib/nwsnd/sequence.h).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "xsflib/files.h"
#include "xsflib/testing/channel_log.h"
#include "xsflib/testing/check.h"

namespace ntdlib::testing
{
namespace archive_play_detail
{

// The channels that sound, in the order that the sound library keeps them. A channel's track is the one whose list
// holds the channel, or -1 when no list does.
template <typename Console, typename Engine, typename Player>
std::vector<xsflib::testing::ChannelState> Channels(Engine& engine, const Player& player)
{
    std::map<nwsnd::ChannelId, int> tracks;
    for (int i = 0; i < 16; ++i)
    {
        if (const auto* track = player.Track(i))
        {
            for (nwsnd::ChannelId place = track->ChannelList(); place != nwsnd::kNoChannel;
                 place = engine.Channels().At(place).next_in_track)
            {
                tracks[place] = i;
            }
        }
    }

    std::vector<xsflib::testing::ChannelState> channels;
    for (const nwsnd::ChannelId place : engine.Channels().InUse())
    {
        const auto& channel = engine.Channels().At(place);
        xsflib::testing::ChannelState state;
        state.id = static_cast<uint32_t>(place);
        const auto track = tracks.find(place);
        state.track = track != tracks.end() ? track->second : -1;
        state.key = channel.key;
        state.original_key = channel.original_key;
        state.velocity = channel.velocity_volume;
        state.envelope = static_cast<uint32_t>(channel.envelope.GetStatus());
        state.level = channel.envelope.Level();
        if (channel.voice != nwsnd::kNoVoice)
        {
            const auto& voice = engine.Voices().At(channel.voice);
            state.volume = voice.Volume();
            state.pitch = voice.Pitch();
        }
        Console::Outputs(channel, state);
        state.note_pan = channel.init_pan;
        channels.push_back(state);
    }

    return channels;
}

} // namespace archive_play_detail

// The test's main for `Console` (see above). Returns 2 for a wrong command line, 1 when the sound can't be played or a
// check fails, and 0 otherwise. The WAV file is written even when the sound never sounds.
template <typename Console>
int ArchivePlay(int argc, char** argv)
{
    using xsflib::testing::Check;

    std::string wav;
    std::string channels_path;
    typename Console::Options options;
    bool known = true;
    for (int i = 4; i + 1 < argc; i += 2)
    {
        const std::string option = argv[i];
        if (option == "--wav")
        {
            wav = argv[i + 1];
        }
        else if (option == "--channels")
        {
            channels_path = argv[i + 1];
        }
        else if (option == "--voices")
        {
            options.voice_count = std::atoi(argv[i + 1]);
        }
        else if (!Console::Option(option, argv[i + 1], options))
        {
            known = false;
        }
    }

    if (argc < 4 || argc % 2 == 1 || !known || options.voice_count < 1)
    {
        std::fprintf(stderr,
                     "usage: archive_play_test <archive> <sound label> <seconds> [--wav <file>] [--channels <file>] "
                     "[--voices <count>]%s\n",
                     Console::kUsage);
        return 2;
    }

    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(argv[1], bytes))
        {
            throw std::runtime_error(std::string("can't read ") + argv[1]);
        }

        const typename Console::Archive archive = Console::Load(std::move(bytes));
        const std::optional<uint32_t> index = archive.FindSound(argv[2]);
        if (!Check(index.has_value(), std::string("the archive has ") + argv[2]))
        {
            return 1;
        }

        typename Console::Model model(archive, options);
        const auto* player = model.engine->StartSequence(*index);
        if (!Check(player != nullptr, std::string(argv[2]) + " starts"))
        {
            return 1;
        }

        const int frames = Console::Frames(std::atoi(argv[3]));
        std::vector<int16_t> samples;
        int peak = 0;
        std::optional<xsflib::testing::ChannelLog> log;
        if (!channels_path.empty())
        {
            log.emplace(channels_path, Console::kSends);
        }

        for (int frame = 0; frame < frames; ++frame)
        {
            model.engine->RunFrame();
            for (const int16_t sample : Console::Output(model))
            {
                samples.push_back(sample);
                peak = std::max(peak, std::abs(static_cast<int>(sample)));
            }
            if (log)
            {
                log->Frame(frame + 1, model.engine->Rng().State(),
                           archive_play_detail::Channels<Console>(*model.engine, *player));
            }
        }

        Check(peak > 0, std::string(argv[2]) + " sounds in its first " + argv[3] + " s");

        if (!wav.empty())
        {
            xsflib::testing::WriteWav(wav, samples);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }

    return xsflib::testing::Summary("archive play");
}

} // namespace ntdlib::testing
