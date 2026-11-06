// SPDX-License-Identifier: MIT

// libultra's sequence bank: the file that the SDK's tools make from a list of sequences. It starts with the revision
// "S1" and the number of sequences in big-endian 16-bit fields. A table of pairs of 32-bit values follows: each
// sequence's offset from the file's start and its length. The sequences follow the table, each on a multiple of 4
// bytes. The bytes between two sequences are padding. Some of them hold stale bytes from the tool.
//
// Nintendo EAD's early sound engine uses the same layout for its files of sequences, banks and samples, with revisions
// 3, 1 and 2 in place of "S1".
//
// The reader keeps every byte up to the end of the last sequence. The writer gives them back byte for byte.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::n64
{

constexpr uint16_t kSeqFileRevision = 0x5331; // "S1"
constexpr uint16_t kEarlyBankFileRevision = 1;
constexpr uint16_t kEarlySampleFileRevision = 2;
constexpr uint16_t kEarlySequenceFileRevision = 3;

class SeqFile
{
public:
    // A sequence's place in the file.
    struct Entry
    {
        friend bool operator==(const Entry&, const Entry&) = default;

        uint32_t offset = 0;
        uint32_t length = 0;
    };

    // Reads the file at the start of `data`. `data` can run on past the file. A file that isn't a sequence bank, or
    // whose table leaves `data`, throws xsflib::FormatError.
    static SeqFile Parse(std::span<const uint8_t> data, uint16_t revision = kSeqFileRevision);

    // Whether `data` starts with a sequence bank: the revision, and a table of at least one sequence inside `data` with
    // offsets that start at or after the table. A file of revision S1 also has them in rising order.
    static bool Matches(std::span<const uint8_t> data, uint16_t revision = kSeqFileRevision);

    const std::vector<Entry>& Entries() const;
    std::span<const uint8_t> Sequence(std::size_t index) const;

    // Gives a sequence new bytes. The later sequences move by the change in length, kept on multiples of 4 bytes.
    void SetSequence(std::size_t index, std::span<const uint8_t> sequence);

    std::vector<uint8_t> Write() const;

private:
    uint16_t revision_ = kSeqFileRevision;
    std::vector<Entry> entries_;
    std::vector<uint8_t> bytes_;
};

} // namespace ntdlib::n64
