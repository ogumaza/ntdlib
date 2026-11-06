// SPDX-License-Identifier: MIT

#include "ntdlib/n64/seq_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::n64
{

namespace
{

constexpr std::size_t kTableOffset = 4;
constexpr std::size_t kEntrySize = 8;

std::size_t TableEnd(std::size_t count)
{
    return kTableOffset + count * kEntrySize;
}

} // namespace

bool SeqFile::Matches(std::span<const uint8_t> data, uint16_t revision)
{
    if (data.size() < kTableOffset || xsflib::LoadBe16(data.data()) != revision)
    {
        return false;
    }

    const std::size_t count = xsflib::LoadBe16(data.data() + 2);
    if (count == 0 || TableEnd(count) > data.size())
    {
        return false;
    }

    uint32_t previous = 0;
    for (std::size_t i = 0; i < count; i++)
    {
        const uint32_t offset = xsflib::LoadBe32(data.data() + kTableOffset + i * kEntrySize);
        const uint32_t length = xsflib::LoadBe32(data.data() + kTableOffset + i * kEntrySize + 4);
        if (offset < TableEnd(count) || offset > data.size() || length > data.size() - offset)
        {
            return false;
        }

        // An early file of samples can name another entry's bytes again.
        if (revision == kSeqFileRevision && offset < previous)
        {
            return false;
        }

        previous = offset;
    }

    return true;
}

SeqFile SeqFile::Parse(std::span<const uint8_t> data, uint16_t revision)
{
    const xsflib::Reader reader(data, true);
    if (data.size() < kTableOffset || reader.U16(0) != revision)
    {
        throw xsflib::FormatError(revision == kSeqFileRevision ? "not a sequence bank of revision S1"
                                                               : "not a file of revision " + std::to_string(revision));
    }

    const std::size_t count = reader.U16(2);
    SeqFile file;
    file.revision_ = revision;
    file.entries_.reserve(count);
    std::size_t end = TableEnd(count);
    for (std::size_t i = 0; i < count; i++)
    {
        const Entry entry = {reader.U32(kTableOffset + i * kEntrySize), reader.U32(kTableOffset + i * kEntrySize + 4)};
        if (entry.offset > data.size() || entry.length > data.size() - entry.offset)
        {
            throw xsflib::FormatError("sequence " + std::to_string(i) + " of a sequence bank leaves the data");
        }

        end = std::max(end, static_cast<std::size_t>(entry.offset) + entry.length);
        file.entries_.push_back(entry);
    }
    if (end > data.size())
    {
        throw xsflib::FormatError("the table of a sequence bank leaves the data");
    }

    const std::span<const uint8_t> kept = data.first(end);
    file.bytes_.assign(kept.begin(), kept.end());

    return file;
}

const std::vector<SeqFile::Entry>& SeqFile::Entries() const
{
    return entries_;
}

std::span<const uint8_t> SeqFile::Sequence(std::size_t index) const
{
    const Entry& entry = entries_.at(index);

    return std::span<const uint8_t>(bytes_).subspan(entry.offset, entry.length);
}

void SeqFile::SetSequence(std::size_t index, std::span<const uint8_t> sequence)
{
    const Entry old = entries_.at(index);

    // The sequences after this one move by a multiple of 4 bytes. A shared sequence stays shared: its entries move
    // together.
    const std::size_t old_end = old.offset + static_cast<std::size_t>(old.length);
    const auto new_length = static_cast<uint32_t>(sequence.size());
    const std::size_t new_end = old.offset + static_cast<std::size_t>(new_length);
    std::size_t next_start = bytes_.size();
    for (const Entry& entry : entries_)
    {
        if (entry.offset >= old_end && entry.offset != old.offset)
        {
            next_start = std::min<std::size_t>(next_start, entry.offset);
        }
    }

    const std::size_t new_next_start = (new_end + 3) / 4 * 4;
    std::vector<uint8_t> bytes(bytes_.begin(), bytes_.begin() + old.offset);
    bytes.insert(bytes.end(), sequence.begin(), sequence.end());
    bytes.resize(new_next_start, 0);
    bytes.insert(bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(next_start), bytes_.end());
    bytes_ = std::move(bytes);

    const auto move = static_cast<int64_t>(new_next_start) - static_cast<int64_t>(next_start);
    for (Entry& entry : entries_)
    {
        if (entry.offset == old.offset)
        {
            entry.length = new_length;
        }
        else if (entry.offset >= next_start)
        {
            entry.offset = static_cast<uint32_t>(entry.offset + move);
        }
    }
}

std::vector<uint8_t> SeqFile::Write() const
{
    std::vector<uint8_t> bytes = bytes_;
    xsflib::StoreBe16(bytes.data(), revision_);
    for (std::size_t i = 0; i < entries_.size(); i++)
    {
        xsflib::StoreBe32(bytes.data() + kTableOffset + i * kEntrySize, entries_[i].offset);
        xsflib::StoreBe32(bytes.data() + kTableOffset + i * kEntrySize + 4, entries_[i].length);
    }

    return bytes;
}

} // namespace ntdlib::n64
