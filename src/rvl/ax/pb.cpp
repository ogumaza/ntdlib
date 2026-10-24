// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/ax/pb.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "xsflib/bytes.h"

namespace ntdlib::rvl::ax
{
namespace
{

// A run of fields that both layouts have: its offset in Pb, its offset in the October 2006 layout and its size, all in
// bytes.
struct Part
{
    std::size_t pb;
    std::size_t october;
    std::size_t size;
};

constexpr std::array<Part, 3> kOctober2006Parts = {{
    {0x00, 0x00, 0x52}, // the links, the selects, the mixer control, the state, the type, the mix and the ITD
    {0x52, 0x5c, 0x70}, // the depop values, the envelope, the addresses, ADPCM, SRC and the low-pass filter
    {0xd6, 0xd4, 0x3e}, // the Wii Remote's flag, mixer control, mix, depop values and SRC
}};

static_assert(offsetof(Pb, lpf) + sizeof(PbLpf) == 0x52 + 0x70 && offsetof(Pb, rmt_iir) == 0xd6 + 0x3e);

} // namespace

static_assert(std::is_trivially_copyable_v<Pb>);

Pb LoadPb(const uint8_t* bytes)
{
    // A PB contains only 16-bit words. The byte order is converted one word at a time.
    std::array<uint16_t, kPbSize / 2> words;
    for (std::size_t i = 0; i < words.size(); i++)
    {
        words[i] = xsflib::LoadBe16(bytes + i * 2);
    }

    return std::bit_cast<Pb>(words);
}

void StorePb(const Pb& pb, uint8_t* bytes)
{
    const auto words = std::bit_cast<std::array<uint16_t, kPbSize / 2>>(pb);
    for (std::size_t i = 0; i < words.size(); i++)
    {
        xsflib::StoreBe16(bytes + i * 2, words[i]);
    }
}

Pb LoadPbOctober2006(const uint8_t* bytes)
{
    std::array<uint16_t, kPbSize / 2> words{};
    for (const Part& part : kOctober2006Parts)
    {
        for (std::size_t i = 0; i < part.size; i += 2)
        {
            words[(part.pb + i) / 2] = xsflib::LoadBe16(bytes + part.october + i);
        }
    }

    return std::bit_cast<Pb>(words);
}

void StorePbOctober2006(const Pb& pb, uint8_t* bytes)
{
    const auto words = std::bit_cast<std::array<uint16_t, kPbSize / 2>>(pb);
    for (const Part& part : kOctober2006Parts)
    {
        for (std::size_t i = 0; i < part.size; i += 2)
        {
            xsflib::StoreBe16(bytes + part.october + i, words[(part.pb + i) / 2]);
        }
    }
}

} // namespace ntdlib::rvl::ax
