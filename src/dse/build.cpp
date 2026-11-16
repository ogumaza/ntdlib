// SPDX-License-Identifier: MIT

#include "ntdlib/dse/build.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/rom.h"

namespace ntdlib::dse
{

namespace
{

// The table: one handler for each code from 0x90 to 0xFF.
constexpr std::size_t kHandlers = 0x100 - 0x90;
constexpr std::size_t kTrackEnd = 0x98 - 0x90;
constexpr std::size_t kLastCode = kHandlers - 1;
constexpr std::size_t kFewestUnsupported = 25;
constexpr std::size_t kMostUnsupported = 80;
constexpr std::size_t kFewestHandlers = 30;

// The limits of a fingerprint's walk.
constexpr std::size_t kMaxFunctionWords = 4096;
constexpr std::size_t kMaxFunctions = 4096;
constexpr uint32_t kUnknownFunction = 0xFFFFFFFF;
constexpr uint32_t kFunctionStart = 0xF0F0F0F0;
constexpr uint32_t kPointer = 0x02AAAAAA;

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325;
constexpr uint64_t kFnvPrime = 0x100000001B3;

// A build that the model covers: the fingerprint of the code that its table reaches, and the hash of its sequencer,
// channels and voices. They lie from `core_before` bytes before the lowest handler to `core_after` bytes after it.
struct KnownBuild
{
    Build build;
    uint64_t fingerprint;
    uint32_t core_before;
    uint32_t core_after;
    uint64_t core_hash;
};

// The earlier build in Explorers of Time's ARM9 binary (North America, NTR-YFTE-USA): the table at 0x020A7FAC, the
// lowest handler at 0x02062F74 and the core from 0x02061800 to 0x02066C00. The later build in Explorers of Sky's (North
// America, NTR-C2SE-USA): the table at 0x020B0B90, the lowest handler at 0x0207191C and the core from 0x02070180 to
// 0x02075828. Explorers of Darkness (NTR-YFYE-USA) holds Time's code at the same addresses.
//
// Two more games run the code of a covered build, as anchors on the games' code show: each of their songs plays the
// same commands on the model for 60 s. Soma Bringer (Japan, NTR-YBSJ-JPN) runs the earlier build: the table at
// 0x020BAC58, the lowest handler at 0x0206F80C and the core from 0x0206C8D0 to 0x0207351C. Its sequencer and voices are
// Time's word for word, apart from the handlers of codes 0x9C and 0x9D. The rest of the stretch differs from Time's:
// the stream player (about 2,400 words), the API of the effects, the offsets in the stream's struct and the inlined
// tick. Professor Layton and the Unwound Future (North America, NTR-C3JE-USA) runs the later build in its overlay 1:
// the table at 0x0207FF34, the lowest handler at 0x0207BBA8 and the core from 0x0207A40C to 0x0207FDE0. Its handlers
// are Sky's. Its core holds 269 words more than Sky's, near its end.
constexpr KnownBuild kKnownBuilds[] = {
    {Build::kEarlier, 0xFA24810A485132CC, 0x02062F74 - 0x02061800, 0x02066C00 - 0x02062F74, 0x6245F20133110B42},
    {Build::kLater, 0x2917D1BCB634EBA2, 0x0207191C - 0x02070180, 0x02075828 - 0x0207191C, 0x9BF34F55901478B4},
    {Build::kEarlier, 0xE3C30C613876FDF9, 0x0206F80C - 0x0206C8D0, 0x0207351C - 0x0206F80C, 0xC3B5594165865567},
    {Build::kLater, 0x2917D1BCB634EBA2, 0x0207BBA8 - 0x0207A40C, 0x0207FDE0 - 0x0207BBA8, 0xFD56950D2BCAB7ED},
};

uint32_t WordAt(const CodeBlock& block, uint32_t address)
{
    return xsflib::LoadLe32(&block.bytes[address - block.address]);
}

bool Holds(const CodeBlock& block, uint32_t address)
{
    return address % 4 == 0 && address >= block.address && address - block.address < block.bytes.size() / 4 * 4;
}

// B or BL, and where it goes.
bool IsBranch(uint32_t word)
{
    return (word & 0x0E000000) == 0x0A000000 && word >> 28 != 0xF;
}

bool IsLink(uint32_t word)
{
    return (word & 0x01000000) != 0;
}

uint32_t BranchTarget(uint32_t address, uint32_t word)
{
    const int32_t offset = static_cast<int32_t>(word << 8) >> 6;
    return address + 8 + static_cast<uint32_t>(offset);
}

// LDR Rd, [PC, #offset]: a load from a literal pool. Gives the pool word's address.
std::optional<uint32_t> LiteralAddress(uint32_t address, uint32_t word)
{
    if ((word & 0x0F7F0000) != 0x051F0000)
    {
        return std::nullopt;
    }

    const uint32_t offset = word & 0xFFF;
    return (word & 0x00800000) != 0 ? address + 8 + offset : address + 8 - offset;
}

// An instruction that always leaves the function: B, BX, LDM or LDR into PC, or a data-processing instruction that
// writes PC.
bool IsExit(uint32_t word)
{
    if (word >> 28 != 0xE)
    {
        return false;
    }

    const bool branch = (word & 0x0F000000) == 0x0A000000;
    const bool exchange = (word & 0x0FFFFFF0) == 0x012FFF10;
    const bool load_multiple = (word & 0x0E108000) == 0x08108000;
    const bool load = (word & 0x0C50F000) == 0x0410F000;
    const uint32_t opcode = word >> 21 & 0xF;
    const bool data_processing =
        (word & 0x0C00F000) == 0x0000F000 && (opcode < 8 || opcode > 11) && (word & 0x02000090) != 0x00000090;

    return branch || exchange || load_multiple || load || data_processing;
}

// Adds `value` to an FNV-1a hash, a byte at a time from the lowest.
void MixInto(uint64_t& hash, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        hash ^= value >> (8 * i) & 0xFF;
        hash *= kFnvPrime;
    }
}

// A word with what moves with the code left out: a branch's offset, or a pointer into main memory, the shared memory
// or the ARM9's fast memory.
uint32_t Masked(uint32_t word)
{
    if ((word & 0x0E000000) == 0x0A000000)
    {
        return word & 0xFF000000;
    }
    if ((word >= 0x01FF8000 && word < 0x02400000) || (word >= 0x027C0000 && word < 0x02800000))
    {
        return kPointer;
    }

    return word;
}

// The fingerprint of the code that a table of handlers reaches. The functions are numbered in the order that the walk
// finds them, from the table's codes in order.
class Fingerprint
{
public:
    explicit Fingerprint(std::span<const CodeBlock> code) : code_(code)
    {
    }

