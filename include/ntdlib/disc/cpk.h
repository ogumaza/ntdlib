// SPDX-License-Identifier: MIT

// CRI Middleware's CPK archives, and CRILAYLA, the compression that their files may use. A CPK is made of chunks. Each
// chunk starts with a 16-byte header: 4 bytes of magic, 4 bytes that ListCpk skips, and the size of the chunk's table
// in 64 bits, little-endian. The table follows the header in CRI's @UTF format. The archive starts with the chunk
// "CPK ". That chunk's table has one row. The row gives
// the offset of the chunk "TOC " (TocOffset) and the offset of the files' data (ContentOffset). The TOC's table has a
// row for each file: the file's folder (DirName), its name (FileName), the bytes that the archive stores (FileSize),
// the size once decompressed (ExtractSize) and the offset (FileOffset). A file's offset in the archive is FileOffset
// plus the lower of TocOffset and ContentOffset. A file stored with CRILAYLA starts with the magic "CRILAYLA". An
// archive may list its files by ID only, in a chunk "ITOC" without a TOC. ListCpk lists no files for such an archive.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::disc
{

// A file that a CPK's TOC lists.
struct CpkFile
{
    std::string path;     // the folder, a slash and the name ("SOUND/a.brsar"), or the name alone for an empty folder
    uint64_t offset;      // the stored bytes' offset in the archive
    uint64_t stored_size; // FileSize: the bytes that the archive stores
    uint64_t size;        // ExtractSize, or FileSize when the TOC gives no ExtractSize or gives 0
};

// Reads `size` bytes at `offset` in a CPK. The range lies inside the archive. Throws Error if the read fails.
using CpkRead = std::function<std::vector<uint8_t>(uint64_t offset, std::size_t size)>;

// Lists the files in the TOC of the CPK of `size` bytes that `read` reads. Reads only the archive's first chunk and
// its TOC. A TOC's names are Shift-JIS. The paths are UTF-8, with %XX for each byte that doesn't decode. A file whose
// stored bytes lie outside the archive is left out. Returns no files for an archive without a TOC. Throws Error if the
// data isn't a CPK, or if its chunks or tables are damaged.
std::vector<CpkFile> ListCpk(const CpkRead& read, uint64_t size);

// Reads a file that ListCpk listed, and decompresses it if it's stored with CRILAYLA. Throws Error if the file is
// larger than `max_size` bytes, if its data is damaged, or if its size isn't the size that the TOC gives.
std::vector<uint8_t> ReadCpkFile(const CpkRead& read, const CpkFile& file, std::size_t max_size);

// True if `data` starts with the magic "CRILAYLA".
bool IsCrilayla(std::span<const uint8_t> data);

// Decompresses CRILAYLA data. The data starts with the magic, the size of the compressed part once decompressed (32
// bits, little-endian), and the offset from 0x10 of 0x100 bytes stored as they are (32 bits, little-endian). The output
// is those 0x100 bytes, then the decompressed part. The compressed stream lies between 0x10 and the stored bytes. It's
// read from its end towards 0x10, and each byte's bits from the most significant. The decompressed part is written from
// its end towards its start. Each step reads a bit. A 0 is followed by a byte in 8 bits. A 1 is followed by a copy:
// a distance in 13 bits, then a length. The copy's source is the distance plus 3 bytes past the byte about to be
// written, among the bytes already written. The length is 3, plus a number of 2 bits. If that number is 3, a number of
// 3 bits follows, then one of 5 bits if that number is 7, then one of 8 bits if that number is 31, then more numbers
// of 8 bits while each is 255. The length adds them all. Throws Error if the data is damaged or truncated, or if it
// would decompress to more than `max_size` bytes.
std::vector<uint8_t> DecompressCrilayla(std::span<const uint8_t> data, std::size_t max_size);

} // namespace ntdlib::disc
