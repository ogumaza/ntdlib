// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/image.h"

#include <algorithm>
#include <utility>

#include "xsflib/binary.h"
#include "xsflib/sha1.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

constexpr std::size_t kHeaderSize = 0x40;
constexpr std::size_t kNameOffset = 0x20;
constexpr std::size_t kNameSize = 20;

} // namespace

Image::Image(std::vector<uint8_t> file) : bytes_(std::move(file))
{
    if (bytes_.size() < kHeaderSize)
    {
        throw xsflib::FormatError("the file is too short for a Nintendo 64 image");
    }

    // A cartridge's first byte is 0x80 in the console's order. The other two orders swap each halfword or each word.
    if (bytes_[0] == 0x37 && bytes_[1] == 0x80)
    {
        for (std::size_t i = 0; i + 1 < bytes_.size(); i += 2)
        {
            std::swap(bytes_[i], bytes_[i + 1]);
        }
    }
    else if (bytes_[0] == 0x40 && bytes_[3] == 0x80)
    {
        for (std::size_t i = 0; i + 3 < bytes_.size(); i += 4)
        {
            std::swap(bytes_[i], bytes_[i + 3]);
            std::swap(bytes_[i + 1], bytes_[i + 2]);
        }
    }
    else if (bytes_[0] != 0x80)
    {
        throw xsflib::FormatError("the file doesn't start like a Nintendo 64 image");
    }

    sha1_ = xsflib::Sha1::Hex(xsflib::Sha1::Of(bytes_));
}

std::span<const uint8_t> Image::Bytes() const
{
    return bytes_;
}

const std::string& Image::Sha1() const
{
    return sha1_;
}

std::string Image::HeaderName() const
{
    std::string name(bytes_.begin() + kNameOffset, bytes_.begin() + kNameOffset + kNameSize);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
    {
        name.pop_back();
    }

    return name;
}

} // namespace ntdlib::sequence_export::n64
