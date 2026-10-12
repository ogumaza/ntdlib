// SPDX-License-Identifier: MIT

// Loads a GameCube or Wii game for ripping (see game.h). YAGCD ("Yet Another GameCube Documentation") documents the
// GameCube disc's layout: the header, the apploader, main.dol and the file system. WiiBrew's "Wii disc" page documents
// the Wii disc's layout: the partition table, a partition's header, the hashed sectors and the file system.

#include "ntdlib/disc/game.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "disc/encodings.h"
#include "disc/file_table.h"
#include "disc/image_parts.h"
#include "disc/title.h"
#include "ntdlib/disc/header.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/dol.h"
#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"

namespace ntdlib::disc
{
namespace
{

namespace fs = std::filesystem;

using detail::DecodeShiftJis;
using detail::DecodeWindows1252;
using detail::FileEntry;
using detail::HoldsTitle;
using detail::kMaxFileSize;
using detail::kSector;
using detail::kSectorData;
using detail::LoadTitle;
using detail::ReadHostFile;
using detail::RejectTitleFile;
constexpr uint64_t kHashes = 0x400; // the hashes at the start of a sector

// The disc's region field (GameFiles::region): the word at kBi2Region in a GameCube disc's bi2.bin, or the word at
// kWiiRegion on a Wii disc. The value kJapan marks a Japanese disc.
constexpr size_t kBi2Region = 0x18;
constexpr uint64_t kWiiRegion = 0x4E000;
constexpr uint32_t kJapan = 0;

// The message for a folder that holds neither an extracted disc nor a title.
constexpr char kNoGame[] =
    "the folder holds no game. An extracted disc holds sys/main.dol and files/, directly or in a DATA folder. A "
    "title's folder holds the title's TMD (.tmd) and its decrypted contents (.app)";

// The message for a Wii image whose game partition is encrypted.
std::string EncryptedMessage(const GameOptions& options)
{
    return "the game partition is encrypted. " + options.program +
           " requires a decrypted partition. NKit, RVZ and WIA images store it that way. Convert this image to RVZ "
           "with Dolphin (right-click the game, then Convert File) and use the RVZ. The conversion keeps all the data";
}

// The message for a disc of the other console.
std::string OtherConsoleMessage(const GameOptions& options)
{
    if (!options.other_console.empty())
    {
        return options.other_console;
    }

    return options.console == Console::kWii ? "it's a GameCube disc, and " + options.program + " is for Wii discs"
                                            : "it's a Wii disc, and " + options.program + " is for GameCube discs";
}

// The message for a damaged file system.
std::string DamagedMessage(const GameOptions& options)
{
    if (!options.damaged_file_system.empty())
    {
        return options.damaged_file_system;
    }

    return options.console == Console::kWii ? "the game partition's file system is damaged"
                                            : "the disc's file system is damaged";
}

// Parses a file system table (fst.bin) into its files (see ParseFileTable). A Wii disc gives the offsets divided by 4.
std::map<std::string, FileEntry> ParseFst(const std::vector<uint8_t>& fst, const GameOptions& options)
{
    return detail::ParseFileTable(fst, options.console == Console::kWii ? 2 : 0, DamagedMessage(options));
}

// A game's data in the form that the game reads it.
class GameData
{
public:
    virtual ~GameData() = default;

    // The source of the data for messages, such as "NKit ISO".
    virtual std::string Source() const = 0;

    // Reads `size` bytes at `offset`. Throws Error.
    virtual std::vector<uint8_t> Read(uint64_t offset, uint64_t size) = 0;

    // The number of bytes that a plain image stores (Image::StoredSize). The offsets of Read count in the same bytes.
    virtual std::optional<uint64_t> StoredSize() const
    {
        return std::nullopt;
    }
};

// A GameCube disc image in the form that the game reads.
class GameDisc : public GameData
{
public:
    GameDisc(std::unique_ptr<Image> image, const GameOptions& options) : image_(std::move(image))
    {
        uint8_t header[0x208];
        image_->Read(0, header, sizeof(header));
        if (xsflib::LoadBe32(header + kGameCubeMagicOffset) != kGameCubeMagic)
        {
            if (xsflib::LoadBe32(header + kWiiMagicOffset) == kWiiMagic)
            {
                throw Error(OtherConsoleMessage(options));
            }

            throw Error(options.not_this_console.empty() ? "it isn't a GameCube disc image" : options.not_this_console);
        }

        // A program that reads plain images may reject one that ends before the disc header's 0x440 bytes.
        const std::optional<uint64_t> stored = image_->StoredSize();
        if (!options.short_header.empty() && stored && *stored < kHeaderSize)
        {
            throw Error(options.short_header);
        }

        nkit_ = std::equal(header + 0x200, header + 0x208, "NKIT v01");
    }

