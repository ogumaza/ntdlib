// SPDX-License-Identifier: MIT

#include "ntdlib/sar/sequence_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kDataBlock = 0x5000;
constexpr uint16_t kLabelBlock = 0x5001;
constexpr uint16_t kLabelReference = 0x5100; // from the table of labels to a label
constexpr uint16_t kDataReference = 0x1F00;  // from a label to its offset in the bytecode
constexpr std::size_t kAlignment = 32;

// A file's bytes in its byte order.
class Out
{
public:
    explicit Out(bool big_endian) : big_endian_(big_endian)
    {
    }

    // Appends `bytes`, and returns where they start.
    std::size_t Append(std::span<const uint8_t> bytes)
    {
        const std::size_t at = bytes_.size();
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return at;
    }

    // Appends `count` zero bytes, and returns where they start.
    std::size_t Zeros(std::size_t count)
    {
        const std::size_t at = bytes_.size();
        bytes_.resize(at + count);
        return at;
    }

    void Align(std::size_t alignment)
    {
        Zeros((alignment - bytes_.size() % alignment) % alignment);
    }

    void U16(std::size_t at, uint16_t value)
    {
        big_endian_ ? xsflib::StoreBe16(&bytes_[at], value) : xsflib::StoreLe16(&bytes_[at], value);
    }

    void U32(std::size_t at, uint32_t value)
    {
        big_endian_ ? xsflib::StoreBe32(&bytes_[at], value) : xsflib::StoreLe32(&bytes_[at], value);
    }

    // A reference: its type, two bytes of padding and an offset.
    void Reference(std::size_t at, uint16_t type, uint32_t offset)
    {
        U16(at, type);
        U32(at + 4, offset);
    }

    std::size_t Size() const
    {
        return bytes_.size();
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
    bool big_endian_ = false;
};

// Appends a block: its magic, its size and `body`, padded. Returns where it starts.
std::size_t AppendBlock(Out& out, const char* magic, std::span<const uint8_t> body)
{
    const std::size_t start = out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(4);
    out.Append(body);
    out.Align(kAlignment);
    out.U32(start + 4, static_cast<uint32_t>(out.Size() - start));

    return start;
}

} // namespace

SequenceFile SequenceFile::Read(std::span<const uint8_t> file)
{
    if (file.size() < 0x14 || (std::memcmp(file.data(), "CSEQ", 4) != 0 && std::memcmp(file.data(), "FSEQ", 4) != 0))
    {
        throw xsflib::FormatError("not a sequence file (CSEQ or FSEQ)");
    }

    const bool big_endian = file[4] == 0xFE && file[5] == 0xFF;
    if (!big_endian && (file[4] != 0xFF || file[5] != 0xFE))
    {
        throw xsflib::FormatError("a sequence file without a byte order mark");
    }

    SequenceFile s;
    s.flavor = file[0] == 'C' ? Flavor::kCtr : big_endian ? Flavor::kCafe : Flavor::kNx;
    if (s.flavor == Flavor::kCtr && big_endian)
    {
        throw xsflib::FormatError("a big-endian CSEQ");
    }

    const xsflib::Reader r(file, big_endian);
    s.version = r.U32(8);
    std::size_t data = 0;
    std::size_t data_size = 0;
    std::size_t labels = 0;
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        const uint16_t type = r.U16(entry);
        if (type == kDataBlock)
        {
            data = r.U32(entry + 4);
            data_size = r.U32(entry + 8);
        }
        else if (type == kLabelBlock)
        {
            labels = r.U32(entry + 4);
        }
    }
    if (data == 0 || labels == 0 || data_size < 8)
    {
        throw xsflib::FormatError("a sequence file without DATA or LABL");
    }

    const std::span<const uint8_t> bytecode = r.Sub(data + 8, data_size - 8);
    s.data.assign(bytecode.begin(), bytecode.end());

    // The labels' references count from the table's start.
    const std::size_t table = labels + 8;
    const uint32_t count = r.U32(table);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t label = table + r.U32(table + 4 + std::size_t{i} * 8 + 4);
        const uint32_t length = r.U32(label + 8);
        s.labels.push_back({r.CString(label + 12, length), r.U32(label + 4)});
    }

    return s;
}

std::vector<uint8_t> SequenceFile::Write() const
{
    Out out(flavor == Flavor::kCafe);

    // The header, with a table of two blocks.
    out.Append(std::span(reinterpret_cast<const uint8_t*>(flavor == Flavor::kCtr ? "CSEQ" : "FSEQ"), 4));
    out.Zeros(0x14 + 2 * 12 - 4);
    out.Align(kAlignment);
    out.U16(4, 0xFEFF);
    out.U16(6, static_cast<uint16_t>(out.Size()));
    out.U32(8, version);
    out.U16(0x10, 2);

    const std::size_t data_block = AppendBlock(out, "DATA", data);
    out.Reference(0x14, kDataBlock, static_cast<uint32_t>(data_block));
    out.U32(0x1C, static_cast<uint32_t>(out.Size() - data_block));

    // The table of labels: a count and a reference to each label, then the labels.
    Out table(flavor == Flavor::kCafe);
    table.Zeros(4 + labels.size() * 8);
    table.U32(0, static_cast<uint32_t>(labels.size()));
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        const Sequence::Label& label = labels[i];
        const std::size_t at = table.Zeros(12);
        table.Reference(4 + i * 8, kLabelReference, static_cast<uint32_t>(at));
        table.Reference(at, kDataReference, label.offset);
        table.U32(at + 8, static_cast<uint32_t>(label.name.size()));
        table.Append(std::span(reinterpret_cast<const uint8_t*>(label.name.data()), label.name.size()));
        table.Zeros(1);
        table.Align(4);
    }

    const std::vector<uint8_t> table_bytes = table.Take();
    const std::size_t label_block = AppendBlock(out, "LABL", table_bytes);
    out.Reference(0x20, kLabelBlock, static_cast<uint32_t>(label_block));
    out.U32(0x28, static_cast<uint32_t>(out.Size() - label_block));
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

} // namespace ntdlib::sar
