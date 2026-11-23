// SPDX-License-Identifier: MIT

// Tests of the decompressed ARM9 binary and overlays (ntdlib/ntr/code.h), on a hand-made compressed stream and on
// images that testing/nds_files.h builds. Then the module parameters and the start-up decompressor, on hand-made code.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
namespace ntr = ntdlib::ntr;

using Bytes = std::vector<uint8_t>;

Bytes Text(const std::string& text)
{
    return {text.begin(), text.end()};
}

// "HEAD", then a compressed part that gives "xyz" six times. Read from its end, the stream's flag byte (0x10) gives
// three bytes to copy ('z', 'y', 'x') and a reference (0xC000): 15 bytes from 3 bytes above. The footer gives the
// compressed part's 14 bytes, the footer's 8, and 4 bytes more once decompressed.
Bytes Compressed()
{
    Bytes data = Text("HEAD");
    const Bytes stream = {0x00, 0xC0, 'x', 'y', 'z', 0x10};
    data.insert(data.end(), stream.begin(), stream.end());
    data.insert(data.end(), {14, 0, 0, 8, 4, 0, 0, 0});

    return data;
}

Bytes Decompressed()
{
    Bytes data = Text("HEAD");
    for (int i = 0; i < 6; i++)
    {
        const Bytes xyz = Text("xyz");
        data.insert(data.end(), xyz.begin(), xyz.end());
    }

    return data;
}

void TestUncompressBackward()
{
    XSFLIB_CHECK(ntr::UncompressBackward(Compressed()) == Decompressed());
}

// A footer that gives more than the data, a reference above the output, a footer that asks for one byte more than
// kMaxUncompressedBytes, and data shorter than a footer.
void TestDamagedData()
{
    Bytes too_long = Compressed();
    too_long[too_long.size() - 8] = 40;
    Bytes too_far = Compressed();
    too_far[4] = 0xFF;
    too_far[5] = 0xCF;
    Bytes too_large = Compressed();
    const std::size_t growth = ntr::kMaxUncompressedBytes - too_large.size() + 1;
    xsflib::StoreLe32(&too_large[too_large.size() - 4], static_cast<uint32_t>(growth));

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::UncompressBackward(too_long); }) ==
                 "the compressed binary is damaged");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::UncompressBackward(too_far); }) ==
                 "the compressed binary is damaged");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::UncompressBackward(too_large); }) ==
                 "the compressed binary is damaged");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { ntr::UncompressBackward(Bytes(4, 0)); }) ==
                 "the compressed binary is damaged");
}

// An ARM9 binary whose module parameters give the compressed part's end, followed by `after`.
Bytes CompressedArm9(const Bytes& after)
{
    Bytes params(0x24, 0);
    Bytes binary = Compressed();
    binary.insert(binary.begin(), params.begin(), params.end());
    xsflib::StoreLe32(&binary[0x14], 0x02000000 + static_cast<uint32_t>(binary.size()));
    xsflib::StoreLe32(&binary[0x1C], 0xDEC00621);
    xsflib::StoreLe32(&binary[0x20], 0x2106C0DE);
    binary.insert(binary.end(), after.begin(), after.end());

    return binary;
}

void TestArm9Code()
{
    nds_files::Image compressed;
    compressed.arm9 = CompressedArm9(Text("tail"));
    const Bytes plain_binary = Text("plain ARM9 code");
    nds_files::Image plain;
    plain.arm9 = plain_binary;
    const auto [compressed_image, compressed_ids] = nds_files::BuildImage(compressed);
    const auto [plain_image, plain_ids] = nds_files::BuildImage(plain);
    Bytes expected = CompressedArm9({});
    expected.resize(0x24);
    const Bytes body = Decompressed();
    expected.insert(expected.end(), body.begin(), body.end());
    const Bytes tail = Text("tail");
    expected.insert(expected.end(), tail.begin(), tail.end());

    const ntr::LoadedCode code = ntr::Arm9Code(ntr::Rom::Parse(compressed_image));
    const ntr::LoadedCode as_stored = ntr::Arm9Code(ntr::Rom::Parse(plain_image));

    XSFLIB_CHECK(code.address == 0x02000000);
    XSFLIB_CHECK(code.bytes == expected);
    XSFLIB_CHECK(!code.overlay);
    XSFLIB_CHECK(as_stored.bytes == plain_binary);
}

