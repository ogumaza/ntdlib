// SPDX-License-Identifier: MIT

// Tests of the writer of the DS's cartridge images (ntdlib/ntr/rom_writer.h), on images that testing/nds_files.h
// builds. A DSi image gets a digest made with a made-up key.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/testing/nds_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
using ntdlib::ntr::DsiKeys;
using ntdlib::ntr::HmacSha1;
using ntdlib::ntr::Rom;
using ntdlib::ntr::WithFiles;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kSector = 0x400;
constexpr uint32_t kBlockSectors = 2;
constexpr std::size_t kHash = 20;

nds_files::Image Spec()
{
    nds_files::Image spec;
    spec.files = {{"sound/sound_data.sdat", {1, 2, 3}},
                  {"readme.txt", {4, 5}},
                  {"sound/bgm/song.bin", {6}},
                  {"data/a/b/c.bin", Bytes(700, 7)}};

    return spec;
}

DsiKeys MadeUpKeys()
{
    DsiKeys keys;
    for (std::size_t i = 0; i < keys.digest.size(); i++)
    {
        keys.digest[i] = static_cast<uint8_t>(0x5A ^ (i * 7));
    }

    return keys;
}

uint32_t Load32(std::span<const uint8_t> b, std::size_t at)
{
    return xsflib::LoadLe32(&b[at]);
}

Bytes Hash(std::span<const uint8_t> key, std::span<const uint8_t> bytes)
{
    const xsflib::Sha1::Digest d = HmacSha1(key, bytes);
    return Bytes(d.begin(), d.end());
}

Bytes Slice(std::span<const uint8_t> b, std::size_t at, std::size_t size)
{
    return Bytes(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + size));
}

// A DSi-enhanced image laid out as the tools lay it out, with a digest made with `keys`.
Bytes DsiImage(const DsiKeys& keys)
{
    nds_files::Image spec = Spec();
    spec.unit_code = 2;
    Bytes image = nds_files::BuildImage(spec).first;
    const uint32_t ntr_end = (Load32(image, 0x80) + kSector - 1) / kSector * kSector;
    image.resize(ntr_end, 0xFF);

    // The DSi's part: 0x3000 bytes, the ARM9i binary, then the ARM7i binary on a multiple of 0x1000 bytes.
    const Bytes arm9i(0x500, 0x95);
    const Bytes arm7i(0x300, 0x75);
    const uint32_t twl_bytes = (0x1000 + 0x300 + kSector - 1) / kSector * kSector;
    const uint32_t sectors = (ntr_end - 0x4000) / kSector + twl_bytes / kSector;
    const uint32_t entries = (sectors + kBlockSectors - 1) / kBlockSectors * kBlockSectors;
    const uint32_t blocks_at = (ntr_end + entries * kHash + 0x1FF) / 0x200 * 0x200;
    const uint32_t used = (blocks_at + entries / kBlockSectors * kHash + 0x1FF) / 0x200 * 0x200;
    const uint32_t dsi = (used + 0x7FFFF) / 0x80000 * 0x80000;
    Bytes part(0x3000 + twl_bytes, 0xFF);
    std::fill_n(part.begin(), 0x3000, 0x33);
    std::ranges::copy(arm9i, part.begin() + 0x3000);
    std::ranges::copy(arm7i, part.begin() + 0x4000);

    const auto put = [&image](std::size_t at, uint32_t value)
    {
        xsflib::StoreLe32(&image[at], value);
    };
    put(0x80, used);
    xsflib::StoreLe16(&image[0x90], static_cast<uint16_t>(dsi / 0x80000));
    xsflib::StoreLe16(&image[0x92], static_cast<uint16_t>(dsi / 0x80000));
    put(0x1C0, dsi + 0x3000);
    put(0x1CC, static_cast<uint32_t>(arm9i.size()));
    put(0x1D0, dsi + 0x4000);
    put(0x1DC, static_cast<uint32_t>(arm7i.size()));
    put(0x1E0, 0x4000);
    put(0x1E4, ntr_end - 0x4000);
    put(0x1E8, dsi + 0x3000);
    put(0x1EC, twl_bytes);
    put(0x1F0, ntr_end);
    put(0x1F4, entries * kHash);
    put(0x1F8, blocks_at);
    put(0x1FC, entries / kBlockSectors * kHash);
    put(0x200, kSector);
    put(0x204, kBlockSectors);
    put(0x210, dsi + 0x3000 + twl_bytes);
    put(0x220, dsi + 0x3000);
    put(0x224, 0x500);
    std::fill_n(image.begin() + 0xF80, 0x80, 0x52); // an RSA signature that nothing checks here
    image[0x14] = 3;                                // a chip of 1 MB

    // The hashes of the sectors of both regions, of their blocks, and the master hash.
    Bytes table;
    for (uint32_t at = 0x4000; at < ntr_end; at += kSector)
    {
        const Bytes h = Hash(keys.digest, std::span(image).subspan(at, kSector));
        table.insert(table.end(), h.begin(), h.end());
    }
    for (uint32_t at = 0x3000; at < part.size(); at += kSector)
    {
        const Bytes h = Hash(keys.digest, std::span(part).subspan(at, kSector));
        table.insert(table.end(), h.begin(), h.end());
    }
    table.resize(entries * kHash, 0);
    Bytes block_table;
    for (std::size_t b = 0; b < entries / kBlockSectors; b++)
    {
        const Bytes h = Hash(keys.digest, std::span(table).subspan(b * kBlockSectors * kHash, kBlockSectors * kHash));
        block_table.insert(block_table.end(), h.begin(), h.end());
    }
    std::ranges::copy(Hash(keys.digest, block_table), image.begin() + 0x328);
    image.insert(image.end(), table.begin(), table.end());
    image.resize(blocks_at, 0xFF);
    image.insert(image.end(), block_table.begin(), block_table.end());
    image.resize(dsi, 0xFF);
    image.insert(image.end(), part.begin(), part.end());
    xsflib::StoreLe16(&image[0x15E], ntdlib::ntr::Crc16(std::span(image).first(0x15E)));

    return image;
}

