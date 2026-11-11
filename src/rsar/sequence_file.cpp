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
#include "rsar/file_blocks.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar
{
namespace
{

constexpr uint32_t kBytecodeOffset = 0xC; // from DATA's start

using detail::CloseBlock;
using detail::CloseFile;
using detail::Header;
using detail::kHeaderSize;
using detail::OpenBlock;
using sar::detail::Out;

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

    s.labels = detail::ReadLabels(r, labels);

    return s;
}

std::vector<uint8_t> SequenceFile::Write() const
{
    // The header, with the offsets and sizes of two blocks.
    Out out = Header("RSEQ", version, 2);

    // DATA: the bytecode's offset, then the bytecode.
    const std::size_t data_block = OpenBlock(out, "DATA");
    out.U32(out.Zeros(4), kBytecodeOffset);
    out.Append(data);
    CloseBlock(out, data_block, 0);

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

    const std::size_t label_block = OpenBlock(out, "LABL");
    out.Append(table.Take());
    CloseBlock(out, label_block, 1);

    return CloseFile(out);
}

} // namespace ntdlib::rsar
