// SPDX-License-Identifier: MIT

// Builds a small Wii disc and stores it in every container format that the reader supports. The writers here follow the
// formats' descriptions. The formats are ISO, with and without hashed sectors, GCZ, CISO, WBFS (whole and split), WDF
// versions 1 and 2 (whole and split), WIA with each of its compression methods, and RVZ with each of its methods, three
// chunk sizes and packed junk. WIA and RVZ store the partition separately or as raw data. Checks that LoadGame reads
// the same system files and files from each image, from a WBFS whose decrypted partition is still marked encrypted, and
// from extracted discs. Also checks that LoadGame rejects an encrypted partition, a GameCube disc and a file that isn't
// a disc.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "disc_files.h"
#include "disc_writers.h"
#include "ntdlib/disc/compression.h"
#include "ntdlib/disc/containers.h"
#include "ntdlib/disc/game.h"
#include "temp_folder.h"

namespace
{

using disc_files::CheckFiles;
using disc_files::CheckRefused;
using disc_files::Fill;
using disc_files::WriteFile;
using disc_writers::AddRawData;
using disc_writers::AllZero;
using disc_writers::BuildGame;
using disc_writers::Bytes;
using disc_writers::Ciso;
using disc_writers::Game;
using disc_writers::GameLayout;
using disc_writers::Gcz;
using disc_writers::GroupData;
using disc_writers::Junk;
using disc_writers::kRvzMethods;
using disc_writers::kWiaMethods;
using disc_writers::Wdf;
using disc_writers::WiaContents;
using disc_writers::WiaFile;
using xsflib::testing::Check;
namespace fs = std::filesystem;

// How the tests load games: as a Wii rip tool named xyz would, with every container.
const ntdlib::disc::GameOptions kOptions{ntdlib::disc::Console::kWii, "xyz", {ntdlib::disc::OpenContainer}};
using ntdlib::disc::Method;

constexpr uint64_t kSector = 0x8000;
constexpr uint64_t kSectorData = 0x7C00;
constexpr uint64_t kPartition = 0x50000;             // where the game partition starts on the disc
constexpr uint64_t kDataOffset = 0x20000;            // and its data, from the partition's start
constexpr uint64_t kData = kPartition + kDataOffset; // its data on the disc

// The test game in a Wii disc's game partition.
const GameLayout kLayout = {
    .id = "RWST01",
    .magic_offset = 0x18,
    .magic = 0x5D1C9EA3,
    .shift = 2,
    .sound_folder = "Sound",
    .sound_file = "a.brsar",
    .sub_sound_file = "b.brsar",
};

// The test disc: the test game in a game partition, the partition's TMD, and the disc around it.
struct TestDisc : Game
{
    // How Iso writes the disc. `hashed` stores the partition's data in sectors with room for hashes. `encrypted` leaves
    // the disc header marking the partition as encrypted. Wiimm's tools do the same when they decrypt a partition.
    // `nkit` writes NKit's header. `scrambled`, with `hashed`, changes every byte of the partition's data as encryption
    // would. The partition is then actually encrypted.
    struct IsoOptions
    {
        bool hashed = false;
        bool encrypted = false;
        bool nkit = false;
        bool scrambled = false;
    };

    // The disc as an ISO image.
    Bytes Iso(IsoOptions options) const;

