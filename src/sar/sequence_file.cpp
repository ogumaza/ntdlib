// SPDX-License-Identifier: MIT

#include "ntdlib/sar/sequence_file.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"
#include "sar/file_parts.h"
#include "sar/format_ids.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kDataBlock = 0x5000;
constexpr uint16_t kLabelBlock = 0x5001;
constexpr uint16_t kLabelReference = 0x5100; // from the table of labels to a label

using detail::kDataReference;
using detail::Out;

// Appends block `index` of type `type`: its magic, its size and `body`, padded.
void AppendBlock(Out& out, std::size_t index, uint16_t type, const char* magic, std::span<const uint8_t> body)
{
    const std::size_t start = detail::StartBlock(out, magic);
    out.Append(body);
    out.Align(detail::kBlockAlignment);
    detail::SetBlock(out, index, type, start);
}

} // namespace

SequenceFile SequenceFile::Read(std::span<const uint8_t> file)
{
    const auto [flavor, r] = detail::OpenFile(file, "SEQ", "sequence file");
    SequenceFile s;
    s.flavor = flavor;
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
    detail::AppendHeader(out, flavor == Flavor::kCtr ? "CSEQ" : "FSEQ", version, 2);
    AppendBlock(out, 0, kDataBlock, "DATA", data);

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

    AppendBlock(out, 1, kLabelBlock, "LABL", table.Take());
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

} // namespace ntdlib::sar
