// SPDX-License-Identifier: MIT

// The steps of archive mode (xsflib/player/archive_source.h) that the GBA drivers share: one song of a ROM, played by a
// driver's ArchivePlayer. Each driver's archive_source.h defines its ArchiveSelection, its LoadedRom and its
// ArchiveTraits.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ntdlib::agb
{

// The steps for the sets of `Program`, with a driver's types. `Selection` contains `rom_path`, `song` and `options`,
// and `overrides` where the driver's detection uses them. `Loaded` contains the ROM (`rom`) and its driver (`info`). It
// is made from the ROM's bytes, with the overrides where `Selection` has them. `Player` is the driver's ArchivePlayer.
template <typename Program, typename Selection, typename Loaded, typename Player>
struct RomArchiveTraits
{
    using Set = typename Program::Set;
    using Prepared = Selection;
    using Archive = Loaded;
    using Model = Player;
    using Snapshot = typename Player::Snapshot;

    static constexpr bool kKeepArchive = false;

    static std::optional<std::string> Prepare(Set& set, Selection& selection)
    {
        return Program::Select(set, selection);
    }

    // The ROM's bytes move out of the set. A const set keeps them, and the ROM gets a copy.
    static std::optional<std::string> LoadArchive(Set& set, const Selection& selection, std::optional<Loaded>& archive)
    {
        auto* bytes = set.File(selection.rom_path);
        if (!bytes)
        {
            return "the set has no file " + selection.rom_path;
        }

        if constexpr (requires { selection.overrides; })
        {
            archive.emplace(std::move(*bytes), selection.overrides);
        }
        else
        {
            archive.emplace(std::move(*bytes));
        }
        return std::nullopt;
    }

    static std::unique_ptr<Player> MakeModel(const Selection&, Loaded& archive)
    {
        return std::make_unique<Player>(archive.rom, archive.info);
    }

    // The player has nothing to start before the song.
    static std::optional<std::string> StartModel(Player&, Loaded&, const Selection&)
    {
        return std::nullopt;
    }

    // The messages of the player's Start: the song can't be played, an option is out of range, or, for some drivers,
    // the ROM or the game's code fails.
    static std::optional<std::string> StartSequence(Player& model, Set&, const Selection& selection)
    {
        try
        {
            model.Start(selection.song, selection.options);
        }
        catch (const std::invalid_argument& e)
        {
            return e.what();
        }
        catch (const std::runtime_error& e)
        {
            return e.what();
        }

        return std::nullopt;
    }

    static bool RunFrame(Player& model, std::vector<int16_t>& out)
    {
        model.RunFrame(out);
        return true;
    }

    static bool Busy(const Player& model)
    {
        return model.Busy();
    }

    static Snapshot Save(const Player& model, const Snapshot* previous)
    {
        return model.Save(previous);
    }

    static void Restore(Player& model, const Snapshot& snapshot)
    {
        model.Restore(snapshot);
    }
};

} // namespace ntdlib::agb
