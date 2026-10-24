// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/sequence_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar
{
namespace
{

constexpr std::size_t kHeaderSize = 0x20;
constexpr uint32_t kBytecodeOffset = 0xC; // from DATA's start
constexpr std::size_t kAlignment = 32;

using sar::detail::Out;

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
    if (file.size() < kHeaderSize || std::memcmp(file.data(), "RSEQ", 4) != 0)
    {
        throw xsflib::FormatError("not a sequence file of the Wii (RSEQ)");
    }

    const xsflib::Reader r(file, true);
    if (r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("an RSEQ file without a byte order mark");
    }

    // The header gives DATA's offset and size, then LABL's.
    const uint32_t data = r.U32(0x10);
    const uint32_t data_size = r.U32(0x14);
    const uint32_t labels = r.U16(0x0E) >= 2 ? r.U32(0x18) : 0;
    if (data == 0 || labels == 0 || data_size < kBytecodeOffset || !r.Magic(data, "DATA") || !r.Magic(labels, "LABL"))
    {
        throw xsflib::FormatError("an RSEQ file without DATA or LABL");
    }

    const uint32_t bytecode = r.U32(data + 8);
    if (bytecode > data_size)
    {
        throw xsflib::FormatError("an RSEQ file's bytecode starts past its DATA block");
    }

    SequenceFile s;
    s.version = r.U16(6);
    const std::span<const uint8_t> commands = r.Sub(data + std::size_t{bytecode}, data_size - bytecode);
    s.data.assign(commands.begin(), commands.end());

    // The labels' offsets count from the table's start.
    const std::size_t table = labels + std::size_t{8};
    const uint32_t count = r.U32(table);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t label = table + r.U32(table + 4 + std::size_t{i} * 4);
        const std::span<const uint8_t> name = r.Sub(label + 8, r.U32(label + 4));
        s.labels.push_back({std::string(name.begin(), name.end()), r.U32(label)});
    }

    return s;
}

std::vector<uint8_t> SequenceFile::Write() const
{
    Out out(true);

    // The header, with the offsets and sizes of two blocks.
    out.Append(std::span(reinterpret_cast<const uint8_t*>("RSEQ"), 4));
    out.Zeros(kHeaderSize - 4);
    out.U16(4, 0xFEFF);
    out.U16(6, version);
    out.U16(0x0C, static_cast<uint16_t>(kHeaderSize));
    out.U16(0x0E, 2);

    // DATA: the bytecode's offset, then the bytecode.
    std::vector<uint8_t> body(4);
    xsflib::StoreBe32(body.data(), kBytecodeOffset);
    body.insert(body.end(), data.begin(), data.end());
    const std::size_t data_block = AppendBlock(out, "DATA", body);
    out.U32(0x10, static_cast<uint32_t>(data_block));
    out.U32(0x14, static_cast<uint32_t>(out.Size() - data_block));

    // LABL: the number of labels and an offset to each, then the labels.
    Out table(true);
    table.Zeros(4 + labels.size() * 4);
    table.U32(0, static_cast<uint32_t>(labels.size()));
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        const Sequence::Label& label = labels[i];
        const std::size_t at = table.Zeros(8);
        table.U32(4 + i * 4, static_cast<uint32_t>(at));
        table.U32(at, label.offset);
        table.U32(at + 4, static_cast<uint32_t>(label.name.size()));
        table.Append(std::span(reinterpret_cast<const uint8_t*>(label.name.data()), label.name.size()));
        table.Zeros(1);
        table.Align(4);
    }

    const std::vector<uint8_t> table_bytes = table.Take();
    const std::size_t label_block = AppendBlock(out, "LABL", table_bytes);
    out.U32(0x18, static_cast<uint32_t>(label_block));
    out.U32(0x1C, static_cast<uint32_t>(out.Size() - label_block));
    out.U32(0x08, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

} // namespace ntdlib::rsar
