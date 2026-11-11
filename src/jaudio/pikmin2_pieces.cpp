// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/pikmin2_pieces.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/jaudio/aaf.h"

namespace ntdlib::jaudio
{
namespace
{

// The bytes that the driver's table gives each name.
constexpr std::size_t kNameSize = 24;

} // namespace

const Pikmin2Piece* Pikmin2PieceOf(uint32_t sound_id)
{
    const uint32_t index = sound_id & 0xffff;
    if ((sound_id & kAafTypeMask) != kAafSequence || index >= kPikmin2Pieces.size())
    {
        return nullptr;
    }

    return &kPikmin2Pieces[index];
}

void AppendPikmin2Pieces(std::vector<uint32_t>& words)
{
    words.push_back(static_cast<uint32_t>(kPikmin2Pieces.size()));
    for (const Pikmin2Piece& piece : kPikmin2Pieces)
    {
        words.push_back(static_cast<uint32_t>(piece.kind) | uint32_t{piece.start_point} << 8 |
                        uint32_t{piece.beat_interval} << 16);
        words.insert(words.end(), piece.info.begin(), piece.info.end());
        for (const std::string_view name : {std::string_view(piece.sequence), std::string_view(piece.conductor)})
        {
            std::array<uint8_t, kNameSize> bytes = {};
            std::copy_n(name.begin(), std::min(name.size(), bytes.size() - 1), bytes.begin());
            for (std::size_t i = 0; i < bytes.size(); i += 4)
            {
                words.push_back(xsflib::LoadBe32(bytes.data() + i));
            }
        }
    }
}

} // namespace ntdlib::jaudio