// The parameters of a binary whose module parameters start at 0x10, written back with new values.
void TestModuleParams()
{
    Bytes binary(0x10, 0);
    const Bytes compressed = CompressedArm9({});
    binary.insert(binary.end(), compressed.begin(), compressed.end());
    xsflib::StoreLe32(&binary[0x10], 0x02000100);
    xsflib::StoreLe32(&binary[0x28], 0x04007530);
    Bytes written = binary;

    const std::optional<ntr::ModuleParams> params = ntr::ReadModuleParams(binary);
    ntr::ModuleParams changed = params.value();
    changed.compressed_end = 0;
    changed.static_bss_end = 0x02001234;
    ntr::WriteModuleParams(written, changed);
    ntr::ModuleParams past = changed;
    past.offset = binary.size() - 0x20;

    XSFLIB_CHECK(params->offset == 0x10);
    XSFLIB_CHECK(params->autoload_list == 0x02000100);
    XSFLIB_CHECK(params->compressed_end == 0x02000000 + compressed.size());
    XSFLIB_CHECK(params->sdk_version == 0x04007530);
    XSFLIB_CHECK(ntr::ReadModuleParams(written) == changed);
    XSFLIB_CHECK(xsflib::LoadLe32(&written[0x2C]) == 0xDEC00621);
    XSFLIB_CHECK(!ntr::ReadModuleParams(Text("no parameters")));
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { ntr::WriteModuleParams(written, past); }) ==
                 "the module parameters lie past the binary's end");
}

// The decompressed binary with the compressed part's end set to 0, and a binary that isn't compressed, as it is.
void TestUncompressedArm9()
{
    nds_files::Image compressed;
    compressed.arm9 = CompressedArm9(Text("tail"));
    nds_files::Image plain;
    plain.arm9 = Text("plain ARM9 code");
    const auto [compressed_image, compressed_ids] = nds_files::BuildImage(compressed);
    const auto [plain_image, plain_ids] = nds_files::BuildImage(plain);
    Bytes expected = ntr::Arm9Code(ntr::Rom::Parse(compressed_image)).bytes;
    xsflib::StoreLe32(&expected[0x14], 0);

    const Bytes binary = ntr::UncompressedArm9(ntr::Rom::Parse(compressed_image));

    XSFLIB_CHECK(binary == expected);
    XSFLIB_CHECK(ntr::ReadModuleParams(binary)->compressed_end == 0);
    XSFLIB_CHECK(ntr::UncompressedArm9(ntr::Rom::Parse(plain_image)) == plain.arm9);
}

// Entry code that loads the word at 0x14 of the parameters and calls the decompressor. The decompressor returns at once
// for 0.
Bytes StartupCode(uint32_t branch_to_exit)
{
    const uint32_t words[] = {
        0xE5910014,     // ldr r0, [r1, #0x14]
        0xEB000000,     // bl 0x0200000C
        0xEAFFFFFE,     // b .
        0xE3500000,     // cmp r0, #0
        branch_to_exit, // beq 0x0200001C
        0xE1A00000,     // nop
        0xE1A00000,     // nop
        0xE12FFF1E,     // bx lr
    };
    Bytes code(sizeof(words), 0);
    for (std::size_t i = 0; i < std::size(words); i++)
    {
        xsflib::StoreLe32(&code[4 * i], words[i]);
    }

    return code;
}

