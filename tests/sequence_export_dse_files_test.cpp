// SPDX-License-Identifier: MIT

// Tests of the search for DSE files in packs (ntdlib/dse/files.h), of the search for their banks (ntdlib/dse/sets.h)
// and of the reading of a game's songs and sound effects for the conversion (ntdlib/sequence_export/dse), on the files
// of dse_song_files.h.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sfz/track_file.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/files.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/testing/dse_files.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export_dse_cases.h"

namespace
{

using namespace sequence_export_dse_cases;

// A file whose header holds `magic`, the id at 0x0E, the main bank's id at 0x40 and the kind at 0x4B.
Bytes Header(const char* magic, uint16_t id, uint8_t kind = 0, uint16_t main_bank = 0)
{
    Bytes bytes(0x50, 0);
    std::copy_n(magic, 4, bytes.begin());
    bytes[0x0E] = static_cast<uint8_t>(id);
    bytes[0x0F] = static_cast<uint8_t>(id >> 8);
    bytes[0x40] = static_cast<uint8_t>(main_bank);
    bytes[0x41] = static_cast<uint8_t>(main_bank >> 8);
    bytes[0x4B] = kind;
    return bytes;
}

// The songs' banks: by name, the folder's lone bank, and none where a folder has two lone banks. A main bank is never
// a lone bank. The main bank in a bank's folder comes before one that comes first elsewhere.
void TestSongSets()
{
    const Bytes song = Header("smdl", 0);
    const Bytes bank = Header("swdl", 0x7F00, 2, 3);
    const Bytes main_bank = Header("swdl", 3, 1);
    const Bytes own_samples = Header("swdl", 0x7F7E);
    const dse_files::NamedFiles named = {
        {"SOUND/BGM/bgm0001.smd", song},        // 0
        {"SOUND/BGM/bgm0001.swd", bank},        // 1
        {"SOUND/MAIN/main.swd", main_bank},     // 2
        {"SOUND/BGM/bgm.swd", main_bank},       // 3
        {"SOUND/BGM/bgm0002.smd", song},        // 4
        {"SOUND/ME/me0000.smd", song},          // 5
        {"SOUND/ME/me0001.smd", song},          // 6
        {"SOUND/ME/me.swd", own_samples},       // 7
        {"SOUND/TWO/a.smd", song},              // 8
        {"SOUND/TWO/b.swd", own_samples},       // 9
        {"SOUND/TWO/c.swd", own_samples},       // 10
        {"SOUND/OTHER/x.smd", song},            // 11
        {"SOUND/OTHER/x.swd", bank},            // 12
        {"SOUND/OTHER/short.swd", Bytes(0x40)}, // a header too short for a bank
        {"lt4/snd/y.smd", song},                // 14
        {"lt4/snd/y.swd", bank},                // 15
        {"ll/main.swd", main_bank},             // 16
        {"lt4/wav/main.swd", main_bank},        // 17
    };
    const std::vector<sets::SongSet> found = sets::FindSongSets(named.files);
    Check(found.size() == 7, "a set for each song");
    if (found.size() != 7)
    {
        return;
    }
    Check(found[0].song == 0 && found[0].bank == 1 && found[0].main_bank == 3,
          "bgm0001 takes the bank of its name, and the main bank of its folder");
    Check(found[1].song == 4 && !found[1].bank, "bgm0002 has no bank of its name, and the main bank isn't a lone bank");
    Check(found[2].bank == 7 && !found[2].main_bank && found[3].bank == 7,
          "me0000 and me0001 take the folder's lone bank, which holds its samples");
    Check(found[4].song == 8 && !found[4].bank, "a folder with two lone banks gives its song none");
    Check(found[5].song == 11 && found[5].bank == 12 && found[5].main_bank == 2,
          "a bank without a main bank in its folder takes the first in the files");
    Check(found[6].song == 14 && found[6].bank == 15 && found[6].main_bank == 17,
          "or else the first whose path starts with the longest part of the bank's");
}

// A DSE file as a pack holds it: the magic word, its size, version 0x415, the id at 0x0E, `name` at 0x20 and the
// kind at 0x4B, with zeros up to `size` bytes.
Bytes Packed(const char* magic, uint16_t id, const std::string& name, std::size_t size = 0x60, uint8_t kind = 0)
{
    Bytes bytes = Header(magic, id, kind);
    bytes.resize(size, 0);
    xsflib::StoreLe32(&bytes[0x08], static_cast<uint32_t>(size));
    xsflib::StoreLe16(&bytes[0x0C], 0x0415);
    std::copy(name.begin(), name.end(), bytes.begin() + 0x20);
    return bytes;
}

// The DSE files of loose files and of packs. A loose file keeps its path. A pack's files take their names, an
// extension for their kind where they have none or one cut short, a number where a name comes again, and _ for a byte
// that a name can't hold. A header with another version is no file.
void TestFindFiles()
{
    Bytes bad_version = Packed("smdl", 1, "OLD.SMD", 0x50);
    xsflib::StoreLe16(&bad_version[0x0C], 0x0401);
    const Bytes pack =
        files::Join({Bytes(0x20, 0xEE), Packed("swdl", 5, "BGM_A"), Packed("smdl", 5, "BGM_A", 0x50),
                     Packed("swdl", 6, "BGM_A"), Packed("smdl", 6, "BGM_A", 0x50), bad_version,
                     Packed("sedl", 7, "", 0x40), Packed("smdl", 8, "a/b\x01.SMD", 0x50),
                     Packed("swdl", 11, "BGM_CUT_SHORT.S"), Packed("smdl", 11, "BGM_CUT_SHORT.S", 0x50)});
    const Bytes starts_as_file = files::Join({Packed("sedl", 9, "S.SED", 0x50), Packed("smdl", 10, "D01.SMD", 0x50)});
    const dse_files::NamedFiles named = {
        {"SOUND/BGM/song.smd", Packed("smdl", 1, "other", 0x50)},
        {"SOUND/BGM/old.smd", bad_version},
        {"sound/bgm.bin", pack},
        {"data/sound.pb", starts_as_file},
        {"readme.txt", Bytes(0x100, 's')},
    };

    const std::vector<sets::NamedFile> found = sets::FindFiles(named.files);

    std::vector<std::string> paths;
    for (const sets::NamedFile& file : found)
    {
        paths.push_back(file.path);
    }
    const std::size_t sedl_at = 0x20 + 0x60 + 0x50 + 0x60 + 0x50 + 0x50;
    Check(paths == std::vector<std::string>{"SOUND/BGM/song.smd", "SOUND/BGM/old.smd", "sound/bgm.bin/BGM_A.swd",
                                            "sound/bgm.bin/BGM_A.smd", "sound/bgm.bin/BGM_A_2.swd",
                                            "sound/bgm.bin/BGM_A_2.smd", "sound/bgm.bin/0x1D0.sed",
                                            "sound/bgm.bin/a_b_.SMD", "sound/bgm.bin/BGM_CUT_SHORT.swd",
                                            "sound/bgm.bin/BGM_CUT_SHORT.smd", "data/sound.pb/S.SED",
                                            "data/sound.pb/D01.SMD"},
          "the loose files, and each file of the packs by its name");
    if (found.size() != 12)
    {
        return;
    }
    Check(found[0].bytes.size() == 0x50 && found[1].bytes.size() == 0x50, "a loose file keeps its bytes");
    Check(found[2].bytes.data() == named.bytes[2].data() + 0x20 && found[2].bytes.size() == 0x60 &&
              found[3].bytes.size() == 0x50 && found[6].bytes.data() == named.bytes[2].data() + sedl_at,
          "a pack's file points into the pack, with the size of its header");
}

// `bytes` as the SDK's LZ77 compresses them with no copies: the type 0x10 and the size, then a flag byte of 0 before
// each 8 bytes.
Bytes Lz77Literals(const Bytes& bytes)
{
    Bytes out = {0x10, static_cast<uint8_t>(bytes.size()), static_cast<uint8_t>(bytes.size() >> 8),
                 static_cast<uint8_t>(bytes.size() >> 16)};
    for (std::size_t at = 0; at < bytes.size(); at += 8)
    {
        out.push_back(0);
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   bytes.begin() + static_cast<std::ptrdiff_t>(std::min(at + 8, bytes.size())));
    }
    return out;
}

