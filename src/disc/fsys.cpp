// SPDX-License-Identifier: MIT

// Reads FSYS archives and decompresses LZSS data (see fsys.h).

#include "ntdlib/disc/fsys.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/disc/error.h"

namespace ntdlib::disc
{
namespace
{

constexpr std::array<uint8_t, 4> kLzssMagic = {'L', 'Z', 'S', 'S'};
constexpr std::array<uint8_t, 4> kFsysMagic = {'F', 'S', 'Y', 'S'};

// LZSS: the header's size, the window's size and the place of the first byte of output in it, and the length of the
// shortest copy. One byte of the stream gives at most 9 bytes of output: a flag byte and eight copies of two bytes
// give 8 × 18 bytes from 17.
constexpr std::size_t kLzssHeader = 16;
constexpr std::size_t kWindow = 4096;
constexpr std::size_t kWindowStart = 0xfee;
constexpr std::size_t kShortest = 3;
constexpr std::size_t kMostPerByte = 9;

// FSYS: the offsets of the number of files and of the archive's information block, the size of the part of an entry
// that the reader reads, and the flag of a file stored with LZSS.
constexpr std::size_t kFileCount = 0xc;
constexpr std::size_t kInformation = 0x18;
constexpr std::size_t kEntry = 0x28;
constexpr uint32_t kCompressed = 0x80000000;

// The `size` bytes at `offset` of `data`. Throws Error if they run past its end. `what` names them in the message.
std::span<const uint8_t> Bytes(std::span<const uint8_t> data, std::size_t offset, std::size_t size, const char* what)
{
    if (offset > data.size() || size > data.size() - offset)
    {
        throw Error(std::string(what) + " runs past the end of its data");
    }

    return data.subspan(offset, size);
}

// The big-endian word at `offset` of `data`.
uint32_t Word(std::span<const uint8_t> data, std::size_t offset, const char* what)
{
    return xsflib::LoadBe32(Bytes(data, offset, 4, what).data());
}

// The text from `offset` of `archive` up to the first zero byte.
std::string NameAt(std::span<const uint8_t> archive, std::size_t offset)
{
    const std::span<const uint8_t> rest =
        Bytes(archive, offset, archive.size() - std::min(offset, archive.size()), "an FSYS file's name");
    const auto end = std::find(rest.begin(), rest.end(), uint8_t{0});
    if (end == rest.end())
    {
        throw Error("an FSYS file's name has no end");
    }

    return std::string(rest.begin(), end);
}

} // namespace

std::vector<uint8_t> DecompressLzss(std::span<const uint8_t> data)
{
    const std::span<const uint8_t> header = Bytes(data, 0, kLzssHeader, "LZSS data's header");
    if (!std::equal(kLzssMagic.begin(), kLzssMagic.end(), header.begin()))
    {
        throw Error("LZSS data doesn't start with \"LZSS\"");
    }

    const uint32_t size = xsflib::LoadBe32(header.data() + 4);
    const uint32_t stored = xsflib::LoadBe32(header.data() + 8);
    if (stored < kLzssHeader)
    {
        throw Error("LZSS data's stored size is smaller than its header");
    }

    const std::span<const uint8_t> stream = Bytes(data, kLzssHeader, stored - kLzssHeader, "LZSS data");
    if (size > stream.size() * kMostPerByte)
    {
        throw Error("LZSS data gives a size that its stream can't hold");
    }

    // The items of each flag byte, until the output has its size (see fsys.h).
    std::vector<uint8_t> out;
    out.reserve(size);
    std::array<uint8_t, kWindow> window = {};
    std::size_t put = kWindowStart;
    std::size_t at = 0;
    const auto emit = [&out, &window, &put](uint8_t byte)
    {
        out.push_back(byte);
        window[put] = byte;
        put = (put + 1) % kWindow;
    };

    while (out.size() < size)
    {
        const uint8_t flags = Bytes(stream, at, 1, "LZSS data's stream")[0];
        at++;
        for (int bit = 0; bit < 8 && out.size() < size; bit++)
        {
            if ((flags >> bit & 1) != 0)
            {
                emit(Bytes(stream, at, 1, "LZSS data's stream")[0]);
                at++;
                continue;
            }

            const std::span<const uint8_t> copy = Bytes(stream, at, 2, "LZSS data's stream");
            at += 2;
            const std::size_t from = copy[0] | (copy[1] & 0xf0u) << 4;
            const std::size_t length = (copy[1] & 0x0fu) + kShortest;
            for (std::size_t k = 0; k < length && out.size() < size; k++)
            {
                emit(window[(from + k) % kWindow]);
            }
        }
    }

    return out;
}

std::vector<FsysFile> ReadFsys(std::span<const uint8_t> archive)
{
    const std::span<const uint8_t> magic = Bytes(archive, 0, kFsysMagic.size(), "an FSYS archive's header");
    if (!std::equal(kFsysMagic.begin(), kFsysMagic.end(), magic.begin()))
    {
        throw Error("the archive doesn't start with \"FSYS\"");
    }

    // The information block gives the offset of the table of the entries' offsets.
    const uint32_t count = Word(archive, kFileCount, "an FSYS archive's header");
    const uint32_t information = Word(archive, kInformation, "an FSYS archive's header");
    const uint32_t table = Word(archive, information, "an FSYS archive's information");

    std::vector<FsysFile> files;
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t at = Word(archive, table + std::size_t{4} * k, "an FSYS archive's table");
        const std::span<const uint8_t> entry = Bytes(archive, at, kEntry, "an FSYS archive's entry");
        const uint32_t offset = xsflib::LoadBe32(entry.data() + 0x4);
        const uint32_t size = xsflib::LoadBe32(entry.data() + 0x8);
        const uint32_t flags = xsflib::LoadBe32(entry.data() + 0xc);
        const uint32_t stored = xsflib::LoadBe32(entry.data() + 0x14);

        FsysFile file = {.id = xsflib::LoadBe32(entry.data()),
                         .name = NameAt(archive, xsflib::LoadBe32(entry.data() + 0x24))};
        if ((flags & kCompressed) != 0)
        {
            file.data = DecompressLzss(Bytes(archive, offset, stored, "an FSYS archive's file"));
            if (file.data.size() != size)
            {
                throw Error("FSYS file " + file.name + " decompresses to a size other than its entry's");
            }
        }
        else
        {
            const std::span<const uint8_t> bytes = Bytes(archive, offset, size, "an FSYS archive's file");
            file.data.assign(bytes.begin(), bytes.end());
        }

        files.push_back(std::move(file));
    }

    return files;
}

} // namespace ntdlib::disc
