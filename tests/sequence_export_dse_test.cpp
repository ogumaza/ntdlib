// SPDX-License-Identifier: MIT

// Tests of the conversion of DSE's songs and sound effects (ntdlib/sequence_export/dse), of the search for DSE files in
// packs (ntdlib/dse/files.h) and of the search for their banks (ntdlib/dse/sets.h), on the files of dse_song_files.h.
// The expected values are worked out by hand from the files and from the driver's model: its keys, its envelopes'
// levels and its timers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/track_file.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/files.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/psg_samples.h"
#include "ntdlib/testing/dse_files.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/sequence_export_check.h"

namespace
{

namespace dse_files = ntdlib::testing::dse_files;
namespace files = ntdlib::testing::dse_song_files;
namespace sets = ntdlib::dse;
using namespace ntdlib::sequence_export::dse;
using dse_files::Bytes;
using xsflib::testing::Check;

// The name that the conversions' files give the program.
constexpr char kProgram[] = "xyz2midi";

// A tick of the driver in milliseconds: 5236 * 64 cycles of the system clock of 33,513,982 Hz. The game's settings give
// the tick of 10 ms (kTenMs).
constexpr double kTickMs = 1000.0 * 5236 * 64 / 33513982.0;
constexpr int kTenMs = ntdlib::dse::Driver::kTickMicroseconds;

// The test's song, its bank and the main bank, read.
struct TestFiles
{
    ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile());
    ntdlib::dse::WaveBank bank = ntdlib::dse::WaveBank::Parse(files::BankFile());
    ntdlib::dse::WaveBank main_bank = ntdlib::dse::WaveBank::Parse(files::MainBankFile());

    SequenceFiles Song() const
    {
        return {.song = &song, .banks = {&bank}, .main_bank = &main_bank};
    }
};

// A track of a sound effect: notes 48 and 52 of program 0 for 24 ticks each, at velocity 0x60.
Bytes EffectTrack()
{
    return files::Join({{1, 0, 0, 0},
                        {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                        files::Note(0x60, 0, 0, 0x18, 1),
                        {0x92, 0x18},
                        files::Note(0x60, 4, 0),
                        {0x92, 0x18, 0x98}});
}

// A bank of sound effects of id 5. Slot 0 is empty. Effect 1 plays EffectTrack on the song's bank. Effect 2 lists a
// bank that no file holds.
Bytes EffectsFile()
{
    return files::EffectsFile({std::nullopt, files::Effect{.tracks = {EffectTrack()}},
                               files::Effect{.tracks = {EffectTrack()}, .banks = {0x1234}}},
                              5);
}

// A track's events of `kind`.
std::vector<TrackEvent> EventsOf(const Performance& p, std::size_t track, EventKind kind)
{
    std::vector<TrackEvent> events;
    for (const TrackEvent& e : p.tracks.at(track))
    {
        if (e.kind == kind)
        {
            events.push_back(e);
        }
    }
    return events;
}

void CheckFiles(const Conversion& c, const std::string& name)
{
    const std::vector<std::string> midi = seqlib::midi::Problems(c.midi);
    const std::vector<std::string> sf2 = seqlib::sf2::Check(c.sf2);
    const std::string sfz =
        midi.empty() ? ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(seqlib::midi::Read(c.midi))) : "";
    Check(midi.empty() && sf2.empty() && sfz.empty(),
          name + ": seqlib's checks: " + (midi.empty() ? "" : midi[0]) + (sf2.empty() ? "" : sf2[0]) + sfz);
}

// Named files with their bytes. The files point into `bytes`.
struct Named
{
    Named(std::initializer_list<std::pair<std::string, Bytes>> list)
    {
        for (const auto& [path, data] : list)
        {
            bytes.push_back(data);
            paths.push_back(path);
        }
        for (std::size_t i = 0; i < bytes.size(); i++)
        {
            files.push_back({paths[i], bytes[i]});
        }
    }