// A compressed pack of a song and its bank: Decompressed gives its bytes decompressed under its path, and FindFiles
// then finds the two files in them. A compressed file without a DSE header, a damaged one and a loose file stay as they
// are.
void TestDecompressed()
{
    const Bytes pack =
        files::Join({Bytes(0x50, 0x11), Packed("smdl", 3, "BG_007.SMD", 0x50), Packed("swdl", 3, "BG_007.SWD")});
    Bytes damaged = Lz77Literals(pack);
    damaged.resize(damaged.size() / 2);
    const dse_files::NamedFiles named = {
        {"snd/BG_007.csnd", Lz77Literals(pack)},
        {"img/face.bin", Lz77Literals(Bytes(0x200, 's'))},
        {"snd/BG_008.csnd", damaged},
        {"SOUND/BGM/song.smd", Packed("smdl", 1, "other", 0x50)},
    };

    std::vector<std::vector<uint8_t>> storage;
    const std::vector<sets::NamedFile> decompressed = sets::Decompressed(named.files, storage);
    Check(decompressed.size() == 4 && storage.size() == 1, "one file decompresses to DSE files");
    if (decompressed.size() != 4)
    {
        return;
    }
    Check(decompressed[0].path == "snd/BG_007.csnd" && std::ranges::equal(decompressed[0].bytes, pack),
          "the pack's bytes, decompressed, under its path");
    for (std::size_t i = 1; i < 4; i++)
    {
        Check(decompressed[i].path == named.files[i].path && decompressed[i].bytes.data() == named.bytes[i].data(),
              "the other files stay as they are");
    }

    std::vector<std::string> paths;
    for (const sets::NamedFile& file : sets::FindFiles(decompressed))
    {
        paths.push_back(file.path);
    }
    Check(paths == std::vector<std::string>{"snd/BG_007.csnd/BG_007.SMD", "snd/BG_007.csnd/BG_007.SWD",
                                            "SOUND/BGM/song.smd"},
          "FindFiles finds the files in the decompressed pack");
}

