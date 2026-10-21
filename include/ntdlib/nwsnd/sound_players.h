// SPDX-License-Identifier: MIT

// The sound players of the Wii U's and the Switch's archive engines, each in its place (its Id()), and the starts that
// wait for the sound thread. A place holds a player of one of the kinds: a sequence, a wave sound or a stream sound.

#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ntdlib::nwsnd
{

template <typename... Kinds>
class SoundPlayers
{
public:
    using Player = std::variant<Kinds...>;

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

    // Adds a player of kind `Kind`, made from its place and `args`, and a start that waits `frames` sound frames.
    // Returns the player.
    template <typename Kind, typename... Args>
    Kind& Add(int frames, Args&&... args)
    {
        const int place = static_cast<int>(players_.size());
        players_.push_back(std::make_unique<Player>(std::in_place_type<Kind>, place, std::forward<Args>(args)...));
        pending_.push_back({place, frames});

        return std::get<Kind>(*players_.back());
    }

    Player& operator[](int place)
    {
        return *players_[static_cast<std::size_t>(place)];
    }

    // The player at `place` if it's of kind `Kind`, or nullptr.
    template <typename Kind>
    Kind* Get(int place)
    {
        if (place < 0 || static_cast<std::size_t>(place) >= players_.size())
        {
            return nullptr;
        }

        return std::get_if<Kind>(players_[static_cast<std::size_t>(place)].get());
    }

    // The sound thread's commands: starts each player that is due, and counts a frame down for each start that waits.
    // A player whose start requires the engine gets `engine`.
    template <typename Engine>
    void StartDue(Engine& engine)
    {
        for (auto it = pending_.begin(); it != pending_.end();)
        {
            if (it->frames > 0)
            {
                --it->frames;
                ++it;
                continue;
            }

            std::visit(
                [&engine](auto& player)
                {
                    if constexpr (requires { player.Start(engine); })
                    {
                        player.Start(engine);
                    }
                    else
                    {
                        player.Start();
                    }
                },
                *players_[static_cast<std::size_t>(it->player)]);
            it = pending_.erase(it);
        }
    }

    // Calls `fn` with each player in order of place. `fn` takes each kind.
    template <typename Fn>
    void ForEach(const Fn& fn)
    {
        for (const std::unique_ptr<Player>& player : players_)
        {
            std::visit(fn, *player);
        }
    }

    // Whether a player hasn't finished.
    bool AnyPlaying() const
    {
        for (const std::unique_ptr<Player>& player : players_)
        {
            if (!std::visit([](const auto& each) { return each.IsFinished(); }, *player))
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

    // Throws std::invalid_argument for a snapshot with more players than these. Changes nothing. A player of another
    // kind can hold a place that a restore emptied. The snapshot's player then takes the place again.
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
