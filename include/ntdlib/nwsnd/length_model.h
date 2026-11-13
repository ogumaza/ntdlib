// SPDX-License-Identifier: MIT

// The length analysis of a sequence sound of an archive of the Wii, the 3DS, the Wii U or the Switch, on ntdlib's model
// of the console's sound library: xsflib's analysis (xsflib/rip/length.h) with a model that counts each track's passes.
// A console's struct gives the type of the console's model and what differs between the consoles: the frame's length,
// the output, the voices, and whether the effects still ring. The Switch's struct is ntdlib::nx::atk::LengthConsole
// (ntdlib/nx/atk/length_model.h), the Wii U's is ntdlib::cafe::snd::LengthConsole (ntdlib/cafe/snd/length_model.h), the
// 3DS's is ntdlib::ctr::snd::LengthConsole (ntdlib/ctr/snd/length_model.h), and the Wii's is
// ntdlib::rvl::snd::LengthConsole (ntdlib/rvl/snd/length_model.h).
//
// A struct may give four more things. Where the console's engine doesn't run frames by itself, `static bool
// RunFrame(Model&)` runs a frame of the model and returns false if the model fails. Where an open of a track keeps the
// track's wait, `kOpenKeepsWait` is true (see TrackPasses::SetHooks). The Wii's struct gives both. Where the game's
// code takes later steps that can change the sound, such as a script that starts the sound and sets its variables,
// `static bool Pending(const Model&)` says whether a step is still to come. Where the frame's sound depends on more
// than the output's samples, such as a volume that the caller applies, `static bool Sounded(const Model&)` replaces the
// test of `Output`. The console's engine gives SetEndlessLoops (see MeasureSequence). The engines' hooks for the
// analysis are in length_hooks.h.

#pragma once

#include <algorithm>
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

// Whether an open of a track on the console of `Console` keeps the track's wait: the struct's kOpenKeepsWait, or false.
template <class Console>
constexpr bool OpenKeepsWait()
{
    if constexpr (requires { Console::kOpenKeepsWait; })
    {
        return Console::kOpenKeepsWait;
    }
    else
    {
        return false;
    }
}

// The passes of each track of a sequence player that changed the sound. A model for xsflib's length analysis counts
// them for OthersLooped. A looping sequence's fade then waits for the tracks that loop at other lengths than the main
// track. A track that another track opens again makes a pass each time. The fade therefore doesn't wait for the tracks
// that a looping track opens again in each of its loops. Set `kOpenKeepsWait` where an open of a track keeps the
// track's wait (OpenKeepsWait). The engine's hooks point to the object. The object therefore can't be copied.
template <class Engine, class Player, bool kOpenKeepsWait = false>
class TrackPasses
{
public:
    TrackPasses(Engine& engine, const Player& player) : engine_(engine), player_(player)
    {
    }

    TrackPasses(const TrackPasses&) = delete;
    TrackPasses& operator=(const TrackPasses&) = delete;

    // Sets the engine's hooks for every track's passes and for each open of a track. Only the passes that changed the
    // sound count. An open of a track that a track opened before is such a pass: the track starts its part again. The
    // first open isn't a pass. On most consoles, the open clears the track's wait, and the track starts again at once.
    // Where the open keeps the wait, the pass counts at the track's next command. Two opens during one wait then make
    // one pass.
    void SetHooks()
    {
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
                if constexpr (kOpenKeepsWait)
                {
                    reopened_[index] = true;
                }
                else
                {
                    ++passes_[index];
                }
            }

