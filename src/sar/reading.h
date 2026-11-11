// SPDX-License-Identifier: MIT

// The parts of a NintendoWare file that the readers of formats.h share: the header, the table of blocks and the
// options of an entry.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar::detail
{

// Optional parameters: 32 flag bits followed by one 32-bit value per set bit.
struct OptionParams
{
    static OptionParams Read(const xsflib::Reader& r, std::size_t off)
    {
        OptionParams p;
        const uint32_t flags = r.U32(off);
        std::size_t q = off + 4;
        for (int bit = 0; bit < 32; bit++)
        {
            if (flags >> bit & 1)
            {
                p.values[bit] = r.U32(q);
                p.value_offset[bit] = q;
                q += 4;
            }
        }

        return p;
    }

    std::array<std::optional<uint32_t>, 32> values{};
    std::array<std::size_t, 32> value_offset{};
};

// Block offsets and sizes in a NintendoWare binary file, indexed by type.
using Blocks = std::map<uint16_t, std::pair<uint32_t, uint32_t>>;

// Checks the header of a file of kind `kind` (SAR, WAV, etc.) and returns a reader in the file's byte order. The magic
// is the kind with C before it on the 3DS, and F on the Wii U and the Switch.
xsflib::Reader OpenFile(std::span<const uint8_t> file, const char* kind, const Options& options);

// The block table of a NintendoWare binary file: {type, offset, size} entries after a 0x14-byte header.
Blocks BlockTable(const xsflib::Reader& r);

// Returns a required block's offset and size as std::size_t. On 64-bit builds, derived offsets cannot wrap at 32 bits
// and point back into the file. On 32-bit builds they can. Corrupt files may then produce misleading errors.
std::pair<std::size_t, std::size_t> RequiredBlock(const Blocks& blocks, uint16_t type, const char* name);

// Returns the offset and size of block `type`, named `name` (such as INFO), in a file that kStopAtFirstProblem's
// messages on the 3DS call `what` (such as "a wave (CWAV)"). A missing block throws FormatError. With
// kStopAtFirstProblem on the 3DS, the message names the file: "a wave (CWAV) has no INFO block". Otherwise the message
// is "no INFO block".
std::pair<std::size_t, std::size_t> BlockOf(const Blocks& blocks, uint16_t type, const char* name, const char* what,
                                            const Options& options);

// Reads a main send and a send to each aux bus at `at`.
inline void ReadSends(const xsflib::Reader& r, std::size_t at, uint8_t& main_send, std::array<uint8_t, 3>& aux_sends)
{
    main_send = r.U8(at);
    for (std::size_t bus = 0; bus < aux_sends.size(); ++bus)
    {
        aux_sends[bus] = r.U8(at + 1 + bus);
    }
}

} // namespace ntdlib::sar::detail
