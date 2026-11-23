// SPDX-License-Identifier: MIT

// DSE's files of version 0x402, the oldest version known. Luminous Arc's songs and wave banks are of that version. They
// contain the chunks of version 0x415 (formats.h) with other layouts inside them:
//
// - A song's settings take 16 bytes, not 48. Bytes 2 and 3 give the ticks of a quarter note, byte 4 the number of
//   tracks, byte 5 the number of channels, byte 6 what version 0x415 gives at byte 4, the high 4 bits of byte 7 the
//   global volume's index, and byte 8 the priority. The tracks are those of version 0x415. Upgraded keeps all 16
//   bytes at 0x20 to 0x2F of the 48 bytes of settings of version 0x415. No build reads those places.
// - A wave bank's header gives the numbers of the waves, the programs and the key groups in single bytes at 0x46,
//   0x47 and 0x48. A chunk's header gives 0x402 where version 0x415 gives 0x415.
// - A wave's entry takes 64 bytes, as in version 0x415. Some banks' entries take 48 bytes and end before the envelope
//   at 0x30. The places differ from version 0x415: the wave's number at 0x01, the format at 0x10 (a byte), the loop at
//   0x11, the sample rate at 0x12 (16 bits), the samples' place at 0x14, and the loop's start and length at 0x18 and
//   0x1C.
// - A program holds a header of 16 bytes (its number at 0x00 and its number of splits at 0x01, both single bytes), 16
//   entries of 8 bytes that the driver doesn't read, 4 LFOs and its splits. A split takes 48 bytes, as in version
//   0x415, with the wave at 0x11 (a byte), the fine tuning at 0x12, the root key at 0x14, the coarse tuning at 0x15,
//   the volume at 0x16, the pan at 0x17 and the key group at 0x18. The envelope is at 0x20, as in version 0x415.
//
// The places are those that Luminous Arc's driver reads (North America, NTR-ANIE-USA): the song's settings at
// 0x020C0784, the bank's header and chunks at 0x020BFA5C, a note's split and wave at 0x020C423C, a program's LFOs at
// 0x020C40B4.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/dse/formats.h"

namespace ntdlib::dse
{

// Whether `file` starts with a DSE header of version 0x402.
bool IsOldestVersion(std::span<const uint8_t> file);

// A song or a wave bank of version 0x402 as its header and its chunks, every byte kept: Write gives the file back byte
// for byte.
struct OldestFile
{
    struct Chunk
    {
        std::array<uint8_t, 16> header = {};
        std::vector<uint8_t> data;
        std::vector<uint8_t> padding; // the bytes up to the next chunk
    };

    // Reads `file`. Throws xsflib::FormatError for another magic word or version, a chunk past the file's end, or a
    // file that doesn't end with its end chunk. The size of a file whose header gives 0 there is the size of `file`.
    static OldestFile Parse(std::span<const uint8_t> file);

    std::vector<uint8_t> Write() const;

    // The size of the file that Write gives.
    std::size_t Size() const;

    std::vector<uint8_t> header;
    std::vector<Chunk> chunks;
};

// The size of the file of version 0x402 at the start of `bytes`: the size that its header gives, or, where the header
// gives 0, the end of its end chunk. Nothing for another file, a size past the end of `bytes`, or chunks that run past
// it.
std::optional<std::size_t> OldestFileSize(std::span<const uint8_t> bytes);

// The song or wave bank of `file`, of version 0x402, laid out as a file of version 0x415 that the driver of version
// 0x415 reads as Luminous Arc's driver reads `file`. Song::Parse and WaveBank::Parse read the result. A song's 16 bytes
// of settings stay as they are at 0x20 of the new settings. Throws as OldestFile::Parse does, and for a table that
// names an entry past its chunk.
std::vector<uint8_t> Upgraded(std::span<const uint8_t> file);

// `bank` as a wave bank of version 0x402, the reverse of Upgraded: Luminous Arc's driver reads the result as the driver
// of version 0x415 reads bank.Write(). The bytes of version 0x415 that version 0x402 has no place for are left out: the
// 16 bytes after a program's LFOs, and the bytes of a wave's entry and of a split that the list above doesn't name.
//
// `like` is a wave bank of version 0x402, or empty. When its chunks, wave slots, programs, LFOs and splits and its
// numbers of programs and key groups match `bank`'s, the result is `like` with `bank`'s values written at their places.
// Every other byte of `like` stays. The bank that Upgraded gives for `like` then writes `like` back byte for byte.
// Otherwise the tables take the layout that Upgraded gives them, with entries of 64 bytes and zeros in the bytes that
// the driver doesn't read, such as a program's 16 entries of 8 bytes. The header is then the header of `like`. Without
// `like`, it is the header of `bank` with the version 0x402, zeros at 0x40 to 0x45 and 0xFF after the counts, as in
// Luminous Arc's banks. The counts of the waves, the programs and the key groups follow `bank`. The size follows the
// result where the header gives one. A header that gives 0 keeps it.
//
// Throws std::invalid_argument for what version 0x402 can't hold: more than 255 wave slots, programs or key groups, a
// program with more than 255 splits or LFOs, a split's wave past 255 and a sample rate past 65,535 Hz. A new layout
// also refuses a bank without samples, and a program that would start past the 64 KiB that the 16-bit offsets of its
// table reach.
std::vector<uint8_t> OldestBank(const WaveBank& bank, std::span<const uint8_t> like = {});

} // namespace ntdlib::dse