// A pack whose files hold a name and the name with _2. A name that comes again takes the first number that no file of
// the pack has had yet, in either order.
void TestFindFilesNumbersPastNamesInThePack()
{
    const Bytes first = files::Join(
        {Packed("smdl", 1, "P32.SMD", 0x50), Packed("smdl", 2, "P32_2.SMD", 0x50), Packed("smdl", 3, "P32.SMD", 0x50)});
    const Bytes later = files::Join({Packed("smdl", 1, "P33.SMD", 0x50), Packed("smdl", 2, "p33.smd", 0x50),
                                     Packed("smdl", 3, "P33_2.SMD", 0x50), Packed("smdl", 4, "P33.SMD", 0x50)});
    const dse_files::NamedFiles named = {{"a.bin", first}, {"b.bin", later}};

    std::vector<std::string> paths;
    for (const sets::NamedFile& file : sets::FindFiles(named.files))
    {
        paths.push_back(file.path);
    }
    Check(paths == std::vector<std::string>{"a.bin/P32.SMD", "a.bin/P32_2.SMD", "a.bin/P32_3.SMD", "b.bin/P33.SMD",
                                            "b.bin/p33_2.smd", "b.bin/P33_2_2.SMD", "b.bin/P33_3.SMD"},
          "a name that comes again takes a number that no file of the pack holds");
}

// `bytes` in Level-5's compression: the method and the size, then the data. Method 1 (LZ77) takes the SDK's LZ77 data
// without its header.
Bytes Level5(const Bytes& bytes, uint32_t method)
{
    const Bytes data = method == 1 ? Lz77Literals(bytes) : files::Join({Bytes(4, 0), bytes});
    Bytes out = data;
    xsflib::StoreLe32(out.data(), static_cast<uint32_t>(bytes.size()) << 3 | method);
    return out;
}

// CRC-16/X.25 of a name, as a GFSA archive's tables give it.
uint16_t GfsaHash(const std::string& name)
{
    uint16_t crc = 0xFFFF;
    for (const char c : name)
    {
        crc ^= static_cast<uint8_t>(c);
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 1) != 0 ? static_cast<uint16_t>(crc >> 1 ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
        }
    }
    return static_cast<uint16_t>(~crc);
}

