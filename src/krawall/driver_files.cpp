// SPDX-License-Identifier: MIT

// Krawall's detection of the modified build's modules: the music files that the game unpacks, and the modules
// mapped after the ROM's end.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/krawall/unpack.h"
#include "driver_internal.h"

namespace ntdlib::krawall
{

namespace
{

// A music file of the modified build lists the samples that the game unpacks into RAM before it plays the module: a
// byte of their count, then a byte for each sample's number in the game's table. The module's data follow, packed, at
// the next multiple of 4 (see Unpack()). They end with a byte of the instruments' count and one of the patterns'.
// Before them come the patterns' data, the module's header, the patterns' offsets in the data and the instruments.
// The game's loader turns the offsets into addresses.
struct MusicFile
{
    uint32_t file = 0;
    uint32_t samples = 0;      // the samples that it lists
    std::vector<uint8_t> data; // its module's data, unpacked
    uint32_t module = 0;       // the module's offset in `data`
};

// The most bytes of a module's data: EWRAM holds no more.
constexpr uint32_t kMaxModuleData = 0x40000;

// The counts at the end of a module's data.
constexpr uint32_t kModuleDataCounts = 2;

// Where detection maps the unpacked modules: at the next multiple of this after the ROM's end.
constexpr uint32_t kUnpackedAlign = 0x10000;

// Unpacks the music file at `file`. Returns false if it isn't one that the game's loader reads.
bool ReadMusicFile(const Rom& rom, uint32_t file, MusicFile& music)
{
    const uint32_t samples = rom.U8(file);
    const uint32_t packed = (file + 1 + samples + 3) & ~3u;
    const uint32_t size = rom.U8(packed) | (rom.U8(packed + 1) << 8) | (rom.U8(packed + 2) << 16);
    if (!rom.Contains(file, packed + 4 - file) || size < kModifiedPatterns + kModuleDataCounts || size > kMaxModuleData)
    {
        return false;
    }

    std::vector<uint8_t> data;
    if (!Unpack(rom, packed, data) || data.size() != size)
    {
        return false;
    }

    const uint32_t instruments = data[size - 2];
    const uint32_t patterns = data[size - 1];
    const uint32_t tail = 4 * patterns + kModifiedPatterns + kModuleDataCounts + kInstrumentSize * instruments;
    if (tail > size)
    {
        return false;
    }

    const uint32_t module = size - tail;
    for (uint32_t p = 0; p < patterns; p++)
    {
        if (xsflib::LoadLe32(&data[module + kModifiedPatterns + 4 * p]) >= module)
        {
            return false;
        }
    }

    music.file = file;
    music.samples = samples;
    music.data = std::move(data);
    music.module = module;

    return true;
}

// Lays out the module of `music` at `base`. The game's loader does the same at the address that its malloc() gives.
// The patterns' offsets become addresses, and the header gets the address of the instruments.
void PlaceModule(MusicFile& music, uint32_t base)
{
    uint8_t* data = music.data.data();
    const uint32_t size = uint32_t(music.data.size());
    const uint32_t patterns = data[size - 1];
    for (uint32_t p = 0; p < patterns; p++)
    {
        uint8_t* entry = data + music.module + kModifiedPatterns + 4 * p;
        xsflib::StoreLe32(entry, base + xsflib::LoadLe32(entry));
    }

    xsflib::StoreLe32(data + music.module + kModuleInstruments,
                      base + size - kModuleDataCounts - kInstrumentSize * data[size - 2]);
}

// Finds the music files that words in the ROM point to: the literals of the game's code and the entries of its tables.
// The entries of tables, runs of 2 or more such words, come first, in their order. The rest follow in the order of the
// first word that points to each. Without such words, a scan of the ROM finds the files, and `scanned` is set.
std::vector<MusicFile> FindMusicFiles(const Rom& rom, bool& scanned)
{
    std::vector<MusicFile> files;
    std::unordered_map<uint32_t, size_t> found;
    std::unordered_set<uint32_t> rejected;

    // Returns the index in `files` of the file at `at`, or -1 if there's none.
    const auto lookup = [&](uint32_t at) -> int
    {
        if (!rom.Contains(at, 16) || rejected.count(at))
        {
            return -1;
        }
        if (const auto it = found.find(at); it != found.end())
        {
            return int(it->second);
        }

        MusicFile music;
        if (!ReadMusicFile(rom, at, music))
        {
            rejected.insert(at);
            return -1;
        }

        found[at] = files.size();
        files.push_back(std::move(music));

        return int(files.size() - 1);
    };

    // Puts the file at index `i` of `files` next in the order, unless it's there already or `i` is -1.
    std::vector<size_t> order;
    std::vector<bool> placed;
    const auto place = [&](int i)
    {
        placed.resize(files.size());
        if (i >= 0 && !placed[size_t(i)])
        {
            placed[size_t(i)] = true;
            order.push_back(size_t(i));
        }
    };

    // The entries of tables first, then the other words, then a scan if no word points to a file.
    for (uint32_t at = kRomBase; rom.Contains(at, 8); at += 4)
    {
        const int first = lookup(rom.U32(at));
        const int second = first >= 0 ? lookup(rom.U32(at + 4)) : -1;
        if (second >= 0)
        {
            place(first);
            place(second);
        }
    }
    for (uint32_t at = kRomBase; rom.Contains(at, 4); at += 4)
    {
        place(lookup(rom.U32(at)));
    }
    scanned = order.empty();
    for (uint32_t at = kRomBase; scanned && rom.Contains(at, 16) && order.size() < kMaxModules; at += 4)
    {
        place(lookup(at));
    }

    std::vector<MusicFile> listed;
    for (size_t i = 0; i < order.size() && i < kMaxModules; i++)
    {
        listed.push_back(std::move(files[order[i]]));
    }

    return listed;
}

// Returns the music files that the game plays once. The game's routines that play a file call the loader, directly or
// through a routine that only calls it, and then krapPlay(). A call of such a routine with ldr r0, =file; movs r1, #0
// plays the file in the jingle mode.
std::vector<uint32_t> MusicJingles(const Rom& rom, uint32_t loader_code)
{
    // The loader starts with push {r4-r7, lr}.
    uint32_t loader = 0;
    for (uint32_t back = 2; back <= 0x80 && !loader; back += 2)
    {
        loader = rom.U16(loader_code - back) == 0xB5F0 ? loader_code - back : 0;
    }
    if (!loader)
    {
        return {};
    }

    std::vector<uint32_t> calls = FindCalls(rom, loader);
    for (uint32_t call : std::vector<uint32_t>(calls))
    {
        // A routine of push {lr}; bl loader; pop {r1}; bx r1.
        if (rom.U16(call - 2) == 0xB500 && rom.U16(call + 4) == 0xBC02 && rom.U16(call + 6) == 0x4708)
        {
            const std::vector<uint32_t> outer = FindCalls(rom, call - 2);
            calls.insert(calls.end(), outer.begin(), outer.end());
        }
    }

    std::vector<uint32_t> jingles;
    for (uint32_t call : calls)
    {
        uint32_t routine = 0;
        for (uint32_t back = 2; back <= 0x40 && !routine; back += 2)
        {
            routine = (rom.U16(call - back) & 0xFF00) == 0xB500 ? call - back : 0;
        }
        for (uint32_t play : routine ? FindCalls(rom, routine) : std::vector<uint32_t>())
        {
            if (rom.U16(play - 2) == 0x2100 && (rom.U16(play - 4) & 0xF800) == 0x4800)
            {
                jingles.push_back(ThumbLiteral(rom, play - 4));
            }
        }
    }

    return jingles;
}

// Lists the modified build's modules: unpacks the music files and maps their modules after the ROM's end, in a copy of
// the ROM that `info.unpacked` holds. Returns the modules of the files that the game plays once. Returns false and sets
// `error` if there are no modules.
bool ListMusicFiles(const Rom& rom, uint32_t loader_code, const std::string& program, DriverInfo& info,
                    std::vector<uint32_t>& jingles, std::string& error)
{
    bool scanned = false;
    std::vector<MusicFile> files = FindMusicFiles(rom, scanned);

    // Each file's module follows the ROM's end, laid out as the game's loader does.
    std::vector<uint8_t> image(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    const uint32_t start = (rom.End() + kUnpackedAlign - 1) & ~(kUnpackedAlign - 1);
    image.resize(start - kRomBase);
    for (MusicFile& music : files)
    {
        const uint32_t base = kRomBase + uint32_t(image.size());
        PlaceModule(music, base);
        image.insert(image.end(), music.data.begin(), music.data.end());
        image.resize((image.size() + 3) & ~size_t(3));
        info.modules.push_back(base + music.module);
        info.files.push_back(music.file);
    }
    if (info.modules.empty())
    {
        error = "found Krawall, but no music files";
        return false;
    }

    auto unpacked = std::make_shared<Rom>();
    unpacked->Assign(std::move(image));
    std::vector<uint32_t> keep_modules, keep_files;
    for (size_t i = 0; i < info.modules.size(); i++)
    {
        ModuleInfo module;
        if (!ReadModule(*unpacked, info.build, info.modules[i], module))
        {
            continue;
        }

        // The game unpacks a file's samples with a routine that detection doesn't read.
        if (files[i].samples)
        {
            info.warnings.push_back("the music file at " + Hex(files[i].file) + " lists " +
                                    std::to_string(files[i].samples) + " samples to unpack. " + program +
                                    " can't read them and leaves the file out");
            continue;
        }

        keep_modules.push_back(info.modules[i]);
        keep_files.push_back(info.files[i]);
    }

    info.modules = std::move(keep_modules);
    info.files = std::move(keep_files);
    info.unpacked = std::move(unpacked);
    if (info.modules.empty())
    {
        error = "found Krawall, but no music files";
        return false;
    }

    for (uint32_t file : MusicJingles(rom, loader_code))
    {
        const auto it = std::find(info.files.begin(), info.files.end(), file);
        if (it != info.files.end())
        {
            jingles.push_back(info.modules[size_t(it - info.files.begin())]);
        }
    }

    info.log.push_back(std::string("music files, from ") +
                       (scanned ? "a scan of the ROM" : "the game's code and tables") + ", and their modules:");
    for (size_t i = 0; i < info.files.size(); i++)
    {
        info.log.push_back("  " + Hex(info.files[i]) + ": " + Hex(info.modules[i]));
    }

    return true;
}

} // namespace

} // namespace ntdlib::krawall
