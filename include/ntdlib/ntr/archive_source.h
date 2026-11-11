// SPDX-License-Identifier: MIT

// The DS's steps of archive mode (xsflib/player/archive_source.h): one sequence of a sound archive (SDAT), played by
// ntr::ArchivePlayer on the model of the sound driver. A program gives the type of its sets, and finds in a set what to
// play:
//
//   using Set                  the program's loaded set. set.File(path) gives the set's file at `path` as a
//                              std::vector<uint8_t>, or null when the set has none.
//   Select(set, selection)     sets the archive's path, the sequence, what the game's code sets before the sequence
//                              starts and the driver's build from the set. Returns the error, or nothing.
//
// The samples go out at kOutputRate as 16-bit values, the left and the right side in turn (AppendPcm16 in sound.h).

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::ntr
{

// What archive mode plays: a sequence of the archive at `archive_path` among the set's files, what a game's code sets
// before the sequence starts, and the build of the driver that plays it (snd::BuildOf).
struct ArchiveSelection
{
    std::string archive_path;
    SequenceRef sequence;
    GameSettings settings;
    snd::Build build;
};

// A sound archive and the bytes that it reads from.
struct LoadedArchive
{
    // Throws xsflib::FormatError when `file` isn't a sound archive (sdat::Archive::Parse).
    explicit LoadedArchive(std::vector<uint8_t> file) : bytes(std::move(file)), archive(sdat::Archive::Parse(bytes))
    {
    }

    LoadedArchive(const LoadedArchive&) = delete;
    LoadedArchive& operator=(const LoadedArchive&) = delete;

    std::vector<uint8_t> bytes;
    sdat::Archive archive;
};

template <typename Program>
struct ArchiveTraits
{
    using Set = typename Program::Set;
    using Prepared = ArchiveSelection;
    using Archive = LoadedArchive;
    using Model = ArchivePlayer;
    using Snapshot = ArchivePlayer::Snapshot;

    static constexpr bool kKeepArchive = false;

    static std::optional<std::string> Prepare(Set& set, ArchiveSelection& selection)
    {
        return Program::Select(set, selection);
    }

    // The archive's bytes move out of the set. A const set keeps them, and the archive takes a copy.
    static std::optional<std::string> LoadArchive(Set& set, const ArchiveSelection& selection,
                                                  std::optional<LoadedArchive>& archive)
    {
        auto* bytes = set.File(selection.archive_path);
        if (!bytes)
        {
            return "the set has no file " + selection.archive_path;
        }

        archive.emplace(std::move(*bytes));
        return std::nullopt;
    }

    static std::unique_ptr<ArchivePlayer> MakeModel(const ArchiveSelection& selection, LoadedArchive& archive)
    {
        ArchivePlayerOptions options;
        options.build = selection.build;
        return std::make_unique<ArchivePlayer>(archive.archive, options);
    }

    // The player starts the driver as it's made.
    static std::optional<std::string> StartModel(ArchivePlayer&, LoadedArchive&, const ArchiveSelection&)
    {
        return std::nullopt;
    }

    // The messages of StartCommands: the archive lacks the sequence, its file or its bank, or the files don't fit.
    static std::optional<std::string> StartSequence(ArchivePlayer& model, Set&, const ArchiveSelection& selection)
    {
        try
        {
            model.Start(selection.sequence, selection.settings);
        }
        catch (const std::runtime_error& e)
        {
            return e.what();
        }

        return std::nullopt;
    }

    static bool RunFrame(ArchivePlayer& model, std::vector<int16_t>& out)
    {
        std::vector<SoundOutput> samples;
        model.RunFrame(samples);
        AppendPcm16(samples, out);
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

} // namespace ntdlib::ntr
