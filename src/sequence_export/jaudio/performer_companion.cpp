// SPDX-License-Identifier: MIT

// The performer's companion (see performer.h and SequenceSound::companion): the sounds that the game's code starts
// beside the sequence, from the sequence's beat, once each game frame.

#include <cmath>

#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{

bool Performer::BeatTrack(int t) const
{
    if (root_track_ < 0 || TrackAt(t).voice >= 0)
    {
        return false;
    }

    const int track = TrackAt(root_track_).children[0];
    return track >= 0 && (t == track || t == TrackAt(track).children[0]);
}

void Performer::InVoice(int voice)
{
    if (voice == in_voice_)
    {
        return;
    }

    if (in_voice_ >= 0)
    {
        CompanionVoice& v = voices_of_companion_[static_cast<std::size_t>(in_voice_)];
        v.tempo = tempo_;
        v.timebase = timebase_;
    }
    else
    {
        music_tempo_ = tempo_;
        music_timebase_ = timebase_;
        music_rate_ = tempo_rate_;
    }

    in_voice_ = voice;
    if (voice >= 0)
    {
        const CompanionVoice& v = voices_of_companion_[static_cast<std::size_t>(voice)];
        tempo_ = v.tempo;
        timebase_ = v.timebase;
        tempo_rate_ = v.sound->tempo_ratio;
        data_ = v.sound->data;
    }
    else
    {
        tempo_ = music_tempo_;
        timebase_ = music_timebase_;
        tempo_rate_ = music_rate_;
        data_ = sound_.data;
    }
}

void Performer::StopVoice(std::size_t voice)
{
    if (voice >= voices_of_companion_.size())
    {
        return;
    }

    const int root = voices_of_companion_[voice].root;
    if (root >= 0 && TrackAt(root).open)
    {
        InVoice(static_cast<int>(voice));
        UpdateSeq(root, false, 1.0f);
        CloseTrack(root);
        InVoice(-1);
    }
}

void Performer::StartVoice(const CompanionSound& start)
{
    if (start.sound == nullptr || start.sound->start >= start.sound->data.size())
    {
        return;
    }

    if (start.voice >= voices_of_companion_.size())
    {
        voices_of_companion_.resize(start.voice + 1);
    }

    // The game stops the voice's sound first (JAISound::stop), then starts the new one.
    StopVoice(start.voice);
    CompanionVoice& v = voices_of_companion_[start.voice];
    v.sound = start.sound;
    v.level = start.level;
    v.accumulator = 1.0f;
    v.tempo = 120;
    v.timebase = 48;

    InVoice(static_cast<int>(start.voice));
    if (v.root < 0)
    {
        v.root = NewTrack(-1, 0);
    }
    else
    {
        // The voice's first track starts afresh. It keeps its MIDI track and its children's.
        Track& old = TrackAt(v.root);
        Track fresh;
        fresh.parent = -1;
        fresh.children = old.children;
        fresh.slots.fill(-1);
        fresh.move[0].value = 1.0f;
        fresh.move[3].value = 0.5f;
        for (MoveParam& m : fresh.move)
        {
            m.target = m.value;
        }
        fresh.slot = old.slot;
        fresh.name = old.name;
        fresh.output = old.output;
        fresh.voice = old.voice;
        old = std::move(fresh);
    }

    Track& tr = TrackAt(v.root);
    tr.pc = start.sound->start;
    tr.open = true;
    std::map<uint8_t, uint16_t> ports;
    for (const auto& [port, value] : start.sound->ports)
    {
        ports[port & 15] = value;
    }
    for (const auto& [port, value] : start.ports)
    {
        ports[port & 15] = value;
    }
    for (const auto& [port, value] : ports)
    {
        tr.ports[port] = value;
        tr.imported = static_cast<uint16_t>(tr.imported | (1u << port));
    }
    InVoice(-1);
}

void Performer::AdvanceCompanion()
{
    if (!companion_)
    {
        return;
    }

    // The voices' ticks in the sub-frame, at their tempos (JASTrack::seqMain).
    for (std::size_t i = 0; i < voices_of_companion_.size(); ++i)
    {
        CompanionVoice& v = voices_of_companion_[i];
        if (v.root < 0 || !TrackAt(v.root).open)
        {
            continue;
        }

        InVoice(static_cast<int>(i));
        while (v.accumulator >= 1.0f)
        {
            v.accumulator -= 1.0f;
            const int ret = TickProc(v.root);
            UpdateSeq(v.root, false, 1.0f);
            if (ret < 0)
            {
                CloseTrack(v.root);
                break;
            }
        }

        if (TrackAt(v.root).open)
        {
            v.accumulator += TicksPerSubFrame();
            UpdateSeq(v.root, false, 1.0f);
        }
        InVoice(-1);
    }

    // The game's frames that end in the sub-frame: the companion's work after the audio frame.
    const double now = static_cast<double>(subframe_) * kSubFrameSeconds;
    while (now >= static_cast<double>(frames_ + 1) * sound_.frame_seconds)
    {
        frames_++;
        const auto port = [this](int child, int p) -> std::optional<uint16_t>
        {
            const int track = root_track_ < 0 ? -1 : TrackAt(root_track_).children[0];
            if (track < 0 || !TrackAt(track).open)
            {
                return std::nullopt;
            }

            const int target = child < 0 ? track : TrackAt(track).children[static_cast<std::size_t>(child & 15)];
            if (target < 0 || !TrackAt(target).open)
            {
                return std::nullopt;
            }

            return TrackAt(target).ports[static_cast<std::size_t>(p & 15)];
        };

        for (const CompanionSound& sound : companion_->Frame(port))
        {
            if (sound.action == CompanionSound::Action::kStart)
            {
                StartVoice(sound);
            }
            else
            {
                StopVoice(sound.voice);
            }
        }
    }
}

} // namespace ntdlib::sequence_export::jaudio
