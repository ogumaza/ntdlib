// SPDX-License-Identifier: MIT

// Tests of the decoder of an address that a lis and an ori or addi load (ntdlib/ppc/load_address.h): both forms, an
// addi with a negative low half, and pairs that aren't such a load. The words are encoded by hand from the PowerPC
// architecture book.

#include <cstdint>
#include <optional>

#include "xsflib/testing/check.h"

#include "ntdlib/ppc/load_address.h"

namespace
{

using ntdlib::ppc::LoadedAddress;
using xsflib::testing::Check;

void TestLoads()
{
    // lis r2, 0x8040; ori r2, r2, 0x9234: the ori takes the low half as it is.
    Check(LoadedAddress(0x3c408040, 0x60429234, 2) == 0x80409234u, "lis and ori");

    // lis r13, 0x8040; addi r13, r13, 0x1234.
    Check(LoadedAddress(0x3da08040, 0x39ad1234, 13) == 0x80401234u, "lis and addi");

    // lis r4, 0x805a; addi r4, r4, -0x6880: the high half is one more than the address's.
    Check(LoadedAddress(0x3c80805a, 0x38849780, 4) == 0x80599780u, "lis and an addi of a negative value");

    // lis r0, 0x8000; ori r0, r0, 0x10.
    Check(LoadedAddress(0x3c008000, 0x60000010, 0) == 0x80000010u, "lis and ori of r0");
}

void TestOtherWords()
{
    Check(LoadedAddress(0x3c408040, 0x60429234, 13) == std::nullopt, "a load of another register");
    Check(LoadedAddress(0x3c608040, 0x38849780, 3) == std::nullopt, "an addi of another register");
    Check(LoadedAddress(0x3c808040, 0x38839780, 4) == std::nullopt, "an addi from another register");
    Check(LoadedAddress(0x3c858040, 0x38849780, 4) == std::nullopt, "an addis that adds a register");
    Check(LoadedAddress(0x3c808040, 0x64849780, 4) == std::nullopt, "an oris");
    Check(LoadedAddress(0x38808040, 0x38849780, 4) == std::nullopt, "an li in place of the lis");
    Check(LoadedAddress(0x3c008000, 0x38000010, 0) == std::nullopt, "an addi of r0: it adds 0 in place of r0");
}

} // namespace

int main()
{
    TestLoads();
    TestOtherWords();

    return xsflib::testing::Summary("ppc load address");
}
