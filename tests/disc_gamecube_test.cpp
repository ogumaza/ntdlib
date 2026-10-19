// SPDX-License-Identifier: MIT

// Builds a small GameCube disc and stores it in every container format that the reader supports. The writers here
// follow the formats' descriptions. The formats are ISO (plain, NKit and split), GCZ, CISO, WDF versions 1 and 2 (whole
// and split), WIA with each of its compression methods, and RVZ with each of its methods, three chunk sizes and packed
// junk. Checks that LoadGame reads the same system files and files from each image and from extracted discs, and that
// it names an NKit image's source. Checks the image test's manifest helper. Also checks that LoadGame rejects an
// extracted disc without main.dol, a Wii disc and a file that isn't a disc.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

#include "disc_files.h"
#include "disc_writers.h"
#include "ntdlib/disc/compression.h"
#include "ntdlib/disc/containers.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/testing/image_manifest.h"
#include "temp_folder.h"

namespace
{

using disc_files::CheckFiles;
using disc_files::CheckRefused;
using disc_files::WriteFile;
using disc_writers::AddRawData;
using disc_writers::BuildGame;
using disc_writers::Bytes;
using disc_writers::Ciso;
using disc_writers::Game;
using disc_writers::GameLayout;
using disc_writers::Gcz;
using disc_writers::kRvzMethods;
using disc_writers::kWiaMethods;
using disc_writers::Wdf;
using disc_writers::WiaContents;
using disc_writers::WiaFile;
using xsflib::testing::Check;
using xsflib::testing::failures;
namespace fs = std::filesystem;

// How the tests load games: as a GameCube rip tool named xyz would, with every container.
const ntdlib::disc::GameOptions kOptions{ntdlib::disc::Console::kGameCube, "xyz", {ntdlib::disc::OpenContainer}};
using ntdlib::disc::Method;

// The test game on a GameCube disc.
const GameLayout kLayout = {
    .id = "GCST01",
    .magic_offset = 0x1C,
    .magic = 0xC2339F3D,
    .shift = 0,
    .sound_folder = "AudioRes",
    .sound_file = "a.baa",
    .sub_sound_file = "b.aw",
};

// The test disc: the test game, which fills the whole disc.
struct TestDisc : Game
{
    // The disc as an ISO image, with NKit's header if `nkit`.
    Bytes Iso(bool nkit) const;
};

Bytes TestDisc::Iso(bool nkit) const
{
    Bytes iso = data;
    if (nkit)
    {
        std::copy_n("NKIT v01", 8, iso.begin() + 0x200);
    }

    return iso;
}

// A WIA or RVZ file of the disc. A GameCube disc is all raw data in both formats.
Bytes Wia(const TestDisc& disc, bool rvz, Method method, uint32_t chunk_size)
{
    const Bytes iso = disc.Iso(false);
    WiaContents wia{.rvz = rvz,
                    .method = method,
                    .chunk_size = chunk_size,
                    .disc_type = 1,
                    .head = Bytes(iso.begin(), iso.begin() + 0x80),
                    .disc_size = iso.size()};
    AddRawData(wia, iso, iso.size(), disc.junk);

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

    Check(game.GameId() == "GCST01" && game.Title() == "TEST DISC", name + ": the ID and title");
    Check(game.boot == disc.boot && game.dol == disc.dol && game.fst == disc.fst,
          name + ": boot.bin, main.dol, fst.bin");
    Check(game.bi2 == disc.bi2 && game.apploader == disc.apploader, name + ": bi2.bin and apploader.img");
    CheckFiles(name, game, disc.files);
}

// Checks ISO images, plain and with NKit's header, and the containers that hold an ISO as it is. Also checks that
// LoadGame names an NKit image's source.
void TestIsoImages(const fs::path& dir, const TestDisc& disc)
{
    const Bytes iso = disc.Iso(false);
    const Bytes nkit = disc.Iso(true);
    WriteFile(dir / "game.iso", iso);
    WriteFile(dir / "nkit.iso", nkit);
    WriteFile(dir / "game.gcz", Gcz(iso));
    WriteFile(dir / "game.ciso", Ciso(iso));
    WriteFile(dir / "v1.wdf", Wdf(iso, 1));
    WriteFile(dir / "v2.wdf", Wdf(nkit, 2));
    CheckGame("ISO", dir / "game.iso", disc);
    CheckGame("NKit ISO", dir / "nkit.iso", disc);
    CheckGame("GCZ", dir / "game.gcz", disc);
    CheckGame("CISO", dir / "game.ciso", disc);
    CheckGame("WDF v1", dir / "v1.wdf", disc);
    CheckGame("WDF v2", dir / "v2.wdf", disc);

    ntdlib::disc::GameFiles nkit_game;
    ntdlib::disc::LoadGame(dir / "nkit.iso", nkit_game, kOptions);
    Check(nkit_game.source == "NKit ISO", "an NKit image's source is \"" + nkit_game.source + "\"");
}

// Checks split images: an ISO and a WDF in pieces. The pieces are named as Wiimm's tools name them.
void TestSplitImages(const fs::path& dir, const TestDisc& disc)
{
    const Bytes iso = disc.Iso(false);
    const size_t cut = iso.size() / 3;
    WriteFile(dir / "split.iso", Bytes(iso.begin(), iso.begin() + static_cast<std::ptrdiff_t>(cut)));
    WriteFile(dir / "split.iso.1", Bytes(iso.begin() + static_cast<std::ptrdiff_t>(cut), iso.begin() + 2 * cut));
    WriteFile(dir / "split.iso.2", Bytes(iso.begin() + 2 * static_cast<std::ptrdiff_t>(cut), iso.end()));
    CheckGame("split ISO", dir / "split.iso", disc);
    const Bytes wdf = Wdf(iso, 2);
    WriteFile(dir / "split.wdf", Bytes(wdf.begin(), wdf.begin() + 0x40000));
    WriteFile(dir / "split.wdf.01", Bytes(wdf.begin() + 0x40000, wdf.end()));
    CheckGame("split WDF", dir / "split.wdf", disc);
}

// Checks WIA and RVZ with every compression method, and RVZ with three chunk sizes and packed junk.
void TestWiaAndRvz(const fs::path& dir, const TestDisc& disc)
{
    for (const auto& [method, method_name] : kWiaMethods)
    {
        const std::string name = "WIA " + method_name;
        const fs::path path = dir / (name + ".wia");
        WriteFile(path, Wia(disc, false, method, 0x200000));
        CheckGame(name, path, disc);
    }

    for (const auto& [method, method_name] : kRvzMethods)
    {
        for (const uint32_t chunk_size : {0x8000u, 0x20000u, 0x200000u})
        {
            const std::string name = "RVZ " + method_name + " " + std::to_string(chunk_size / 1024) + " KiB";
            const fs::path path = dir / (name + ".rvz");
            WriteFile(path, Wia(disc, true, method, chunk_size));
            CheckGame(name, path, disc);
        }
    }
}

// Checks extracted discs in Dolphin's layout (a DATA folder) and with the disc's folder given directly.
void TestExtractedDiscs(const fs::path& dir, const TestDisc& disc)
{
    for (const fs::path& root : {dir / "extracted" / "DATA", dir / "disc"})
    {
        WriteFile(root / "sys" / "boot.bin", disc.boot);
        WriteFile(root / "sys" / "bi2.bin", disc.bi2);
        WriteFile(root / "sys" / "apploader.img", disc.apploader);
        WriteFile(root / "sys" / "main.dol", disc.dol);
        WriteFile(root / "sys" / "fst.bin", disc.fst);
        for (const auto& [path, data] : disc.files)
        {
            WriteFile(root / "files" / fs::path(std::u8string(path.begin(), path.end())), data);
        }
    }

    CheckGame("extracted disc", dir / "extracted", disc);
    CheckGame("extracted disc folder", dir / "disc", disc);
}

// Checks the image test's helper. TestIsoImages and TestExtractedDiscs write game.iso and the disc's folder first. The
// folder and the image match the folder's manifest, and a manifest with a wrong hash gives one failure. The check
// prints that failure, and the test then takes it back.
void TestImageManifest(const fs::path& dir, const TestDisc& disc)
{
    {
        std::ofstream manifest(dir / "disc" / "MANIFEST.txt");
        manifest << "source: game.iso\n";
        const auto line = [&manifest](const std::string& path, const Bytes& data)
        {
            manifest << xsflib::Sha1::Hex(xsflib::Sha1::Of(data)) << "  " << data.size() << "  " << path << "\n";
        };
        line("sys/boot.bin", disc.boot);
        line("sys/main.dol", disc.dol);
        line("sys/fst.bin", disc.fst);
        for (const auto& [path, data] : disc.files)
        {
            line("files/" + path, data);
        }
    }

    const ntdlib::testing::ImageManifest manifest = ntdlib::testing::ReadImageManifest(dir / "disc" / "MANIFEST.txt");
    Check(manifest.source == "game.iso" && manifest.hashes.size() == 3 + disc.files.size(),
          "image manifest: its source and every line");
    std::string folder = (dir / "disc").string();
    std::string roms = dir.string();
    char* arguments[] = {const_cast<char*>("image_test"), folder.data(), roms.data()};
    Check(ntdlib::testing::RunImageTest(3, arguments, kOptions) == 0, "image manifest: the folder and the image match");
    ntdlib::testing::ImageManifest wrong = manifest;
    wrong.hashes.begin()->second.assign(40, '0');
    const int failures_before = failures;
    ntdlib::testing::CheckGameAgainstManifest("wrong manifest", dir / "game.iso", wrong, kOptions);
    const bool caught = failures == failures_before + 1;
    failures = failures_before;
    Check(caught, "image manifest: a wrong hash is one failure");
}

// Checks an extracted disc that lacks a system file: without its main.dol, and with a folder in its place. A folder in
// place of a system file that may be missing reads as no file.
void TestBrokenExtractedDisc(const fs::path& dir, const TestDisc& disc)
{
    const fs::path broken = dir / "broken";
    fs::remove_all(broken); // an earlier run into the same folder left its main.dol
    WriteFile(broken / "sys" / "boot.bin", disc.boot);
    WriteFile(broken / "sys" / "fst.bin", disc.fst);
    const std::string kLacking = "must contain sys/boot.bin, sys/main.dol and sys/fst.bin";
    CheckRefused("extracted disc without main.dol", broken, kLacking, kOptions);
    fs::create_directories(broken / "sys" / "main.dol");
    CheckRefused("extracted disc with a folder for main.dol", broken, kLacking, kOptions);
    fs::remove(broken / "sys" / "main.dol");
    WriteFile(broken / "sys" / "main.dol", disc.dol);
    fs::create_directories(broken / "sys" / "bi2.bin");
    ntdlib::disc::GameFiles folder_bi2;
    const auto folder_error = ntdlib::disc::LoadGame(broken, folder_bi2, kOptions);
    Check(!folder_error && folder_bi2.bi2.empty(), "extracted disc with a folder for bi2.bin: read with no bi2.bin");
}

// Checks what the reader rejects: a Wii disc and something that isn't a disc.
void TestRefusals(const fs::path& dir, const TestDisc& disc)
{
    Bytes wii = disc.Iso(false);
    xsflib::StoreBe32(wii.data() + 0x18, 0x5D1C9EA3);
    xsflib::StoreBe32(wii.data() + 0x1C, 0);
    WriteFile(dir / "wii.iso", wii);
    CheckRefused("Wii disc", dir / "wii.iso", "Wii disc", kOptions);
    WriteFile(dir / "text.iso", Bytes(0x50000, 'x'));
    CheckRefused("not a disc", dir / "text.iso", "isn't a GameCube disc", kOptions);
}

} // namespace

// usage: ntdlib_disc_gamecube_test [folder]. With a folder, the images are written there and kept.
int main(int argc, char** argv)
{
    const TestDisc disc{BuildGame(kLayout)};
    std::optional<ntdlib::testing::TempFolder> temp;
    if (argc <= 1)
    {
        temp.emplace("disc_gamecube_test");
    }

    const fs::path dir = temp ? temp->Path() : fs::path(argv[1]);
    fs::create_directories(dir);

    TestIsoImages(dir, disc);
    TestSplitImages(dir, disc);
    TestWiaAndRvz(dir, disc);
    TestExtractedDiscs(dir, disc);
    TestImageManifest(dir, disc);
    TestBrokenExtractedDisc(dir, disc);
    TestRefusals(dir, disc);

    return xsflib::testing::Summary("disc GameCube");
}
