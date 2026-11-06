// SPDX-License-Identifier: MIT

// Checks the model of the two games' user procedures against runs of the games' routines from hand-set states:
// fn_1039c0, fn_104290, fn_104318 and fn_103b80 in Super Mario Party Jamboree's main, and fn_479c04, fn_479d34 and
// fn_479c60 in Mario Party Superstars'. Each case starts from the constructor's state and calls a procedure a few times
// with the same slot and count. The expected values are what the game's routine wrote to the slot's variable after
// each call.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nx/bex/user_procedures.h"

using namespace xsflib::testing;

namespace
{

namespace bex = ntdlib::nx::bex;

// The results of `calls` calls of `procedure`, with the slot and the count in variables 14 and 15: the track's for
// Jamboree, and the sound's for Superstars.
std::vector<int> Calls(bex::UserProcedures& procedures, uint16_t procedure, int slot, int count, int calls)
{
    std::vector<int> results;
    for (int i = 0; i < calls; ++i)
    {
        bex::ProcedureVariables variables;
        variables.local.fill(-1);
        variables.global.fill(-1);
        variables.track.fill(-1);
        auto& set = procedures.Handler() == bex::ProcedureHandler::kSuperstars ? variables.local : variables.track;
        set[14] = static_cast<int16_t>(slot);
        set[15] = static_cast<int16_t>(count);
        procedures.Run(procedure, variables);
        results.push_back(set[14]);
    }

    return results;
}

std::vector<int> Fresh(bex::ProcedureHandler handler, uint16_t procedure, int slot, int count)
{
    bex::UserProcedures procedures(handler);
    return Calls(procedures, procedure, slot, count, 8);
}

void TestJamboreeDraws()
{
    using V = std::vector<int>;
    constexpr auto kJamboree = bex::ProcedureHandler::kJamboree;
    for (uint16_t p : {uint16_t{0}, uint16_t{1}})
    {
        XSFLIB_CHECK(Fresh(kJamboree, p, 0, 3) == V({1, 0, 2, 0, 1, 2, 0, 2}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 1, 5) == V({1, 4, 2, 3, 1, 0, 4, 0}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 427, 3) == V({1, 0, 2, 0, 1, 2, 0, 2}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 511, 4) == V({3, 2, 3, 2, 3, 1, 0, 1}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 600, 3) == V({1, 0, 2, 0, 1, 2, 0, 2}));
        XSFLIB_CHECK(Fresh(kJamboree, p, -1, 3) == V({1, 0, 2, 0, 1, 2, 0, 2}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 5, 7) == V({6, 0, 3, 2, 4, 2, 3, 1}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 5, 25) == V({21, 19, 7, 2, 13, 21, 0, 9}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 2, 2) == V({0, 1, 0, 1, 0, 1, 0, 1}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 2, 1) == V(8, 0) && Fresh(kJamboree, p, 2, 0) == V(8, 0));
        XSFLIB_CHECK(Fresh(kJamboree, p, 2, -1) == V(8, 0));
        XSFLIB_CHECK(Fresh(kJamboree, p, 3, 1000) == V({271, 794, 782, 427, 638, 346, 150, 859}));
        XSFLIB_CHECK(Fresh(kJamboree, p, 3, 32767) == V({15504, 28070, 10545, 22598, 2993, 5119, 14885, 10769}));
    }

    // The two procedures draw from separate banks, and the slots from separate generators. A call with another count
    // steps the same generator.
    bex::UserProcedures banks(kJamboree);
    V results;
    for (uint16_t p : {uint16_t{0}, uint16_t{1}, uint16_t{0}, uint16_t{1}})
    {
        results.push_back(Calls(banks, p, 0, 5, 1)[0]);
    }
    XSFLIB_CHECK(results == V({1, 1, 4, 4}));

    bex::UserProcedures slots(kJamboree);
    results.clear();
    for (int slot : {0, 1, 0, 1})
    {
        results.push_back(Calls(slots, 0, slot, 5, 1)[0]);
    }
    XSFLIB_CHECK(results == V({1, 1, 4, 4}));

    bex::UserProcedures counts(kJamboree);
    results.clear();
    for (int count : {3, 5, 3, 5, 2, 2, 1, 3})
    {
        results.push_back(Calls(counts, 0, 0, count, 1)[0]);
    }
    XSFLIB_CHECK(results == V({1, 4, 0, 2, 1, 0, 0, 2}));

    // A slot of 512 or more, or below 0, draws from slot 0's generator.
    bex::UserProcedures shared(kJamboree);
    results.clear();
    for (int slot : {0, 600, 512, -1})
    {
        results.push_back(Calls(shared, 0, slot, 5, 1)[0]);
    }
    XSFLIB_CHECK(results == V({1, 4, 2, 3}));
}

void TestJamboreeCountsAndTables()
{
    using V = std::vector<int>;
    constexpr auto kJamboree = bex::ProcedureHandler::kJamboree;
    XSFLIB_CHECK(Fresh(kJamboree, 6, 3, 3) == V({0, 1, 2, 3, 0, 1, 2, 3}));
    XSFLIB_CHECK(Fresh(kJamboree, 6, 0, 0) == V(8, 0));
    XSFLIB_CHECK(Fresh(kJamboree, 6, 0, 1) == V({0, 1, 0, 1, 0, 1, 0, 1}));
    XSFLIB_CHECK(Fresh(kJamboree, 6, 70, 2) == V({0, 1, 2, 0, 1, 2, 0, 1}));
    XSFLIB_CHECK(Fresh(kJamboree, 6, 63, 4) == V({0, 1, 2, 3, 4, 0, 1, 2}));
    XSFLIB_CHECK(Fresh(kJamboree, 6, -1, 2) == V({0, 1, 2, 0, 1, 2, 0, 1}));
    XSFLIB_CHECK(Fresh(kJamboree, 6, 5, -1) == V(8, 0));

    // A slot of 64 or more, or below 0, counts in slot 0.
    bex::UserProcedures shared(kJamboree);
    V counted;
    for (int slot : {0, 70, 64, -1})
    {
        counted.push_back(Calls(shared, 6, slot, 9, 1)[0]);
    }
    XSFLIB_CHECK(counted == V({0, 1, 2, 3}));

    // Procedure 5 reads table 0 after fn_104318 has loaded it with 1000 + its index. Table 1 isn't loaded, and the
    // call changes nothing.
    bex::UserProcedures tables(kJamboree);
    std::array<int16_t, bex::UserProcedures::kTableSize> values{};
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        values[i] = static_cast<int16_t>(1000 + i);
    }
    tables.SetTable(0, values);
    V results;
    for (int entry : {0, 5, 127})
    {
        results.push_back(Calls(tables, 5, entry, 0, 1)[0]);
    }
    XSFLIB_CHECK(results == V({1000, 1005, 1127}));
    XSFLIB_CHECK(Calls(tables, 5, 5, 1, 1) == V({5}));

