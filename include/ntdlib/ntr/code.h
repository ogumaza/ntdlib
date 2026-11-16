// SPDX-License-Identifier: MIT

// The ARM9 binary of a cartridge image and its overlays as the console runs them, decompressed.
//
// The SDK compresses a binary from its end backwards, in place. The last 8 bytes of the compressed part are a footer:
// the length of the compressed part (24 bits) and the footer's length (8 bits), then how many bytes longer the
// decompressed part is. The bytes before the compressed part stay as they are. The stream runs from below the footer
// down to the compressed part's start. Each flag byte gives 8 steps, from its top bit: a 0 copies one byte, and a 1
// reads two bytes as a 16-bit value. Its top 4 bits plus 3 give the count of bytes to copy, and its low 12 bits plus 3
// how far above the output's current end they lie. The output grows downwards from the end of the decompressed part.
//
// The ARM9 binary's module parameters are nine words: the start and the end of the autoload list, the start of the
// autoloaded blocks, the start and the end of the static part's BSS, the address where the compressed part ends (0 for
// a binary that isn't compressed), the SDK's version, then 0xDEC00621 and 0x2106C0DE. An overlay's flags mark a
// compressed overlay (bit 24) and give the compressed part's length (bits 0 to 23).
//
// The binary's entry code calls the start-up decompressor with the word at 0x14 of the module parameters, the
// compressed part's end. The decompressor returns at once for 0: its first two instructions are cmp r0, #0 and a
// branch to its return.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/ntr/rom.h"

namespace ntdlib::ntr
{

// Decompresses `data`, a binary whose compressed part ends at its end. Throws xsflib::FormatError when the footer or
// the stream reaches outside the data.
std::vector<uint8_t> UncompressBackward(std::span<const uint8_t> data);

// A binary as the console loads it: its address, its bytes decompressed, and the overlay's ID for an overlay.
struct LoadedCode
{
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
    std::optional<uint32_t> overlay = {};
};

// The ARM9 binary, decompressed when its module parameters mark it compressed. The bytes after the compressed part
// follow the decompressed part. Throws xsflib::FormatError when the compressed part doesn't decompress.
LoadedCode Arm9Code(const Rom& rom);

// The ARM9 binary (Arm9Code), then each ARM9 overlay in the order of its table, each decompressed (OverlayBytes). An
// overlay that doesn't decompress is left out.
std::vector<LoadedCode> Arm9CodeAndOverlays(const Rom& rom);

// Whether the flags of `overlay` mark it compressed.
bool OverlayCompressed(const Overlay& overlay);

// The bytes of `overlay` as the console loads them, from `file`, the overlay's file. A compressed overlay decompresses
// from the compressed size in its flags. A size of 0, or one past the file's end, takes the whole file. Throws
// xsflib::FormatError when a compressed overlay doesn't decompress.
std::vector<uint8_t> OverlayBytes(std::span<const uint8_t> file, const Overlay& overlay);

// Clears the flags of entry `index` of the ARM9 overlay table in `image`, a cartridge image. The console then loads the
// overlay's file as it is. Throws std::out_of_range when the table has no such entry.
void ClearArm9OverlayFlags(std::span<uint8_t> image, std::size_t index);

// The ARM9 binary's module parameters, and their offset in the binary.
struct ModuleParams
{
    friend bool operator==(const ModuleParams&, const ModuleParams&) = default;

    std::size_t offset = 0;
    uint32_t autoload_list = 0;
    uint32_t autoload_list_end = 0;
    uint32_t autoload_start = 0;
    uint32_t static_bss_start = 0;
    uint32_t static_bss_end = 0;
    uint32_t compressed_end = 0; // 0 for a binary that isn't compressed
    uint32_t sdk_version = 0;
};

// The module parameters of `arm9`, an ARM9 binary as the image stores it or decompressed. Nothing when the binary has
// no module parameters.
std::optional<ModuleParams> ReadModuleParams(std::span<const uint8_t> arm9);

// Writes the first seven words of `params` at its offset in `arm9`. Throws std::out_of_range when the nine words don't
// fit there.
void WriteModuleParams(std::span<uint8_t> arm9, const ModuleParams& params);

// The ARM9 binary decompressed (Arm9Code), with the compressed part's end in its module parameters set to 0. The
// console runs it as it is: the start-up decompressor returns at once.
std::vector<uint8_t> UncompressedArm9(const Rom& rom);

// The start-up decompressor and its call in the entry code. Its body lies from `body` up to its return.
struct StartupDecompressor
{
    friend bool operator==(const StartupDecompressor&, const StartupDecompressor&) = default;

    uint32_t call = 0;    // the bl in the entry code
    uint32_t routine = 0; // cmp r0, #0
    uint32_t body = 0;    // the instruction after the branch to the return
    uint32_t exit = 0;    // the return: bx lr
};

// The start-up decompressor of `code`, the ARM9 binary at its address: the routine that the entry code calls with the
// word at 0x14 of the module parameters. The call is a ldr r0, [rn, #0x14] and a bl within 0x400 bytes of `entry`. The
// routine starts with cmp r0, #0 and a beq to its return, a bx lr within 0x200 bytes. Nothing when the code doesn't
// have that shape.
std::optional<StartupDecompressor> FindStartupDecompressor(const LoadedCode& code, uint32_t entry);

} // namespace ntdlib::ntr
