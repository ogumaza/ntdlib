// SPDX-License-Identifier: MIT

#include "ntdlib/nx/lz4.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::nx
{
namespace
{

constexpr uint32_t kFieldBits = 4;
constexpr uint32_t kFieldMax = 15;
constexpr uint8_t kContinues = 255;
constexpr std::size_t kMinCopy = 4;

// Reads the bytes of `block` in turn.
class BlockReader
{
public:
    explicit BlockReader(std::span<const uint8_t> block) : block_(block)
    {
    }

    bool AtEnd() const
    {
        return at_ == block_.size();
    }

    uint8_t Byte()
    {
        Need(1);

        return block_[at_++];
    }

    // The next `count` bytes.
    std::span<const uint8_t> Bytes(std::size_t count)
    {
        Need(count);
        const std::span<const uint8_t> bytes = block_.subspan(at_, count);
        at_ += count;

        return bytes;
    }

    // A field of the token, `value`, with the bytes that continue it.
    std::size_t Length(uint32_t value)
    {
        std::size_t length = value;
        if (value == kFieldMax)
        {
            uint8_t more = 0;
            do
            {
                more = Byte();
                length += more;
            } while (more == kContinues);
        }

        return length;
    }

private:
    void Need(std::size_t count) const
    {
        if (count > block_.size() - at_)
        {
            throw xsflib::FormatError("the LZ4 block ends early");
        }
    }

    std::span<const uint8_t> block_;
    std::size_t at_ = 0;
};

} // namespace

std::vector<uint8_t> DecompressLz4Block(std::span<const uint8_t> block, std::size_t size)
{
    const auto wrong_size = [size]
    {
        return xsflib::FormatError("the LZ4 block doesn't hold " + std::to_string(size) + " bytes");
    };

    BlockReader r(block);
    std::vector<uint8_t> out;
    for (;;)
    {
        const uint8_t token = r.Byte();
        const std::span<const uint8_t> literals = r.Bytes(r.Length(token >> kFieldBits));
        if (literals.size() > size - out.size())
        {
            throw wrong_size();
        }

        out.insert(out.end(), literals.begin(), literals.end());
        if (r.AtEnd())
        {
            break;
        }

        const uint32_t low = r.Byte();
        const uint32_t offset = low | uint32_t{r.Byte()} << 8;
        const std::size_t length = r.Length(token & kFieldMax) + kMinCopy;
        if (offset == 0 || offset > out.size())
        {
            throw xsflib::FormatError("the LZ4 block copies bytes that it hasn't written");
        }

        if (length > size - out.size())
        {
            throw wrong_size();
        }

        // Byte by byte: the copy may repeat bytes that it writes.
        for (std::size_t i = 0; i < length; i++)
        {
            const uint8_t repeated = out[out.size() - offset];
            out.push_back(repeated);
        }
    }

    if (out.size() != size)
    {
        throw wrong_size();
    }

    return out;
}

} // namespace ntdlib::nx