// A GFSA archive of `folders`, each a name with its last '/' and its files' names and data in Level-5's compression.
// The tables are stored. The folders' table and each folder's files come in another order than the names, as the
// archive orders them by their hashes.
Bytes Gfsa(const std::vector<std::pair<std::string, std::vector<std::pair<std::string, Bytes>>>>& folders)
{
    Bytes folder_table;
    Bytes file_table;
    std::string names;
    Bytes data;
    for (const auto& folder : folders)
    {
        names += folder.first + '\0';
    }
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder)
    {
        Bytes entry(8, 0);
        xsflib::StoreLe16(&entry[0], GfsaHash(folder->first));
        xsflib::StoreLe16(&entry[2], static_cast<uint16_t>(folder->second.size()));
        xsflib::StoreLe16(&entry[4], static_cast<uint16_t>(file_table.size() / 8));
        folder_table.insert(folder_table.end(), entry.begin(), entry.end());
        for (auto file = folder->second.rbegin(); file != folder->second.rend(); ++file)
        {
            const std::size_t offset = data.size();
            Bytes file_entry(8, 0);
            xsflib::StoreLe16(&file_entry[0], GfsaHash(file->first));
            xsflib::StoreLe16(&file_entry[2], static_cast<uint16_t>(offset >> 2));
            xsflib::StoreLe32(&file_entry[4], static_cast<uint32_t>(file->second.size() & 0xFFFF) |
                                                  static_cast<uint32_t>(offset >> 18) << 16 |
                                                  static_cast<uint32_t>(file->second.size() >> 16) << 28);
            file_table.insert(file_table.end(), file_entry.begin(), file_entry.end());
            data.insert(data.end(), file->second.begin(), file->second.end());
            data.resize((data.size() + 3) / 4 * 4, 0);
        }
    }
    for (const auto& folder : folders)
    {
        for (const auto& file : folder.second)
        {
            names += file.first + '\0';
        }
    }

    Bytes archive(0x28, 0);
    std::copy_n("GFSA", 4, archive.begin());
    const auto append = [&archive](std::size_t at, const Bytes& table)
    {
        xsflib::StoreLe32(&archive[at], static_cast<uint32_t>(archive.size()));
        archive.insert(archive.end(), table.begin(), table.end());
        archive.resize((archive.size() + 3) / 4 * 4, 0);
    };
    append(0x04, Level5(folder_table, 0));
    append(0x08, Level5(file_table, 0));
    append(0x0C, Level5(Bytes(4, 0), 0));
    append(0x10, Level5(Bytes(names.begin(), names.end()), 0));
    append(0x14, data);
    xsflib::StoreLe32(&archive[0x18], static_cast<uint32_t>(folders.size()));
    xsflib::StoreLe32(&archive[0x1C], static_cast<uint32_t>(file_table.size() / 8));
    return archive;
}

// A GFSA archive's files that hold DSE files once decompressed take its place in Decompressed, under its path, and
// FindFiles finds the DSE files in them. Its other files don't. An archive with damaged tables stays as it is.
void TestGfsaArchive()
{
    const Bytes pack =
        files::Join({Bytes(0x2C, 0x22), Packed("smdl", 4, "BG_004.SMD", 0x50), Packed("swdl", 4, "BG_004.SWD")});
    const Bytes archive = Gfsa({
        {"img/", {{"face.bin", Level5(Bytes(0x80, 's'), 1)}}},
        {"snd/", {{"BG_004.cse", Level5(pack, 1)}, {"BG_005.cse", Level5(pack, 0)}, {"SE.bin", Level5(pack, 7)}}},
    });
    Bytes damaged = archive;
    damaged[0x18] = 9;
    const dse_files::NamedFiles named = {{"lt4.fa", archive}, {"other.fa", damaged}};

    std::vector<std::vector<uint8_t>> storage;
    const std::vector<sets::NamedFile> decompressed = sets::Decompressed(named.files, storage);
    std::vector<std::string> paths;
    for (const sets::NamedFile& file : decompressed)
    {
        paths.push_back(file.path);
    }
    Check(paths == std::vector<std::string>{"lt4.fa/snd/BG_004.cse", "lt4.fa/snd/BG_005.cse", "other.fa"},
          "the archive's files that hold DSE files in its place, under its path");
    if (decompressed.size() != 3)
    {
        return;
    }
    Check(std::ranges::equal(decompressed[0].bytes, pack) && std::ranges::equal(decompressed[1].bytes, pack),
          "each file decompressed");

    paths.clear();
    for (const sets::NamedFile& file : sets::FindFiles(decompressed))
    {
        paths.push_back(file.path);
    }
    Check(paths == std::vector<std::string>{"lt4.fa/snd/BG_004.cse/BG_004.SMD", "lt4.fa/snd/BG_004.cse/BG_004.SWD",
                                            "lt4.fa/snd/BG_005.cse/BG_004.SMD", "lt4.fa/snd/BG_005.cse/BG_004.SWD",
                                            "other.fa/BG_004.SMD", "other.fa/BG_004.SWD", "other.fa/BG_004_2.SMD",
                                            "other.fa/BG_004_2.SWD"},
          "FindFiles finds the files in the archive's packs, and in the damaged archive's bytes");
}

