// SPDX-License-Identifier: MIT

// A game's JAudio sound files, from a folder or from the files of a disc or a title, and the sequences to convert. A
// JAudio2 game has a start-up archive (BAA), a sequence archive and wave archives. A JAudio1 game, such as The Wind
// Waker, has another kind of start-up archive (AAF, ntdlib/jaudio/aaf.h), a sequence archive or table, wave archives
// and, on the Wii U, a file of its sounds' names.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"

namespace ntdlib::sequence_export::jaudio
{

// What a game's code does with its sounds that its start-up archive doesn't say. An unknown game gets the defaults.
struct GameRules
{
    // The volumes, other than 1, that the game's code gives its effect categories. A category is a group of the sound
    // table. The sound library multiplies an effect's volume by the volume of the effect's category.
    std::map<uint8_t, float> category_volumes;

    // The direct release that the sound library gives an oscillator with neither a release table nor a direct release
    // (OscillatorState::Release). With 0, such an oscillator stops at once when its note is released.
    uint16_t default_release = 0;

    // The echo that the game's code sets up, or nothing. A sequence that sends to it ends when the echo falls silent.
    std::optional<EchoLine> echo;

    // The tracks that play in the sounds whose tracks the game's code mutes, by the sounds' IDs (SoundEntry::Id). The
    // other sounds play every track (SequenceSound::tracks).
    std::map<uint32_t, TrackMask> tracks;
};

// The rules of the game whose start-up archive has the SHA-1 `archive_sha1`, as 40 lowercase hex digits.
GameRules RulesOf(const std::string& archive_sha1);

// How to open a game.
struct GameOptions
{
    // List a JAudio1 game's effects too, after the other sequences, in the sound table's order. The other sequences
    // keep their places. Each effect plays the effects sequence and starts in the first slot of its category
    // (SequenceSound::effect). JAudio1's default table of categories gives each category four slots, and The Wind Waker
    // keeps it. An effect takes its name from the name file. Without one, it's named after its category and index, as
    // "se4-0123". It plays at its volume in the sound table, times its category's volume (GameRules::category_volumes).
    bool jaudio1_effects = false;
};

class Game
{
public:
    // Open the start-up archive at `baa`, a BAA or an AAF. The game keeps its sequence archive in a Seqs folder beside
    // it, and its wave archives in a Waves folder (JAudio2) or a Banks folder (JAudio1). The reader also looks beside
    // the archive itself. The Wind Waker HD keeps its sequences in a sequence table (.bdt) in Seqs, and its sounds'
    // names in a name file (.asn) beside the archive. `program` is the name that the warnings give the program, such as
    // "xyz2midi". `options` says which sounds to list. Throw FormatError if the archive can't be read.
    static Game FromFolder(const std::filesystem::path& baa, const std::string& program,
                           const GameOptions& options = {});

    // Open the start-up archive among the files of a disc or a title that `disc` holds, with `program` and `options`
    // as for FromFolder. Return nothing if the disc has none. Throw FormatError if the archive can't be read.
    static std::optional<Game> FromDiscFiles(std::shared_ptr<ntdlib::disc::GameFiles> disc, const std::string& program,
                                             const GameOptions& options = {});

    const SoundArchive& Archive() const
    {
        return *archive_;
    }

    // The start-up archive's path among the files of a disc or a title (FromDiscFiles). It's empty for a game from
    // FromFolder.
    const std::string& DiscPath() const
    {
        return disc_path_;
    }

    // The sequences to convert: the sounds of the sound table that play sequences, in its order, then the files of the
    // sequence archive that no sound plays, named after their files. A JAudio1 game's effects play in its effects
    // sequence. The list leaves that sequence out. It leaves the effects out too, unless GameOptions::jaudio1_effects
    // lists them at its end. Its sequences take their names from the name file or their files.
    const std::vector<SequenceSound>& Sequences() const
    {
        return sequences_;
    }

    // Read a wave archive by name, or return nothing if it can't be found.
    std::optional<std::vector<uint8_t>> ReadWaveArchive(const std::string& name) const;

    // What the reader found missing or damaged, such as a missing sequence archive.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Read the start-up archive in `bytes`, and look up its game's rules. Throw FormatError if the archive can't be
    // read.
    void OpenArchive(std::vector<uint8_t> bytes);

    // List the sequences once the archive and the sequence archive are read.
    void FindSequences();

    // The sequence of JAudio1 effect `s`, which plays in the effects sequence `sequence` (GameOptions).
    SequenceSound EffectSound(const SoundEntry& s, std::span<const uint8_t> sequence) const;

    // Read a JAudio1 game's name file, `names`, named `file` in the warnings.
    void ReadNames(std::span<const uint8_t> names, const std::string& file);

    // Read the disc's file at `path`. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadDiscFile(const std::string& path) const;

    std::unique_ptr<SoundArchive> archive_;
    GameRules rules_;
    GameOptions options_;
    bool jaudio1_ = false;
    uint16_t effects_file_ = 0; // a JAudio1 game's effects sequence
    std::vector<uint8_t> sequence_archive_;
    std::string sequence_archive_name_;
    bool sequence_table_ = false;           // the sequences are in a table (ReadSequenceTable), not a RARC
    std::map<uint32_t, std::string> names_; // a JAudio1 game's names, by sound ID (ReadAsnSounds)
    std::vector<ArchiveFile> sequence_files_;
    std::vector<SequenceSound> sequences_;
    std::vector<std::string> warnings_;

    // Where the wave archives are: the folders to look in, or the disc's files, their paths by lowercase name.
    std::vector<std::filesystem::path> wave_folders_;
    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::string disc_path_;
    std::map<std::string, std::string> disc_waves_;
};

} // namespace ntdlib::sequence_export::jaudio
