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

#include "ntdlib/arm/branch.h"
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

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325;
constexpr uint64_t kFnvPrime = 0x100000001B3;

// A build that the model covers: the fingerprint of the code that its table reaches, and the hash of its sequencer,
// channels and voices. They run from `core_before` bytes before the lowest handler to `core_after` bytes after it.
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
// Two more games run the code of a covered build. Soma Bringer (Japan, NTR-YBSJ-JPN) runs the earlier build: the table
// at 0x020BAC58, the lowest handler at 0x0206F80C and the core from 0x0206C8D0 to 0x0207351C. Its sequencer and voices
// are Time's word for word, apart from the handlers of codes 0x9C and 0x9D. The rest of the stretch differs from
// Time's: the stream player (about 2,400 words), the API of the effects, the offsets in the stream's struct and the
// inlined tick. Professor Layton and the Unwound Future (North America, NTR-C3JE-USA) runs the later build in its
// overlay 1: the table at 0x0207FF34, the lowest handler at 0x0207BBA8 and the core from 0x0207A40C to 0x0207FDE0. Its
// handlers are Sky's. Its core holds 269 words more than Sky's, near its end.
//
// Luminous Arc 3: Eyes (Japan, NTR-BLUJ-JPN) runs the later build with code that differs more: the table at 0x0211A104
// and the lowest handler at 0x0205EEB8. Its handlers of codes 0x98, 0x99 and 0xF8 differ from Sky's, and its linker put
// the driver's functions in another order. Its core is the stretch from the driver's thread (0x0205E3B0) to 0x02062FC4.
// The stretch contains Sky's functions from 0x02070E0C to 0x020756E8 in their order, with 206 words more. Inazuma
// Eleven 2: Fire (Japan, revision 1, NTR-BEEJ-JPN-rev1) has the same fingerprint, and a core of the same order with 22
// words more than Luminous Arc 3's: the table at 0x020A9098, the lowest handler at 0x0208F2AC and the core from
// 0x0208E7A4 to 0x02093410. Professor Layton and the Last Specter (North America, NTR-BLFE-USA) runs the code of the
// same order in its overlay 0, with another fingerprint: the table at 0x020580F4, the lowest handler at 0x02048134 and
// the core from 0x020475B4 to 0x0204C214. Its handlers differ from Sky's in the same codes, and in 33 more by immediate
// values only, such as offsets of the driver's fields.
//
// Inazuma Eleven (Japan, revision 1, NTR-YEEJ-JPN-rev1) runs the later build too: the table at 0x020B3F84, the lowest
// handler at 0x02028334 and the core from 0x02026BDC to 0x0202C420. The core starts with Sky's function at 0x02070180
// and ends with Sky's at 0x020756E8. Its handlers of codes 0x98, 0x99, 0xAF and 0xD4 differ from Sky's. Fushigi no
// Dungeon: Fuurai no Shiren 4 (Japan, NTR-B4FJ-JPN) has the same handlers, with another fingerprint: the table at
// 0x020D6DE8, the lowest handler at 0x020837C0 and the core from 0x02082054 to 0x0208737C. The core starts with Sky's
// function at 0x02070180 and ends with Sky's at 0x020755B8. The linker put Sky's five loader functions from 0x02075600
// on elsewhere.
//
// Professor Layton and the Diabolical Box (North America, NTR-YLTE-USA) runs the earlier build with code that differs
// more: the table at 0x02043790 and the lowest handler at 0x02008894. Its handlers of codes 0xAF and 0xB4 differ from
// Time's only in the offsets of fields. Its handlers of codes 0x9C, 0x9D and 0xCB are Sky's: the end of a repeat
// (0x9D, 0x020089C8) restores the octave. Its sequence step (0x020084A8) has the delay and the separate fade step of
// Luminous Arc 2's. The earlier build still plays the game's songs as the game does. No song of the game uses a
// repeat or code 0xCB, and a song can't set the delay. The linker put the driver's functions in another order. The
// core is the stretch from the tick's timer (0x020080E8, Time's 0x02062898) to 0x0200B840 (Time's 0x02066BA4).
//
// Two games run builds between the earlier and the later build. The model's middle and late middle builds follow their
// code. Luminous Arc 2 (North America, NTR-YL2E-USA) runs the middle build: the table at 0x0210AB5C, the lowest handler
// at 0x0207477C and the core from the driver's thread (0x0207400C) to 0x020778A8. Fushigi no Dungeon: Fuurai no Shiren
// DS 2 (Japan, NTR-YWFJ-JPN) runs the late middle build: the table at 0x02169F00, the lowest handler at 0x0212A180 and
// the core from the driver's thread (0x02129968) to 0x0212D83C. Both cores hold the functions in the order of Luminous
// Arc 3's.
//
// Inazuma Eleven 3: Sekai e no Chousen!! Bomber (Japan, NTR-BEZJ-JPN) runs the later build with code that a newer
// compiler built: the table at 0x0209357C, the lowest handler at 0x0207ACF4 and the core from the driver's thread
// (0x0207A410) to 0x0207E7D0. Its handlers of 24 codes differ from Sky's. Inazuma Eleven 3: Sekai e no Chousen!! The
// Ogre (Japan, NTR-BOEJ-JPN) holds the same code: the table at 0x02091794, the lowest handler at 0x020785C0 and the
// core from 0x02077CDC to 0x0207C09C.
//
// Fushigi no Dungeon: Fuurai no Shiren 5 (Japan, NTR-B5FJ-JPN) runs the later build in Thumb code: the table at
// 0x020F49C4, the lowest handler at 0x020956B8 and the core from the driver's thread (0x02095004) to 0x0209802A.
//
// Ni no Kuni: Shikkoku no Madoushi (Japan, NTR-B2KJ-JPN) holds the handlers and the core of Inazuma Eleven 3, with one
// function more (0x02058C58, 15 words) that reloads the bank and the program of each track: the table at 0x020696F0,
// the lowest handler at 0x0205700C and the core from the driver's thread (0x02056728) to 0x0205AB24. No code in the
// game loads a song. The game's code therefore never plays the six songs that its files hold, and no song reaches the
// extra function. The entry gives the songs the later build. Inazuma Eleven 3 runs that build with the same code.
//
// Luminous Arc (North America, NTR-ANIE-USA) runs the oldest build: the table at 0x021123B4, the lowest handler at
// 0x020C2FE8 and the core from the tick's timer (0x020C2904, Time's 0x02062898) to 0x020C5850. The oldest build reads
// files of version 0x402.
constexpr KnownBuild kKnownBuilds[] = {
    {Build::kEarlier, 0xFA24810A485132CC, 0x02062F74 - 0x02061800, 0x02066C00 - 0x02062F74, 0x6245F20133110B42},
    {Build::kLater, 0x2917D1BCB634EBA2, 0x0207191C - 0x02070180, 0x02075828 - 0x0207191C, 0x9BF34F55901478B4},
    {Build::kEarlier, 0xE3C30C613876FDF9, 0x0206F80C - 0x0206C8D0, 0x0207351C - 0x0206F80C, 0xC3B5594165865567},
    {Build::kLater, 0x2917D1BCB634EBA2, 0x0207BBA8 - 0x0207A40C, 0x0207FDE0 - 0x0207BBA8, 0xFD56950D2BCAB7ED},
    {Build::kLater, 0x131E36E765E35C87, 0x0205EEB8 - 0x0205E3B0, 0x02062FC4 - 0x0205EEB8, 0xF7A3F8DD547E1E3A},
    {Build::kLater, 0x131E36E765E35C87, 0x0208F2AC - 0x0208E7A4, 0x02093410 - 0x0208F2AC, 0xE38521D4A7B96453},
    {Build::kLater, 0x2EE7144E2BF539F0, 0x02048134 - 0x020475B4, 0x0204C214 - 0x02048134, 0xA59250901B6FBDCE},
    {Build::kLater, 0x099C0B4EA18FA46D, 0x02028334 - 0x02026BDC, 0x0202C420 - 0x02028334, 0x1B3F8C50C9701FCD},
    {Build::kLater, 0xAF3610AC736FC153, 0x020837C0 - 0x02082054, 0x0208737C - 0x020837C0, 0xB379E7A82F2288D0},
    {Build::kEarlier, 0x33D2A071A1989F05, 0x02008894 - 0x020080E8, 0x0200B840 - 0x02008894, 0x839509EE7F73C48C},
    {Build::kMiddle, 0x2458EE396DC0C404, 0x0207477C - 0x0207400C, 0x020778A8 - 0x0207477C, 0xB2D2624F5DD2221C},
    {Build::kLateMiddle, 0xE8E13658C62B994A, 0x0212A180 - 0x02129968, 0x0212D83C - 0x0212A180, 0x3FFE33AA58F43D33},
    {Build::kLater, 0xAA764B7A1EFA29E3, 0x0207ACF4 - 0x0207A410, 0x0207E7D0 - 0x0207ACF4, 0x7245A0085BCBF2AF},
    {Build::kLater, 0x87EDF33C59A3E6FB, 0x020956B8 - 0x02095004, 0x0209802A - 0x020956B8, 0xA5F6935219DFBD23},
    {Build::kLater, 0xAA764B7A1EFA29E3, 0x0205700C - 0x02056728, 0x0205AB24 - 0x0205700C, 0x3362B07803346A7A},
    {Build::kOldest, 0x86B637E1A3E33CC9, 0x020C2FE8 - 0x020C2904, 0x020C5850 - 0x020C2FE8, 0xD38A11D030FD0B3B},
};

