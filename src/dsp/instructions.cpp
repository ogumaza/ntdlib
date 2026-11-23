// SPDX-License-Identifier: MIT

// The DSP's instruction set: decoding, the main instructions, the parallel load/store extensions, and the static
// analysis of IRAM for loop ends and polling loops.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>

#include "dsp/instructions_internal.h"
#include "ntdlib/dsp/dsp.h"

namespace ntdlib::dsp
{

template <typename Memory>
int Dsp<Memory>::SizeOf(uint16_t op) const
{
    return TwoWords(Decode(op)) ? 2 : 1;
}

template <typename Memory>
uint16_t Dsp<Memory>::FetchSecondWord()
{
    const uint16_t word = ReadImem(regs_.pc);
    regs_.pc++;

    return word;
}

template <typename Memory>
void Dsp<Memory>::SkipNextInstruction()
{
    regs_.pc = static_cast<uint16_t>(regs_.pc + SizeOf(ReadImem(regs_.pc)));
}

template <typename Memory>
void Dsp<Memory>::BuildLoopMap()
{
    // A linear walk of IRAM that decodes each instruction from the start, data included. LOOP and LOOPI end their loop
    // at the next address. BLOOP and BLOOPI end theirs at the address in their second word. That address may be
    // outside IRAM. The polling loops are recognised by their words.
    std::fill(loop_end_.begin(), loop_end_.end(), uint8_t{0});
    std::fill(idle_.begin(), idle_.end(), uint8_t{0});

    const auto word = [this](uint32_t address) -> uint16_t
    {
        return address < Memory::kIramSize ? iram_[address] : 0;
    };

    for (uint32_t address = 0; address < Memory::kIramSize;)
    {
        const uint16_t op = iram_[address];
        const Op decoded = Decode(op);
        if (decoded == Op::kLoop || decoded == Op::kLoopi)
        {
            loop_end_[address + 1] = 1;
        }
        else if (decoded == Op::kBloop || decoded == Op::kBloopi)
        {
            loop_end_[word(address + 1)] = 1;
        }

        // `word` gives 0 past IRAM. No polling loop holds a 0.
        const auto starts_at = [&](const PollingLoop& loop)
        {
            for (uint32_t i = 0; i < loop.size; i++)
            {
                if (word(address + i) != loop.words[i])
                {
                    return false;
                }
            }

            return true;
        };

        if (std::any_of(std::begin(kPollingLoops), std::end(kPollingLoops), starts_at))
        {
            idle_[address] = 1;
        }

        address += TwoWords(decoded) ? 2 : 1;
    }
}

template <typename Memory>
void Dsp<Memory>::Hold(HeldWrites& held, uint8_t reg, uint16_t value, typename PendingWrite::Kind kind)
{
    held.writes[held.count++] = {kind, reg, value};
}

template <typename Memory>
std::span<const uint16_t> Dsp<Memory>::FrequentWords()
{
    return kFrequentWords;
}

template <typename Memory>
void Dsp<Memory>::UseGenericHandlers()
{
    handlers_ = GenericHandlers();
}

// A table of handlers holds 65,536 of them: 512 KB. A copy on the stack could overflow a thread that a plugin host
// starts. Each table is therefore filled in place.
template <typename Memory>
const typename Dsp<Memory>::Handler* Dsp<Memory>::GenericHandlers()
{
    static std::array<Handler, 0x10000> table;
    static const bool kFilled = []
    {
        table.fill([](Dsp& dsp, uint16_t op) { dsp.Execute(op); });

        return true;
    }();
    static_cast<void>(kFilled);

    return table.data();
}

template <typename Memory>
const typename Dsp<Memory>::Handler* Dsp<Memory>::Handlers()
{
    static std::array<Handler, 0x10000> table;
    static const bool kFilled = []
    {
        std::copy_n(GenericHandlers(), table.size(), table.begin());

        // A fold over all the words would pass the nesting limit of older Clang builds (256).
        []<std::size_t... i>(std::index_sequence<i...>)
        {
            constexpr std::array<Handler, sizeof...(i)> kWordHandlers = {&ExecuteWord<kFrequentWords[i]>...};
            for (std::size_t k = 0; k < kWordHandlers.size(); ++k)
            {
                table[kFrequentWords[k]] = kWordHandlers[k];
            }
        }(std::make_index_sequence<std::size(kFrequentWords)>());

        return true;
    }();
    static_cast<void>(kFilled);

    return table.data();
}

template <typename Memory>
template <uint16_t word>
void Dsp<Memory>::ExecuteWord(Dsp& dsp, uint16_t op)
{
    static_cast<void>(op);
    constexpr auto kKind = static_cast<uint8_t>(DecodeConstant(word));
    dsp.ExecuteOp(word, kKind);
}

template <typename Memory>
void Dsp<Memory>::Execute(uint16_t op)
{
    ExecuteOp(op, static_cast<uint8_t>(Decode(op)));
}

} // namespace ntdlib::dsp
