// SPDX-License-Identifier: MIT

// Tests of the Yaz0 decoder (ntdlib/yaz0.h): literals, short repeats, repeats whose length takes a third byte, more
// than one group of chunks, and the damaged cases. Each block and its expected bytes were worked out by hand.

#include "ntdlib/yaz0.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/testing/yaz0_files.h"

namespace
{

using ntdlib::testing::yaz0_files::Yaz0Block;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

void TestRepeats()
{
    // Four literals, a repeat of 12 bytes from 4 back, then three literals.
    Check(Text(ntdlib::DecodeYaz0(Yaz0Block(19, {0xf7, 'A', 'B', 'C', 'D', 0xa0, 0x03, 'X', 'Y', 'Z'}))) ==
              "ABCDABCDABCDABCDXYZ",
          "a short repeat");
    // Three literals, a repeat of 6 bytes from 3 back, and a literal.
    Check(Text(ntdlib::DecodeYaz0(Yaz0Block(10, {0xE8, 'a', 'b', 'c', 0x40, 0x02, 'd'}))) == "abcabcabcd",
          "a repeat that overlaps the bytes it repeats");
    // A literal, then a repeat of 0x12 + 0x10 bytes from 1 back.
    Check(Text(ntdlib::DecodeYaz0(Yaz0Block(35, {0x80, 'Q', 0x00, 0x00, 0x10}))) == std::string(35, 'Q'),
          "a repeat whose length takes a third byte");
    // A literal, a repeat of 0x12 + 2 bytes from 1 back, and a literal.
    Check(Text(ntdlib::DecodeYaz0(Yaz0Block(22, {0xA0, 'a', 0x00, 0x00, 2, 'b'}))) == std::string(21, 'a') + "b",
          "a long repeat between literals");
    // Eight literals, then a second byte of chunks.
    Check(Text(ntdlib::DecodeYaz0(Yaz0Block(9, {0xFF, '1', '2', '3', '4', '5', '6', '7', '8', 0x80, '9'}))) ==
              "123456789",
          "a second group of chunks");
}

void TestDamaged()
{
    Check(ErrorOf([&] { ntdlib::DecodeYaz0(Yaz0Block(4, {0x00, 0x10, 0x00})); }) ==
              "Yaz0 data repeats bytes before its start",
          "a repeat before the start");
    Check(!ErrorOf([&] { ntdlib::DecodeYaz0(Yaz0Block(19, {0xf7, 'A', 'B'})); }).empty(), "truncated Yaz0 data");
    Check(ErrorOf([&] { ntdlib::DecodeYaz0(Yaz0Block(0x4000001, {})); }) == "Yaz0 data decodes to more than 64 MiB",
          "a decoded size over 64 MiB");
    Check(ErrorOf([&] { ntdlib::DecodeYaz0(std::vector<uint8_t>{'R', 'A', 'R', 'C'}); }) == "not Yaz0 data",
          "data that isn't Yaz0");
    Check(ErrorOf([&] { ntdlib::DecodeYaz0(std::vector<uint8_t>{'Y', 'a', 'z', '0', 0, 0, 0, 4}); }) == "not Yaz0 data",
          "a header shorter than 16 bytes");
}

void TestDecodeIfYaz0()
{
    const std::vector<uint8_t> plain = {'R', 'A', 'R', 'C'};
    const std::vector<uint8_t> packed = Yaz0Block(4, {0xF0, 'R', 'A', 'R', 'C'});
    Check(ntdlib::IsYaz0(packed) && !ntdlib::IsYaz0(plain), "IsYaz0 checks the magic");

    std::vector<uint8_t> decoded;
    const std::span<const uint8_t> same = ntdlib::DecodeIfYaz0(plain, decoded);
    Check(same.data() == plain.data() && decoded.empty(), "DecodeIfYaz0 gives data that isn't Yaz0 as it is");
    const std::span<const uint8_t> unpacked = ntdlib::DecodeIfYaz0(packed, decoded);
    Check(unpacked.data() == decoded.data() && Text(unpacked) == "RARC", "DecodeIfYaz0 decodes Yaz0 data");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("yaz0",
                                    []
                                    {
                                        TestRepeats();
                                        TestDamaged();
                                        TestDecodeIfYaz0();
                                    });
}