    std::string Source() const override
    {
        return (nkit_ ? "NKit " : "") + image_->Format();
    }

    std::optional<uint64_t> StoredSize() const override
    {
        return image_->StoredSize();
    }

    std::vector<uint8_t> Read(uint64_t offset, uint64_t size) override
    {
        if (size > kMaxFileSize)
        {
            throw Error("a file on the disc is too large");
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        image_->Read(offset, data.data(), data.size());

        // NKit keeps its header at 0x200-0x21F, in the unused end of the disc header's title. The original disc has
        // zeros there.
        if (nkit_ && offset < kNkitHeaderEnd && offset + size > kNkitHeader)
        {
            const uint64_t from = std::max(offset, kNkitHeader);
            const uint64_t to = std::min(offset + size, kNkitHeaderEnd);
            std::fill(data.begin() + static_cast<std::ptrdiff_t>(from - offset),
                      data.begin() + static_cast<std::ptrdiff_t>(to - offset), uint8_t{0});
        }

        return data;
    }

private:
    static constexpr uint64_t kNkitHeader = 0x200;
    static constexpr uint64_t kNkitHeaderEnd = 0x220;

    std::unique_ptr<Image> image_;
    bool nkit_ = false;
};

// The game partition of a Wii disc image in the form that the game reads: decrypted and without the hashes.
class GamePartition : public GameData
{
public:
    GamePartition(std::unique_ptr<Image> image, const GameOptions& options) : image_(std::move(image))
    {
        uint8_t header[0x400];
        image_->Read(0, header, sizeof(header));
        if (xsflib::LoadBe32(header + kWiiMagicOffset) != kWiiMagic)
        {
            if (xsflib::LoadBe32(header + kGameCubeMagicOffset) == kGameCubeMagic)
            {
                throw Error(OtherConsoleMessage(options));
            }

            throw Error(options.not_this_console.empty() ? "it isn't a Wii disc image" : options.not_this_console);
        }

        nkit_ = std::equal(header + 0x200, header + 0x208, "NKIT v01");
        const uint64_t partition = FindGamePartition();
        uint8_t partition_header[0x2C0];
        image_->Read(partition, partition_header, sizeof(partition_header));
        const uint32_t tmd_size = xsflib::LoadBe32(partition_header + 0x2A4);
        if (tmd_size > 0x10000)
        {
            throw Error("the game partition's TMD is invalid");
        }

        tmd_.resize(tmd_size);
        image_->Read(partition + uint64_t{xsflib::LoadBe32(partition_header + 0x2A8)} * 4, tmd_.data(), tmd_size);

        // WIA and RVZ store the partition's data decrypted. Other images store it as on the disc. The data is encrypted
        // unless the disc header turns encryption off (byte 0x61). It's in hashed sectors unless the header turns
        // hashes off (byte 0x60). NKit images turn these off. Wiimm's tools leave both bytes at 0 in images whose
        // partitions they decrypt. A partition that the header calls encrypted therefore still counts as decrypted if
        // its data starts with a Wii disc header. Encrypted data starts that way with a chance of one in four billion.
        data_offset_ = partition + uint64_t{xsflib::LoadBe32(partition_header + 0x2B8)} * 4;
        stored_ = image_->OpenPartitionData(data_offset_);
        hashed_ = !header[0x60];
        if (!stored_ && !header[0x61])
        {
            uint8_t first[0x20];
            image_->Read(data_offset_ + (hashed_ ? kHashes : 0), first, sizeof(first));
            if (xsflib::LoadBe32(first + kWiiMagicOffset) != kWiiMagic)
            {
                throw Error(EncryptedMessage(options));
            }
        }

        uint8_t boot[0x20];
        Read(0, boot, sizeof(boot));
        if (xsflib::LoadBe32(boot + kWiiMagicOffset) != kWiiMagic)
        {
            throw Error("the game partition is damaged, or encrypted although the disc header says it isn't");
        }
    }

