// SPDX-License-Identifier: MIT

// Checks the DEFLATE and zlib decoder in inflate.h: stored, fixed and dynamic blocks, a wrong checksum, streams cut
// short, the zlib header's checks, a block of an unknown type, a single distance code of one bit, and a raw stream with
// the bytes that it uses. Each failure is checked by its message.

#include "ntdlib/inflate.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

namespace
{

using ntdlib::DecodeDeflate;
using ntdlib::ZlibDecompress;

void TestInflate()
{
    static const uint8_t kStored[] = {0x78, 0x01, 0x01, 0x15, 0x00, 0xEA, 0xFF, 0x47, 0x42, 0x41, 0x20,
                                      0x73, 0x6F, 0x75, 0x6E, 0x64, 0x20, 0x64, 0x72, 0x69, 0x76, 0x65,
                                      0x72, 0x20, 0x74, 0x65, 0x73, 0x74, 0x4E, 0xB2, 0x07, 0xA0};
    static const uint8_t kFixed[] = {0x78, 0x01, 0x73, 0x77, 0x72, 0x54, 0x28, 0xCE, 0x2F, 0xCD,
                                     0x4B, 0x51, 0x48, 0x29, 0xCA, 0x2C, 0x4B, 0x2D, 0x52, 0x28,
                                     0x49, 0x2D, 0x2E, 0x01, 0x00, 0x4E, 0xB2, 0x07, 0xA0};
    static const uint8_t kDynamic[] = {
        0x78, 0xDA, 0x4D, 0xCD, 0x41, 0x0E, 0x80, 0x30, 0x08, 0x04, 0xC0, 0xAF, 0xEC, 0xD7, 0x6A, 0xE5, 0x60,
        0x6C, 0x0B, 0x51, 0xE2, 0xFB, 0x8D, 0x0B, 0x89, 0x5C, 0x08, 0x81, 0x61, 0x59, 0xEA, 0x02, 0xBF, 0x5A,
        0x3F, 0x71, 0xB7, 0x69, 0x43, 0xF0, 0xE8, 0xD1, 0x05, 0x43, 0xD5, 0xE0, 0x32, 0x4D, 0xB1, 0x3E, 0xB2,
        0xC9, 0xDA, 0xD3, 0x05, 0x48, 0xCD, 0x25, 0x31, 0x3B, 0x96, 0x38, 0xE3, 0x45, 0x0D, 0xAB, 0x4F, 0x4A,
        0x26, 0x71, 0x38, 0x4E, 0x53, 0x04, 0xFF, 0x93, 0x4B, 0x5C, 0x88, 0x17, 0x22, 0x42, 0x44, 0xB8};
    const std::string kText = "GBA sound driver test";
    const std::string kWords =
        "note track sample voice loop tempo note bend track voice sample note loop note note tempo bend voice loop "
        "track sample note bend tempo voice note sample track loop note bend voice sample";

    // A dynamic block holding "A", with a distance code that no symbol uses: a single code, one bit long.
    static const uint8_t kOneBitDistance[] = {0x78, 0x01, 0x05, 0xC0, 0x01, 0x09, 0x00, 0x00, 0x00, 0x80,
                                              0xA0, 0x6D, 0xFE, 0x3F, 0xA5, 0x02, 0x00, 0x42, 0x00, 0x42};

    std::vector<uint8_t> out;
    std::string err;

    XSFLIB_CHECK(ZlibDecompress(kStored, sizeof kStored, out, err));
    XSFLIB_CHECK(std::string(out.begin(), out.end()) == kText);
    XSFLIB_CHECK(ZlibDecompress(kFixed, sizeof kFixed, out, err));
    XSFLIB_CHECK(std::string(out.begin(), out.end()) == kText);
    XSFLIB_CHECK(ZlibDecompress(kDynamic, sizeof kDynamic, out, err));
    XSFLIB_CHECK(std::string(out.begin(), out.end()) == kWords);

    std::vector<uint8_t> bad(kDynamic, kDynamic + sizeof kDynamic);
    bad.back() ^= 1; // Adler-32 mismatch

    XSFLIB_CHECK(!ZlibDecompress(bad.data(), bad.size(), out, err) && err == "zlib checksum mismatch");
    XSFLIB_CHECK(!ZlibDecompress(kDynamic, 20, out, err) && err == "truncated DEFLATE stream");
    XSFLIB_CHECK(!ZlibDecompress(kDynamic, sizeof kDynamic - 4, out, err) &&
                 err == "zlib stream ends before its checksum");

    // The header: at least 6 bytes, the method 8 (DEFLATE), a check that makes the first two bytes a multiple of 31,
    // and no preset dictionary. 0x78BB passes the check with the dictionary's flag set.
    static const uint8_t kMethod7[] = {0x77, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t kDictionary[] = {0x78, 0xBB, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    XSFLIB_CHECK(!ZlibDecompress(kStored, 5, out, err) && err == "zlib stream too short");
    XSFLIB_CHECK(!ZlibDecompress(kMethod7, sizeof kMethod7, out, err) && err == "not a zlib stream");
    XSFLIB_CHECK(!ZlibDecompress(kDictionary, sizeof kDictionary, out, err) &&
                 err == "zlib preset dictionaries aren't supported");

    // A last block of type 3, which DEFLATE doesn't define: the bits 1, then 11.
    static const uint8_t kType3[] = {0x78, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
    XSFLIB_CHECK(!ZlibDecompress(kType3, sizeof kType3, out, err) && err == "invalid DEFLATE block type");

    // A single code that doesn't fill its code space has to be one bit long. zlib and puff both require that. The same
    // block with a distance code of two bits is corrupt.
    std::vector<uint8_t> two_bit(kOneBitDistance, kOneBitDistance + sizeof kOneBitDistance);
    two_bit[14] = 0x65;

    XSFLIB_CHECK(ZlibDecompress(kOneBitDistance, sizeof kOneBitDistance, out, err));
    XSFLIB_CHECK(std::string(out.begin(), out.end()) == "A");
    XSFLIB_CHECK(!ZlibDecompress(two_bit.data(), two_bit.size(), out, err) && err == "corrupt DEFLATE stream");

    // The DEFLATE stream inside a zlib stream: DecodeDeflate stops at the end of the last block, before the Adler-32.
    std::size_t used = 0;
    XSFLIB_CHECK(DecodeDeflate(std::span(kFixed).subspan(2), out, used, err));
    XSFLIB_CHECK(std::string(out.begin(), out.end()) == kText && used == sizeof kFixed - 6);
    XSFLIB_CHECK(!DecodeDeflate(std::span(kDynamic).subspan(2, 18), out, used, err)); // truncated
}

} // namespace

int main()
{
    TestInflate();

    return xsflib::testing::Summary("inflate");
}