// Songs whose banks are in another folder or follow them in a pack. A song in a folder without banks takes the only
// bank of its name, or else the only bank of its id. A song followed by a bank of its id in its folder takes it. A song
// in a folder with banks takes neither.
void TestSongSetsApart()
{
    const Bytes in_folder = Header("swdl", 0x41);
    const dse_files::NamedFiles named = {
        {"mid/x.smd", Header("smdl", 0x10)},          // 0
        {"mid/y.smd", Header("smdl", 0x40)},          // 1
        {"mid/z.smd", Header("smdl", 0x41)},          // 2
        {"BGM/x.swd", Header("swdl", 0x22)},          // 3
        {"BGM/me.swd", Header("swdl", 0x40)},         // 4
        {"BGM/z1.swd", in_folder},                    // 5
        {"BGM/z2.swd", in_folder},                    // 6
        {"pack/SR_000_D.SMD", Header("smdl", 0x200)}, // 7
        {"pack/SR_000.SWD", Header("swdl", 0x200)},   // 8
        {"pack/SR_003.SMD", Header("smdl", 0x200)},   // 9
        {"pack/SR_003.SWD", Header("swdl", 0x200)},   // 10
        {"pack/x.smd", Header("smdl", 0x22)},         // 11
        {"pack/SE.SWD", Header("swdl", 0x300)},       // 12
        {"pack/SE2.SWD", Header("swdl", 0x301)},      // 13
    };

    const std::vector<sets::SongSet> found = sets::FindSongSets(named.files);

    Check(found.size() == 6, "a set for each song");
    if (found.size() != 6)
    {
        return;
    }
    Check(found[0].bank == 3, "a song apart from banks takes the bank of its name in another folder");
    Check(found[1].bank == 4, "or else the only bank of its id");
    Check(!found[2].bank, "two banks of its id give it none");
    Check(found[3].song == 7 && found[3].bank == 8, "a song followed by a bank of its id takes it");
    Check(found[4].song == 9 && found[4].bank == 10, "a bank of its name comes first");
    Check(found[5].song == 11 && !found[5].bank, "a song in a folder with banks takes no bank of another folder");
}

