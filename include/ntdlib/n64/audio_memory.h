// SPDX-License-Identifier: MIT

// The memory that a model of the Nintendo 64's audio reads and writes. AudioMemory is the plain form for a model: a
// work area for the states and the output, and the read-only bytes of the game's banks and samples. A work address
// starts at 0 and an image address at kImageBase. The bytes are in the console's order.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ntdlib::n64
{

// What a model of the RSP's direct memory access reaches.
class AudioBus
{
public:
    virtual ~AudioBus() = default;
    virtual void Read(uint32_t address, uint8_t* out, std::size_t size) const = 0;
    virtual void Write(uint32_t address, const uint8_t* in, std::size_t size) = 0;

protected:
    AudioBus() = default;
    AudioBus(const AudioBus&) = default;
    AudioBus& operator=(const AudioBus&) = default;
};

class AudioMemory : public AudioBus
{
public:
    static constexpr uint32_t kImageBase = 0x40000000;

    // `image` must outlive the memory and every copy of it. `work_size` is the work area's size in bytes.
    AudioMemory(std::span<const uint8_t> image, std::size_t work_size) : image_(image), work_(work_size, 0)
    {
    }

    // Takes `size` bytes of the work area, at a multiple of 16 bytes, and returns their address.
    uint32_t Allocate(std::size_t size)
    {
        const std::size_t start = (used_ + 15) & ~std::size_t{15};
        if (start + size > work_.size())
        {
            throw std::length_error("the audio work area is full");
        }

        used_ = start + size;

        return static_cast<uint32_t>(start);
    }

    // Bytes that lie outside the memory read as zero. The console's DMA would read other memory there.
    void Read(uint32_t address, uint8_t* out, std::size_t size) const override
    {
        const std::span<const uint8_t> bytes = address >= kImageBase ? image_ : std::span<const uint8_t>(work_);
        const std::size_t start = address >= kImageBase ? address - kImageBase : address;
        const std::size_t available = start < bytes.size() ? std::min(size, bytes.size() - start) : 0;
        if (available != 0)
        {
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(start), available, out);
        }

        std::fill_n(out + available, size - available, uint8_t{0});
    }

    // Writes that leave the work area are dropped.
    void Write(uint32_t address, const uint8_t* in, std::size_t size) override
    {
        if (address >= work_.size())
        {
            return;
        }

        const std::size_t available = std::min(size, work_.size() - address);
        std::copy_n(in, available, work_.begin() + static_cast<std::ptrdiff_t>(address));
    }

    std::span<uint8_t> Work()
    {
        return work_;
    }

    std::span<const uint8_t> Work() const
    {
        return work_;
    }

    // The bytes of the work area that Allocate has given out.
    std::size_t UsedBytes() const
    {
        return used_;
    }

    // Takes back what Allocate gave out after it had given out `used` bytes.
    void GiveBackTo(std::size_t used)
    {
        used_ = used;
    }

    std::span<const uint8_t> Image() const
    {
        return image_;
    }

private:
    std::span<const uint8_t> image_;
    std::vector<uint8_t> work_;
    std::size_t used_ = 0;
};

} // namespace ntdlib::n64