    std::string Source() const override
    {
        return (nkit_ ? "NKit " : "") + image_->Format();
    }

    const std::vector<uint8_t>& Tmd() const
    {
        return tmd_;
    }

    // The word in the disc's region field, outside the partitions.
    uint32_t Region()
    {
        uint8_t region[4];
        image_->Read(kWiiRegion, region, sizeof(region));

        return xsflib::LoadBe32(region);
    }

    void Read(uint64_t offset, uint8_t* out, size_t size)
    {
        if (stored_)
        {
            stored_->Read(offset, out, size);

            return;
        }
        if (!hashed_)
        {
            image_->Read(data_offset_ + offset, out, size);

            return;
        }

        while (size)
        {
            const uint64_t sector = offset / kSectorData;
            const size_t within = static_cast<size_t>(offset % kSectorData);
            const size_t n = static_cast<size_t>(std::min<uint64_t>(size, kSectorData - within));
            image_->Read(data_offset_ + sector * kSector + kHashes + within, out, n);
            out += n;
            offset += n;
            size -= n;
        }
    }

    std::vector<uint8_t> Read(uint64_t offset, uint64_t size) override
    {
        if (size > kMaxFileSize)
        {
            throw Error("a file in the game partition is too large");
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        Read(offset, data.data(), data.size());

        return data;
    }

private:
    // The offset of the first game partition (type 0) in the four groups of the partition table.
    uint64_t FindGamePartition()
    {
        uint8_t table[0x20];
        image_->Read(0x40000, table, sizeof(table));
        for (int group = 0; group < 4; ++group)
        {
            const uint32_t count = xsflib::LoadBe32(table + group * 8);
            if (count > 64)
            {
                throw Error("the partition table is invalid");
            }

            std::vector<uint8_t> entries(size_t{count} * 8);
            image_->Read(uint64_t{xsflib::LoadBe32(table + group * 8 + 4)} * 4, entries.data(), entries.size());
            for (uint32_t i = 0; i < count; ++i)
            {
                if (xsflib::LoadBe32(entries.data() + i * 8 + 4) == 0)
                {
                    return uint64_t{xsflib::LoadBe32(entries.data() + i * 8)} * 4;
                }
            }
        }

        throw Error("the disc has no game partition");
    }

    std::unique_ptr<Image> image_;
    std::unique_ptr<PartitionData> stored_; // the partition's data, if the image stores it separately
    uint64_t data_offset_ = 0;
    bool hashed_ = true;
    bool nkit_ = false;
    std::vector<uint8_t> tmd_;
};

void LoadImage(const fs::path& path, GameFiles& out, const GameOptions& options)
{
    std::shared_ptr<GameData> game;
    if (options.console == Console::kWii)
    {
        auto partition = std::make_shared<GamePartition>(OpenImage(path, options.console, options.image), options);
        out.source = partition->Source();
        out.tmd = partition->Tmd();
        out.region = partition->Region();
        game = partition;
    }
    else
    {
        game = std::make_shared<GameDisc>(OpenImage(path, options.console, options.image), options);
        out.source = game->Source();
        out.region = xsflib::LoadBe32(game->Read(kHeaderSize + kBi2Region, 4).data());
    }

    // The system files: the header, bi2, the apploader, main.dol and the file system table. The apploader starts with a
    // 0x20-byte header that gives its size and the size of its trailer. The disc header gives the offsets of main.dol
    // and the file system table, and the table's size. A Wii disc gives all three divided by 4.
    const int shift = options.console == Console::kWii ? 2 : 0;
    out.boot = game->Read(0, kHeaderSize);
    if (options.system_files)
    {
        out.bi2 = game->Read(kHeaderSize, 0x2000);
        const std::vector<uint8_t> apploader = game->Read(0x2440, 0x20);
        out.apploader = game->Read(0x2440, 0x20 + uint64_t{xsflib::LoadBe32(apploader.data() + 0x14)} +
                                               xsflib::LoadBe32(apploader.data() + 0x18));
        const uint64_t dol_offset = uint64_t{xsflib::LoadBe32(out.boot.data() + 0x420)} << shift;
        const std::vector<uint8_t> dol_header = game->Read(dol_offset, 0x100);
        out.dol = game->Read(dol_offset, Dol::Size(dol_header));
    }

    // With `file_past_end`, a file system that runs past the image's end is damaged.
    const uint64_t fst_offset = uint64_t{xsflib::LoadBe32(out.boot.data() + kHeaderFstOffset)} << shift;
    const uint64_t fst_size = uint64_t{xsflib::LoadBe32(out.boot.data() + kHeaderFstSize)} << shift;
    const std::optional<uint64_t> stored = game->StoredSize();
    if (options.file_past_end && stored && fst_offset + fst_size > *stored)
    {
        throw Error(DamagedMessage(options));
    }
    out.fst = game->Read(fst_offset, fst_size);

    auto files = std::make_shared<std::map<std::string, FileEntry>>(ParseFst(out.fst, options));
    out.files.clear();
    for (const auto& [name, entry] : *files)
    {
        out.files.push_back({name, entry.size, entry.index});
    }

    out.read_file =
        [game, files, past_end = options.file_past_end, stored](const std::string& name, std::vector<uint8_t>& data)
    {
        const auto file = files->find(name);
        if (file == files->end())
        {
            return false;
        }

        if (past_end && stored && file->second.offset + file->second.size > *stored)
        {
            throw Error(past_end(name));
        }

        data = game->Read(file->second.offset, file->second.size);

        return true;
    };
}

void LoadFolder(const fs::path& root, GameFiles& out, const GameOptions& options)
{
    out.source = "extracted disc";
    const fs::path sys = root / "sys";
    if (!ReadHostFile(sys / "boot.bin", out.boot) || out.boot.size() < kHeaderSize ||
        !ReadHostFile(sys / "main.dol", out.dol) || !ReadHostFile(sys / "fst.bin", out.fst))
    {
        throw Error("the extracted disc must contain sys/boot.bin, sys/main.dol and sys/fst.bin");
    }
    if (options.console == Console::kWii && xsflib::LoadBe32(out.boot.data() + kWiiMagicOffset) != kWiiMagic)
    {
        throw Error(xsflib::LoadBe32(out.boot.data() + kGameCubeMagicOffset) == kGameCubeMagic
                        ? OtherConsoleMessage(options)
                        : "sys/boot.bin isn't a Wii disc's header");
    }
    if (options.console == Console::kGameCube &&
        xsflib::LoadBe32(out.boot.data() + kGameCubeMagicOffset) != kGameCubeMagic)
    {
        throw Error(xsflib::LoadBe32(out.boot.data() + kWiiMagicOffset) == kWiiMagic
                        ? OtherConsoleMessage(options)
                        : "sys/boot.bin isn't a GameCube disc's header");
    }

    // A folder may leave these files out. A Wii folder keeps its tmd.bin beside sys/.
    ReadHostFile(sys / "bi2.bin", out.bi2);
    ReadHostFile(sys / "apploader.img", out.apploader);
    if (options.console == Console::kWii)
    {
        ReadHostFile(root / "tmd.bin", out.tmd);
    }

    // The files are those in the folder. There may be fewer than the file system table lists. An entry whose type or
    // size can't be read, such as a broken symbolic link, is left out. The walk goes on past it. AppleDouble files are
    // left out too. macOS writes one, named "._" and the file's name, beside each file that it copies to a FAT or
    // exFAT drive.
    const fs::path files = root / "files";
    std::error_code error;
    out.files.clear();
    for (auto it = fs::recursive_directory_iterator(files, error); !error && it != fs::recursive_directory_iterator();
         it.increment(error))
    {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || it->path().filename().u8string().starts_with(u8"._"))
        {
            continue;
        }

        const uint64_t size = it->file_size(entry_error);
        if (!entry_error)
        {
            const std::u8string name = it->path().lexically_relative(files).generic_u8string();
            out.files.push_back({std::string(name.begin(), name.end()), size});
        }
    }