    Bytes tmd;
};

TestDisc BuildDisc()
{
    TestDisc disc{BuildGame(kLayout), Bytes(0x208, 0)};
    Fill(disc.tmd, 0, disc.tmd.size(), 4);

    return disc;
}

Bytes TestDisc::Iso(IsoOptions options) const
{
    const uint64_t sectors = (data.size() + kSectorData - 1) / kSectorData;
    const uint64_t data_size = options.hashed ? sectors * kSector : data.size();
    Bytes iso(kData + data_size, 0);

    // The disc header, with the flags that turn hashes and encryption off.
    std::copy_n(boot.begin(), 0x60, iso.begin());
    iso[0x60] = options.hashed ? 0 : 1;
    iso[0x61] = options.encrypted ? 0 : 1;
    if (options.nkit)
    {
        std::copy_n("NKIT v01", 8, iso.begin() + 0x200);
    }

    // The partition table, and the partition's header with the offsets of its TMD and data divided by 4.
    xsflib::StoreBe32(iso.data() + 0x40000, 1);
    xsflib::StoreBe32(iso.data() + 0x40004, 0x40020 / 4);
    xsflib::StoreBe32(iso.data() + 0x40020, kPartition / 4);
    xsflib::StoreBe32(iso.data() + 0x40024, 0);
    xsflib::StoreBe32(iso.data() + kPartition + 0x2A4, static_cast<uint32_t>(tmd.size()));
    xsflib::StoreBe32(iso.data() + kPartition + 0x2A8, 0x2C0 / 4);
    xsflib::StoreBe32(iso.data() + kPartition + 0x2B8, kDataOffset / 4);
    xsflib::StoreBe32(iso.data() + kPartition + 0x2BC, static_cast<uint32_t>(data_size / 4));
    std::copy(tmd.begin(), tmd.end(), iso.begin() + kPartition + 0x2C0);

    if (!options.hashed)
    {
        std::copy(data.begin(), data.end(), iso.begin() + kData);

        return iso;
    }

    // Each sector: 0x400 bytes of hashes and 0x7C00 of data. The hashes can be any bytes because nothing checks them.
    for (uint64_t s = 0; s < sectors; ++s)
    {
        uint8_t* sector = iso.data() + kData + s * kSector;
        std::fill_n(sector, 0x400, static_cast<uint8_t>(0xEE));
        const uint64_t n = std::min(kSectorData, data.size() - s * kSectorData);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(s * kSectorData), n, sector + 0x400);
    }

    if (options.scrambled)
    {
        std::for_each(iso.begin() + kData, iso.end(), [](uint8_t& b) { b ^= 0xA5; });
    }

    return iso;
}

// A WBFS file with one disc in its first slot: 512-byte sectors and 128 KiB blocks.
Bytes Wbfs(const Bytes& iso)
{
    const uint64_t kHdSector = 512;
    const uint64_t kBlock = 1 << 17;
    const uint64_t kBlocksPerDisc = 143432 * 2 / (kBlock / kSector);
    const uint64_t kInfoSize = (0x100 + kBlocksPerDisc * 2 + kHdSector - 1) / kHdSector * kHdSector;
    uint64_t next_block = (kHdSector + kInfoSize + kBlock - 1) / kBlock;
    Bytes wbfs(next_block * kBlock, 0);
    std::copy_n("WBFS", 4, wbfs.begin());
    wbfs[8] = 9;
    wbfs[9] = 17;
    wbfs[10] = 1;
    wbfs[12] = 1;
    std::copy_n(iso.begin(), 0x100, wbfs.begin() + kHdSector);
    for (uint64_t i = 0; i * kBlock < iso.size(); ++i)
    {
        const uint64_t size = std::min<uint64_t>(kBlock, iso.size() - i * kBlock);
        if (AllZero(iso, i * kBlock, size))
        {
            continue;
        }

        xsflib::StoreBe16(wbfs.data() + kHdSector + 0x100 + i * 2, static_cast<uint16_t>(next_block++));
        wbfs.insert(wbfs.end(), iso.begin() + static_cast<std::ptrdiff_t>(i * kBlock),
                    iso.begin() + static_cast<std::ptrdiff_t>(i * kBlock + size));
        wbfs.resize(next_block * kBlock, 0);
    }

    xsflib::StoreBe32(wbfs.data() + 4, static_cast<uint32_t>(wbfs.size() / kHdSector));

    return wbfs;
}