// Checks every hash of a DSi image's digest against a plain HMAC-SHA1 of its bytes. Hashes that hold zeros count as
// stale, and give `zeros`.
bool DigestHolds(std::span<const uint8_t> image, const DsiKeys& keys, std::size_t& zeros)
{
    zeros = 0;
    const auto check = [&](std::span<const uint8_t> hash, std::span<const uint8_t> bytes)
    {
        if (std::ranges::all_of(hash, [](uint8_t b) { return b == 0; }))
        {
            zeros++;
            return true;
        }
        return Bytes(hash.begin(), hash.end()) == Hash(keys.digest, bytes);
    };
    bool holds = true;
    const uint32_t ntr = Load32(image, 0x1E0);
    const uint32_t ntr_sectors = Load32(image, 0x1E4) / kSector;
    const uint32_t twl = Load32(image, 0x1E8);
    const uint32_t twl_sectors = Load32(image, 0x1EC) / kSector;
    const std::span<const uint8_t> table = image.subspan(Load32(image, 0x1F0), Load32(image, 0x1F4));
    const std::span<const uint8_t> blocks = image.subspan(Load32(image, 0x1F8), Load32(image, 0x1FC));
    for (uint32_t i = 0; i < ntr_sectors + twl_sectors; i++)
    {
        const std::size_t at =
            i < ntr_sectors ? ntr + std::size_t{i} * kSector : twl + std::size_t{i - ntr_sectors} * kSector;
        holds &= check(table.subspan(std::size_t{i} * kHash, kHash), image.subspan(at, kSector));
    }
    for (std::size_t b = 0; b < blocks.size() / kHash; b++)
    {
        holds &=
            check(blocks.subspan(b * kHash, kHash), table.subspan(b * kBlockSectors * kHash, kBlockSectors * kHash));
    }
    holds &= check(image.subspan(0x328, kHash), blocks);

    return holds;
}

// Every file of `image` holds the bytes that `files` gives it, or those of `old`.
bool FilesHold(const Rom& image, const Rom& old, const std::map<uint32_t, Bytes>& files)
{
    bool holds = image.Files().size() == old.Files().size();
    for (const ntdlib::ntr::RomFile& file : old.Files())
    {
        const auto data = image.FileData(file.id);
        const auto change = files.find(file.id);
        const auto old_data = old.FileData(file.id);
        const Bytes want = change != files.end() ? change->second : Bytes(old_data.begin(), old_data.end());
        holds &= Bytes(data.begin(), data.end()) == want && image.Files()[file.id].path == file.path;
    }

    return holds;
}

