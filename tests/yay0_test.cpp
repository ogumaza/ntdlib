// SPDX-License-Identifier: MIT

// Tests of the Yay0 decoder (ntdlib/yay0.h). Each test builds its block by hand in the layout that the header
// describes, and the expected bytes were worked out by hand from the same layout.

#include "ntdlib/yay0.h"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/testing/byte_writer.h"

namespace
{

using xsflib::testing::ErrorOf;

using Bytes = std::vector<uint8_t>;

Bytes Text(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

// A block's header: the magic, the decoded size, and the places of the references and the literals.
Bytes Header(std::string_view magic, uint32_t size, uint32_t references, uint32_t literals)
{
    return ntdlib::testing::ByteWriter().Append(Text(magic)).U32(size).U32(references).U32(literals).Take();
}

void Append(Bytes& out, std::initializer_list<uint8_t> bytes)
{
    out.insert(out.end(), bytes);
}

void TestYay0()
{
    // Three literals, a reference of 6 bytes from 3 bytes back, and a literal: the bits 1110 1000. A count of 6 takes
    // the nibble 4.
    Bytes block = Header("Yay0", 10, 20, 22);
    Append(block, {0xE8, 0, 0, 0});
    Append(block, {0x40, 0x02});
    Append(block, {'a', 'b', 'c', 'd'});
    XSFLIB_CHECK(ntdlib::DecodeYay0(block) == Text("abcabcabcd"));

    // A nibble of 0 reads the count from the literals: 2 + 18 bytes of the last byte, between the literals 'a' and 'b'.
    // The bits are 101.
    Bytes longer = Header("Yay0", 22, 20, 22);
    Append(longer, {0xA0, 0, 0, 0});
    Append(longer, {0x00, 0x00});
    Append(longer, {'a', 2, 'b'});
    Bytes expected(21, 'a');
    expected.push_back('b');
    XSFLIB_CHECK(ntdlib::DecodeYay0(longer) == expected);

    // A reference stops at the decoded size: 'a', 'b', 'c', then 2 of the 6 bytes that the reference gives.
    Bytes short_size = block;
    short_size[7] = 5;
    XSFLIB_CHECK(ntdlib::DecodeYay0(short_size) == Text("abcab"));

    // A block that ends early.
    Bytes cut = block;
    cut.resize(24);
    XSFLIB_CHECK(!ErrorOf<xsflib::FormatError>([&] { ntdlib::DecodeYay0(cut); }).empty());

    // A reference to bytes before the start: a literal, then 3 bytes from 3 back.
    Bytes early = Header("Yay0", 4, 20, 22);
    Append(early, {0x80, 0, 0, 0});
    Append(early, {0x10, 0x02});
    Append(early, {'a'});
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntdlib::DecodeYay0(early); }) ==
                 "a compressed block repeats bytes before its start");

    // Another format's magic, and a decoded size over 64 MiB.
    Bytes other = block;
    other[2] = 'z';
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntdlib::DecodeYay0(other); }) == "not Yay0 data");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntdlib::DecodeYay0(Header("Yay0", 0x4000001, 16, 16)); }) ==
                 "Yay0 data decodes to more than 64 MiB");
    const Bytes short_header = {'Y', 'a', 'y', '0', 0, 0, 0, 4};
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntdlib::DecodeYay0(short_header); }) == "not Yay0 data");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("yay0", [] { TestYay0(); });
}