    DriverCode Of(std::size_t block, uint32_t table)
    {
        for (std::size_t i = 0; i < kHandlers; i++)
        {
            Mix(Id(block, WordAt(code_[block], table + 4 * static_cast<uint32_t>(i))));
        }
        for (std::size_t i = 0; i < functions_.size(); i++)
        {
            Walk(functions_[i].first, functions_[i].second);
        }

        uint32_t lowest = 0xFFFFFFFF;
        for (std::size_t i = 0; i < kHandlers; i++)
        {
            lowest = std::min(lowest, WordAt(code_[block], table + 4 * static_cast<uint32_t>(i)));
        }

        return {.table = table,
                .block = block,
                .lowest_handler = lowest,
                .fingerprint = hash_,
                .functions = functions_.size(),
                .words = words_};
    }

private:
    void Mix(uint32_t value)
    {
        MixInto(hash_, value);
    }

    // The number of the function at `address`, from code in block `from`: in that block, or else in the first.
    uint32_t Id(std::size_t from, uint32_t address)
    {
        std::size_t block = from;
        if (!Holds(code_[block], address))
        {
            block = 0;
            if (!Holds(code_[block], address))
            {
                return kUnknownFunction;
            }
        }

        const auto [found, added] = ids_.try_emplace({block, address}, static_cast<uint32_t>(functions_.size()));
        if (added)
        {
            if (functions_.size() == kMaxFunctions)
            {
                ids_.erase(found);
                return kUnknownFunction;
            }
            functions_.emplace_back(block, address);
        }

        return found->second;
    }

    // The function's words, up to the first exit that no forward branch of the function passes.
    std::size_t Length(const CodeBlock& block, uint32_t entry) const
    {
        uint32_t farthest = entry;
        std::size_t words = 0;
        for (uint32_t address = entry; words < kMaxFunctionWords && Holds(block, address); address += 4)
        {
            const uint32_t word = WordAt(block, address);
            words++;
            if (IsBranch(word) && !IsLink(word))
            {
                const uint32_t target = BranchTarget(address, word);
                if (target > address && target - entry < 4 * kMaxFunctionWords)
                {
                    farthest = std::max(farthest, target);
                }
            }
            if (address >= farthest && IsExit(word))
            {
                break;
            }
        }

        return words;
    }