void TestHmacAndCrc()
{
    // RFC 2202's test cases 1, 2 and 6 for HMAC-SHA1, and the CRC-16 of "123456789" from 0xFFFF.
    const auto hex = [](std::span<const uint8_t> key, const std::string& text)
    {
        const std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        return xsflib::Sha1::Hex(HmacSha1(key, data));
    };
    const std::string jefe = "Jefe";

    XSFLIB_CHECK(hex(Bytes(20, 0x0B), "Hi There") == "b617318655057264e28bc0b6fb378c8ef146be00");
    XSFLIB_CHECK(hex(std::span(reinterpret_cast<const uint8_t*>(jefe.data()), jefe.size()),
                     "what do ya want for nothing?") == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    XSFLIB_CHECK(hex(Bytes(80, 0xAA), "Test Using Larger Than Block-Size Key - Hash Key First") ==
                 "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    const std::string digits = "123456789";
    XSFLIB_CHECK(ntdlib::ntr::Crc16(std::span(reinterpret_cast<const uint8_t*>(digits.data()), digits.size())) ==
                 0x4B37);
}

void TestUnchanged()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());
    const Rom rom = Rom::Parse(image);
    const auto sdat = rom.FileData(ids[0]);

    const auto none = WithFiles(image, {});
    const auto same = WithFiles(image, {{ids[0], Bytes(sdat.begin(), sdat.end())}});
    const Bytes dsi = DsiImage(MadeUpKeys());
    const auto dsi_same = WithFiles(dsi, {{ids[1], {4, 5}}});

    XSFLIB_CHECK(none.image == image && none.stale.empty());
    XSFLIB_CHECK(same.image == image && same.stale.empty());
    XSFLIB_CHECK(dsi_same.image == dsi && dsi_same.stale.empty());
}

void TestGrowAndShrink()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());
    const Rom old = Rom::Parse(image);
    const std::map<uint32_t, Bytes> grown = {{ids[0], Bytes(0x300, 0xA1)}};
    const std::map<uint32_t, Bytes> shrunk = {{ids[3], Bytes(10, 0xC3)}};

    const auto bigger = WithFiles(image, grown);
    const auto smaller = WithFiles(image, shrunk);
    const auto back = WithFiles(bigger.image, {{ids[0], {1, 2, 3}}});

    const Rom b = Rom::Parse(bigger.image);
    XSFLIB_CHECK(FilesHold(b, old, grown) && bigger.stale.empty());
    XSFLIB_CHECK(b.Files()[ids[0]].start == old.Files()[ids[0]].start);
    for (const uint32_t id : {ids[1], ids[2], ids[3]})
    {
        XSFLIB_CHECK(b.Files()[id].start == old.Files()[id].start + 0x200);
    }
    XSFLIB_CHECK(b.Header().used_size == old.Header().used_size + 0x200);
    XSFLIB_CHECK(bigger.image.size() == image.size() + 0x200);
    XSFLIB_CHECK(std::all_of(bigger.image.begin() + b.Files()[ids[0]].end,
                             bigger.image.begin() + b.Files()[ids[1]].start, [](uint8_t x) { return x == 0xFF; }));
    XSFLIB_CHECK(xsflib::LoadLe16(&bigger.image[0x15E]) == ntdlib::ntr::Crc16(std::span(bigger.image).first(0x15E)));
    // The builder writes no CRC.
    Bytes with_crc = image;
    xsflib::StoreLe16(&with_crc[0x15E], ntdlib::ntr::Crc16(std::span(image).first(0x15E)));
    XSFLIB_CHECK(back.image == with_crc && back.stale.empty());

    // The last file moves the data's end by the change in its size alone.
    const Rom s = Rom::Parse(smaller.image);
    XSFLIB_CHECK(FilesHold(s, old, shrunk));
    XSFLIB_CHECK(s.Header().used_size == s.Files()[ids[3]].start + 10 && smaller.image.size() == s.Header().used_size);
}

void TestCapacityAndSignature()
{
    auto [image, ids] = nds_files::BuildImage(Spec());
    const uint32_t used = Load32(image, 0x80);
    Bytes signature(0x88, 0x5C);
    signature[0] = 'a';
    signature[1] = 'c';
    image.insert(image.end(), signature.begin(), signature.end());
    image.resize(0x20000, 0xFF); // a full chip of 128 KB

    const auto grown = WithFiles(image, {{ids[3], Bytes(0x20000, 0xC4)}});

    const Rom rom = Rom::Parse(grown.image);
    XSFLIB_CHECK(grown.image.size() == 0x40000 && grown.image[0x14] == 1);
    XSFLIB_CHECK(rom.Header().used_size == used + 0x20000 - 700);
    XSFLIB_CHECK(Slice(grown.image, rom.Header().used_size, 0x88) == signature);
    XSFLIB_CHECK(std::all_of(grown.image.begin() + rom.Header().used_size + 0x88, grown.image.end(),
                             [](uint8_t x) { return x == 0xFF; }));
    XSFLIB_CHECK(grown.stale.size() == 1 && grown.stale[0].what == "the signature for download play" &&
                 grown.stale[0].offset == rom.Header().used_size);
}

