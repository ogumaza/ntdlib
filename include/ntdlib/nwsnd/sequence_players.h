// SPDX-License-Identifier: MIT

// The sequence players of the Wii U's and the Switch's archive engines, each in its place (its Id()), and the starts
// that wait for the sound thread.

#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ntdlib::nwsnd
{

template <typename Player>
class SequencePlayers
{
public:
    // A start that waits holds its player's place, not a pointer. A restore can put another player object in that
    // place.
    struct PendingStart
    {
        int player; // the player's place: its Id()
        int frames; // sound frames before it starts
    };

    // A copy of the players and of the starts that wait.
    struct Snapshot
    {
        std::vector<Player> players;
        std::vector<PendingStart> pending;
    };

    // Adds a player made from its place and `args`, and a start that waits `frames` sound frames. Returns the player.
    template <typename... Args>
    Player& Add(int frames, Args&&... args)
    {
        const int place = static_cast<int>(players_.size());
        players_.push_back(std::make_unique<Player>(place, std::forward<Args>(args)...));
        pending_.push_back({place, frames});

        return *players_.back();
    }

    Player& operator[](int place)
    {
        return *players_[static_cast<std::size_t>(place)];
    }

    // The sound thread's commands: starts each player that is due, and counts a frame down for each start that waits.
    void StartDue()
    {
        for (auto it = pending_.begin(); it != pending_.end();)
        {
            if (it->frames > 0)
            {
                --it->frames;
                ++it;
                continue;
            }

            players_[static_cast<std::size_t>(it->player)]->Start();
            it = pending_.erase(it);
        }
    }

    // Calls `fn` with each player in order of place.
    template <typename Fn>
    void ForEach(const Fn& fn)
    {
        for (const std::unique_ptr<Player>& player : players_)
        {
            fn(*player);
        }
    }

    // Whether a player hasn't finished.
    bool AnyPlaying() const
    {
        for (const std::unique_ptr<Player>& player : players_)
        {
            if (!player->IsFinished())
            {
                return true;
            }
        }

        return false;
    }

    Snapshot Save() const
    {
        Snapshot snapshot{{}, pending_};
        for (const std::unique_ptr<Player>& player : players_)
        {
            snapshot.players.push_back(*player);
        }

        return snapshot;
    }

    // Throws std::invalid_argument for a snapshot with more players than these. Changes nothing.
    void CheckRestore(const Snapshot& snapshot) const
    {
        if (snapshot.players.size() > players_.size())
        {
            throw std::invalid_argument("a snapshot with players that the engine doesn't have");
        }
    }

    // The players that started after the snapshot go. The others keep their places. The starts that wait reach the
    // players in their places. Call CheckRestore first.
    void Restore(const Snapshot& snapshot)
    {
        players_.erase(players_.begin() + static_cast<std::ptrdiff_t>(snapshot.players.size()), players_.end());
        for (std::size_t i = 0; i < players_.size(); ++i)
        {
            *players_[i] = snapshot.players[i];
        }
        pending_ = snapshot.pending;
    }

private:
    std::vector<std::unique_ptr<Player>> players_;
    std::vector<PendingStart> pending_;
};

} // namespace ntdlib::nwsnd