    // Procedures 2 to 4 read the music sync, and the model leaves them to the caller.
    bex::ProcedureVariables variables;
    XSFLIB_CHECK(!tables.Run(2, variables) && !tables.Run(3, variables) && !tables.Run(4, variables));
    XSFLIB_CHECK(!tables.Run(7, variables));
    XSFLIB_CHECK(tables.KeepsState(0) && tables.KeepsState(1) && tables.KeepsState(6));
    XSFLIB_CHECK(!tables.KeepsState(2) && !tables.KeepsState(5) && !tables.KeepsState(7));
}

void TestSuperstars()
{
    using V = std::vector<int>;
    constexpr auto kSuperstars = bex::ProcedureHandler::kSuperstars;
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 0, 3) == V({1, 0, 2, 0, 1, 2, 0, 2}));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 1, 5) == V({1, 4, 2, 3, 1, 0, 4, 0}));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 5, 7) == V({6, 0, 3, 2, 4, 2, 3, 1}));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 25, 25) == V({21, 19, 7, 2, 13, 21, 0, 9}));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 2, 2) == V({1, 0, 0, 1, 0, 0, 0, 1}));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 2, 1) == V(8, 0) && Fresh(kSuperstars, 0, 2, 0) == V(8, 0));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 2, -1) == V(8, 0));
    XSFLIB_CHECK(Fresh(kSuperstars, 0, 3, 1000) == V({271, 794, 782, 427, 638, 346, 150, 859}));

    bex::UserProcedures counts(kSuperstars);
    V results;
    for (int count : {3, 5, 3, 5, 2, 2, 1, 3})
    {
        results.push_back(Calls(counts, 0, 0, count, 1)[0]);
    }
    XSFLIB_CHECK(results == V({1, 4, 0, 2, 0, 0, 0, 2}));

    // The game's fn_479c60 returns at once for any procedure but 0.
    bex::UserProcedures other(kSuperstars);
    XSFLIB_CHECK(Calls(other, 1, 7, 3, 1) == V({7}));
    XSFLIB_CHECK(other.KeepsState(0) && !other.KeepsState(1) && !other.KeepsState(6));
    XSFLIB_CHECK(!bex::UserProcedures().KeepsState(0));
}

// A table's file: a map whose key `key` holds an array of `values`, each a positive fixint but the first three: -200 as
// an int16, 300 as a uint16 and -32 as a negative fixint.
std::vector<uint8_t> TableFile(std::size_t values, const char* key = "Value")
{
    std::vector<uint8_t> file = {0x81, 0xa5};
    file.insert(file.end(), key, key + 5);
    file.insert(file.end(), {0xdc, static_cast<uint8_t>(values >> 8), static_cast<uint8_t>(values & 0xff)});
    for (std::size_t i = 0; i < values; ++i)
    {
        if (i == 0)
        {
            file.insert(file.end(), {0xd1, 0xff, 0x38});
        }
        else if (i == 1)
        {
            file.insert(file.end(), {0xcd, 0x01, 0x2c});
        }
        else if (i == 2)
        {
            file.push_back(0xe0);
        }
        else
        {
            file.push_back(static_cast<uint8_t>(i));
        }
    }

    return file;
}

void TestTableFiles()
{
    const auto table = bex::ReadProcedureTable(TableFile(128));
    XSFLIB_CHECK(table[0] == -200 && table[1] == 300 && table[2] == -32 && table[3] == 3 && table[127] == 127);

    const auto error = [](const std::vector<uint8_t>& file)
    {
        return ErrorOf<xsflib::FormatError>([&file] { bex::ReadProcedureTable(file); });
    };
    XSFLIB_CHECK(!error(TableFile(127)).empty() && !error(TableFile(129)).empty());
    XSFLIB_CHECK(!error(TableFile(128, "Vaxue")).empty());
    std::vector<uint8_t> with_float = TableFile(128);
    with_float[with_float.size() - 1] = 0xc2; // false, not an integer
    XSFLIB_CHECK(!error(with_float).empty());
    std::vector<uint8_t> cut = TableFile(128);
    cut.resize(cut.size() - 1);
    XSFLIB_CHECK(!error(cut).empty());
}

} // namespace

int main()
{
    TestJamboreeDraws();
    TestJamboreeCountsAndTables();
    TestSuperstars();
    TestTableFiles();

    return xsflib::testing::Summary("nx_bex_user_procedures");
}