void TestDsi()
{
    const DsiKeys keys = MadeUpKeys();
    const Bytes image = DsiImage(keys);
    const Rom old = Rom::Parse(image);
    const uint32_t sdat = old.Find("sound/sound_data.sdat")->id;
    const std::map<uint32_t, Bytes> files = {{sdat, Bytes(0x80000, 0xD1)}};
    std::size_t zeros = 0;
    XSFLIB_CHECK(DigestHolds(image, keys, zeros) && zeros == 0);

    // With the key, every hash holds, and the DSi's part moves to the next multiple of 0x80000 bytes.
    const auto keyed = WithFiles(image, files, keys);
    const Rom rom = Rom::Parse(keyed.image);
    XSFLIB_CHECK(FilesHold(rom, old, files));
    XSFLIB_CHECK(DigestHolds(keyed.image, keys, zeros) && zeros == 0);
    XSFLIB_CHECK(Load32(keyed.image, 0x1C0) == Load32(image, 0x1C0) + 0x80000);
    XSFLIB_CHECK(Load32(keyed.image, 0x210) == Load32(image, 0x210) + 0x80000);
    XSFLIB_CHECK(Load32(keyed.image, 0x220) == Load32(keyed.image, 0x1C0));
    XSFLIB_CHECK(xsflib::LoadLe16(&keyed.image[0x92]) * 0x80000u == Load32(image, 0x1C0) + 0x80000 - 0x3000);
    XSFLIB_CHECK(Bytes(rom.Arm9i().begin(), rom.Arm9i().end()) == Bytes(old.Arm9i().begin(), old.Arm9i().end()));
    XSFLIB_CHECK(Bytes(rom.Arm7i().begin(), rom.Arm7i().end()) == Bytes(old.Arm7i().begin(), old.Arm7i().end()));
    XSFLIB_CHECK(keyed.image.size() == Load32(keyed.image, 0x210));
    XSFLIB_CHECK(keyed.stale.size() == 1 && keyed.stale[0].what == "the header's RSA signature");

    // Without it, the hashes that the change touches hold zeros, and the others still hold.
    const auto unkeyed = WithFiles(image, files);
    XSFLIB_CHECK(unkeyed.image.size() == keyed.image.size());
    XSFLIB_CHECK(DigestHolds(unkeyed.image, keys, zeros) && zeros > 0);
    std::map<std::string, ntdlib::ntr::StaleHash> stale;
    for (const ntdlib::ntr::StaleHash& hash : unkeyed.stale)
    {
        stale[hash.what] = hash;
    }
    // The FAT's entries of the files after the archive change first.
    const uint32_t first_sector = (old.Header().fat_offset - 0x4000) / kSector;
    XSFLIB_CHECK(stale.size() == 4 && stale.contains("the digest's master hash") &&
                 stale.contains("the header's RSA signature"));
    XSFLIB_CHECK(stale["the digest's sector hashes"].offset == Load32(unkeyed.image, 0x1F0) + first_sector * kHash);
    XSFLIB_CHECK(stale["the digest's sector hashes"].count + stale["the digest's block hashes"].count + 1 == zeros);

    // A small change keeps the DSi's part in its place.
    const auto small = WithFiles(image, {{sdat, Bytes(0x900, 0xD2)}}, keys);
    XSFLIB_CHECK(DigestHolds(small.image, keys, zeros) && zeros == 0);
    XSFLIB_CHECK(Load32(small.image, 0x1C0) == Load32(image, 0x1C0) && small.image.size() == image.size());
}

void TestRefusals()
{
    const auto [image, ids] = nds_files::BuildImage(Spec());
    Bytes dsi = DsiImage(MadeUpKeys());
    xsflib::StoreLe32(&dsi[0x1F8], Load32(dsi, 0x1F8) + 0x200);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithFiles(image, {{6, {1}}}); }) ==
                 "file 6 lies past the file allocation table");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithFiles(image, {{0, {1}}}); }) ==
                 "file 0 is an overlay's: its table describes it");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { WithFiles(dsi, {{ids[0], {1}}}); }) ==
                 "the DSi image's digest isn't laid out as the SDK's tools lay it out");
}

} // namespace

int main()
{
    TestHmacAndCrc();
    TestUnchanged();
    TestGrowAndShrink();
    TestCapacityAndSignature();
    TestDsi();
    TestRefusals();

    return xsflib::testing::Summary("DS ROM writer");
}
