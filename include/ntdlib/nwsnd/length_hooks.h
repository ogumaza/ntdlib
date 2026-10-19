// SPDX-License-Identifier: MIT

// The hooks that a rip tool's length analysis sets on an engine of the sequence player (sequence.h): one for the
// sequence's loops, one for every track's passes, and one for each open of a track by another track (0x88). An engine
// derives from LengthHooks. The sequence player then calls FollowsLoops, OnLoop, OnTrackLoop and OnCommand on it.

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace ntdlib::nwsnd
{

class LengthHooks
{
public:
    using LoopHook = std::function<void(int track, uint32_t target)>;
    using OpenTrackHook = std::function<void(int track, int opened, uint32_t offset)>;

    // The hooks for the sequence's loops and for every track's passes. While the loop hook is set, the players report
    // their loops to it, and every track's passes to the track loop hook if that's set too (see SequenceSoundPlayer).
    // The model follows the loops only while the loop hook is set.
    void SetLoopHook(LoopHook hook)
    {
        on_loop_ = std::move(hook);
    }

    void SetTrackLoopHook(LoopHook hook)
    {
        on_track_loop_ = std::move(hook);
    }

    // The hook for each open of a track by another track (0x88). The hook receives the track that runs the command,
    // the track that it opens and the offset where that track starts. The hook gets every open, the first included,
    // whether the loop hook is set or not. A length analysis can count each open after the first as a pass of the
    // opened track. An open of the track that runs the command, or of a track that the sound doesn't allocate, opens
    // nothing and doesn't reach the hook.
    void SetOpenTrackHook(OpenTrackHook hook)
    {
        on_open_track_ = std::move(hook);
    }

    // The sequence player calls these. It calls OnCommand after each command that a track runs.
    bool FollowsLoops() const
    {
        return static_cast<bool>(on_loop_);
    }

    template <typename Player>
    void OnLoop(const Player&, int track, uint32_t target)
    {
        on_loop_(track, target);
    }

    template <typename Player>
    void OnTrackLoop(const Player&, int track, uint32_t target)
    {
        if (on_track_loop_)
        {
            on_track_loop_(track, target);
        }
    }

    template <typename Player>
    void OnCommand(const Player& player, int track, uint32_t command, int32_t arg1, int32_t arg2)
    {
        if (command == 0x88 && on_open_track_ && arg1 != track && player.Track(arg1))
        {
            on_open_track_(track, arg1, static_cast<uint32_t>(arg2));
        }
    }

private:
    LoopHook on_loop_;
    LoopHook on_track_loop_;
    OpenTrackHook on_open_track_;
};

} // namespace ntdlib::nwsnd
