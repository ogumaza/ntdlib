// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/rip/names.h"
#include "ntdlib/rip/player_names.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"

namespace ntdlib::ntr::rip
{

namespace
{

constexpr uint32_t kNoSequence = 0xFFFFFFFF;

// The words of a player's name (ntdlib::rip::PlayerNameWords) that make the player a DS game's music player, and the
// words that keep it one of the game's players of effects, voices or cries. Pokémon Diamond, Pearl, Platinum,
// HeartGold and SoulSilver play the field's music, the towns' and the routes' pieces, on PLAYER_FIELD, and the fanfares
// (the ME's sequences: SEQ_ME_LVUP, SEQ_FANFA1) on PLAYER_ME. HeartGold's music start (0x02005D48) starts the sequences
// of player 1, PLAYER_FIELD, through 0x02005DF4. No label of those players has BGM in it. Pokémon Mystery Dungeon:
// Blue Rescue Team plays its fanfares (SND_ME_M_LEVELUP) on PLAYER_ME, and Ryouri Navi its jingles on PLAYER_JINGLE.
constexpr std::string_view kMusicWords[] = {"field", "me", "jingle"};
constexpr std::string_view kEffectWords[] = {"se", "sfx", "voice", "pv"};

// Whether one of `words` is among `wanted`.
bool HasWord(const std::vector<std::string>& words, std::span<const std::string_view> wanted)
{
    const auto among_wanted = [&](const std::string& word)
    {
        return std::ranges::find(wanted, word) != wanted.end();
    };
    return std::ranges::any_of(words, among_wanted);
}

// Whether a player named `name` plays a DS game's music: a word of kMusicWords and none of kEffectWords among the
// name's words.
bool IsDsMusicPlayerName(std::string_view name)
{
    const std::vector<std::string> words = ntdlib::rip::PlayerNameWords(name);
    return HasWord(words, kMusicWords) && !HasWord(words, kEffectWords);
}

// A player that plays music in a game, although no word of its name says so (IsMusicPlayerName, kMusicWords).
struct GameMusicPlayer
{
    std::string_view game; // the first three letters of the game's code, for every region of the game
    std::string_view player;
};

// Empty: every game known to play music on such a player names the player with a word of kMusicWords.
constexpr std::array<GameMusicPlayer, 0> kGameMusicPlayers = {};

// Whether kGameMusicPlayers lists the player `name` for the game `game_code`.
bool IsGameMusicPlayer(std::string_view game_code, std::string_view name)
{
    const std::string_view game = game_code.substr(0, 3);
    const auto listed = [&](const GameMusicPlayer& row)
    {
        return row.game == game && row.player == name;
    };

    return std::ranges::any_of(kGameMusicPlayers, listed);
}

template <typename Info>
const Info* RecordOf(const std::vector<std::optional<Info>>& records, uint32_t index)
{
    return index < records.size() && records[index] ? &*records[index] : nullptr;
}

// A file's bytes, or none for an ID past the FAT.
std::span<const uint8_t> FileOf(const sdat::Archive& archive, uint32_t id)
{
    return id < archive.files.size() ? archive.File(id) : std::span<const uint8_t>{};
}

// A sequence archive's sequence, or nothing when the archive's file can't be read or has no such sequence.
std::optional<sdat::ArchivedSequence> ArchivedOf(const sdat::Archive& archive, const SequenceRef& sequence)
{
    const sdat::SequenceArchiveInfo* info = RecordOf(archive.sequence_archives, sequence.index);
    if (!info)
    {
        return std::nullopt;
    }

    try
    {
        const sdat::SequenceArchive parsed = sdat::SequenceArchive::Parse(FileOf(archive, info->file_id));
        if (*sequence.archived < parsed.sequences.size() && parsed.sequences[*sequence.archived].offset != kNoSequence)
        {
            return parsed.sequences[*sequence.archived];
        }
    }
    catch (const xsflib::FormatError&)
    {
    }

    return std::nullopt;
}

// A sequence's file, bank and player, or nothing when the archive has no such sequence.
struct SequenceParts
{
    uint32_t file_id = 0;
    uint16_t bank = 0;
    uint8_t player = 0;
};

std::optional<SequenceParts> PartsOf(const sdat::Archive& archive, const SequenceRef& sequence)
{
    if (!sequence.archived)
    {
        const sdat::SequenceInfo* info = RecordOf(archive.sequences, sequence.index);
        return info ? std::optional<SequenceParts>({info->file_id, info->bank, info->player}) : std::nullopt;
    }

    const std::optional<sdat::ArchivedSequence> entry = ArchivedOf(archive, sequence);
    return entry ? std::optional<SequenceParts>(
                       {archive.sequence_archives[sequence.index]->file_id, entry->bank, entry->player})
                 : std::nullopt;
}

} // namespace

bool IsMusic(const sdat::Archive& archive, const SequenceRef& sequence, std::string_view game_code)
{
    if (xsflib::AsciiLower(Label(archive, sequence)).find("bgm") != std::string::npos)
    {
        return true;
    }

    const std::optional<SequenceParts> parts = PartsOf(archive, sequence);
    const sdat::PlayerInfo* player = parts ? RecordOf(archive.players, parts->player) : nullptr;
    if (!player)
    {
        return false;
    }

    return ntdlib::rip::IsMusicPlayerName(player->name) || IsDsMusicPlayerName(player->name) ||
           IsGameMusicPlayer(game_code, player->name);
}

std::vector<Job> SelectSequences(const sdat::Archive& archive, const std::regex& only, bool bgm,
                                 std::string_view game_code)
{
    std::vector<Job> jobs;
    for (const SequenceRef& sequence : Sequences(archive))
    {
        std::string label = Label(archive, sequence);
        if (std::regex_search(label, only) && (!bgm || IsMusic(archive, sequence, game_code)))
        {
            jobs.push_back({sequence, std::move(label), std::nullopt});
        }
    }

    return jobs;
}

std::vector<std::string> MissingData(const sdat::Archive& archive, const SequenceRef& sequence)
{
    const std::optional<SequenceParts> parts = PartsOf(archive, sequence);
    if (!parts)
    {
        return {sequence.archived ? "the sequence archive's sequence" : "the sequence's record"};
    }

    std::vector<std::string> missing;
    if (FileOf(archive, parts->file_id).empty())
    {
        missing.push_back(sequence.archived ? "the sequence archive's file" : "the sequence's file");
    }

    const sdat::BankInfo* bank = RecordOf(archive.banks, parts->bank);
    if (!bank)
    {
        missing.push_back("bank " + std::to_string(parts->bank) + ", which the archive doesn't list");
        return missing;
    }
    if (FileOf(archive, bank->file_id).empty())
    {
        missing.push_back("bank " + ntdlib::rip::NameOrNumber(bank->name, parts->bank));
    }
    for (const uint16_t index : bank->wave_archives)
    {
        if (index == sdat::kNoWaveArchive)
        {
            continue;
        }

        const sdat::WaveArchiveInfo* wave_archive = RecordOf(archive.wave_archives, index);
        if (!wave_archive)
        {
            missing.push_back("wave archive " + std::to_string(index) + ", which the archive doesn't list");
        }
        else if (FileOf(archive, wave_archive->file_id & sdat::kFileIdBits).empty())
        {
            missing.push_back("wave archive " + ntdlib::rip::NameOrNumber(wave_archive->name, index));
        }
    }

    return missing;
}

std::optional<std::vector<uint8_t>> KeepSequenceData(const sdat::Archive& archive,
                                                     std::span<const SequenceRef> sequences)
{
    std::set<uint32_t> kept;
    for (const SequenceRef& sequence : sequences)
    {
        if (!MissingData(archive, sequence).empty())
        {
            return std::nullopt;
        }

        const SequenceParts parts = *PartsOf(archive, sequence);
        const sdat::BankInfo& bank = *archive.banks[parts.bank];
        kept.insert(parts.file_id);
        kept.insert(bank.file_id);
        for (const uint16_t index : bank.wave_archives)
        {
            if (index != sdat::kNoWaveArchive)
            {
                kept.insert(archive.wave_archives[index]->file_id & sdat::kFileIdBits);
            }
        }
    }

    try
    {
        sdat::ArchiveModel model = sdat::ReadArchiveModel(archive.bytes);
        for (std::size_t id = 0; id < model.files.size(); id++)
        {
            if (!kept.contains(static_cast<uint32_t>(id)))
            {
                model.files[id].clear();
            }
        }

        std::vector<uint8_t> library = sdat::BuildArchive(model);
        const sdat::Archive read = sdat::Archive::Parse(library);
        for (const uint32_t id : kept)
        {
            if (!std::ranges::equal(read.File(id), archive.File(id)))
            {
                return std::nullopt;
            }
        }

        return library;
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

uint32_t SoundId(const SequenceRef& sequence)
{
    constexpr uint32_t kArchived = 0x80000000;
    if (!sequence.archived)
    {
        if (sequence.index >= kArchived)
        {
            throw std::out_of_range("no sound ID for sequence " + std::to_string(sequence.index));
        }
        return sequence.index;
    }

    if (sequence.index > 0x7FFF || *sequence.archived > 0xFFFF)
    {
        throw std::out_of_range("no sound ID for sequence " + std::to_string(*sequence.archived) +
                                " of sequence archive " + std::to_string(sequence.index));
    }

    return kArchived | sequence.index << 16 | *sequence.archived;
}

SequenceRef SequenceOf(uint32_t sound_id)
{
    if ((sound_id & 0x80000000) == 0)
    {
        return {sound_id};
    }

    return {(sound_id >> 16) & 0x7FFF, sound_id & 0xFFFF};
}

} // namespace ntdlib::ntr::rip
