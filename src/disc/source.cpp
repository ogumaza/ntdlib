// SPDX-License-Identifier: MIT

#include "ntdlib/disc/source.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <string>
#include <utility>
#include <vector>

namespace ntdlib::disc
{

Source::Source(const std::vector<std::filesystem::path>& parts, std::string noun) : noun_(std::move(noun))
{
    for (const std::filesystem::path& path : parts)
    {
        Part part;
        part.file.open(path, std::ios::binary);
        if (!part.file)
        {
            throw Error("can't open " + path.filename().string());
        }

        part.file.seekg(0, std::ios::end);
        part.start = size_;
        part.size = static_cast<uint64_t>(part.file.tellg());
        size_ += part.size;
        parts_.push_back(std::move(part));
    }
}

uint64_t Source::Size() const
{
    return size_;
}

void Source::Read(uint64_t offset, uint8_t* out, size_t size)
{
    if (offset > size_ || size > size_ - offset)
    {
        throw Error("the " + noun_ + " is truncated");
    }

    for (Part& part : parts_)
    {
        if (!size || offset >= part.start + part.size)
        {
            continue;
        }

        const uint64_t within = offset - part.start;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, part.size - within));
        part.file.clear();
        part.file.seekg(static_cast<std::streamoff>(within));
        if (!part.file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n)))
        {
            throw Error("reading the " + noun_ + " failed");
        }

        out += n;
        offset += n;
        size -= n;
    }
}

std::vector<uint8_t> Source::Read(uint64_t offset, size_t size)
{
    std::vector<uint8_t> data(size);
    Read(offset, data.data(), size);

    return data;
}

void Source::ReadPadded(uint64_t offset, uint8_t* out, size_t size)
{
    const size_t stored = offset < size_ ? static_cast<size_t>(std::min<uint64_t>(size, size_ - offset)) : 0;
    if (stored)
    {
        Read(offset, out, stored);
    }

    std::fill(out + stored, out + size, uint8_t{0});
}

} // namespace ntdlib::disc