// A WIA or RVZ file of the disc. With `partitions`, WIA and RVZ store the game partition's data decrypted and without
// hashes. The disc header then marks the partition as encrypted. A real disc's header does the same. Without
// `partitions`, the whole disc is raw data. Dolphin does that when it converts an NKit image.
Bytes Wia(const TestDisc& disc, bool rvz, Method method, uint32_t chunk_size, bool partitions)
{
    const Bytes iso = disc.Iso({.encrypted = partitions, .nkit = !partitions});
    const uint64_t iso_size =
        partitions ? kData + (disc.data.size() + kSectorData - 1) / kSectorData * kSector : iso.size();
    WiaContents wia{.rvz = rvz,
                    .method = method,
                    .chunk_size = chunk_size,
                    .disc_type = 2,
                    .head = Bytes(iso.begin(), iso.begin() + 0x80),
                    .disc_size = iso_size,
                    .partitions = partitions ? 1u : 0u};

    // Raw data: from 0x80 up to the partition's data, or to the end. The partition's junk lies in the raw data when the
    // partition isn't stored separately.
    std::vector<Junk> raw_junk;
    if (!partitions)
    {
        for (const Junk& j : disc.junk)
        {
            raw_junk.push_back({kData + j.offset, j.size, j.seed});
        }
    }

    AddRawData(wia, iso, partitions ? kData : iso.size(), raw_junk);

    // The partition: two segments, the first a whole number of chunks covering the system files.
    if (partitions)
    {
        const uint64_t sectors_per_chunk = std::max<uint64_t>(1, chunk_size / kSector);
        const uint64_t lists = std::max<uint64_t>(1, chunk_size / 0x200000);
        const uint64_t sectors = (disc.data.size() + kSectorData - 1) / kSectorData;
        const uint64_t first_sectors =
            std::min(sectors, (3 + sectors_per_chunk - 1) / sectors_per_chunk * sectors_per_chunk);
        wia.partition_table.assign(0x30, 0);
        std::fill_n(wia.partition_table.begin(), 16, static_cast<uint8_t>(0xAB));
        uint64_t sector = 0;
        for (int s = 0; s < 2; ++s)
        {
            const uint64_t count = s == 0 ? first_sectors : sectors - first_sectors;
            const uint64_t group_count = (count + sectors_per_chunk - 1) / sectors_per_chunk;
            uint8_t* p = wia.partition_table.data() + 0x10 + s * 16;
            xsflib::StoreBe32(p, static_cast<uint32_t>(kData / kSector + sector));
            xsflib::StoreBe32(p + 4, static_cast<uint32_t>(count));
            xsflib::StoreBe32(p + 8, static_cast<uint32_t>(wia.groups.size()));
            xsflib::StoreBe32(p + 12, static_cast<uint32_t>(group_count));
            for (uint64_t g = 0; g < group_count; ++g)
            {
                const uint64_t first = sector + g * sectors_per_chunk;
                const uint64_t n = std::min(sectors_per_chunk, sector + count - first);
                GroupData group;
                group.lists.assign(lists * 2, 0);
                group.data.assign(n * kSectorData, 0);
                const uint64_t start = first * kSectorData;
                const uint64_t size = std::min<uint64_t>(n * kSectorData, disc.data.size() - start);
                std::copy_n(disc.data.begin() + static_cast<std::ptrdiff_t>(start), size, group.data.begin());
                for (const Junk& j : disc.junk)
                {
                    if (j.offset >= start && j.offset + j.size <= start + n * kSectorData)
                    {
                        group.junk.push_back({j.offset - start, j.size, j.seed});
                    }
                }

                wia.groups.push_back(group);
            }

            sector += count;
        }
    }

    return WiaFile(wia);
}

// Checks that `game` holds the test disc's files.
void CheckGame(const std::string& name, const fs::path& path, const TestDisc& disc)
{
    ntdlib::disc::GameFiles game;
    const auto error = ntdlib::disc::LoadGame(path, game, kOptions);
    Check(!error, name + ": " + error.value_or(""));
    if (error)
    {
        return;
    }

    Check(game.GameId() == "RWST01" && game.Title() == "TEST DISC", name + ": the ID and title");
    Check(!game.IsTitle(), name + ": a disc, not a title");
    Check(game.boot == disc.boot && game.dol == disc.dol && game.fst == disc.fst,
          name + ": boot.bin, main.dol, fst.bin");
    Check(game.bi2 == disc.bi2 && game.apploader == disc.apploader, name + ": bi2.bin and apploader.img");
    Check(game.tmd == disc.tmd, name + ": the TMD");
    Check(game.SoundArchives() == std::vector<std::string>{"Sound/a.brsar", "Sound/sub/b.brsar"}, name + ": archives");
    CheckFiles(name, game, disc.files);
}

