// SPDX-License-Identifier: MIT

// JAudio's sequence index (BARC): the table that divides a file of sequences into its sequences. The index starts with
// "BARC", four bytes, a word and the count of sequences, then the name of the sequences' file in 16 bytes. A 32-byte
// entry follows for each sequence: its name in up to 14 bytes, the number of the sequence whose data it shares (0xffff
// for none), two words, then the sequence's offset and size in the file. Luigi's Mansion keeps its index in its
// start-up archive (aaf.h). Pikmin keeps its index in its executable. The Switch's release of Pikmin keeps it
// little-endian, and widens the word at +8 to 64 bits: the game stores a pointer there (0x27a6c0 in its main). Its
// count is then at +16, and its entries start at +36.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/jaudio/rarc.h"

namespace ntdlib::jaudio
{

// The sequences of a file of sequences, `archive`, that the index at the start of `index` divides, as ReadRarc gives a
// RARC's files: each sequence's number in the index as its id, its name, its data in `archive`, and the number of the
// sequence whose data the game reads (ArchiveFile::data_id). The names are cut to 13 characters, with or without their
// extension. A little-endian index has the layout of the Switch's release of Pikmin. Throws FormatError if the index
// is damaged, if a sequence lies outside `archive`, or if a sequence's chain of shared data loops or names a sequence
// past the index's end.
std::vector<ArchiveFile> ReadBarcSequenceFiles(std::span<const uint8_t> index, std::span<const uint8_t> archive,
                                               bool big_endian = true);

// The offset of the first sequence index in `data`, such as a game's executable: a "BARC" on a 4-byte boundary whose
// count of sequences, in the byte order that `big_endian` gives, is at most 0x10000 and whose entries fit in `data`.
// Returns nothing if `data` holds no such index.
std::optional<std::size_t> FindBarc(std::span<const uint8_t> data, bool big_endian = true);

} // namespace ntdlib::jaudio