uint32_t WordAt(const CodeBlock& block, uint32_t address)
{
    return xsflib::LoadLe32(&block.bytes[address - block.address]);
}

bool Holds(const CodeBlock& block, uint32_t address)
{
    return address % 4 == 0 && address >= block.address && address - block.address < block.bytes.size() / 4 * 4;
}

// A Thumb function's address has bit 0 set. Its halfwords start at the address without that bit.
bool HoldsThumb(const CodeBlock& block, uint32_t address)
{
    const uint32_t start = address & ~1u;
    return (address & 1) != 0 && start >= block.address && start - block.address + 2 <= block.bytes.size();
}

bool HoldsHalf(const CodeBlock& block, uint32_t address)
{
    return address % 2 == 0 && address >= block.address && address - block.address + 2 <= block.bytes.size();
}

uint16_t HalfAt(const CodeBlock& block, uint32_t address)
{
    return xsflib::LoadLe16(&block.bytes[address - block.address]);
}

// Thumb's BL and BLX: a pair of halfwords. BLX goes to ARM code on a word.
bool IsThumbCall(uint16_t first, uint16_t second)
{
    return (first & 0xF800) == 0xF000 && (second & 0xE800) == 0xE800;
}

// Where a Thumb call goes: a Thumb function's address with bit 0 set, or an ARM function's.
uint32_t ThumbCallTarget(uint32_t address, uint16_t first, uint16_t second)
{
    const int32_t offset =
        static_cast<int32_t>((uint32_t{first} & 0x7FF) << 21 | (uint32_t{second} & 0x7FF) << 10) >> 9;
    const uint32_t target = address + 4 + static_cast<uint32_t>(offset);
    return (second & 0xF800) == 0xF800 ? target | 1 : target & ~3u;
}

