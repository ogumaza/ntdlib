// SPDX-License-Identifier: MIT

// The layout of the DS's sound archive (SDAT) that the SDK's tools write: a model of an archive, its fields and files,
// and a builder that lays an archive out from its model.
//
// The tools lay an archive out as its header, the SYMB block when the archive has names, then the INFO, FAT and FILE
// blocks. SYMB holds each kind's list, with the lists of the sequence archives' sequences right after the list of the
// sequence archives. The names follow in the lists' order, each archive's name before its sequences' names, each with a
// zero byte at its end. The archive's header gives SYMB's size without the zeros that bring INFO to a multiple of 4
// bytes. The size in SYMB's block header counts them. INFO holds each kind's list followed by its records. FILE holds
// the files in the order of their IDs, each on a multiple of 32 bytes from the archive's start, with zeros up to the
// next. The archive ends on a multiple of 32 bytes.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sdat/formats.h"

namespace ntdlib::sdat
{

// An archive as the fields of its tables and the bytes of its files. The records' names aren't used: `symbols` holds
// the names.
struct ArchiveModel
{
    uint16_t version = 0x0100;
    std::optional<Symbols> symbols; // none: the archive has no SYMB block
    std::vector<std::optional<SequenceInfo>> sequences;
    std::vector<std::optional<SequenceArchiveInfo>> sequence_archives;
    std::vector<std::optional<BankInfo>> banks;
    std::vector<std::optional<WaveArchiveInfo>> wave_archives;
    std::vector<std::optional<PlayerInfo>> players;
    std::vector<std::optional<GroupInfo>> groups;
    std::vector<std::optional<StreamPlayerInfo>> stream_players;
    std::vector<std::optional<StreamInfo>> streams;
    std::vector<std::vector<uint8_t>> files; // by ID
};

// Reads an archive's model. Throws xsflib::FormatError when the archive can't be read, or when BuildArchive doesn't
// give the archive's bytes back from the model.
ArchiveModel ReadArchiveModel(std::span<const uint8_t> sdat);

// Lays an archive out from its model.
std::vector<uint8_t> BuildArchive(const ArchiveModel& model);

} // namespace ntdlib::sdat
