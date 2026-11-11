// SPDX-License-Identifier: MIT

// DSE's steps of archive mode (xsflib/player/archive_source.h): a song or a sound effect of DSE, played by dse::Player
// with the banks that the game loads for it. A program gives the type of its sets, and finds in a set what to play:
//
//   using Set                  the program's loaded set. set.File(path) gives the set's file at `path` as a
//                              std::vector<uint8_t>, or null when the set has none.
//   Select(set, selection)     sets the paths of the sequence's files and the driver's build from the set
//                              (SelectionOf). Returns the error, or nothing.
//
// A frame is one of DSE's ticks. The samples go out at ntr::kOutputRate as 16-bit values, the left and the right side
// in turn (ntr::AppendPcm16).

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::dse
{

// What archive mode plays: a song, or a sound effect of a bank of sound effects, with the paths of the banks that the
// game loads for it, and the build of the driver that plays it (BuildOf).
struct ArchiveSelection
{
    SequenceRef sequence = {};
    std::vector<std::string> bank_paths = {}; // in the order that the game loads them
    std::optional<std::string> main_bank_path = {};
    Build build = Build::kLater;
};

// The selection of `sequence` among `files` (FindSequenceSet), on a driver of `build`. Nothing when the files hold no
// such sequence.
std::optional<ArchiveSelection> SelectionOf(std::span<const NamedFile> files, const SequenceRef& sequence, Build build);

// The files of a selection, read.
struct LoadedFiles
{
    LoadedFiles() = default;
    LoadedFiles(const LoadedFiles&) = delete;
    LoadedFiles& operator=(const LoadedFiles&) = delete;

    // The files for dse::Player. They point into this object.
    SequenceFiles Files() const;

    std::optional<Song> song;
    std::optional<SoundEffects> effects;
    uint16_t effect = 0;
    std::vector<std::unique_ptr<WaveBank>> banks;
    std::optional<WaveBank> main_bank;
};

template <typename Program>
struct ArchiveTraits
{
    using Set = typename Program::Set;
    using Prepared = ArchiveSelection;
    using Archive = LoadedFiles;
    using Model = Player;
    using Snapshot = Player::Snapshot;

    static constexpr bool kKeepArchive = false;

    static std::optional<std::string> Prepare(Set& set, ArchiveSelection& selection)
    {
        return Program::Select(set, selection);
    }

    // Reads the selection's files from the set. A file that DSE can't read throws xsflib::FormatError.
    static std::optional<std::string> LoadArchive(Set& set, const ArchiveSelection& selection,
                                                  std::optional<LoadedFiles>& archive)
    {
        const std::vector<uint8_t>* sequence = set.File(selection.sequence.path);
        if (!sequence)
        {
            return "the set has no file " + selection.sequence.path;
        }

        LoadedFiles& loaded = archive.emplace();
        if (selection.sequence.effect)
        {
            loaded.effects = SoundEffects::Parse(*sequence);
            loaded.effect = *selection.sequence.effect;
        }
        else
        {
            loaded.song = Song::Parse(*sequence);
        }
        for (const std::string& path : selection.bank_paths)
        {
            const std::vector<uint8_t>* bank = set.File(path);
            if (!bank)
            {
                return "the set has no file " + path;
            }
            loaded.banks.push_back(std::make_unique<WaveBank>(WaveBank::Parse(*bank)));
        }
        if (selection.main_bank_path)
        {
            const std::vector<uint8_t>* bank = set.File(*selection.main_bank_path);
            if (!bank)
            {
                return "the set has no file " + *selection.main_bank_path;
            }
            loaded.main_bank = WaveBank::Parse(*bank);
        }
        return std::nullopt;
    }

    static std::unique_ptr<Player> MakeModel(const ArchiveSelection& selection, LoadedFiles&)
    {
        PlayerOptions options;
        options.build = selection.build;
        return std::make_unique<Player>(options);
    }

    // Loads the banks as the game does, and starts the sequence (Player::Start).
    static std::optional<std::string> StartModel(Player& model, LoadedFiles& archive, const ArchiveSelection&)
    {
        const SequenceFiles files = archive.Files();
        model.LoadBanks(files);
        if (!model.Start(files))
        {
            return "the driver refuses the sound effect";
        }
        return std::nullopt;
    }

    // StartModel starts the sequence.
    static std::optional<std::string> StartSequence(Player&, Set&, const ArchiveSelection&)
    {
        return std::nullopt;
    }

    static bool RunFrame(Player& model, std::vector<int16_t>& out)
    {
        std::vector<ntr::SoundOutput> samples;
        model.RunTick(samples);
        ntr::AppendPcm16(samples, out);
        return true;
    }

    static bool Busy(const Player& model)
    {
        return model.Driver().SequencePlays() || model.BusyChannels() != 0;
    }

    static Snapshot Save(const Player& model, const Snapshot*)
    {
        return model.Save();
    }

    static void Restore(Player& model, const Snapshot& snapshot)
    {
        model.Restore(snapshot);
    }
};

} // namespace ntdlib::dse
