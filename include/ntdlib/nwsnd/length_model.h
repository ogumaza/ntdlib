// SPDX-License-Identifier: MIT

// The length analysis of a sequence sound of an archive of the 3DS, the Wii U or the Switch, on ntdlib's model of the
// console's sound library: xsflib's analysis (xsflib/rip/length.h) with a model that counts each track's passes. A
// console's struct gives the type of the console's model and what differs between the consoles: the frame's length,
// the output, the voices, and whether the effects still ring. The Switch's struct is ntdlib::nx::atk::LengthConsole
// (ntdlib/nx/atk/length_model.h), the Wii U's is ntdlib::cafe::snd::LengthConsole (ntdlib/cafe/snd/length_model.h), and
// the 3DS's is ntdlib::ctr::snd::LengthConsole (ntdlib/ctr/snd/length_model.h).
// The engines' hooks for the analysis are in length_hooks.h.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>

#include "xsflib/rip/length.h"

#include "ntdlib/nwsnd/sequence.h"

namespace ntdlib::nwsnd
{

// The counted loops that a track runs in, each a track and its loop's body (SequenceTrack::CallStack).
using CountedLoopSet = std::set<std::pair<int, uint32_t>>;

// The analysis's rules in the frames of a console whose frames last `frame_ms`: at most 600 s, and a sequence that
// stays busy without a sound for 180 s has ended. The other rules are xsflib's defaults: a frame's commands sound from
// the next frame on, and a sequence that ends ends at its first idle frame when that comes before its last sound's
// frame ends.
inline xsflib::rip::LengthRules LengthRulesFor(double frame_ms)
{
    constexpr double kSilentMs = 180000.0;
    xsflib::rip::LengthRules rules;
    rules.frame_ms = frame_ms;
    rules.max_frames = static_cast<uint64_t>(rules.max_ms / frame_ms);
    rules.silent_frames = static_cast<uint64_t>(kSilentMs / frame_ms);

    return rules;
}

// The model that xsflib's length analysis (xsflib::rip::MeasureLength) reads: the console's model and the sound's
// player. The model also counts each track's passes. A looping sequence's fade then waits for the tracks that loop at
// other lengths than the main track (OthersLooped). A track that another track opens again makes a pass each time. The
// fade therefore doesn't wait for the tracks that a looping track opens again in each of its loops.
template <class Console>
class LengthModel
{
public:
    using Model = typename Console::Model;
    using Engine = std::remove_reference_t<decltype(*std::declval<Model&>().engine)>;
    using Player = std::remove_pointer_t<decltype(std::declval<Engine&>().StartSequence(0))>;

    LengthModel(Model& model, Player& player) : model_(model), engine_(*model.engine), player_(player)
    {
    }

    bool RunFrame()
    {
        engine_.RunFrame();
        ++frames_;

        return true;
    }

    // The frames run so far.
    uint64_t Frames() const
    {
        return frames_;
    }

    bool Sounded() const
    {
        return xsflib::rip::Audible(Console::Output(model_));
    }

    bool Ringing() const
    {
        return Console::Ringing(model_);
    }

    std::size_t VoiceCount() const
    {
        return Console::VoiceCount(model_);
    }

    xsflib::rip::VoicePlace Voice(std::size_t index) const
    {
        return Console::Voice(model_, index);
    }

    bool Active() const
    {
        return player_.IsActive();
    }

    bool Settled() const
    {
        return player_.IsSettled(engine_);
    }

    bool Stuck() const
    {
        return player_.IsStuck(engine_);
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    bool Busy() const
    {
        return engine_.IsBusy();
    }

    bool ChannelsChanging() const
    {
        return engine_.Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return engine_.Channels().ActiveCount();
    }

    // Sets the hooks for the analysis's loops, for every track's passes and for each open of a track. Only the passes
    // that changed the sound count. An open of a track that a track opened before is such a pass: the open clears the
    // track's wait, and the track starts its part again at once. The first open isn't a pass.
    template <typename Hook>
    void OnLoop(Hook hook)
    {
        engine_.SetLoopHook(std::move(hook));
        const auto count_pass = [this](int track, uint32_t)
        {
            const auto* looping = player_.Track(track);
            if (looping && !looping->IsIdle())
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };
        engine_.SetTrackLoopHook(count_pass);
        const auto count_open = [this](int, int opened, uint32_t)
        {
            const auto index = static_cast<std::size_t>(opened);
            if (opened_[index])
            {
                ++passes_[index];
            }

            opened_[index] = true;
        };
        engine_.SetOpenTrackHook(count_open);
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, or that waits for ever, doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        for (int i = 0; i < kTrackCount; ++i)
        {
            const auto* other = player_.Track(i);
            if (i != track && other && other->IsOpen() && !other->IsIdle() && !other->WaitsForEver(engine_) &&
                passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
    }

private:
    Model& model_;
    Engine& engine_;
    Player& player_;
    uint64_t frames_ = 0;
    std::array<int, kTrackCount> passes_{};  // each track's passes that changed the sound
    std::array<bool, kTrackCount> opened_{}; // whether another track has opened each track
};

// The counted loops that the player's open tracks are in.
template <class Player>
CountedLoopSet CountedLoops(const Player& player)
{
    CountedLoopSet loops;
    for (int i = 0; i < kTrackCount; ++i)
    {
        const auto* track = player.Track(i);
        if (!track || !track->IsOpen())
        {
            continue;
        }

        for (const auto& entry : track->CallStack())
        {
            if (entry.is_loop && entry.loop_count != 0)
            {
                loops.insert({i, entry.address});
            }
        }
    }

    return loops;
}

// Measures sequence sound `sound` on a model that `make_model()` makes, a std::unique_ptr<Console::Model> with the
// caller's settings, such as the effects. The counted loops in `endless_loops` loop for ever. Returns nothing for a
// sound that doesn't start. A sequence can repeat its music in a counted loop that the game plays for longer than the
// limit. When the measure reaches the limit inside counted loops, it runs again on a new model with those loops
// endless, where the console's engine can make them endless (the Switch's). Their passes then count as loops of the
// sequence. The analysis's exceptions pass through: xsflib::rip::TryMeasure turns them into nothing.
template <class Console, class MakeModel>
std::optional<xsflib::rip::Timing> MeasureSequence(const MakeModel& make_model, uint32_t sound,
                                                   const CountedLoopSet& endless_loops = {})
{
    const std::unique_ptr<typename Console::Model> model = make_model();
    auto& engine = *model->engine;
    constexpr bool kEndless = requires { engine.SetEndlessLoops(endless_loops); };
    if constexpr (kEndless)
    {
        engine.SetEndlessLoops(endless_loops);
    }
    auto* player = engine.StartSequence(sound);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel<Console> length_model(*model, *player);
    const xsflib::rip::LengthRules rules = LengthRulesFor(Console::kFrameMs);
    const std::optional<xsflib::rip::Timing> timing = xsflib::rip::MeasureLength(length_model, rules);
    if constexpr (kEndless)
    {
        if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
        {
            if (const CountedLoopSet running = CountedLoops(*player); !running.empty())
            {
                return MeasureSequence<Console>(make_model, sound, running);
            }
        }
    }

    return timing;
}

} // namespace ntdlib::nwsnd
