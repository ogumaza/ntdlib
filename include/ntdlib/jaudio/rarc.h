// SPDX-License-Identifier: MIT

// RARC archives, such as JAudio2's sequence archive of BMS files. Some games compress the archive with Yaz0, JSystem's
// compression (Twilight Princess, for example).

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::jaudio
{

// A file of a sequence archive (RARC).
struct ArchiveFile
{
    uint16_t id = 0;
    std::string name;
    std::span<const uint8_t> data;
};

// Reads the files of an RARC archive. The spans point into `rarc`. Throws FormatError if it isn't one.
std::vector<ArchiveFile> ReadRarc(std::span<const uint8_t> rarc);

// Whether `data` is compressed with Yaz0: whether it starts with "Yaz0".
bool IsYaz0(std::span<const uint8_t> data);

// Decodes data compressed with Yaz0. Throws FormatError if it's damaged, or if it decodes to more than 64 MiB.
std::vector<uint8_t> DecodeYaz0(std::span<const uint8_t> data);

// A sequence archive with the sequences of another release's sequence table, such as The Wind Waker HD's
// `Seqs/JaiSeq.bdt`. The table is a 32-bit count, zeros up to 0x20, and then a 32-bit offset and size for each
// sequence. The result is `rarc` with each file's data replaced by the table's entry whose index is the file's id. Each
// file keeps its entry: its id, its name and its flags. Each file's data starts on a 32-byte boundary. A Yaz0 archive
// is decoded first. Throws FormatError if either is damaged, or if a file's id is past the table's end.
std::vector<uint8_t> RepackSequenceTable(std::span<const uint8_t> rarc, std::span<const uint8_t> table);

} // namespace ntdlib::jaudio
