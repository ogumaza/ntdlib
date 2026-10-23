// SPDX-License-Identifier: MIT

#include "sar/stream_details.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar::detail
{
namespace
{

constexpr uint16_t kListReference = 0x0100;  // a list of 32-bit values
constexpr uint16_t kTableReference = 0x0101; // a table of references
constexpr uint16_t kStreamTrack = 0x220E;
constexpr uint16_t kSends = 0x220F;
constexpr uint32_t kNone = 0xFFFFFFFF;

// Appends sends to `out`: the main send, a send to each aux bus, and a byte of padding.
void AppendSends(Out& out, uint8_t main, const std::array<uint8_t, 3>& aux)
{
    const std::size_t at = out.Zeros(8);
    out.U8(at, main);
    for (std::size_t bus = 0; bus < aux.size(); ++bus)
    {
        out.U8(at + 1 + bus, aux[bus]);
    }
}

} // namespace

std::vector<uint8_t> StreamHeader(uint32_t version, bool big_endian)
{
    Out out(big_endian);
    if (version < kSendsVersion)
    {
        out.Zeros(16);
        return out.Take();
    }

    out.Zeros(version < kNothingVersion ? 24 : 36);
    if (version >= kNothingVersion)
    {
        out.U32(28, kNone);
        out.U32(32, kNone);
    }

    return out.Take();
}

std::vector<uint8_t> StreamDetailsOf(std::span<const uint8_t> header, const StreamSoundInfo& stream, bool sends,
                                     bool big_endian)
{
    Out out(big_endian);
    out.Append(header);
    out.U16(0, stream.valid_tracks);
    out.U16(2, stream.channel_count);
    const std::size_t table = out.Size();
    out.Reference(4, kTableReference, static_cast<uint32_t>(table));
    if (sends)
    {
        out.U32(12, std::bit_cast<uint32_t>(stream.pitch));
    }

    // The table, then each track's info, its channels' indices and its sends.
    out.Zeros(4 + 8 * stream.tracks.size());
    out.U32(table, static_cast<uint32_t>(stream.tracks.size()));
    for (std::size_t t = 0; t < stream.tracks.size(); ++t)
    {
        const StreamTrackInfo& track = stream.tracks[t];
        const std::size_t info = out.Zeros(sends ? 24 : 12);
        out.Reference(table + 4 + 8 * t, kStreamTrack, static_cast<uint32_t>(info - table));
        out.U8(info, track.volume);
        out.U8(info + 1, track.pan);
        out.U8(info + 2, track.span);
        out.U8(info + 3, track.flags);
        const std::size_t indices = out.Zeros(4 + track.channels.size());
        out.Align(4);
        out.Reference(info + 4, kListReference, static_cast<uint32_t>(indices - info));
        out.U32(indices, static_cast<uint32_t>(track.channels.size()));
        for (std::size_t c = 0; c < track.channels.size(); ++c)
        {
            out.U8(indices + 4 + c, track.channels[c]);
        }
        if (sends)
        {
            out.U8(info + 20, track.low_pass);
            out.U8(info + 21, track.biquad_type);
            out.U8(info + 22, track.biquad_value);
            out.Reference(info + 12, kSends, static_cast<uint32_t>(out.Size() - info));
            AppendSends(out, track.main_send, track.aux_sends);
        }
    }
    if (sends)
    {
        out.Reference(16, kSends, static_cast<uint32_t>(out.Size()));
        AppendSends(out, stream.main_send, stream.aux_sends);
    }

    return out.Take();
}

} // namespace ntdlib::sar::detail
