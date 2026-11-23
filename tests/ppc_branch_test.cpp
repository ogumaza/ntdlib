// SPDX-License-Identifier: MIT

// Tests of the encoder and the decoder of PowerPC's relative branches (ntdlib/ppc/branch.h) on words that a game's code
// holds: a branch forward and back, the farthest offsets either way, and the words that aren't a relative bl.

#include <cstdint>
#include <optional>

#include "xsflib/testing/check.h"

#include "ntdlib/ppc/branch.h"

namespace
{

using ntdlib::ppc::Branch;
using ntdlib::ppc::BranchOffset;
using ntdlib::ppc::CallTarget;
using ntdlib::ppc::IsCall;
using xsflib::testing::Check;

void TestBranch()
{
    Check(Branch(0x80004000, 0x80004010) == 0x48000010, "b forward");
    Check(Branch(0x80004010, 0x80004000) == 0x4bfffff0, "b back");
    Check(Branch(0x80000000, 0x81fffffc) == 0x49fffffc, "b to the farthest address forward");
    Check(Branch(0x82000000, 0x80000000) == 0x4a000000, "b to the farthest address back");
    Check(Branch(0x80004000, 0x80004000) == 0x48000000, "b to itself");
}

void TestCallTarget()
{
    // bl 0x800bd810 at 0x8029db28, and bl back by 0x10 at 0x80004010.
    Check(IsCall(0x4be1fce9) && CallTarget(0x8029db28, 0x4be1fce9) == 0x800bd810u, "bl back");
    Check(CallTarget(0x80004000, 0x48000011) == 0x80004010u, "bl forward");
    Check(BranchOffset(0x4a000001) == -0x2000000 && BranchOffset(0x49fffffd) == 0x1fffffc,
          "the farthest offsets either way");
    Check(CallTarget(0x80004000, Branch(0x80004000, 0x80004010)) == std::nullopt, "a b isn't a call");
    Check(CallTarget(0x80004000, 0x48000013) == std::nullopt, "an absolute bla isn't a relative call");
    Check(CallTarget(0x80004000, 0x4e800021) == std::nullopt, "a blrl isn't a relative call");
    Check(CallTarget(0x80004000, 0x4c000001) == std::nullopt, "opcode 19 with LK isn't a relative call");
}

} // namespace

int main()
{
    TestBranch();
    TestCallTarget();

    return xsflib::testing::Summary("ppc branch");
}
