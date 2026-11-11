// SPDX-License-Identifier: MIT

// What the tests of the DS archive player share: the test archive, its files and the reads of the player's memory.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/heap.h"
#include "ntdlib/testing/sdat_files.h"

namespace ntr_archive_player_tests
{

namespace files = ntdlib::testing::sdat_files;
namespace ntr = ntdlib::ntr;
namespace sdat = ntdlib::sdat;
namespace snd = ntdlib::ntr::snd;
using files::Bytes;
using xsflib::testing::ErrorOf;

// Track 0 plays key 60 for 48 ticks, waits 48 more and ends.
const Bytes kNote = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

// The sequence SEQ_A (file 0) plays at volume 100 on the archive's player 1. That player may take channels 4 to 11. The
// sequence archive (file 1) holds one sequence at volume 127 on player 0. Both play bank 0 (file 2). The bank's one
// instrument plays the only wave of wave archive 0 (file 3).
inline files::Archive ArchiveSpec()
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 0, "SEQ_A", 1)};
    spec.items[1] = {files::SequenceArchiveItem(1, "SSAR", {"SE"})};
    spec.items[2] = {files::BankItem(2, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK")};
    spec.items[3] = {files::FileItem(3, "WAVE")};
    spec.items[4] = {files::PlayerItem(1, 0, 0, "PLAYER_0"), files::PlayerItem(1, 0x0FF0, 0, "PLAYER_1")};
    const files::Wave wave{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    spec.files = {files::Sequence(kNote), files::SequenceArchive({{.channel_priority = 50}}, kNote),
                  files::Bank({{.type = 1, .region = {.wave = 0}}}), files::WaveArchive({wave})};
    return spec;
}

inline Bytes ArchiveFile()
{
    return files::SoundArchive(ArchiveSpec());
}

// An archive whose sequence SEQ_A plays `sequence`, and whose bank releases its notes at `release`.
inline Bytes ArchiveFile(const Bytes& sequence, int release = 127)
{
    files::Archive spec = ArchiveSpec();
    spec.files[0] = files::Sequence(sequence);
    spec.files[2] = files::Bank({{.type = 1, .region = {.wave = 0, .release = static_cast<uint8_t>(release)}}});
    return files::SoundArchive(spec);
}

// Track 0 plays key 60, then key 62 on its channel with a tie, and ends. The bank's release of 100 keeps the channel
// for 305 frames after the end (see TestBusyWhileReleasing).
const Bytes kTiedNotes = {0xC8, 0x01, 0x3C, 0x7F, 0x30, 0x3E, 0x7F, 0x30, 0xFF};

inline uint32_t Align32(uint32_t value)
{
    return (value + 31) & ~31u;
}

inline uint32_t Word16(std::span<const uint8_t> memory, uint32_t address)
{
    return xsflib::Reader(memory).U16(address - ntr::ArchivePlayer::kMainMemory);
}

} // namespace ntr_archive_player_tests