    std::vector<std::string> paths;
    std::vector<Bytes> bytes;
    std::vector<sets::NamedFile> files;
};

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
    const Named named = {
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
    xsflib::StoreLe16(&bad_version[0x0C], 0x0402);
    const Bytes pack =
        files::Join({Bytes(0x20, 0xEE), Packed("swdl", 5, "BGM_A"), Packed("smdl", 5, "BGM_A", 0x50),
                     Packed("swdl", 6, "BGM_A"), Packed("smdl", 6, "BGM_A", 0x50), bad_version,
                     Packed("sedl", 7, "", 0x40), Packed("smdl", 8, "a/b\x01.SMD", 0x50),
                     Packed("swdl", 11, "BGM_CUT_SHORT.S"), Packed("smdl", 11, "BGM_CUT_SHORT.S", 0x50)});
    const Bytes starts_as_file = files::Join({Packed("sedl", 9, "S.SED", 0x50), Packed("smdl", 10, "D01.SMD", 0x50)});
    const Named named = {
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
    const Named named = {
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
    const Named named = {{"a.bin", first}, {"b.bin", later}};

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
    const Named named = {{"lt4.fa", archive}, {"other.fa", damaged}};

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

// Songs whose banks lie in another folder or follow them in a pack. A song in a folder without banks takes the only
// bank of its name, or else the only bank of its id. A song followed by a bank of its id in its folder takes it. A song
// in a folder with banks takes neither.
void TestSongSetsApart()
{
    const Bytes in_folder = Header("swdl", 0x41);
    const Named named = {
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
    const Named named = {
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

// The zones of the bank's programs.
void TestPresetZones()
{
    const TestFiles test;
    const std::vector<PresetZone> one = PresetZones(test.bank, {.bank = files::kBankId, .program = 1});
    const std::vector<PresetZone> two = PresetZones(test.bank, {.bank = files::kBankId, .program = 2});
    Check(one.size() == 2 && two.size() == 2, "a zone for each split");
    if (one.size() != 2 || two.size() != 2)
    {
        return;
    }

    // Program 1's volume 0x70 times the split's 0x7F, over 127.
    const ntdlib::dse::EnvelopeValues unscaled = {.multiplier = 0,
                                                  .attack_begin = 0,
                                                  .attack_time = 0x08,
                                                  .decay_time = 0x0C,
                                                  .sustain_level = 0x50,
                                                  .hold_time = 0x04,
                                                  .sustain_time = 0x30,
                                                  .release_time = 0x20};
    Check(one[0].low_key == 0 && one[0].high_key == 63 && one[0].wave == 1 && one[0].format == 0 &&
              one[0].tuning == 100 && one[0].volume == 0x70 && one[0].pan == 0x20 && one[0].envelope == unscaled,
          "program 1's first split");
    // A coarse tuning of -1 adds -256 to the tuning of -300.
    Check(one[1].low_key == 48 && one[1].high_key == 127 && one[1].wave == 2 && one[1].format == 2 &&
              one[1].tuning == -556 && one[1].pan == 0x60,
          "program 1's second split, with its coarse tuning");
    Check(two[0].format == 3 && two[1].format == 4 && two[1].low_velocity == 0x20 && two[1].low_key == 64,
          "program 2's PSG and noise, from velocity 0x20");

    // A channel's channel_values takes the place of a split's value up to 0x7F.
    ntdlib::dse::EnvelopeValues channel_values;
    channel_values.attack_time = 0x7F;
    channel_values.release_time = 0x10;
    channel_values.sustain_level = 0x80;
    const std::vector<PresetZone> overridden =
        PresetZones(test.bank, {.bank = files::kBankId, .program = 1, .envelope = channel_values});
    Check(overridden.size() == 2 && overridden[0].envelope.attack_time == 0x7F &&
              overridden[0].envelope.release_time == 0x10 && overridden[0].envelope.sustain_level == 0x50 &&
              overridden[1].envelope.release_time == 0x10,
          "the channel's envelope values up to 0x7F take the splits' place");
    Check(PresetZones(test.bank, {.bank = files::kBankId, .program = 4}).empty() &&
              PresetZones(test.bank, {.bank = files::kBankId, .program = 200}).empty(),
          "a program that the bank lacks has no zones");
}

// The waves. A bank without samples plays its waves from the main bank's. The main bank's samples hold the bytes
// i * 7 and i * 13 + 5 for each i.
void TestWaves()
{
    const TestFiles test;
    BankSet banks(test.Song());
    const Pcm* pcm16 = banks.Wave(files::kBankId, 0);
    const Pcm* pcm8 = banks.Wave(files::kBankId, 1);
    const Pcm* adpcm = banks.Wave(files::kBankId, 2);
    const Pcm* psg = banks.Wave(files::kBankId, 3);
    const Pcm* noise = banks.Wave(files::kBankId, 4);
    const Pcm* lacking = banks.Wave(files::kBankId, 5);

    // 64 words of PCM16 from 0: 128 samples, looped from word 4, sample 8.
    Check(pcm16 != nullptr && pcm16->channels[0].size() == 128 && pcm16->channels[0][0] == 0x0500 &&
              pcm16->channels[0][1] == 0x1207 && pcm16->loop && pcm16->loop_start == 8 && pcm16->loop_end == 128 &&
              pcm16->sample_rate == 32000,
          "PCM16 from the main bank's samples, looped");
    // 32 words of PCM8 from 0x100: byte 0x100 is 128 * 7 = 0x380, and byte 0x101 is 128 * 13 + 5 = 0x685.
    Check(pcm8 != nullptr && pcm8->channels[0].size() == 128 && pcm8->channels[0][0] == -128 * 256 &&
              pcm8->channels[0][1] == -123 * 256 && !pcm8->loop && pcm8->sample_rate == 16000,
          "PCM8, once");
    // 32 words of IMA-ADPCM: a word of header and 248 samples, looped from word 9: sample (9 - 1) * 8.
    Check(adpcm != nullptr && adpcm->channels[0].size() == 248 && adpcm->loop && adpcm->loop_start == 64 &&
              adpcm->sample_rate == 22050,
          "IMA-ADPCM, looped after its header");
    Check(psg != nullptr && psg->channels[0] == ntdlib::sequence_export::PsgSamples(3) && psg->loop &&
              psg->loop_start == 0 && psg->loop_end == 128 &&
              psg->sample_rate == ntdlib::sequence_export::kPsgSampleRate,
          "the PSG at the wave's duty of 3");
    Check(noise != nullptr && noise->channels[0] == ntdlib::sequence_export::NoiseSamples() && noise->loop,
          "the noise");
    Check(lacking == nullptr &&
              banks.Warnings() == std::vector<std::string>{"w32512_5: no bank holds the wave's samples"},
          "a wave that the main bank lacks gives a warning");
    Check(BankSet::WaveName(files::kBankId, 1) == "w32512_1", "the waves' names");
}

// The envelopes' fits to the driver's levels. A voice plays the square of its envelope's level over 127.
void TestEnvelopes()
{
    // The splits' default: attack, hold and decay of 0 start at full level, and a sustain time of 0x7F holds it.
    const ntdlib::dse::EnvelopeValues held = {.multiplier = 1,
                                              .attack_begin = 0,
                                              .attack_time = 0,
                                              .decay_time = 0,
                                              .sustain_level = 0x7F,
                                              .hold_time = 0,
                                              .sustain_time = 0x7F,
                                              .release_time = 0x28};
    const std::vector<uint8_t> levels = ntdlib::dse::EnvelopeLevels(held, kTenMs, 100);
    const Sf2Envelope sf2 = ConvertEnvelope(held, kTenMs);
    Check(std::ranges::all_of(levels, [](uint8_t level) { return level == 127; }) && sf2.attack == -12000 &&
              sf2.sustain == 0,
          "an envelope at full level from its start");

    // The driver's release falls linearly in level to 0 in its ticks. At half of them, the voice plays (1/2)^2:
    // -12.0 dB. SoundFont's release falls 100 dB in its time.
    const std::optional<int32_t> ticks = ntdlib::dse::EnvelopeTicks(1, 0x28, kTenMs);
    const double release_ms = std::pow(2.0, sf2.release / 1200.0) * 1000.0;
    const double half_db = ticks ? -100.0 * (*ticks * kTickMs / 2.0) / release_ms : 0.0;
    Check(ticks && std::abs(half_db + 12.04) < 0.5,
          "the release passes the driver's level at half its time within 0.5 dB: " + std::to_string(half_db));
    // The driver's level falls by 127 / ticks in each tick of the release, from the first.
    const std::vector<uint8_t> released = ntdlib::dse::EnvelopeLevels(held, kTenMs, *ticks + 2, 0);
    Check(released.size() == static_cast<std::size_t>(*ticks + 2) && released.back() == 0 &&
              std::abs(released[static_cast<std::size_t>(*ticks / 2 - 1)] - 63.5) <= 1.0,
          "the driver's release falls linearly");

    // Program 0's split: an attack from 0x20, a decay to a sustain of 0x60, which plays 40 * log10(96 / 127) =
    // -4.86 dB: 48.6 cB, and (96 / 127)^2 = 57.1 % of full amplitude.
    const ntdlib::dse::EnvelopeValues slow = {.multiplier = 1,
                                              .attack_begin = 0x20,
                                              .attack_time = 0x10,
                                              .decay_time = 0x20,
                                              .sustain_level = 0x60,
                                              .hold_time = 0,
                                              .sustain_time = 0x7F,
                                              .release_time = 0x28};
    const Sf2Envelope slow_sf2 = ConvertEnvelope(slow, kTenMs);
    const SfzEnvelope slow_sfz = ConvertEnvelopeForSfz(slow, kTenMs);
    Check(slow_sf2.sustain == 49 && std::abs(slow_sfz.sustain - 57.14) < 0.01, "the sustain's level");
    // The decay starts where the driver's does: the attack of 0x10 takes a tick and the level holds for another, then
    // the decay of 0x20 takes 3 ticks to the sustain.
    const std::vector<uint8_t> fall = ntdlib::dse::EnvelopeLevels(slow, kTenMs, 8);
    const double decay_starts_ms = (slow_sfz.attack + slow_sfz.hold) * 1000.0;
    Check(ntdlib::dse::EnvelopeTicks(1, 0x10, kTenMs) == 1 && ntdlib::dse::EnvelopeTicks(1, 0x20, kTenMs) == 3 &&
              fall == std::vector<uint8_t>{127, 127, 116, 106, 96, 96, 96, 96} &&
              std::abs(decay_starts_ms - 2.0 * kTickMs) < 1e-6,
          "the decay starts after two ticks at full level: " + std::to_string(decay_starts_ms) + " ms");
    // SoundFont's decay falls 100 dB in its time. It reaches the sustain's 4.86 dB after about 3 ticks.
    const double sustain_ms = std::pow(2.0, slow_sf2.decay / 1200.0) * 1000.0 * 4.862 / 100.0;
    Check(std::abs(sustain_ms - 3.0 * kTickMs) < kTickMs,
          "the decay reaches the sustain within a tick of the driver's: " + std::to_string(sustain_ms) + " ms");

    // No attack, no hold and no decay: the level falls from full, from the first tick, linearly to 0 in the ticks of
    // the sustain's time. At half of those ticks, the voice plays (1/2)^2: -12.0 dB. SoundFont's decay falls 100 dB in
    // its time.
    const ntdlib::dse::EnvelopeValues fading = {.multiplier = 1,
                                                .attack_begin = 0,
                                                .attack_time = 0,
                                                .decay_time = 0,
                                                .sustain_level = 0x7F,
                                                .hold_time = 0,
                                                .sustain_time = 0x60,
                                                .release_time = 0x28};
    const std::optional<int32_t> fading_ticks = ntdlib::dse::EnvelopeTicks(1, 0x60, kTenMs);
    const std::vector<uint8_t> fading_levels = ntdlib::dse::EnvelopeLevels(fading, kTenMs, 1);
    Check(fading_ticks && *fading_ticks > 254 && fading_levels[0] == 126, "the level falls from the first tick");
    const Sf2Envelope fading_sf2 = ConvertEnvelope(fading, kTenMs);
    const SfzEnvelope fading_sfz = ConvertEnvelopeForSfz(fading, kTenMs);
    const double half_ms = fading_ticks ? *fading_ticks * kTickMs / 2.0 : 0.0;
    const double fading_db =
        -100.0 * (half_ms - fading_sfz.hold * 1000.0) / (std::pow(2.0, fading_sf2.decay / 1200.0) * 1000.0);
    Check(fading_sf2.sustain == 1440 && std::abs(fading_db + 12.04) < 3.0,
          "a fall from the first tick passes the driver's level at half its time within 3 dB: " +
              std::to_string(fading_db));
}

// The tick of 16.715 ms: each of the driver's stages takes fewer ticks, each a video frame long, and the envelopes'
// times follow the driver's.
void TestSlowTickEnvelopes()
{
    constexpr int kSlow = ntdlib::dse::Driver::kSlowTickMicroseconds;
    const ntdlib::dse::EnvelopeValues e = {.multiplier = 1,
                                           .attack_begin = 0,
                                           .attack_time = 0x30,
                                           .decay_time = 0x40,
                                           .sustain_level = 0x40,
                                           .hold_time = 0,
                                           .sustain_time = 0x7F,
                                           .release_time = 0x50};
    const std::optional<int32_t> ten = ntdlib::dse::EnvelopeTicks(1, 0x50, kTenMs);
    const std::optional<int32_t> slow = ntdlib::dse::EnvelopeTicks(1, 0x50, kSlow);
    Check(ten && slow && std::abs(*slow - *ten * 10000.0 / kSlow) <= 1.0, "a stage's ticks follow the tick");

    constexpr double kFrameMs = 1000.0 * 560190 / 33513982.0;
    const Sf2Envelope a = ConvertEnvelope(e, kTenMs);
    const Sf2Envelope b = ConvertEnvelope(e, kSlow);
    const auto ms = [](int timecents)
    {
        return std::pow(2.0, timecents / 1200.0) * 1000.0;
    };
    // The driver's release lasts the same number of ticks in either tick. The release's fit is the same against it.
    const double ten_ratio = ms(a.release) / (*ten * kTickMs);
    const double slow_ratio = ms(b.release) / (*slow * kFrameMs);
    Check(std::abs(slow_ratio / ten_ratio - 1.0) < 0.002 && a.release != b.release,
          "the release follows the driver's release in frames: " + std::to_string(ms(a.release)) + " and " +
              std::to_string(ms(b.release)) + " ms");
    Check(std::abs(ms(a.attack) - ms(b.attack)) < 2.0 * kFrameMs, "the attack keeps its time within two frames");
    Check(a.sustain == b.sustain, "the sustain doesn't depend on the tick");
    const SfzEnvelope sfz = ConvertEnvelopeForSfz(e, kSlow);
    Check(std::abs(sfz.release * 1000.0 - ms(b.release) * 0.6949) < 1.0, "SFZ's release from the same fit");
}

// A preset's LFO with a phase of 50 ms runs whole ticks, like the driver's: 5 ticks of 10 ms, or 2 of 16.715 ms. A
// phase shorter than a tick takes one tick. Waveform 0 takes two phases for a cycle.
void TestLfoTicks()
{
    PresetKey key;
    key.lfos[0] = {.type = 1, .output = 1, .waveform = 0, .amplitude = 100, .phase_ms = 50};
    key.lfos[1] = {.type = 1, .output = 1, .waveform = 0, .amplitude = 100, .phase_ms = 5};
    const std::vector<LfoShape> ten = PresetLfos(key, ntdlib::dse::Driver::kTickMicroseconds);
    const std::vector<LfoShape> slow = PresetLfos(key, ntdlib::dse::Driver::kSlowTickMicroseconds);
    Check(ten.size() == 2 && slow.size() == 2, "both LFOs");
    Check(ten.size() == 2 && ten[0].hz == 10.0 && ten[1].hz == 50.0, "10 ms: 5 ticks a phase, and one tick");
    Check(slow.size() == 2 && std::abs(slow[0].hz - 1e6 / (2.0 * 2 * 16'715)) < 1e-9 &&
              std::abs(slow[1].hz - 1e6 / (2.0 * 16'715)) < 1e-9,
          "16.715 ms: 2 ticks a phase, and one tick");
}

// The performer. Track 4 plays keys 57, 53 and 50 (octave 4 and keys 9, 5 and 2) at velocity 127, 16 ticks apart, on
// MIDI track 4. Tracks 0 to 3 loop back to their loops' starts. The settings give 48 ticks to a quarter note.
void TestPerform()
{
    const TestFiles test;
    const Performance p = Perform(test.Song());
    const std::vector<TrackEvent> ons = EventsOf(p, 4, EventKind::kNoteOn);
    std::vector<std::pair<uint32_t, int>> keys;
    for (const TrackEvent& e : ons)
    {
        keys.emplace_back(e.tick, e.key);
    }
    Check(keys == std::vector<std::pair<uint32_t, int>>{{0, 57}, {16, 53}, {32, 50}} &&
              std::ranges::all_of(ons, [](const TrackEvent& e) { return e.value == 127; }),
          "track 4's notes, keys and velocities");
    Check(p.timebase == 48 && !p.tempo.empty(), "the timebase and the tempo");
    Check(p.loop.has_value() && !p.truncated, "the song loops");
    Check(!p.presets.empty() &&
              std::ranges::all_of(p.presets, [](const PresetKey& key) { return key.bank == files::kBankId; }),
          "the presets of the song's bank");

    // A sound effect plays on the first sequence of the pool. Effect 1 plays keys 48 and 52 at velocity 0x60, 24
    // ticks apart, and ends.
    const ntdlib::dse::SoundEffects effects = ntdlib::dse::SoundEffects::Parse(EffectsFile());
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    effect.effect = 1;
    const Performance fx = Perform(effect);
    const std::vector<TrackEvent> fx_ons = EventsOf(fx, 0, EventKind::kNoteOn);
    Check(fx_ons.size() == 2 && fx_ons[0].tick == 0 && fx_ons[0].key == 48 && fx_ons[0].value == 0x60 &&
              fx_ons[1].tick == 24 && fx_ons[1].key == 52 && !fx.loop && !fx.truncated,
          "a sound effect's notes");

    // An effect whose track ends more than 10 s after its last sound has died away: the two notes, a wait of 2000
    // ticks (20 s) and the end. The performance ends where the sound dies away. After a wait of 900 ticks (9 s), it
    // ends with the track.
    const auto ending = [](uint16_t wait)
    {
        Bytes track =
            files::Join({{1, 0, 0, 0},
                         {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                         files::Note(0x60, 0, 0, 0x18, 1),
                         {0x92, 0x18},
                         files::Note(0x60, 4, 0),
                         {0x92, 0x18, 0x93, static_cast<uint8_t>(wait), static_cast<uint8_t>(wait >> 8), 0x98}});
        return ntdlib::dse::SoundEffects::Parse(files::EffectsFile({files::Effect{.tracks = {track}}}, 5));
    };
    const ntdlib::dse::SoundEffects late_effects = ending(2000);
    const ntdlib::dse::SoundEffects in_time_effects = ending(900);
    effect.effect = 0;
    effect.effects = &late_effects;
    const Performance late = Perform(effect);
    effect.effects = &in_time_effects;
    const Performance in_time = Perform(effect);
    Check(EventsOf(late, 0, EventKind::kNoteOn).size() == 2 && late.end_tick >= 48 && late.end_tick < 300 &&
              late.seconds < 3.0 && !late.truncated,
          "a track's end long after the last sound: the performance ends at the sound");
    Check(in_time.end_tick == 948 && !in_time.truncated, "a track's end within 10 s of it: the performance ends there");

    effect.effects = &effects;
    effect.effect = 0;
    const Performance refused = Perform(effect);
    Check(EventsOf(refused, 0, EventKind::kNoteOn).empty() &&
              refused.warnings == std::vector<std::string>{"the driver refuses the sound effect"},
          "an empty slot plays nothing");
}

// A song whose track 1 holds a note of 3000 ticks and then loops 50 ticks of waits, and whose track 2 plays a note,
// waits 1100 ticks, plays another note, waits `last` ticks and ends, without a loop. Track 1's passes repeat from the
// start, but track 2 hasn't gone round a loop before it ends. The song then holds track 1's note. It ends 10 s after
// its last change, the note at tick 1100. At 120 beats a minute and 48 ticks a beat, a tick takes 10.4167 ms of the
// driver's nominal time, and 10.4156 ms of the time that kTickMs gives. 10 s therefore comes 960.09 ticks after tick
// 1100, in tick 2061. A wait of 50 ticks ends track 2 in the tick that ends one of track 1's passes.
// With the tick of 16.715 ms, the driver's ticks come once a video frame, and a sequence's ticks keep their times: the
// MIDI tempo follows the driver's tick against its nominal length.
void TestSlowTick()
{
    const TestFiles test;
    const Performance ten = Perform(test.Song());
    const Performance slow = Perform(test.Song(), {.tick_microseconds = ntdlib::dse::Driver::kSlowTickMicroseconds});
    constexpr double kFrameMs = 1000.0 * 560190 / 33513982.0;
    Check(ten.tick_microseconds == 10000 && slow.tick_microseconds == 16715, "the performance keeps its tick");
    Check(EventsOf(slow, 4, EventKind::kNoteOn).size() == EventsOf(ten, 4, EventKind::kNoteOn).size() &&
              EventsOf(slow, 4, EventKind::kNoteOn)[1].tick == 16,
          "the notes keep their ticks");
    // A tick of the sequence takes tempo / timebase microseconds of the driver's nominal ticks.
    const double ten_bpm = ten.tempo.at(0).bpm * kTickMs / 10.0;
    const double slow_bpm = slow.tempo.at(0).bpm * kFrameMs / 16.715;
    Check(std::abs(ten_bpm - slow_bpm) < 1e-9, "the tempo follows the tick: " + std::to_string(ten.tempo[0].bpm) +
                                                   " and " + std::to_string(slow.tempo[0].bpm));
    Check(std::abs(ten.seconds - slow.seconds) < 2.0 * kFrameMs / 1000.0,
          "the length keeps its time within two frames: " + std::to_string(ten.seconds) + " and " +
              std::to_string(slow.seconds) + " s");
}

void TestHeldSound()
{
    const TestFiles test;
    const Bytes select = {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40};
    for (const uint8_t last : {uint8_t{0x18}, uint8_t{0x32}})
    {
        const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
            {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
             files::Join({{1, 1, 0, 0}, select, files::Note(0x60, 0, 0, 3000, 2), {0x99, 0x92, 0x32, 0x98}}),
             files::Join({{2, 2, 0, 0},
                          select,
                          files::Note(0x60, 4, 0, 0x18, 1),
                          {0x93, 0x4C, 0x04},
                          files::Note(0x60, 7, 0),
                          {0x92, last, 0x98}})}));
        SequenceFiles files = test.Song();
        files.song = &song;
        const Performance p = Perform(files);
        std::vector<uint32_t> ticks;
        for (const TrackEvent& e : EventsOf(p, 2, EventKind::kNoteOn))
        {
            ticks.push_back(e.tick);
        }
        const std::string wait = " (a last wait of " + std::to_string(last) + " ticks)";
        Check(ticks == std::vector<uint32_t>{0, 1100},
              "a track without a loop plays on while another holds a note" + wait);
        Check(p.holds && !p.loop && p.end_tick == 2061 && !p.truncated,
              "the held note ends the song 10 s after the last change" + wait);
    }
}

// The conversions of the song and of the sound effect.
void TestConvert()
{
    const TestFiles test;
    const Conversion c = ConvertSequence(test.Song(), "song", {}, kProgram);
    CheckFiles(c, "the song");
    // Program 3 plays wave 5, which the main bank lacks.
    Check(c.warnings == std::vector<std::string>{"w32512_5: no bank holds the wave's samples"},
          "a warning for the wave without samples alone");

    // The PSG's timer steps it once a period, 16 samples of its wave: the zone plays at a PCM wave's rate of the same
    // timer, 16 times the step's rate.
    const std::vector<PresetZone> two = PresetZones(test.bank, {.bank = files::kBankId, .program = 2});
    if (two.size() == 2)
    {
        PresetZone pcm = two[0];
        pcm.format = 0;
        const ZoneTuning psg_tuning = TuningOf(two[0], ntdlib::sequence_export::kPsgSampleRate);
        const ZoneTuning pcm_tuning = TuningOf(pcm, ntdlib::sequence_export::kPsgSampleRate);
        Check(std::abs((psg_tuning.coarse * 100 + psg_tuning.fine) - (pcm_tuning.coarse * 100 + pcm_tuning.fine)) <= 1,
              "the PSG's tuning");
    }

    // Program 1's first zone: a volume of 112 and a pan of 0x20. (112 / 127)^2 = 0.77775, and the pan's gain against
    // the centre's is hypot(32, 96) / hypot(64, 64) = 1.11803.
    const std::vector<PresetZone> one = PresetZones(test.bank, {.bank = files::kBankId, .program = 1});
    Check(!one.empty() && std::abs(ZoneLevel(one[0]) - 0.86955) < 1e-4, "a zone's level with its pan");

    const ntdlib::dse::SoundEffects effects = ntdlib::dse::SoundEffects::Parse(EffectsFile());
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    effect.effect = 1;
    const Conversion fx = ConvertSequence(effect, "fx_001", {}, kProgram);
    CheckFiles(fx, "the sound effect");
    const std::string sf2(fx.sf2.begin(), fx.sf2.end());
    Check(sf2.find("Converted by xyz2midi from sound effect fx_001.") != std::string::npos, "the SoundFont's comment");
}

// A sound effect of `track` on `test`'s banks, converted.
Conversion ConvertEffect(const TestFiles& test, const Bytes& track)
{
    const ntdlib::dse::SoundEffects effects =
        ntdlib::dse::SoundEffects::Parse(files::EffectsFile({files::Effect{.tracks = {track}}}, 5));
    SequenceFiles effect = test.Song();
    effect.song = nullptr;
    effect.effects = &effects;
    return ConvertSequence(effect, "fx_000", {}, kProgram);
}

// The ticks and keys of a track's note-offs.
std::vector<std::pair<uint32_t, int>> NoteOffs(const Performance& p, std::size_t track)
{
    std::vector<std::pair<uint32_t, int>> offs;
    for (const TrackEvent& e : EventsOf(p, track, EventKind::kNoteOff))
    {
        offs.emplace_back(e.tick, e.key);
    }
    return offs;
}

constexpr char kCut[] = "note cut short by another note's voice (rings on in its release)";

// A note that another note cuts rings on in its release where another note sounds on its part. Key 60 of program 0
// sounds from tick 0 to 96. Keys 64, 65 and 67 of program 1 start at ticks 0, 6 and 12, on the split whose wave loops.
// Key group 1 allows a channel two voices: key 67 takes the voice of key 64 or key 65. Their release of 8 ticks
// counts. A release of no ticks doesn't.
void TestCuts()
{
    const Bytes track = files::Join({{1, 0, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                     files::Note(0x60, 0, 1, 0x60, 1),
                                     {0xAC, 0x01},
                                     files::Note(0x60, 4, 0, 0x30, 1),
                                     {0x92, 0x06},
                                     files::Note(0x60, 5, 0),
                                     {0x92, 0x06},
                                     files::Note(0x60, 7, 0, 0x0C, 1),
                                     {0x92, 0x60, 0x98}});
    TestFiles test;
    const Conversion c = ConvertEffect(test, track);
    CheckFiles(c, "the cut");
    const std::vector<std::pair<uint32_t, int>> offs = NoteOffs(c.performance, 0);
    Check(std::ranges::count_if(offs, [](const auto& off) { return off.first == 12; }) == 1,
          "key 67 cuts a note at tick 12");
    Check(c.performance.approximations.contains(kCut) && c.performance.approximations.at(kCut) == 1,
          "the cut rings on in its release");

    test.bank.instruments->programs.at(1)->splits.at(1)[0x2E] = 0;
    const Conversion quick = ConvertEffect(test, track);
    Check(NoteOffs(quick.performance, 0) == offs && !quick.performance.approximations.contains(kCut),
          "a release of no ticks doesn't count");
}

// Key group 1 of the test's bank allows two voices of a channel. With one, a note of the group takes the voice of the
// group's last note on the channel, and the zones of program 1's splits take the group as their exclusive class.
void TestKeyGroups()
{
    TestFiles test;
    Check(std::ranges::all_of(PresetZones(test.bank, {.bank = files::kBankId, .program = 1}),
                              [](const PresetZone& z) { return z.exclusive_class == 0; }),
          "a key group of two voices has no exclusive class");
    test.bank.instruments->key_groups.at(1)[2] = 1;
    const std::vector<PresetZone> one = PresetZones(test.bank, {.bank = files::kBankId, .program = 1});
    const std::vector<PresetZone> two = PresetZones(test.bank, {.bank = files::kBankId, .program = 2});
    Check(one.size() == 2 && std::ranges::all_of(one, [](const PresetZone& z) { return z.exclusive_class == 1; }) &&
              std::ranges::all_of(two, [](const PresetZone& z) { return z.exclusive_class == 0; }),
          "the zones of a key group of one voice take it as their exclusive class");

    // Key 60 of program 0 sounds from tick 0 to 96. Key 64 of program 1 starts at 0 for 48 ticks, and key 65 at 12.
    // Both play program 1's second split, whose wave loops. Key 65 takes key 64's voice at 12. The three notes share a
    // part, and keys 64 and 65 a preset: key 65's exclusive class cuts key 64 where key 60 still sounds.
    const Bytes track = files::Join({{1, 0, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                     files::Note(0x60, 0, 1, 0x60, 1),
                                     {0xAC, 0x01},
                                     files::Note(0x60, 4, 0, 0x30, 1),
                                     {0x92, 0x0C},
                                     files::Note(0x60, 5, 0, 0x0C, 1),
                                     {0x92, 0x60, 0x98}});
    const Conversion c = ConvertEffect(test, track);
    CheckFiles(c, "the exclusive cut");
    const std::vector<std::pair<uint32_t, int>> offs = NoteOffs(c.performance, 0);
    Check(std::ranges::find(offs, std::pair<uint32_t, int>{12, 64}) != offs.end() &&
              std::ranges::find(offs, std::pair<uint32_t, int>{96, 60}) != offs.end(),
          "key 65 cuts key 64");
    Check(!c.performance.approximations.contains(kCut), "the exclusive class cuts the note");

    const seqlib::sf2::Records sf2 = seqlib::sf2::Read(c.sf2);
    std::vector<uint16_t> classes;
    for (std::size_t zone = 0; zone + 1 < sf2.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = sf2.ZoneGens(zone);
        if (const auto found = gens.find(seqlib::sf2::kExclusiveClass); found != gens.end())
        {
            classes.push_back(found->second);
        }
    }
    Check(classes == std::vector<uint16_t>{1, 1}, "program 1's zones have exclusive class 1");
    std::size_t groups = 0;
    for (const seqlib::sfz::File& file : c.sfz.files)
    {
        for (std::size_t at = 0; (at = file.text.find(" group=1 off_by=1\n", at)) != std::string::npos; at++)
        {
            groups++;
        }
    }
    Check(groups == 2, "program 1's regions have group 1 and off_by 1");
}

// The songs and sound effects of a game's files, read. Effect 2's list names a bank that no file holds.
// A bank's instruments. Program 0 plays wave 0 of the main bank, and program 1 waves 1 and 2. Program 2 plays the PSG
// and the noise with three LFOs: a pitch LFO, a volume LFO of type 3 that a level of 0 leaves out, and a pan LFO.
// Program 3 plays wave 5, which the main bank lacks.
void TestConvertBank()
{
    const TestFiles test;
    const std::vector<uint32_t> every = {0, 1, 2, 3, 9, 200};
    const ntdlib::sequence_export::BankConversion c =
        ConvertBank(test.bank, &test.main_bank, every, kTenMs, "B_TEST", kProgram);
    Check(c.sound_fonts.size() == 1 && c.sound_fonts[0].first == 0 && c.sound_fonts[0].last == 3,
          "a bank: one SoundFont of programs 0 to 3");
    const std::vector<uint8_t>& sf2 = c.sound_fonts.empty() ? std::vector<uint8_t>() : c.sound_fonts[0].sf2;
    Check(seqlib::sf2::Check(sf2).empty(), "a bank: seqlib's check of the SoundFont");
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<std::pair<uint16_t, uint16_t>> presets;
    for (std::size_t p = 0; p + 1 < records.Count("phdr", 38); p++)
    {
        const uint8_t* r = records.Record("phdr", 38, p);
        presets.emplace_back(xsflib::LoadLe16(r + 22), xsflib::LoadLe16(r + 20));
    }
    Check(presets == std::vector<std::pair<uint16_t, uint16_t>>{{0, 0}, {0, 1}, {0, 2}, {0, 3}},
          "a bank: a preset at each program that plays");

    // Programs 0 and 1 take waves 0, 1 and 2. Program 2 takes the PSG at wave 3 and the noise at wave 4.
    Check(c.sound_fonts.size() == 1 && c.sound_fonts[0].samples == 5 && c.sfz.samples.size() == 5,
          "a bank: a sample for each wave, and a WAV file for each");
    std::vector<std::string> names = {"conductor"};
    for (const seqlib::sfz::File& file : c.sfz.files)
    {
        names.push_back(file.name.substr(0, file.name.size() - 4));
    }
    Check(names == std::vector<std::string>{"conductor", "B_TEST_0", "B_TEST_1", "B_TEST_2", "B_TEST_3"},
          "a bank: an SFZ file of each program that plays");
    Check(ntdlib::testing::SfzProblem(c.sfz, names).empty(), "a bank: the SFZ files' samples and loops");
    const std::string& first = c.sfz.files.size() == 4 ? c.sfz.files[0].text : std::string();
    Check(first.find("// Program 0 of B_TEST, converted by xyz2midi.") == 0 &&
              first.find("bend_up=200 bend_down=-200") != std::string::npos &&
              first.find("<region> sample=w32512_0.wav lokey=0 hikey=127") != std::string::npos,
          "a bank: program 0's file plays its wave");
    const std::string& third = c.sfz.files.size() == 4 ? c.sfz.files[2].text : std::string();
    Check(third.find(" lfo1_pitch=") != std::string::npos && third.find(" lfo2_pan=") != std::string::npos &&
              third.find("_volume=") == std::string::npos,
          "a bank: the program's LFOs");
    Check(c.warnings == std::vector<std::string>{"program 9 of bank 32512 plays nothing",
                                                 "program 200 of bank 32512 plays nothing",
                                                 "w32512_5: no bank holds the wave's samples"},
          "a bank: the programs that play nothing, and the wave that no bank holds");

    // One program by itself, and programs given out of order and twice.
    const ntdlib::sequence_export::BankConversion one =
        ConvertBank(test.bank, &test.main_bank, std::vector<uint32_t>{0}, kTenMs, "B_TEST", kProgram);
    Check(one.sound_fonts.size() == 1 && one.sound_fonts[0].first == 0 && one.sfz.files.size() == 1 &&
              one.sfz.files[0].name == "B_TEST_0.sfz" && one.sfz.samples.size() == 1,
          "a bank: one program by itself");
    const ntdlib::sequence_export::BankConversion unsorted =
        ConvertBank(test.bank, &test.main_bank, std::vector<uint32_t>{3, 0, 3}, kTenMs, "B_TEST", kProgram);
    Check(unsorted.sound_fonts.size() == 1 && unsorted.sound_fonts[0].first == 0 && unsorted.sound_fonts[0].last == 3 &&
              unsorted.sfz.files.size() == 2 && unsorted.sfz.files[0].name == "B_TEST_0.sfz" &&
              unsorted.sfz.files[1].name == "B_TEST_3.sfz",
          "a bank: programs out of order convert in ascending order, each once");

    // A bank without its main bank plays nothing of its waves.
    const ntdlib::sequence_export::BankConversion without =
        ConvertBank(test.bank, nullptr, std::vector<uint32_t>{0}, kTenMs, "B_TEST", kProgram);
    Check(without.sfz.samples.empty() && !without.warnings.empty(), "a bank without its main bank has no samples");

    Check(
        ntdlib::dse::ProgramLfos(test.bank, 2)[0].type == 1 && ntdlib::dse::ProgramLfos(test.bank, 2)[0].output == 1 &&
            ntdlib::dse::ProgramLfos(test.bank, 2)[0].phase_ms == 40 &&
            ntdlib::dse::ProgramLfos(test.bank, 2)[1].type == 3 && ntdlib::dse::ProgramLfos(test.bank, 2)[3].type == 0,
        "a program's LFOs");
    Check(ntdlib::dse::ProgramLfos(test.bank, 0)[0].type == 0 && ntdlib::dse::ProgramLfos(test.bank, 9)[0].type == 0,
          "a program without LFOs, and a program that the bank lacks");
}

void TestReadSequences()
{
    const Named named = {
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
    TestPresetZones();
    TestWaves();
    TestEnvelopes();
    TestSlowTickEnvelopes();
    TestLfoTicks();
    TestPerform();
    TestSlowTick();
    TestHeldSound();
    TestConvert();
    TestCuts();
    TestKeyGroups();
    TestConvertBank();
    TestReadSequences();
    TestSfzWaveNames();
    TestSfzWaveNamesConverted();

    return xsflib::testing::Summary("DSE conversion");
}