// The decompressor's call and body, and code whose branch doesn't reach a return or which has no load of the word.
void TestFindStartupDecompressor()
{
    const ntr::LoadedCode code{.address = 0x02000000, .bytes = StartupCode(0x0A000001)};
    const ntr::LoadedCode no_return{.address = 0x02000000, .bytes = StartupCode(0x0A000000)};
    ntr::LoadedCode no_load = code;
    xsflib::StoreLe32(&no_load.bytes[0], 0xE5910010);

    const ntr::StartupDecompressor expected{
        .call = 0x02000004, .routine = 0x0200000C, .body = 0x02000014, .exit = 0x0200001C};

    const std::optional<ntr::StartupDecompressor> found = ntr::FindStartupDecompressor(code, 0x02000000);

    XSFLIB_CHECK(found == expected);
    XSFLIB_CHECK(!ntr::FindStartupDecompressor(no_return, 0x02000000));
    XSFLIB_CHECK(!ntr::FindStartupDecompressor(no_load, 0x02000000));
    XSFLIB_CHECK(!ntr::FindStartupDecompressor(code, 0x02000020));
}

// An overlay whose flags mark it compressed, and the same overlay with damaged data. The damaged overlay is left out.
void TestOverlays()
{
    nds_files::Image spec;
    spec.arm9_overlay = Compressed();
    auto [image, ids] = nds_files::BuildImage(spec);
    const uint32_t table = xsflib::LoadLe32(&image[0x50]);
    xsflib::StoreLe32(&image[table + 28], 1u << 24 | static_cast<uint32_t>(spec.arm9_overlay.size()));
    nds_files::Image damaged_spec = spec;
    damaged_spec.arm9_overlay[damaged_spec.arm9_overlay.size() - 8] = 40;
    auto [damaged, damaged_ids] = nds_files::BuildImage(damaged_spec);
    xsflib::StoreLe32(&damaged[table + 28], 1u << 24);

    const std::vector<ntr::LoadedCode> code = ntr::Arm9CodeAndOverlays(ntr::Rom::Parse(image));
    const std::vector<ntr::LoadedCode> without = ntr::Arm9CodeAndOverlays(ntr::Rom::Parse(damaged));

    XSFLIB_CHECK(code.size() == 2);
    XSFLIB_CHECK(code.at(1).address == 0x02100000);
    XSFLIB_CHECK(code.at(1).overlay == 0u);
    XSFLIB_CHECK(code.at(1).bytes == Decompressed());
    XSFLIB_CHECK(without.size() == 1);
}

// An overlay's bytes come from the compressed size in its flags. The size leaves out padding after the compressed part.
// Cleared flags load the file as it is.
void TestOverlayFlags()
{
    Bytes padded = Compressed();
    padded.insert(padded.end(), 4, 0xFF);
    ntr::Overlay overlay{.flags = 1u << 24 | static_cast<uint32_t>(Compressed().size())};
    XSFLIB_CHECK(ntr::OverlayCompressed(overlay));
    XSFLIB_CHECK(ntr::OverlayBytes(padded, overlay) == Decompressed());
    XSFLIB_CHECK(ntr::OverlayBytes(Compressed(), {.flags = 1u << 24}) == Decompressed());
    XSFLIB_CHECK(ntr::OverlayBytes(padded, {}) == padded);

    nds_files::Image spec;
    spec.arm9_overlay = Compressed();
    auto [image, ids] = nds_files::BuildImage(spec);
    const uint32_t table = xsflib::LoadLe32(&image[0x50]);
    xsflib::StoreLe32(&image[table + 28], 1u << 24);
    ntr::ClearArm9OverlayFlags(image, 0);
    const ntr::Rom rom = ntr::Rom::Parse(image);
    XSFLIB_CHECK(rom.Arm9Overlays().at(0).flags == 0 && !ntr::OverlayCompressed(rom.Arm9Overlays().at(0)));
    XSFLIB_CHECK(!ErrorOf<std::out_of_range>([&] { ntr::ClearArm9OverlayFlags(image, 1); }).empty());
}

} // namespace

int main()
{
    TestUncompressBackward();
    TestDamagedData();
    TestArm9Code();
    TestModuleParams();
    TestUncompressedArm9();
    TestFindStartupDecompressor();
    TestOverlays();
    TestOverlayFlags();

    return xsflib::testing::Summary("DS code");
}