// A Thumb branch's target, for B and B<cond>. Nothing for other halfwords.
std::optional<uint32_t> ThumbBranchTarget(uint32_t address, uint16_t half)
{
    if ((half & 0xF800) == 0xE000)
    {
        return address + 4 + static_cast<uint32_t>(static_cast<int32_t>(uint32_t{half} << 21) >> 20);
    }
    if ((half & 0xF000) == 0xD000 && (half & 0x0F00) < 0x0E00)
    {
        return address + 4 + static_cast<uint32_t>(static_cast<int32_t>(uint32_t{half} << 24) >> 23);
    }

    return std::nullopt;
}

// LDR Rd, [PC, #offset] in Thumb: the pool word's address.
std::optional<uint32_t> ThumbLiteralAddress(uint32_t address, uint16_t half)
{
    if ((half & 0xF800) != 0x4800)
    {
        return std::nullopt;
    }

    return ((address + 4) & ~3u) + 4 * (uint32_t{half} & 0xFF);
}

// A Thumb halfword that always leaves the function: B, POP into PC, BX, or MOV into PC.
bool IsThumbExit(uint16_t half)
{
    return (half & 0xF800) == 0xE000 || (half & 0xFF00) == 0xBD00 || (half & 0xFF87) == 0x4700 ||
           (half & 0xFF87) == 0x4687;
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
    return MaskedMovingWord(word).value_or(word);
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
                .thumb = (lowest & 1) != 0,
                .lowest_handler = lowest & ~1u,
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
        const auto holds = [address](const CodeBlock& code)
        {
            return (address & 1) != 0 ? HoldsThumb(code, address) : Holds(code, address);
        };
        std::size_t block = from;
        if (!holds(code_[block]))
        {
            block = 0;
            if (!holds(code_[block]))
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

    void Walk(std::size_t index, uint32_t entry)
    {
        if ((entry & 1) != 0)
        {
            WalkThumb(index, entry & ~1u);
            return;
        }

        const CodeBlock& block = code_[index];
        const std::size_t words = ArmFunctionWords(block, entry);
        const uint32_t end = entry + 4 * static_cast<uint32_t>(words);
        Mix(kFunctionStart);
        Mix(static_cast<uint32_t>(words));
        words_ += words;

        for (uint32_t address = entry; address < end; address += 4)
        {
            const uint32_t word = WordAt(block, address);
            Mix(Masked(word));
            if (arm::IsBranch(word))
            {
                const uint32_t target = arm::BranchTarget(address, word);
                if (arm::IsLink(word) || target < entry || target >= end)
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

    // The Thumb function's halfwords, up to the first exit that no forward branch of the function passes.
    std::size_t ThumbLength(const CodeBlock& block, uint32_t entry) const
    {
        uint32_t farthest = entry;
        std::size_t halves = 0;
        for (uint32_t address = entry; halves < 2 * kMaxFunctionWords && HoldsHalf(block, address); address += 2)
        {
            const uint16_t half = HalfAt(block, address);
            halves++;
            if (HoldsHalf(block, address + 2) && IsThumbCall(half, HalfAt(block, address + 2)))
            {
                address += 2;
                halves++;
                continue;
            }
            if (const std::optional<uint32_t> target = ThumbBranchTarget(address, half);
                target && *target > address && *target - entry < 8 * kMaxFunctionWords)
            {
                farthest = std::max(farthest, *target);
            }
            if (address >= farthest && IsThumbExit(half))
            {
                break;
            }
        }

        return halves;
    }

    // As Walk, for Thumb code: a call's offset counts as the number of the function that it calls, and a literal as
    // its masked word.
    void WalkThumb(std::size_t index, uint32_t entry)
    {
        const CodeBlock& block = code_[index];
        const std::size_t halves = ThumbLength(block, entry);
        const uint32_t end = entry + 2 * static_cast<uint32_t>(halves);
        Mix(kFunctionStart);
        Mix(static_cast<uint32_t>(halves));
        words_ += halves;

        for (uint32_t address = entry; address < end; address += 2)
        {
            const uint16_t half = HalfAt(block, address);
            if (address + 2 < end && IsThumbCall(half, HalfAt(block, address + 2)))
            {
                const uint16_t second = HalfAt(block, address + 2);
                Mix((half & 0xF800u) | (second & 0xF800u) << 16);
                Mix(Id(index, ThumbCallTarget(address, half, second)));
                address += 2;
                continue;
            }
            Mix(half);
            if (const std::optional<uint32_t> target = ThumbBranchTarget(address, half);
                target && (*target < entry || *target >= end))
            {
                Mix(Id(index, *target | 1));
            }
            if (const std::optional<uint32_t> literal = ThumbLiteralAddress(address, half))
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

// As CoreHash, for Thumb code: a call counts as its two opcodes without the offset, and a word that a load from a
// literal pool reads as the word with what moves left out.
std::optional<uint64_t> ThumbCoreHash(const CodeBlock& block, uint32_t start, uint32_t end)
{
    if (start >= end || !HoldsHalf(block, start) || !HoldsHalf(block, end - 2))
    {
        return std::nullopt;
    }

    std::vector<bool> literal((end - start) / 2);
    for (uint32_t address = start; address < end; address += 2)
    {
        const std::optional<uint32_t> pool = ThumbLiteralAddress(address, HalfAt(block, address));
        if (pool && *pool >= start && *pool + 4 <= end)
        {
            literal[(*pool - start) / 2] = true;
        }
    }

    uint64_t hash = kFnvOffset;
    for (uint32_t address = start; address < end; address += 2)
    {
        const uint16_t half = HalfAt(block, address);
        if (literal[(address - start) / 2])
        {
            MixInto(hash, Masked(WordAt(block, address)));
            address += 2;
        }
        else if (address + 2 < end && IsThumbCall(half, HalfAt(block, address + 2)))
        {
            MixInto(hash, (half & 0xF800u) | (HalfAt(block, address + 2) & 0xF800u) << 16);
            address += 2;
        }
        else
        {
            MixInto(hash, half);
        }
    }

    return hash;
}

// Whether the 112 words at `table` have a table's shape. Each handler starts with an instruction that always runs. The
// handlers are all ARM code or all Thumb code.
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
    const auto starts_code = [&block, unsupported](const auto& handler)
    {
        if ((handler.first & 1) != (unsupported & 1))
        {
            return false;
        }
        if ((handler.first & 1) != 0)
        {
            const uint16_t half = HalfAt(block, handler.first & ~1u);
            return half != 0 && (half & 0xF000) != 0xD000;
        }
        return WordAt(block, handler.first) >> 28 == 0xE;
    };

    return std::ranges::all_of(counts, starts_code) && most->second >= kFewestUnsupported &&
           most->second <= kMostUnsupported && counts.size() >= kFewestHandlers &&
           WordAt(block, table + 4 * kTrackEnd) != unsupported && WordAt(block, table + 4 * kLastCode) == unsupported;
}

} // namespace

std::size_t ArmFunctionWords(const CodeBlock& block, uint32_t entry)
{
    uint32_t farthest = entry;
    std::size_t words = 0;
    for (uint32_t address = entry; words < kMaxFunctionWords && Holds(block, address); address += 4)
    {
        const uint32_t word = WordAt(block, address);
        words++;
        if (arm::IsBranch(word) && !arm::IsLink(word))
        {
            const uint32_t target = arm::BranchTarget(address, word);
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
            run = Holds(block, word) || HoldsThumb(block, word) ? run + 1 : 0;
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
            const uint32_t start = driver.lowest_handler - known.core_before;
            const uint32_t end = driver.lowest_handler + known.core_after;
            if (driver.fingerprint != known.fingerprint ||
                (driver.thumb ? ThumbCoreHash(code[driver.block], start, end)
                              : CoreHash(code[driver.block], start, end)) != known.core_hash)
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

const char* NameOf(Build build)
{
    switch (build)
    {
    case Build::kEarlier:
        return "earlier";
    case Build::kMiddle:
        return "middle";
    case Build::kLateMiddle:
        return "late middle";
    case Build::kOldest:
        return "oldest";
    case Build::kLater:
        break;
    }

    return "later";
}

} // namespace ntdlib::dse