            opened_[index] = true;
        };
        engine_.SetOpenTrackHook(count_open);
        if constexpr (kOpenKeepsWait)
        {
            const auto count_start = [this](int track, uint32_t)
            {
                const auto index = static_cast<std::size_t>(track);
                if (reopened_[index])
                {
                    ++passes_[index];
                    reopened_[index] = false;
                }
            };
            engine_.SetCommandStartHook(count_start);
        }
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. Only the tracks
    // in `tracks`, a bit for each, count. A track whose last pass changed nothing, that waits for ever or that isn't in
    // `tracks` doesn't hold the fade back.
    bool OthersLooped(int track, uint32_t tracks = ~0u) const
    {
        for (int i = 0; i < kTrackCount; ++i)
        {
            const auto* other = player_.Track(i);
            if (i != track && (tracks & (1u << i)) != 0 && other && other->IsOpen() && !other->IsIdle() &&
                !other->WaitsForEver(engine_) && passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
    }

private:
    Engine& engine_;
    const Player& player_;
    std::array<int, kTrackCount> passes_{};    // each track's passes that changed the sound
    std::array<bool, kTrackCount> opened_{};   // whether another track has opened each track
    std::array<bool, kTrackCount> reopened_{}; // whether each track's next command starts its part again after an open
};

// The model that xsflib's length analysis (xsflib::rip::MeasureLength) reads: the console's model and the sound's
// player. The model also counts each track's passes (TrackPasses).
template <class Console>
class LengthModel
{
public:
    using Model = typename Console::Model;
    using Engine = std::remove_reference_t<decltype(*std::declval<Model&>().engine)>;
    using Player = std::remove_pointer_t<decltype(std::declval<Engine&>().StartSequence(0))>;

    LengthModel(Model& model, Player& player)
        : model_(model), engine_(*model.engine), player_(player), passes_(engine_, player_)
    {
    }

    bool RunFrame()
    {
        if constexpr (requires { Console::RunFrame(model_); })
        {
            if (!Console::RunFrame(model_))
            {
                return false;
            }
        }
        else
        {
            engine_.RunFrame();
        }
        ++frames_;

        return true;
    }

    // The frames run so far.
    uint64_t Frames() const
    {
        return frames_;
    }

    // Whether the frame made a sound: the struct's Sounded, or whether the output is audible.
    bool Sounded() const
    {
        if constexpr (requires { Console::Sounded(model_); })
        {
            return Console::Sounded(model_);
        }
        else
        {
            return xsflib::rip::Audible(Console::Output(model_));
        }
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

    // A sequence that waits for a value from a step still to come (Pending) isn't settled or stuck.
    bool Settled() const
    {
        return player_.IsSettled(engine_) && !Pending();
    }

    bool Stuck() const
    {
        return player_.IsStuck(engine_) && !Pending();
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    // The steps still to come keep the sound going.
    bool Busy() const
    {
        return engine_.IsBusy() || Pending();
    }

    bool ChannelsChanging() const
    {
        return engine_.Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return engine_.Channels().ActiveCount();
    }

    // Sets the hooks for the analysis's loops and for every track's passes (TrackPasses::SetHooks).
    template <typename Hook>
    void OnLoop(Hook hook)
    {
        engine_.SetLoopHook(std::move(hook));
        passes_.SetHooks();
    }

    bool OthersLooped(int track) const
    {
        return passes_.OthersLooped(track);
    }

private:
    // Whether a step of the game's code is still to come: the struct's Pending, or false.
    bool Pending() const
    {
        if constexpr (requires { Console::Pending(model_); })
        {
            return Console::Pending(model_);
        }
        else
        {
            return false;
        }
    }

    Model& model_;
    Engine& engine_;
    Player& player_;
    TrackPasses<Engine, Player, OpenKeepsWait<Console>()> passes_;
    uint64_t frames_ = 0;
};

// The loops that a measure at the limit makes endless: the outermost loop that each of the player's open tracks is in,
// where that loop is a counted one. A track whose outermost loop is endless loops already, and keeps its inner loops.
template <class Player>
CountedLoopSet OutermostCountedLoops(const Player& player)
{
    CountedLoopSet loops;
    for (int i = 0; i < kTrackCount; ++i)
    {
        const auto* track = player.Track(i);
        if (!track || !track->IsOpen())
        {
            continue;
        }

        const auto stack = track->CallStack();
        const auto outermost = std::ranges::find_if(stack, [](const auto& entry) { return entry.is_loop; });
        if (outermost != stack.end() && outermost->loop_count != 0)
        {
            loops.insert({i, outermost->address});
        }
    }

    return loops;
}

// Measures sequence sound `sound` on a model that `make_model()` makes, a std::unique_ptr<Console::Model> with the
// caller's settings, such as the effects. The counted loops in `endless_loops` loop for ever. Returns nothing for a
// sound that doesn't start. A sequence can repeat its music in a counted loop that the game plays for longer than the
// limit. When the measure reaches the limit inside counted loops, it runs again on a new model with each track's
// outermost counted loop endless. Their passes then count as loops of the sequence. An inner loop that runs at the
// limit, such as a fill, still ends, and repeats in each pass of the outer loop as the game plays it. The analysis's
// exceptions pass through: xsflib::rip::TryMeasure turns them into nothing.
template <class Console, class MakeModel>
std::optional<xsflib::rip::Timing> MeasureSequence(const MakeModel& make_model, uint32_t sound,
                                                   const CountedLoopSet& endless_loops = {})
{
    const std::unique_ptr<typename Console::Model> model = make_model();
    auto& engine = *model->engine;
    engine.SetEndlessLoops(endless_loops);
    auto* player = engine.StartSequence(sound);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel<Console> length_model(*model, *player);
    const xsflib::rip::LengthRules rules = LengthRulesFor(Console::kFrameMs);
    const std::optional<xsflib::rip::Timing> timing = xsflib::rip::MeasureLength(length_model, rules);
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        if (const CountedLoopSet running = OutermostCountedLoops(*player); !running.empty())
        {
            return MeasureSequence<Console>(make_model, sound, running);
        }
    }

    return timing;
}

} // namespace ntdlib::nwsnd