// The banks of sound effects. A list's id takes the bank in the effects' folder, else a bank that no song plays, named
// after the id's low byte where several are, else a song's bank.
void TestEffectSets()
{
    const Bytes effects = files::EffectsFile(
        {std::nullopt, files::Effect{.tracks = {EffectTrack()}, .banks = {0x6430, 0x7F01, 0x1234}}}, 5);
    const Bytes system = files::EffectsFile(
        {files::Effect{.tracks = {EffectTrack()}}, files::Effect{.tracks = {EffectTrack()}, .banks = {0x7F01, 0x7F00}}},
        6);
    const Bytes song = Header("smdl", 0);
    const Bytes waves_in_main = Header("swdl", 0x6430, 2, 0);
    const dse_files::NamedFiles named = {
        {"SOUND/SE/fx.sed", effects},                      // 0
        {"SOUND/SYSTEM/sys.sed", system},                  // 1
        {"SOUND/SWD/046.swd", Header("swdl", 0x6430)},     // 2
        {"SOUND/SWD/048.swd", waves_in_main},              // 3
        {"SOUND/SE/motion.swd", Header("swdl", 0x7F01)},   // 4
        {"SOUND/BGM/bgm0110.smd", song},                   // 5
        {"SOUND/BGM/bgm0110.swd", Header("swdl", 0x7F01)}, // 6
        {"SOUND/BGM/bgm0111.smd", song},                   // 7
        {"SOUND/BGM/bgm0111.swd", Header("swdl", 0x7F00)}, // 8
        {"SOUND/BGM/bgm.swd", Header("swdl", 0, 1)},       // 9
        {"SOUND/SE/broken.sed", Header("sedl", 7)},        // 10
    };
    const std::vector<sets::EffectSet> found = sets::FindEffectSets(named.files);
    Check(found.size() == 3 && found[0].effects == 0 && found[1].effects == 1 && found[2].effects == 10,
          "a set for each bank of sound effects");
    if (found.size() != 3 || found[0].slots.size() != 2 || found[1].slots.size() != 2)
    {
        return;
    }
    const sets::EffectBanks& fx = found[0].slots[1];
    Check(
        fx.banks == std::vector<std::size_t>{3, 4} && fx.missing == std::vector<uint16_t>{0x1234} && fx.main_bank == 9,
        "048.swd for 0x6430, the folder's motion.swd for 0x7F01, no bank for 0x1234, and 048.swd's main bank");
    Check(found[0].slots[0].banks == std::vector<std::size_t>{8}, "an empty slot's list, of a song's bank alone");
    Check(found[1].slots[1].banks == std::vector<std::size_t>{4, 8} && !found[1].slots[1].main_bank,
          "outside the folder, the bank that no song plays comes before a song's");
    Check(found[2].slots.empty(), "a bank of sound effects that can't be read has no slots");
}

void TestReadSequences()
{
    const dse_files::NamedFiles named = {
        {"SOUND/BGM/bgm0001.smd", files::SongFile()}, {"SOUND/BGM/bgm0001.swd", files::BankFile()},
        {"SOUND/BGM/bgm.swd", files::MainBankFile()}, {"SOUND/BGM/bgm0002.smd", files::SongFile()},
        {"SOUND/SE/fx.sed", EffectsFile()},           {"SOUND/SE/fx.swd", files::BankFile()},
    };
    std::vector<std::string> warnings;
    const std::vector<SequenceInput> inputs = ReadSequences(named.files, warnings);
    std::vector<std::string> names;
    for (const SequenceInput& input : inputs)
    {
        names.push_back(input.folder + input.name);
    }
    Check(names == std::vector<std::string>{"SOUND/BGM/bgm0001", "SOUND/SE/fx_001", "SOUND/SE/fx_002"},
          "the songs with their banks, and each sound effect");
    Check(warnings == std::vector<std::string>{"SOUND/BGM/bgm0002.smd: no wave bank goes with the song",
                                               "SOUND/SE/fx.sed: no file holds bank 0x1234 of sound effect 2"},
          "a song without its bank and a missing bank give warnings");
    if (inputs.size() == 3)
    {
        Check(inputs[0].banks.size() == 1 && inputs[0].main_bank && inputs[1].effects && inputs[1].effect == 1 &&
                  inputs[1].banks.size() == 1 && inputs[1].main_bank && inputs[2].banks.empty() &&
                  inputs[0].banks[0] != inputs[1].banks[0],
              "each input's banks");
        Check(inputs[0].bank_paths == std::vector<std::string>{"SOUND/BGM/bgm0001.swd"} &&
                  inputs[1].bank_paths == std::vector<std::string>{"SOUND/SE/fx.swd"},
              "each input names its banks' files");
        Check(inputs[0].ref == ntdlib::dse::SequenceRef{"SOUND/BGM/bgm0001.smd"} &&
                  inputs[2].ref == ntdlib::dse::SequenceRef{"SOUND/SE/fx.sed", uint16_t{2}},
              "each input names its file and its slot");
    }
}

