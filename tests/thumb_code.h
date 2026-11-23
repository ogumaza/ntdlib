// SPDX-License-Identifier: MIT

// A writer of Thumb code for the tests of the routine reader and the searches: halfwords given as numbers, with bl,
// blx and the branches to labels resolved at the end. The instructions that the tests use are named below in the
// encodings of the architecture reference manual's Thumb chapter.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ntdlib::testing
{

// The Thumb instructions that the tests write.
constexpr uint16_t ThumbPush(uint16_t low_registers)
{
    return static_cast<uint16_t>(0xB500 | low_registers); // push {..., lr}
}

constexpr uint16_t ThumbPop(uint16_t low_registers)
{
    return static_cast<uint16_t>(0xBD00 | low_registers); // pop {..., pc}
}

constexpr uint16_t ThumbMovs(int rd, uint32_t immediate)
{
    return static_cast<uint16_t>(0x2000 | (rd << 8) | immediate);
}

constexpr uint16_t ThumbCmp(int rn, uint32_t immediate)
{
    return static_cast<uint16_t>(0x2800 | (rn << 8) | immediate);
}

constexpr uint16_t ThumbCopy(int rd, int rs)
{
    return static_cast<uint16_t>(0x1C00 | (rs << 3) | rd); // adds rd, rs, #0
}

constexpr uint16_t ThumbLdr(int rd, int rb, uint32_t offset)
{
    return static_cast<uint16_t>(0x6800 | ((offset / 4) << 6) | (rb << 3) | rd);
}

constexpr uint16_t ThumbStr(int rd, int rb, uint32_t offset)
{
    return static_cast<uint16_t>(0x6000 | ((offset / 4) << 6) | (rb << 3) | rd);
}

constexpr uint16_t ThumbLdrh(int rd, int rb, uint32_t offset)
{
    return static_cast<uint16_t>(0x8800 | ((offset / 2) << 6) | (rb << 3) | rd);
}

constexpr uint16_t ThumbLdrb(int rd, int rb, uint32_t offset)
{
    return static_cast<uint16_t>(0x7800 | (offset << 6) | (rb << 3) | rd);
}

constexpr uint16_t ThumbLdrSp(int rd, uint32_t offset)
{
    return static_cast<uint16_t>(0x9800 | (rd << 8) | (offset / 4));
}

constexpr uint16_t ThumbStrSp(int rd, uint32_t offset)
{
    return static_cast<uint16_t>(0x9000 | (rd << 8) | (offset / 4));
}

constexpr uint16_t ThumbSubSp(uint32_t bytes)
{
    return static_cast<uint16_t>(0xB080 | (bytes / 4));
}

constexpr uint16_t ThumbAddSp(uint32_t bytes)
{
    return static_cast<uint16_t>(0xB000 | (bytes / 4));
}

constexpr uint16_t kThumbBxLr = 0x4770;

// The conditions of a conditional branch.
constexpr uint32_t kThumbEq = 0x0;
constexpr uint32_t kThumbNe = 0x1;
constexpr uint32_t kThumbGe = 0xA;

class ThumbCode
{
public:
    explicit ThumbCode(uint32_t base) : base_(base)
    {
    }

    // The address of the next halfword.
    uint32_t Here() const
    {
        return base_ + static_cast<uint32_t>(2 * halves_.size());
    }

    void Half(uint16_t halfword)
    {
        halves_.push_back(halfword);
    }

    // Binds `label` here: a Thumb routine's address, with bit 0 set, is At(label) | 1.
    void Bind(const std::string& label)
    {
        labels_[label] = Here();
    }

    // Binds `label` at an address outside the code, such as a routine in ARM code.
    void BindAt(const std::string& label, uint32_t address)
    {
        labels_[label] = address;
    }

    uint32_t At(const std::string& label) const
    {
        return labels_.at(label);
    }

    // bl to a Thumb routine, or blx to an ARM routine.
    void Bl(const std::string& label)
    {
        calls_.emplace_back(halves_.size(), label, false);
        halves_.push_back(0);
        halves_.push_back(0);
    }

    void Blx(const std::string& label)
    {
        calls_.emplace_back(halves_.size(), label, true);
        halves_.push_back(0);
        halves_.push_back(0);
    }

    // A conditional branch to `label`.
    void B(uint32_t condition, const std::string& label)
    {
        branches_.emplace_back(halves_.size(), label);
        halves_.push_back(static_cast<uint16_t>(0xD000 | (condition << 8)));
    }

    // Pads to a word with a nop (mov r8, r8).
    void Align()
    {
        if (halves_.size() % 2 != 0)
        {
            halves_.push_back(0x46C0);
        }
    }

    std::vector<uint8_t> Finish()
    {
        for (const auto& [at, label, exchange] : calls_)
        {
            const uint32_t address = base_ + static_cast<uint32_t>(2 * at);
            const uint32_t target = exchange ? labels_.at(label) & ~3u : labels_.at(label);
            const uint32_t offset = target - ((address + 4) & (exchange ? ~3u : ~0u));
            halves_[at] = static_cast<uint16_t>(0xF000 | ((offset >> 12) & 0x7FF));
            halves_[at + 1] = static_cast<uint16_t>((exchange ? 0xE800 : 0xF800) | ((offset >> 1) & 0x7FF));
        }
        for (const auto& [at, label] : branches_)
        {
            const uint32_t address = base_ + static_cast<uint32_t>(2 * at);
            const auto offset = static_cast<int32_t>(labels_.at(label) - (address + 4));
            if (offset < -256 || offset > 254)
            {
                throw std::logic_error("a conditional branch out of range");
            }
            halves_[at] = static_cast<uint16_t>(halves_[at] | ((static_cast<uint32_t>(offset) >> 1) & 0xFF));
        }

        std::vector<uint8_t> bytes;
        for (const uint16_t halfword : halves_)
        {
            bytes.push_back(static_cast<uint8_t>(halfword));
            bytes.push_back(static_cast<uint8_t>(halfword >> 8));
        }

        return bytes;
    }

private:
    uint32_t base_ = 0;
    std::vector<uint16_t> halves_;
    std::map<std::string, uint32_t> labels_;
    std::vector<std::tuple<std::size_t, std::string, bool>> calls_;
    std::vector<std::pair<std::size_t, std::string>> branches_;
};

} // namespace ntdlib::testing