    void Walk(std::size_t index, uint32_t entry)
    {
        const CodeBlock& block = code_[index];
        const std::size_t words = Length(block, entry);
        const uint32_t end = entry + 4 * static_cast<uint32_t>(words);
        Mix(kFunctionStart);
        Mix(static_cast<uint32_t>(words));
        words_ += words;

        for (uint32_t address = entry; address < end; address += 4)
        {
            const uint32_t word = WordAt(block, address);
            Mix(Masked(word));
            if (IsBranch(word))
            {
                const uint32_t target = BranchTarget(address, word);
                if (IsLink(word) || target < entry || target >= end)
                {
                    Mix(Id(index, target));
                }
            }
            if (const std::optional<uint32_t> literal = LiteralAddress(address, word))
            {
                Mix(Holds(block, *literal) ? Masked(WordAt(block, *literal)) : kUnknownFunction);
            }
        }
    }

    std::span<const CodeBlock> code_;
    std::vector<std::pair<std::size_t, uint32_t>> functions_;
    std::map<std::pair<std::size_t, uint32_t>, uint32_t> ids_;
    uint64_t hash_ = kFnvOffset;
    std::size_t words_ = 0;
};

// The hash of the words of `block` from `start` to `end`, with what moves with the code left out. Nothing when the
// block doesn't hold them all.
std::optional<uint64_t> CoreHash(const CodeBlock& block, uint32_t start, uint32_t end)
{
    if (start >= end || !Holds(block, start) || !Holds(block, end - 4))
    {
        return std::nullopt;
    }

    uint64_t hash = kFnvOffset;
    for (uint32_t address = start; address < end; address += 4)
    {
        MixInto(hash, Masked(WordAt(block, address)));
    }

    return hash;
}

// Whether the 112 words at `table` have a table's shape. Each handler starts with an instruction that always runs.
bool IsTable(const CodeBlock& block, uint32_t table)
{
    std::map<uint32_t, std::size_t> counts;
    for (std::size_t i = 0; i < kHandlers; i++)
    {
        counts[WordAt(block, table + 4 * static_cast<uint32_t>(i))]++;
    }
    const auto most =
        std::ranges::max_element(counts, [](const auto& a, const auto& b) { return a.second < b.second; });
    const uint32_t unsupported = most->first;
    const auto starts_code = [&block](const auto& handler)
    {
        return WordAt(block, handler.first) >> 28 == 0xE;
    };

    return std::ranges::all_of(counts, starts_code) && most->second >= kFewestUnsupported &&
           most->second <= kMostUnsupported && counts.size() >= kFewestHandlers &&
           WordAt(block, table + 4 * kTrackEnd) != unsupported && WordAt(block, table + 4 * kLastCode) == unsupported;
}

} // namespace

std::vector<DriverCode> FindDriverCode(std::span<const CodeBlock> code)
{
    std::vector<DriverCode> found;
    for (std::size_t b = 0; b < code.size(); b++)
    {
        const CodeBlock& block = code[b];
        std::size_t run = 0;
        for (uint32_t address = block.address; Holds(block, address); address += 4)
        {
            const uint32_t word = WordAt(block, address);
            run = Holds(block, word) ? run + 1 : 0;
            if (run < kHandlers)
            {
                continue;
            }

            const uint32_t table = address - 4 * static_cast<uint32_t>(kHandlers - 1);
            if (IsTable(block, table))
            {
                found.push_back(Fingerprint(code).Of(b, table));
            }
        }
    }

    return found;
}

std::optional<Build> BuildOf(std::span<const CodeBlock> code)
{
    std::optional<Build> build;
    for (const DriverCode& driver : FindDriverCode(code))
    {
        for (const KnownBuild& known : kKnownBuilds)
        {
            if (driver.fingerprint != known.fingerprint ||
                CoreHash(code[driver.block], driver.lowest_handler - known.core_before,
                         driver.lowest_handler + known.core_after) != known.core_hash)
            {
                continue;
            }
            if (build && *build != known.build)
            {
                return std::nullopt;
            }
            build = known.build;
        }
    }

    return build;
}

std::optional<Build> BuildOf(const ntr::Rom& rom)
{
    std::vector<ntr::LoadedCode> loaded;
    try
    {
        loaded = ntr::Arm9CodeAndOverlays(rom);
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }

    std::vector<CodeBlock> code;
    for (const ntr::LoadedCode& part : loaded)
    {
        code.push_back({part.address, part.bytes});
    }

    return BuildOf(code);
}

} // namespace ntdlib::dse