// R-C70: the songs of a folder share its SFZ samples. Two banks of one id in different files get distinct names.
void TestSfzWaveNames()
{
    const auto bank = [](uint16_t id)
    {
        ntdlib::dse::WaveBank read;
        xsflib::StoreLe16(&read.header[0x0E], id);
        return std::make_shared<const ntdlib::dse::WaveBank>(std::move(read));
    };
    const SequenceInput first{.name = "M201", .banks = {bank(258), bank(7)}, .bank_paths = {"p/M201.swd", "p/x.swd"}};
    const SequenceInput second{.name = "E03", .banks = {bank(258)}, .bank_paths = {"p/E03.swd"}};
    const SequenceInput third{.name = "E04", .banks = {bank(258)}, .bank_paths = {"q/M201.swd"}};
    const std::vector<const SequenceInput*> two = {&first, &second};
    const std::vector<const SequenceInput*> three = {&first, &second, &third};

    const std::vector<std::map<uint16_t, std::string>> names = SfzBankNames(two);
    const std::vector<std::map<uint16_t, std::string>> by_path = SfzBankNames(three);
    BankSet banks(first.Files());
    banks.SetSfzNames(names.at(0));

    using Names = std::vector<std::map<uint16_t, std::string>>;
    Check(names == Names{{{258, "M201"}}, {{258, "E03"}}}, "two banks of one id take their files' stems");
    Check(banks.SfzWaveName(258, 27) == "M201_27" && banks.SfzWaveName(7, 3) == "w7_3" &&
              BankSet::WaveName(258, 27) == "w258_27",
          "a lone bank keeps its name, and so do the SoundFonts' samples");
    Check(by_path == Names{{{258, "p_M201"}}, {{258, "E03"}}, {{258, "q_M201"}}},
          "a shared stem gives way to the path");

    // A stem "w<id>" of a kept bank's id, in any case, gives way to the path. So do two stems that differ only in case.
    const SequenceInput kept{.name = "M202", .banks = {bank(258), bank(7)}, .bank_paths = {"p/W7.swd", "p/x.swd"}};
    const SequenceInput upper{.name = "M201", .banks = {bank(258)}, .bank_paths = {"p/M201.swd"}};
    const SequenceInput lower{.name = "m201", .banks = {bank(258)}, .bank_paths = {"q/m201.swd"}};
    const std::vector<const SequenceInput*> beside_kept = {&kept, &second};
    const std::vector<const SequenceInput*> cases = {&upper, &lower};
    Check(SfzBankNames(beside_kept) == Names{{{258, "p_W7"}}, {{258, "E03"}}},
          "a stem of a kept bank's name gives way to the path");
    Check(SfzBankNames(cases) == Names{{{258, "p_M201"}}, {{258, "q_m201"}}},
          "stems that differ only in case give way to the paths");
}

// ConvertSequence with the SFZ banks' names gives the bank's waves those names in the samples and in the SFZ files'
// sample= lines. The MIDI file and the SoundFont stay the same.
void TestSfzWaveNamesConverted()
{
    const TestFiles test;
    const Conversion plain = ConvertSequence(test.Song(), "song", {}, kProgram);
    const Conversion named = ConvertSequence(test.Song(), "song", {}, kProgram, {{files::kBankId, "M201"}});

    const auto all_named = [](const Conversion& c, const std::string& prefix)
    {
        bool lines = false;
        for (const seqlib::sfz::File& file : c.sfz.files)
        {
            for (std::size_t at = file.text.find("sample="); at != std::string::npos;
                 at = file.text.find("sample=", at + 1))
            {
                const std::size_t end = file.text.find('\n', at);
                const std::string line = file.text.substr(at, end == std::string::npos ? end : end - at);
                if (line.find(prefix) == std::string::npos)
                {
                    return false;
                }
                lines = true;
            }
        }
        const bool samples = std::ranges::all_of(
            c.sfz.samples, [&](const seqlib::sfz::Sample& sample) { return sample.name.starts_with(prefix); });
        return lines && samples && !c.sfz.samples.empty();
    };
    Check(all_named(plain, "w32512_"), "without names, the waves keep the bank's id");
    Check(all_named(named, "M201_"), "the samples and the sample= lines take the bank's name");
    Check(named.midi == plain.midi && named.sf2 == plain.sf2 && named.sfz.samples.size() == plain.sfz.samples.size(),
          "the names change no other file");
}

} // namespace

int main()
{
    TestSongSets();
    TestFindFiles();
    TestDecompressed();
    TestFindFilesNumbersPastNamesInThePack();
    TestGfsaArchive();
    TestSongSetsApart();
    TestEffectSets();
    TestReadSequences();
    TestSfzWaveNames();
    TestSfzWaveNamesConverted();

    return xsflib::testing::Summary("DSE conversion: files");
}
