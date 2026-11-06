// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"

namespace ntdlib::ntr::rip
{

namespace
{

constexpr uint16_t kNoWaveArchive = 0xFFFF;
constexpr uint32_t kFileIdBits = 0xFFFFFF;
constexpr uint32_t kNoSequence = 0xFFFFFFFF;

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

// A sequence archive's sequence, or nothing when the archive's file can't be read or lacks it.
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

// A sequence's file, bank and player, or nothing when the archive lacks the sequence.
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

std::string NameOrNumber(const std::string& name, const char* kind, std::size_t index)
{
    return name.empty() ? kind + std::to_string(index) : name;
}

} // namespace

bool IsMusic(const sdat::Archive& archive, const SequenceRef& sequence)
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

    // The player's name, word by word, each without the digits it ends in (PLAYER_BGM1).
    bool music = false;
    bool effects = false;
    const std::string name = xsflib::AsciiLower(player->name);
    for (std::size_t start = 0; start <= name.size();)
    {
        const std::size_t end = std::min(name.find('_', start), name.size());
        std::string word = name.substr(start, end - start);
        word.erase(word.find_last_not_of("0123456789") + 1);
        music = music || word == "bgm" || word == "music" || word == "song" || word == "fanfare";
        effects = effects || word == "se";
        start = end + 1;
    }

    return music && !effects;
}

std::vector<Job> SelectSequences(const sdat::Archive& archive, const std::regex& only, bool bgm)
{
    std::vector<Job> jobs;
    for (const SequenceRef& sequence : Sequences(archive))
    {
        std::string label = Label(archive, sequence);
        if (std::regex_search(label, only) && (!bgm || IsMusic(archive, sequence)))
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
        missing.push_back("bank " + NameOrNumber(bank->name, "#", parts->bank));
    }
    for (const uint16_t index : bank->wave_archives)
    {
        if (index == kNoWaveArchive)
        {
            continue;
        }

        const sdat::WaveArchiveInfo* wave_archive = RecordOf(archive.wave_archives, index);
        if (!wave_archive)
        {
            missing.push_back("wave archive " + std::to_string(index) + ", which the archive doesn't list");
        }
        else if (FileOf(archive, wave_archive->file_id & kFileIdBits).empty())
        {
            missing.push_back("wave archive " + NameOrNumber(wave_archive->name, "#", index));
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
            if (index != kNoWaveArchive)
            {
                kept.insert(archive.wave_archives[index]->file_id & kFileIdBits);
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