// Checks ISO images, without hashes as NKit stores them and with hashed sectors, and the containers that hold an ISO
// as it is. Also checks that LoadGame names an NKit image's source.
void TestIsoImages(const fs::path& dir, const TestDisc& disc)
{
    const Bytes iso = disc.Iso({.nkit = true});
    const Bytes hashed = disc.Iso({.hashed = true});
    WriteFile(dir / "nkit.iso", iso);
    WriteFile(dir / "hashed.iso", hashed);
    WriteFile(dir / "game.gcz", Gcz(iso));
    WriteFile(dir / "hashed.gcz", Gcz(hashed));
    WriteFile(dir / "game.ciso", Ciso(hashed));
    WriteFile(dir / "game.wbfs", Wbfs(hashed));
    WriteFile(dir / "v1.wdf", Wdf(hashed, 1));
    WriteFile(dir / "v2.wdf", Wdf(iso, 2));
    CheckGame("NKit ISO", dir / "nkit.iso", disc);
    CheckGame("hashed ISO", dir / "hashed.iso", disc);
    CheckGame("GCZ", dir / "game.gcz", disc);
    CheckGame("hashed GCZ", dir / "hashed.gcz", disc);
    CheckGame("CISO", dir / "game.ciso", disc);
    CheckGame("WBFS", dir / "game.wbfs", disc);
    CheckGame("WDF v1", dir / "v1.wdf", disc);
    CheckGame("WDF v2", dir / "v2.wdf", disc);

    ntdlib::disc::GameFiles nkit;
    ntdlib::disc::LoadGame(dir / "nkit.iso", nkit, kOptions);
    Check(nkit.source == "NKit ISO", "an NKit image's source is \"" + nkit.source + "\"");
}

// Checks split images: a WBFS and a WDF in pieces. The pieces are named as Wiimm's tools name them.
void TestSplitImages(const fs::path& dir, const TestDisc& disc)
{
    const Bytes iso = disc.Iso({.nkit = true});
    const Bytes hashed = disc.Iso({.hashed = true});
    const Bytes wbfs = Wbfs(hashed);
    const size_t cut = wbfs.size() / 3;
    WriteFile(dir / "split.wbfs", Bytes(wbfs.begin(), wbfs.begin() + static_cast<std::ptrdiff_t>(cut)));
    WriteFile(dir / "split.wbf1", Bytes(wbfs.begin() + static_cast<std::ptrdiff_t>(cut), wbfs.begin() + 2 * cut));
    WriteFile(dir / "split.wbf2", Bytes(wbfs.begin() + 2 * static_cast<std::ptrdiff_t>(cut), wbfs.end()));
    CheckGame("split WBFS", dir / "split.wbfs", disc);
    const Bytes wdf = Wdf(iso, 2);
    WriteFile(dir / "split.wdf", Bytes(wdf.begin(), wdf.begin() + 0x40000));
    WriteFile(dir / "split.wdf.01", Bytes(wdf.begin() + 0x40000, wdf.end()));
    CheckGame("split WDF", dir / "split.wdf", disc);
}