    std::sort(out.files.begin(), out.files.end(), [](const GameFile& a, const GameFile& b) { return a.path < b.path; });
    for (std::size_t i = 0; i < out.files.size(); i++)
    {
        out.files[i].index = static_cast<uint32_t>(i);
    }
    out.read_file = [files](const std::string& name, std::vector<uint8_t>& data)
    {
        return ReadHostFile(files / xsflib::PathFromUtf8(name), data);
    };
}

} // namespace

std::string GameFiles::GameId() const
{
    if (IsTitle())
    {
        return title_game_id;
    }

    return boot.size() >= 6 ? std::string(boot.begin(), boot.begin() + 6) : std::string();
}

std::string GameFiles::Title() const
{
    if (IsTitle())
    {
        return title_name;
    }

    if (boot.size() < 0x60)
    {
        return std::string();
    }

    // The title runs from 0x20 to its first NUL, for at most 0x40 bytes. The region field shows whether the disc is
    // Japanese. Without the field, the ID's region letter does.
    const auto end = std::find(boot.begin() + 0x20, boot.begin() + 0x60, 0);
    const auto title = std::span(boot).subspan(0x20, static_cast<size_t>(end - (boot.begin() + 0x20)));
    const bool japanese = region ? *region == kJapan : boot[3] == 'J';

    return japanese ? DecodeShiftJis(title) : DecodeWindows1252(title);
}

bool GameFiles::IsTitle() const
{
    return !title_game_id.empty();
}

std::vector<std::string> GameFiles::SoundArchives() const
{
    std::vector<std::string> archives;
    for (const GameFile& file : files)
    {
        if (xsflib::AsciiLower(file.path).ends_with(".brsar"))
        {
            archives.push_back(file.path);
        }
    }

    return archives;
}

std::optional<Console> DiscConsole(std::span<const uint8_t> header)
{
    if (header.size() >= 0x20 && xsflib::LoadBe32(header.data() + kGameCubeMagicOffset) == kGameCubeMagic)
    {
        return Console::kGameCube;
    }
    if (header.size() >= 0x1C && xsflib::LoadBe32(header.data() + kWiiMagicOffset) == kWiiMagic)
    {
        return Console::kWii;
    }

    return std::nullopt;
}

std::optional<std::string> ImageFormat(std::span<const uint8_t> header)
{
    const std::string_view text(reinterpret_cast<const char*>(header.data()), header.size());
    if (std::string format = ContainerFormat(header); !format.empty())
    {
        return format;
    }
    if (text.starts_with(detail::kWbfsMagic))
    {
        return "WBFS";
    }
    if (text.starts_with(detail::kWdfMagic))
    {
        return "WDF";
    }
    if (DiscConsole(header))
    {
        return "ISO";
    }

    return std::nullopt;
}

std::optional<std::string> LoadGame(const std::filesystem::path& path, GameFiles& out, const GameOptions& options)
{
    try
    {
        out.region.reset();
        std::error_code error;
        if (fs::is_directory(path, error))
        {
            const fs::path data = path / "DATA";
            if (!options.titles || fs::is_regular_file(path / "sys" / "main.dol", error))
            {
                LoadFolder(fs::is_regular_file(path / "sys" / "main.dol", error) ? path : data, out, options);
            }
            else if (fs::is_regular_file(data / "sys" / "main.dol", error))
            {
                LoadFolder(data, out, options);
            }
            else if (HoldsTitle(path))
            {
                LoadTitle(path, out, options);
            }
            else
            {
                throw Error(kNoGame);
            }
        }
        else
        {
            if (options.titles)
            {
                RejectTitleFile(path, options);
            }

            LoadImage(path, out, options);
        }

        // An extracted GameCube disc gives its region in bi2.bin. A Wii partition's bi2.bin doesn't.
        if (!out.region && out.bi2.size() >= kBi2Region + 4 && DiscConsole(out.boot) == Console::kGameCube)
        {
            out.region = xsflib::LoadBe32(out.bi2.data() + kBi2Region);
        }
    }
    catch (const std::exception& e)
    {
        return e.what();
    }

    return std::nullopt;
}

} // namespace ntdlib::disc
