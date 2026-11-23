// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/code.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/ntr/rom.h"

namespace ntdlib::ntr
{

namespace
{

constexpr std::size_t kFooterBytes = 8;
constexpr uint32_t kOverlayCompressed = 1u << 24;
constexpr uint32_t kCompressedSizeMask = 0xFFFFFF;

// The header's fields of the ARM9 overlay table's offset and size, a table entry's size, and the entry's flags.
constexpr std::size_t kArm9OverlayTableField = 0x50;
constexpr std::size_t kOverlayEntryBytes = 32;
constexpr std::size_t kOverlayFlagsField = 28;

// The module parameters' last two words, where they are in the parameters, and the parameters' size.
constexpr uint8_t kModuleParamsMagic[] = {0x21, 0x06, 0xC0, 0xDE, 0xDE, 0xC0, 0x06, 0x21};
constexpr std::size_t kMagicInParams = 0x1C;
constexpr std::size_t kParamsBytes = 0x24;

// The entry code's load of the compressed part's end (ldr r0, [rn, #0x14]), the start-up decompressor's first two
// instructions (cmp r0, #0 and beq), and its return (bx lr). The search's reaches, in bytes.
constexpr uint32_t kLoadCompressedEndMask = 0xFFF0FFFF;
constexpr uint32_t kLoadCompressedEnd = 0xE5900014;
constexpr uint32_t kCompareR0WithZero = 0xE3500000;
constexpr uint32_t kBeqMask = 0xFF000000;
constexpr uint32_t kBeq = 0x0A000000;
constexpr uint32_t kBlMask = 0xFF000000;
constexpr uint32_t kBl = 0xEB000000;
constexpr uint32_t kBxLr = 0xE12FFF1E;
constexpr uint32_t kEntryReach = 0x400;
constexpr uint32_t kRoutineReach = 0x200;

[[noreturn]] void Damaged()
{
    throw xsflib::FormatError("the compressed binary is damaged");
}

// The target of the branch with link or without at `address`: an ARM branch's 24-bit offset in words, from the
// instruction's address plus 8.
uint32_t BranchTarget(uint32_t address, uint32_t instruction)
{
    const uint32_t offset = (instruction & 0xFFFFFF) << 8;
    const int32_t words = static_cast<int32_t>(offset) >> 8;

    return address + 8 + static_cast<uint32_t>(words) * 4;
}

// The word of `code` at `address`, or nothing outside it.
std::optional<uint32_t> WordAt(const LoadedCode& code, uint32_t address)
{
    if (code.bytes.size() < 4 || address < code.address || address % 4 != 0 ||
        address - code.address > code.bytes.size() - 4)
    {
        return std::nullopt;
    }

    return xsflib::LoadLe32(&code.bytes[address - code.address]);
}

// The start-up decompressor at `routine`, when it has the decompressor's start and a return within reach.
std::optional<StartupDecompressor> DecompressorAt(const LoadedCode& code, uint32_t call, uint32_t routine)
{
    const std::optional<uint32_t> compare = WordAt(code, routine);
    const std::optional<uint32_t> branch = WordAt(code, routine + 4);
    if (compare != kCompareR0WithZero || !branch || (*branch & kBeqMask) != kBeq)
    {
        return std::nullopt;
    }

    const uint32_t exit = BranchTarget(routine + 4, *branch);
    if (exit <= routine + 4 || exit - routine > kRoutineReach || WordAt(code, exit) != kBxLr)
    {
        return std::nullopt;
    }

    return StartupDecompressor{.call = call, .routine = routine, .body = routine + 8, .exit = exit};
}

} // namespace

std::vector<uint8_t> UncompressBackward(std::span<const uint8_t> data)
{
    const std::size_t end = data.size();
    if (end < kFooterBytes)
    {
        Damaged();
    }

    const uint32_t lengths = xsflib::LoadLe32(&data[end - kFooterBytes]);
    const std::size_t compressed = lengths & kCompressedSizeMask;
    const std::size_t footer = lengths >> 24;
    const std::size_t growth = xsflib::LoadLe32(&data[end - 4]);
    if (footer < kFooterBytes || footer > compressed || compressed > end || end > kMaxUncompressedBytes ||
        growth > kMaxUncompressedBytes - end)
    {
        Damaged();
    }

    std::vector<uint8_t> out(data.begin(), data.end());
    out.resize(end + growth, 0);
    const std::size_t stop = end - compressed;
    std::size_t source = end - footer;
    std::size_t target = out.size();
    while (source > stop)
    {
        const uint8_t flags = data[--source];
        for (int bit = 7; bit >= 0 && source > stop; bit--)
        {
            if ((flags >> bit & 1) == 0)
            {
                if (target == 0)
                {
                    Damaged();
                }
                out[--target] = data[--source];
                continue;
            }

            if (source < stop + 2)
            {
                Damaged();
            }
            source -= 2;
            const uint16_t value = static_cast<uint16_t>(data[source] | data[source + 1] << 8);
            const std::size_t count = (value >> 12) + 3;
            const std::size_t distance = (value & 0xFFF) + 3;
            if (count > target || target - 1 + distance >= out.size())
            {
                Damaged();
            }
            for (std::size_t i = 0; i < count; i++)
            {
                target--;
                out[target] = out[target + distance];
            }
        }
    }

    return out;
}

LoadedCode Arm9Code(const Rom& rom)
{
    const std::span<const uint8_t> binary = rom.Arm9();
    LoadedCode code{.address = rom.Header().arm9.ram_address, .bytes = {binary.begin(), binary.end()}};

    const std::optional<ModuleParams> params = ReadModuleParams(binary);
    if (!params || params->compressed_end == 0)
    {
        return code;
    }
    if (params->compressed_end < code.address || params->compressed_end - code.address > binary.size())
    {
        Damaged();
    }

    const std::size_t end = params->compressed_end - code.address;
    code.bytes = UncompressBackward(binary.first(end));
    code.bytes.insert(code.bytes.end(), binary.begin() + static_cast<std::ptrdiff_t>(end), binary.end());

    return code;
}

std::vector<LoadedCode> Arm9CodeAndOverlays(const Rom& rom)
{
    std::vector<LoadedCode> code = {Arm9Code(rom)};
    for (const Overlay& overlay : rom.Arm9Overlays())
    {
        if (overlay.file_id >= rom.Files().size())
        {
            continue;
        }

        LoadedCode loaded{.address = overlay.ram_address, .overlay = overlay.id};
        try
        {
            loaded.bytes = OverlayBytes(rom.FileData(overlay.file_id), overlay);
        }
        catch (const xsflib::FormatError&)
        {
            continue;
        }
        code.push_back(std::move(loaded));
    }

    return code;
}

bool OverlayCompressed(const Overlay& overlay)
{
    return (overlay.flags & kOverlayCompressed) != 0;
}

std::vector<uint8_t> OverlayBytes(std::span<const uint8_t> file, const Overlay& overlay)
{
    if (!OverlayCompressed(overlay))
    {
        return {file.begin(), file.end()};
    }

    const std::size_t size = overlay.flags & kCompressedSizeMask;
    return UncompressBackward(size == 0 || size > file.size() ? file : file.first(size));
}

void ClearArm9OverlayFlags(std::span<uint8_t> image, std::size_t index)
{
    if (image.size() < kArm9OverlayTableField + 8)
    {
        throw std::out_of_range("the image has no header");
    }

    const uint32_t table = xsflib::LoadLe32(&image[kArm9OverlayTableField]);
    const uint32_t table_size = xsflib::LoadLe32(&image[kArm9OverlayTableField + 4]);
    const std::size_t entry = std::size_t{table} + kOverlayEntryBytes * index;
    if (kOverlayEntryBytes * (index + 1) > table_size || entry + kOverlayEntryBytes > image.size())
    {
        throw std::out_of_range("the ARM9 overlay table has no entry " + std::to_string(index));
    }

    xsflib::StoreLe32(&image[entry + kOverlayFlagsField], 0);
}

std::optional<ModuleParams> ReadModuleParams(std::span<const uint8_t> arm9)
{
    const auto magic = std::ranges::search(arm9, kModuleParamsMagic);
    const std::size_t at = static_cast<std::size_t>(magic.begin() - arm9.begin());
    if (magic.empty() || at < kMagicInParams)
    {
        return std::nullopt;
    }

    const std::size_t offset = at - kMagicInParams;
    const auto word = [&](std::size_t index)
    {
        return xsflib::LoadLe32(&arm9[offset + 4 * index]);
    };

    return ModuleParams{.offset = offset,
                        .autoload_list = word(0),
                        .autoload_list_end = word(1),
                        .autoload_start = word(2),
                        .static_bss_start = word(3),
                        .static_bss_end = word(4),
                        .compressed_end = word(5),
                        .sdk_version = word(6)};
}

void WriteModuleParams(std::span<uint8_t> arm9, const ModuleParams& params)
{
    if (params.offset > arm9.size() || arm9.size() - params.offset < kParamsBytes)
    {
        throw std::out_of_range("the module parameters lie past the binary's end");
    }

    const uint32_t words[] = {params.autoload_list,    params.autoload_list_end, params.autoload_start,
                              params.static_bss_start, params.static_bss_end,    params.compressed_end,
                              params.sdk_version};
    for (std::size_t i = 0; i < std::size(words); i++)
    {
        xsflib::StoreLe32(&arm9[params.offset + 4 * i], words[i]);
    }
}

std::vector<uint8_t> UncompressedArm9(const Rom& rom)
{
    std::vector<uint8_t> binary = Arm9Code(rom).bytes;
    if (std::optional<ModuleParams> params = ReadModuleParams(binary); params && params->compressed_end != 0)
    {
        params->compressed_end = 0;
        WriteModuleParams(binary, *params);
    }

    return binary;
}

std::optional<StartupDecompressor> FindStartupDecompressor(const LoadedCode& code, uint32_t entry)
{
    for (uint32_t at = entry; at - entry < kEntryReach; at += 4)
    {
        const std::optional<uint32_t> load = WordAt(code, at);
        const std::optional<uint32_t> call = WordAt(code, at + 4);
        if (!load || !call)
        {
            return std::nullopt;
        }
        if ((*load & kLoadCompressedEndMask) == kLoadCompressedEnd && (*call & kBlMask) == kBl)
        {
            return DecompressorAt(code, at + 4, BranchTarget(at + 4, *call));
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::ntr