// Checks WIA and RVZ with every compression method, with and without the partition stored separately.
void TestWiaAndRvz(const fs::path& dir, const TestDisc& disc)
{
    for (const auto& [method, method_name] : kWiaMethods)
    {
        for (const bool partitions : {true, false})
        {
            const std::string name = "WIA " + method_name + (partitions ? "" : " raw");
            const fs::path path = dir / (name + ".wia");
            WriteFile(path, Wia(disc, false, method, 0x200000, partitions));
            CheckGame(name, path, disc);
        }
    }

    for (const auto& [method, method_name] : kRvzMethods)
    {
        for (const uint32_t chunk_size : {0x8000u, 0x20000u, 0x200000u})
        {
            for (const bool partitions : {true, false})
            {
                const std::string name = "RVZ " + method_name + " " + std::to_string(chunk_size / 1024) + " KiB" +
                                         (partitions ? "" : " raw");
                const fs::path path = dir / (name + ".rvz");
                WriteFile(path, Wia(disc, true, method, chunk_size, partitions));
                CheckGame(name, path, disc);
            }
        }
    }
}

// Checks extracted discs in Dolphin's layout (a DATA folder) and with the partition's folder given directly.
void TestExtractedDiscs(const fs::path& dir, const TestDisc& disc)
{
    for (const fs::path& root : {dir / "extracted" / "DATA", dir / "partition"})
    {
        WriteFile(root / "sys" / "boot.bin", disc.boot);
        WriteFile(root / "sys" / "bi2.bin", disc.bi2);
        WriteFile(root / "sys" / "apploader.img", disc.apploader);
        WriteFile(root / "sys" / "main.dol", disc.dol);
        WriteFile(root / "sys" / "fst.bin", disc.fst);
        WriteFile(root / "tmd.bin", disc.tmd);
        for (const auto& [path, data] : disc.files)
        {
            WriteFile(root / "files" / fs::path(std::u8string(path.begin(), path.end())), data);
        }
    }

    CheckGame("extracted disc", dir / "extracted", disc);
    CheckGame("extracted partition", dir / "partition", disc);
}

// Checks a partition decrypted as Wiimm's tools leave it. The disc header still calls the partition encrypted.
void TestDecryptedWbfs(const fs::path& dir, const TestDisc& disc)
{
    WriteFile(dir / "decrypted.wbfs", Wbfs(disc.Iso({.hashed = true, .encrypted = true})));
    CheckGame("decrypted WBFS with the encryption flag on", dir / "decrypted.wbfs", disc);
}

// Checks what the reader rejects: an encrypted partition, a GameCube disc, and something that isn't a disc.
void TestRefusals(const fs::path& dir, const TestDisc& disc)
{
    WriteFile(dir / "encrypted.iso", disc.Iso({.hashed = true, .encrypted = true, .scrambled = true}));
    CheckRefused("encrypted ISO", dir / "encrypted.iso", "Convert this image to RVZ", kOptions);
    WriteFile(dir / "encrypted.wbfs", Wbfs(disc.Iso({.hashed = true, .encrypted = true, .scrambled = true})));
    CheckRefused("encrypted WBFS", dir / "encrypted.wbfs", "encrypted", kOptions);
    Bytes gamecube = disc.Iso({});
    xsflib::StoreBe32(gamecube.data() + 0x18, 0);
    xsflib::StoreBe32(gamecube.data() + 0x1C, 0xC2339F3D);
    WriteFile(dir / "gamecube.iso", gamecube);
    CheckRefused("GameCube disc", dir / "gamecube.iso", "GameCube", kOptions);
    WriteFile(dir / "text.iso", Bytes(0x50000, 'x'));
    CheckRefused("not a disc", dir / "text.iso", "isn't a Wii disc", kOptions);
}

} // namespace

// usage: ntdlib_disc_wii_test [folder]. With a folder, the images are written there and kept.
int main(int argc, char** argv)
{
    const TestDisc disc = BuildDisc();
    std::optional<ntdlib::testing::TempFolder> temp;
    if (argc <= 1)
    {
        temp.emplace("disc_wii_test");
    }

    const fs::path dir = temp ? temp->Path() : fs::path(argv[1]);
    fs::create_directories(dir);

    TestIsoImages(dir, disc);
    TestSplitImages(dir, disc);
    TestWiaAndRvz(dir, disc);
    TestExtractedDiscs(dir, disc);
    TestDecryptedWbfs(dir, disc);
    TestRefusals(dir, disc);

    return xsflib::testing::Summary("disc Wii");
}
