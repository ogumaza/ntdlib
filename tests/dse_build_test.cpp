// SPDX-License-Identifier: MIT

// Tests of the build of DSE's driver in a game's code (ntdlib/dse/build.h), on made-up ARM code: a table of 112
// handlers, the handlers, and a function that they call.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/build.h"

namespace
{

namespace dse = ntdlib::dse;

constexpr uint32_t kReturn = 0xE12FFF1E; // bx lr
constexpr std::size_t kHandlers = 112;

// What a made-up driver's code holds, so that a test can change one part.
struct Driver
{
    uint32_t constant = 0x00001234; // a word in the literal pool of the called function
    uint8_t first_handler_value = 1;
};

bool Unsupported(std::size_t code)
{
    return code % 8 == 3 || code % 8 == 5 || code == kHandlers - 1;
}

// The driver's code at `base`: the table, the shared handler of the unsupported codes, a handler for each other code
// (mov r0, #value; bl helper; bx lr), then the helper. The helper loads a pointer into its code and a constant.
std::vector<uint8_t> DriverCode(uint32_t base, const Driver& driver)
{
    const uint32_t unsupported = base + 4 * kHandlers;
    const uint32_t first = unsupported + 4;
    std::size_t supported = 0;
    for (std::size_t code = 0; code < kHandlers; code++)
    {
        supported += Unsupported(code) ? 0 : 1;
    }
    const uint32_t helper = first + 12 * static_cast<uint32_t>(supported);

    std::vector<uint32_t> words;
    for (std::size_t code = 0, handler = 0; code < kHandlers; code++)
    {
        words.push_back(Unsupported(code) ? unsupported : first + 12 * static_cast<uint32_t>(handler++));
    }
    words.push_back(kReturn);
    for (std::size_t handler = 0; handler < supported; handler++)
    {
        const uint32_t at = first + 12 * static_cast<uint32_t>(handler);
        const uint32_t value = handler == 0 ? driver.first_handler_value : static_cast<uint32_t>(handler + 1);
        words.push_back(0xE3A00000 | value);
        words.push_back(0xEB000000 | ((helper - (at + 4 + 8)) >> 2 & 0xFFFFFF));
        words.push_back(kReturn);
    }
    words.push_back(0xE59F1004); // ldr r1, [pc, #4]
    words.push_back(0xE59F2004); // ldr r2, [pc, #4]
    words.push_back(kReturn);
    words.push_back(helper);
    words.push_back(driver.constant);

    std::vector<uint8_t> bytes(4 * words.size());
    for (std::size_t i = 0; i < words.size(); i++)
    {
        xsflib::StoreLe32(&bytes[4 * i], words[i]);
    }

    return bytes;
}

std::vector<dse::DriverCode> Find(uint32_t base, const Driver& driver)
{
    const std::vector<uint8_t> bytes = DriverCode(base, driver);
    const std::vector<dse::CodeBlock> code = {{base, bytes}};

    return dse::FindDriverCode(code);
}

// The table, and the code that it reaches: the shared handler, 83 handlers and the helper.
void TestFindsTheTable()
{
    const std::vector<dse::DriverCode> found = Find(0x02000000, {});

    XSFLIB_CHECK(found.size() == 1);
    XSFLIB_CHECK(found.at(0).table == 0x02000000);
    XSFLIB_CHECK(found.at(0).block == 0);
    XSFLIB_CHECK(found.at(0).functions == 85);
    XSFLIB_CHECK(found.at(0).words == 1 + 83 * 3 + 3);
}

// The same code at another address, or in an overlay after another block, gives the same fingerprint.
void TestFingerprintMovesWithTheCode()
{
    const uint64_t at_start = Find(0x02000000, {}).at(0).fingerprint;
    const std::vector<uint8_t> moved = DriverCode(0x02180000, {});
    const std::vector<uint8_t> other(0x400, 0);
    const std::vector<dse::CodeBlock> code = {{0x02000000, other}, {0x02180000, moved}};

    const std::vector<dse::DriverCode> found = dse::FindDriverCode(code);

    XSFLIB_CHECK(Find(0x02100000, {}).at(0).fingerprint == at_start);
    XSFLIB_CHECK(found.size() == 1);
    XSFLIB_CHECK(found.at(0).block == 1);
    XSFLIB_CHECK(found.at(0).table == 0x02180000);
    XSFLIB_CHECK(found.at(0).fingerprint == at_start);
}

// A changed immediate in a handler, or a changed constant in the pool of a function that the handlers call, gives
// another fingerprint.
void TestFingerprintSeesChangedCode()
{
    const uint64_t original = Find(0x02000000, {}).at(0).fingerprint;

    const uint64_t handler = Find(0x02000000, {.first_handler_value = 0x7F}).at(0).fingerprint;
    const uint64_t constant = Find(0x02000000, {.constant = 0x00001235}).at(0).fingerprint;

    XSFLIB_CHECK(handler != original);
    XSFLIB_CHECK(constant != original);
}

// Made-up code is no build that the model covers. Neither is code without a table, nor a table of Thumb handlers.
void TestUnknownBuild()
{
    const std::vector<uint8_t> bytes = DriverCode(0x02000000, {});
    std::vector<uint8_t> thumb = bytes;
    for (std::size_t i = 0; i < kHandlers; i++)
    {
        thumb[4 * i] |= 1;
    }
    const std::vector<uint8_t> plain(0x1000, 0);

    const std::vector<dse::CodeBlock> made_up = {{0x02000000, bytes}};
    const std::vector<dse::CodeBlock> thumb_code = {{0x02000000, thumb}};
    const std::vector<dse::CodeBlock> no_table = {{0x02000000, plain}};

    XSFLIB_CHECK(!dse::BuildOf(made_up));
    XSFLIB_CHECK(!dse::BuildOf(std::vector<dse::CodeBlock>{}));
    XSFLIB_CHECK(dse::FindDriverCode(thumb_code).empty());
    XSFLIB_CHECK(dse::FindDriverCode(no_table).empty());
}

} // namespace

int main()
{
    TestFindsTheTable();
    TestFingerprintMovesWithTheCode();
    TestFingerprintSeesChangedCode();
    TestUnknownBuild();

    return xsflib::testing::Summary("DSE build");
}
