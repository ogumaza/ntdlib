// SPDX-License-Identifier: MIT

// A test of the reader of modules of a GameCube game's code (ntdlib/rel_module.h) on a small synthetic module. The
// expected values follow from the layout that the reader's header describes.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/rel_module.h"
#include "ntdlib/testing/big_endian_bytes.h"

using BigEndianBytes = ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// A module with 3 sections: none, code, and data of 0x20 bytes. Its relocations against itself set the data's first two
// words to the data's offsets 0x10 and 0x18, in section `target` (2, the data). The table of the two words then runs up
// to 0x10, the first target.
std::vector<uint8_t> ModuleBytes(uint8_t target = 2)
{
    BigEndianBytes b;
    b.U32(5).U32(0).U32(0).U32(3).U32(0x40).PadTo(0x28).U32(0x88).U32(8).PadTo(0x40);
    b.U32(0).U32(0).U32(0x60 | 1).U32(8).U32(0x68).U32(0x20).PadTo(0x68);
    b.U32(0).U32(0).U32(0x12345678).U32(0).U32(1).U32(2).U32(3).U32(4);
    b.U32(5).U32(0x90);
    b.U16(0).U8(202).U8(2).U32(0);
    b.U16(0).U8(1).U8(target).U32(0x10);
    b.U16(4).U8(1).U8(2).U32(0x18);
    b.U16(0).U8(203).U8(0).U32(0);

    return b.Data();
}

void TestRelModule()
{
    const std::vector<uint8_t> file = ModuleBytes();
    const ntdlib::RelModule module(file, "test.rel");

    const std::size_t table = module.At(2, 0);
    const std::vector<std::pair<uint16_t, std::size_t>> pointers = module.TablePointers(table);
    Check(table == 0x68, "section 2 starts at 0x68");
    Check(pointers == std::vector<std::pair<uint16_t, std::size_t>>{{0, 0x78}, {1, 0x80}},
          "the table's two words point at the two objects");
    Check(module.Object(0x78).size() == 8 && module.Object(0x80).size() == 8,
          "an object runs up to the next target or to the section's end");
    Check(!module.Pointer(table + 8).has_value(), "a word that no relocation sets points nowhere");
    Check(ErrorOf([&] { module.At(2, 0x20); }) == "test.rel's section 2 has no data at offset 32",
          "a place past a section fails");
    Check(ErrorOf([&] { module.At(0, 0); }) == "test.rel's section 0 has no data at offset 0",
          "a section without data fails");
    Check(ErrorOf([&] { module.Object(0x10); }) == "an object lies outside test.rel's sections",
          "an object in the header fails");

    const std::vector<uint8_t> past = ModuleBytes(5);
    Check(ErrorOf([&] { ntdlib::RelModule(past, "test.rel"); }) ==
              "test.rel has a relocation into section 5, past its sections",
          "a relocation into a section past the module's 3 fails");
}

} // namespace

int main()
{
    TestRelModule();

    return xsflib::testing::Summary("rel_module");
}
