// SPDX-License-Identifier: MIT

#include "ntdlib/nx/bex/user_procedures.h"

#include <algorithm>
#include <string>

#include "xsflib/binary.h"

#include "nx/bex/message_pack.h"

namespace ntdlib::nx::bex
{

namespace
{

// Where a procedure finds a variable: kind 0 the sound's variables, 1 the track's, 2 the global ones (the pointers at
// +0, +0x10 and +8 of the callback's parameter).
struct VariablePlace
{
    int kind = 1;
    int index = 0;
};

int16_t& At(ProcedureVariables& variables, VariablePlace place)
{
    auto& set = place.kind == 0 ? variables.local : (place.kind == 2 ? variables.global : variables.track);
    return set[static_cast<std::size_t>(place.index) & 15];
}

// The places that Jamboree's constructor gives the procedures (fn_1039c0). No code of the game changes them
// (fn_104354, fn_10438c and fn_1043a8 never ran in the Eden runs of the game).
constexpr VariablePlace kDrawSlot{1, 14};   // +0x8: the slot, and the result
constexpr VariablePlace kDrawCount{1, 15};  // +0xc
constexpr VariablePlace kTableIndex{1, 15}; // +0x201c
constexpr VariablePlace kTableEntry{1, 14}; // +0x2020: the entry, and the result
constexpr VariablePlace kCountLimit{1, 15}; // +0x3134
constexpr VariablePlace kCountSlot{1, 14};  // +0x3138: the slot, and the result

// The generators' step: the state times 48,271 in 32 bits, then modulo 2^31 - 1. The product wraps before the modulo.
uint32_t Step(uint32_t state)
{
    return (state * 48271u) % 0x7FFFFFFFu;
}

// The most draws of one call. Jamboree's handler stops there. Superstars' handler would draw for ever once its last
// result is 0 with its generator at 0, and the model stops it there too.
constexpr int kMostDraws = 64;

// A draw for a count of 3 or more: steps the generator from `start` until the result, the state modulo `count`, differs
// from the slot's last one, at most kMostDraws times. Leaves the state in `state` and the result in `last`, and returns
// the result.
uint32_t Draw(uint32_t start, uint32_t count, uint32_t& state, uint32_t& last)
{
    uint32_t next = start;
    uint32_t result = 0;
    for (int draw = 0; draw < kMostDraws; ++draw)
    {
        next = Step(next);
        result = next % count;
        if (result != last)
        {
            break;
        }
    }

    state = next;
    last = result;

    return result;
}

} // namespace

UserProcedures::UserProcedures(ProcedureHandler handler) : handler_(handler)
{
    for (auto& bank : states_)
    {
        bank.fill(1);
    }
    for (auto& bank : last_)
    {
        bank.fill(0xFFFFFFFFu);
    }
}

void UserProcedures::SetTable(std::size_t index, std::span<const int16_t, kTableSize> values)
{
    if (index >= kTableCount)
    {
        return;
    }

    std::ranges::copy(values, tables_[index].begin());
    loaded_[index] = true;
}

bool UserProcedures::Run(uint16_t procedure, ProcedureVariables& variables)
{
    if (handler_ == ProcedureHandler::kJamboree)
    {
        switch (procedure)
        {
        case 0:
        case 1:
            DrawJamboree(procedure, variables);
            return true;
        case 5:
            ReadTable(variables);
            return true;
        case 6:
            Count(variables);
            return true;
        default:
            return false;
        }
    }
    if (handler_ == ProcedureHandler::kSuperstars && procedure == 0)
    {
        DrawSuperstars(variables);
        return true;
    }

    return false;
}

bool UserProcedures::KeepsState(uint16_t procedure) const
{
    if (handler_ == ProcedureHandler::kJamboree)
    {
        return procedure == 0 || procedure == 1 || procedure == 6;
    }

    return handler_ == ProcedureHandler::kSuperstars && procedure == 0;
}

void UserProcedures::DrawJamboree(int bank, ProcedureVariables& variables)
{
    // The slot reads unsigned. A slot of 512 or more draws from slot 0. A count of 3 or more draws: a result equal to
    // the slot's last one is drawn again, up to 64 draws in all. A state of 0 counts as 1. A count of 2 gives the
    // opposite of the last result's low bit, and a lower count gives 0.
    int16_t& slot_variable = At(variables, kDrawSlot);
    const auto slot_value = static_cast<uint16_t>(slot_variable);
    const std::size_t slot = slot_value < kJamboreeSlots ? slot_value : 0;
    const int16_t count = At(variables, kDrawCount);
    uint32_t& state = states_[static_cast<std::size_t>(bank)][slot];
    uint32_t& last = last_[static_cast<std::size_t>(bank)][slot];
    if (count >= 3)
    {
        slot_variable = static_cast<int16_t>(Draw(state != 0 ? state : 1, static_cast<uint32_t>(count), state, last));
    }
    else if (count == 2)
    {
        last = 1u & ~last;
        slot_variable = static_cast<int16_t>(last);
    }
    else
    {
        last = 0;
        slot_variable = 0;
    }
}

void UserProcedures::ReadTable(ProcedureVariables& variables)
{
    // An unloaded table changes nothing. The tables lie one after another, 128 entries each. An entry past the end of
    // its table reads the next table, and the model leaves an entry past the last table's end alone.
    const int16_t table = At(variables, kTableIndex);
    int16_t& entry = At(variables, kTableEntry);
    if (table < 0 || static_cast<std::size_t>(table) >= kTableCount || !loaded_[static_cast<std::size_t>(table)])
    {
        return;
    }

    const long long flat = static_cast<long long>(table) * static_cast<long long>(kTableSize) + entry;
    if (flat < 0 || flat >= static_cast<long long>(kTableCount * kTableSize))
    {
        return;
    }

    entry = tables_[static_cast<std::size_t>(flat) / kTableSize][static_cast<std::size_t>(flat) % kTableSize];
}

void UserProcedures::Count(ProcedureVariables& variables)
{
    // The slot reads unsigned. A slot of 64 or more counts in slot 0. The result is the count before the call. The
    // count then goes up by 1, and back to 0 once it would pass the limit. A count therefore takes the values 0 to
    // the limit.
    const int16_t limit = At(variables, kCountLimit);
    int16_t& slot_variable = At(variables, kCountSlot);
    const auto slot_value = static_cast<uint16_t>(slot_variable);
    uint16_t& counter = counters_[slot_value < kCounterSlots ? slot_value : 0];
    const uint16_t now = counter;
    counter = limit < static_cast<int16_t>(now + 1) ? 0 : static_cast<uint16_t>(now + 1);
    slot_variable = static_cast<int16_t>(now);
}

void UserProcedures::DrawSuperstars(ProcedureVariables& variables)
{
    // The slot is the sound's variable 14, and the count its variable 15. A count of 3 or more draws again while the
    // result equals the slot's last one. A count of 2 steps the generator and gives the new state's low bit, and a
    // lower count gives 0. The game reads no slot outside 0 to 31. The model leaves the variables alone there. A
    // generator at 0 stays at 0, and the game would draw for ever once its last result is 0. The model stops after
    // 64 draws. Jamboree's handler does the same.
    int16_t& slot_variable = variables.local[14];
    const int16_t count = variables.local[15];
    if (slot_variable < 0 || static_cast<std::size_t>(slot_variable) >= kSuperstarsSlots)
    {
        return;
    }

    uint32_t& state = states_[0][static_cast<std::size_t>(slot_variable)];
    uint32_t& last = last_[0][static_cast<std::size_t>(slot_variable)];
    if (count >= 3)
    {
        slot_variable = static_cast<int16_t>(Draw(state, static_cast<uint32_t>(count), state, last));
    }
    else if (count == 2)
    {
        state = Step(state);
        last = state & 1u;
        slot_variable = static_cast<int16_t>(last);
    }
    else
    {
        last = 0;
        slot_variable = 0;
    }
}

std::array<int16_t, UserProcedures::kTableSize> ReadProcedureTable(std::span<const uint8_t> file)
{
    detail::MessagePack reader(file, "the table of user procedure 5");
    const detail::Value root = reader.Read();
    const detail::Value* value = root.Get("Value");
    const std::vector<detail::Value>* items = value ? value->Array() : nullptr;
    if (!items || items->size() != UserProcedures::kTableSize)
    {
        throw xsflib::FormatError("the table of user procedure 5 isn't a map with an array of 128 values in \"Value\"");
    }

    std::array<int16_t, UserProcedures::kTableSize> table{};
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        const int64_t* number = std::get_if<int64_t>(&(*items)[i].v);
        if (!number)
        {
            throw xsflib::FormatError("the table of user procedure 5 holds a value that isn't an integer");
        }
        table[i] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint64_t>(*number) & 0xFFFFu));
    }

    return table;
}

} // namespace ntdlib::nx::bex
