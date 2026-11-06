// SPDX-License-Identifier: MIT

// Konami's steps of archive mode (xsflib/player/archive_source.h): one song of a GBA ROM, played by
// konami::ArchivePlayer on the models of the driver and the sound hardware. A program gives the type of its sets, and
// finds in a set what to play:
//
//   using Set                  the program's loaded set. set.File(path) gives the set's file at `path` as a
//                              std::vector<uint8_t>, or null when the set has none.
//   Select(set, selection)     sets the ROM's path, the song, the overrides of the driver's detection and the
//                              player's options from the set. Returns the error, or nothing.
//
// The samples go out at the player's Rate() as 16-bit values, the left and the right side in turn.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/archive_player.h"
#include "ntdlib/konami/driver.h"

namespace ntdlib::konami
{

// What archive mode plays: song `song` of the ROM at `rom_path` among the set's files, with the driver that detection
// finds there, and the player's options.
struct ArchiveSelection
{
    std::string rom_path;
    int song = 0;
    DriverOverrides overrides;
    ArchivePlayerOptions options;
};

// A ROM and its driver.
struct LoadedRom
{
    // Throws xsflib::FormatError when detection finds no driver in `image`.
    LoadedRom(std::vector<uint8_t> image, const DriverOverrides& overrides)
    {
        rom.Assign(std::move(image));
        std::string error;
        if (!DetectDriver(rom, overrides, info, error))
        {
            throw xsflib::FormatError(error.empty() ? "the ROM shows no sign of Konami's driver" : error);
        }
    }

    LoadedRom(const LoadedRom&) = delete;
    LoadedRom& operator=(const LoadedRom&) = delete;

    Rom rom;
    DriverInfo info;
};

template <typename Program>
struct ArchiveTraits
{
    using Set = typename Program::Set;
    using Prepared = ArchiveSelection;
    using Archive = LoadedRom;
    using Model = ArchivePlayer;
    using Snapshot = ArchivePlayer::Snapshot;

    static constexpr bool kKeepArchive = false;

    static std::optional<std::string> Prepare(Set& set, ArchiveSelection& selection)
    {
        return Program::Select(set, selection);
    }

    // The ROM's bytes move out of the set. A const set keeps them, and the ROM takes a copy.
    static std::optional<std::string> LoadArchive(Set& set, const ArchiveSelection& selection,
                                                  std::optional<LoadedRom>& archive)
    {
        auto* bytes = set.File(selection.rom_path);
        if (!bytes)
        {
            return "the set has no file " + selection.rom_path;
        }

        archive.emplace(std::move(*bytes), selection.overrides);
        return std::nullopt;
    }

    static std::unique_ptr<ArchivePlayer> MakeModel(const ArchiveSelection&, LoadedRom& archive)
    {
        return std::make_unique<ArchivePlayer>(archive.rom, archive.info);
    }

    // The player has nothing to start before the song.
    static std::optional<std::string> StartModel(ArchivePlayer&, LoadedRom&, const ArchiveSelection&)
    {
        return std::nullopt;
    }

    // The messages of ArchivePlayer::Start: the song can't be played, an option lies out of range, or the ROM holds no
    // mixer.
    static std::optional<std::string> StartSequence(ArchivePlayer& model, Set&, const ArchiveSelection& selection)
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

    static bool RunFrame(ArchivePlayer& model, std::vector<int16_t>& out)
    {
        model.RunFrame(out);
        return true;
    }

    static bool Busy(const ArchivePlayer& model)
    {
        return model.Busy();
    }

    static Snapshot Save(const ArchivePlayer& model, const Snapshot* previous)
    {
        return model.Save(previous);
    }

    static void Restore(ArchivePlayer& model, const Snapshot& snapshot)
    {
        model.Restore(snapshot);
    }
};

} // namespace ntdlib::konami
