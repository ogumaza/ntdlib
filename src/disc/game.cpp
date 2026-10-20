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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"

#include "disc/cpk_files.h"
#include "disc/encodings.h"
#include "disc/file_table.h"
#include "disc/image_parts.h"
#include "disc/title.h"
#include "ntdlib/disc/header.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/disc/source.h"
#include "ntdlib/dol.h"

namespace ntdlib::disc
{
namespace
{

namespace fs = std::filesystem;

using detail::AddCpkSoundArchives;
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

// NKit keeps its header at 0x200-0x21F, in the unused end of the disc header's title. The header starts with
// "NKIT v01". The original disc has zeros there.
constexpr uint64_t kNkitHeader = 0x200;
constexpr uint64_t kNkitHeaderEnd = 0x220;

// Where the disc header gives the offset of main.dol: in bytes on a GameCube disc, and in units of 4 bytes in a Wii
// partition.
constexpr size_t kHeaderDolOffset = 0x420;

// The size of main.dol's header. The header gives the size of the whole file (Dol::Size).
constexpr size_t kDolHeaderSize = 0x100;

// A GameCube disc's system area, the disc header and bi2.bin, ends here. The apploader follows it.
constexpr uint64_t kSystemAreaEnd = kHeaderSize + 0x2000;

// The capacity of a GameCube disc.
constexpr uint64_t kGameCubeDiscSize = 1'459'978'240;

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

// Checks that `header` starts a disc header of the console that `options` names. Throws Error with
// OtherConsoleMessage for a header of the other console, and with `neither` for any other data. A header of neither
// console passes when `options` names the GameCube and `intact_without_magic` returns true. That function checks that
// the rest of the disc is intact. Some discs that never went on sale, such as tech demos, leave the GameCube's magic
// word out of their header.
void CheckConsole(const uint8_t* header, const GameOptions& options, const std::string& neither,
                  const std::function<bool()>& intact_without_magic = {})
{
    const bool gamecube = xsflib::LoadBe32(header + kGameCubeMagicOffset) == kGameCubeMagic;
    const bool wii = xsflib::LoadBe32(header + kWiiMagicOffset) == kWiiMagic;
    if (options.console == Console::kWii ? wii : gamecube)
    {
        return;
    }

    const bool other_console = options.console == Console::kWii ? gamecube : wii;
    if (!other_console && options.console == Console::kGameCube && intact_without_magic && intact_without_magic())
    {
        return;
    }

    throw Error(other_console ? OtherConsoleMessage(options) : neither);
}

// Whether a GameCube disc's file system table and main.dol are intact: the table parses, and main.dol is a DOL whose
// entry point lies in one of its sections.
bool SystemFilesIntact(std::span<const uint8_t> fst, std::span<const uint8_t> dol)
{
    try
    {
        detail::ParseFileTable(fst, 0, std::string());
    }
    catch (const Error&)
    {
        return false;
    }

    const std::optional<Dol> parsed = Dol::Parse(dol);

    return parsed && !parsed->At(parsed->Entry()).empty();
}

// Whether `image` holds a GameCube disc whose header leaves out the magic word but is otherwise intact. The header must
// place main.dol and the file system table past the system area. Both must end inside the bytes that a plain image
// stores, and inside a GameCube disc's capacity. Neither may be larger than the loaders read (kMaxFileSize). Both must
// be intact (SystemFilesIntact). A read that fails gives false.
bool IntactWithoutMagic(Image& image)
{
    try
    {
        uint8_t header[kHeaderSize];
        image.Read(0, header, sizeof(header));
        const uint64_t end = std::min(image.StoredSize().value_or(kGameCubeDiscSize), kGameCubeDiscSize);
        const uint64_t dol_offset = xsflib::LoadBe32(header + kHeaderDolOffset);
        const uint64_t fst_offset = xsflib::LoadBe32(header + kHeaderFstOffset);
        const uint64_t fst_size = xsflib::LoadBe32(header + kHeaderFstSize);
        if (dol_offset < kSystemAreaEnd || fst_offset < kSystemAreaEnd || dol_offset + kDolHeaderSize > end ||
            fst_offset + fst_size > end || fst_size > kMaxFileSize)
        {
            return false;
        }

        std::vector<uint8_t> dol(kDolHeaderSize);
        image.Read(dol_offset, dol.data(), dol.size());
        const uint64_t dol_size = Dol::Size(dol);
        if (dol_offset + dol_size > end || dol_size > kMaxFileSize)
        {
            return false;
        }

        dol.resize(static_cast<size_t>(dol_size));
        image.Read(dol_offset, dol.data(), dol.size());
        std::vector<uint8_t> fst(static_cast<size_t>(fst_size));
        image.Read(fst_offset, fst.data(), fst.size());

        return SystemFilesIntact(fst, dol);
    }
    catch (const Error&)
    {
        return false;
    }
}

// The message for an image of neither console.
std::string NotThisConsoleMessage(const GameOptions& options)
{
    if (!options.not_this_console.empty())
    {
        return options.not_this_console;
    }

    return options.console == Console::kWii ? "it isn't a Wii disc image" : "it isn't a GameCube disc image";
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

// True if `header`, the first 0x208 bytes of a disc at least, holds NKit's header.
bool IsNkit(const uint8_t* header)
{
    return std::equal(header + kNkitHeader, header + kNkitHeader + 8, "NKIT v01");
}

// The source of a game's data for messages: the image's format, after "NKit " for an NKit image.
std::string ImageSource(const Image& image, bool nkit)
{
    return (nkit ? "NKit " : "") + image.Format();
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
        CheckConsole(header, options, NotThisConsoleMessage(options), [this] { return IntactWithoutMagic(*image_); });

        // A program that reads plain images may reject one that ends before the disc header's 0x440 bytes.
        const std::optional<uint64_t> stored = image_->StoredSize();
        if (!options.short_header.empty() && stored && *stored < kHeaderSize)
        {
            throw Error(options.short_header);
        }

        nkit_ = IsNkit(header);
    }

    std::string Source() const override
    {
        return ImageSource(*image_, nkit_);
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

        // The original disc has zeros in place of NKit's header.
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
        CheckConsole(header, options, NotThisConsoleMessage(options));

        nkit_ = IsNkit(header);
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
        return ImageSource(*image_, nkit_);
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

        ReadByBlocks(offset, out, size, kSectorData, [this](uint64_t sector, size_t within, uint8_t* piece, size_t n)
                     { image_->Read(data_offset_ + sector * kSector + kHashes + within, piece, n); });
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
    // and the file system table, and the table's size. A Wii disc gives all three divided by 4. Its file system table
    // gives the files' offsets divided by 4 too.
    const int shift = options.console == Console::kWii ? 2 : 0;
    out.boot = game->Read(0, kHeaderSize);
    if (options.system_files)
    {
        out.bi2 = game->Read(kHeaderSize, 0x2000);
        const std::vector<uint8_t> apploader = game->Read(0x2440, 0x20);
        out.apploader = game->Read(0x2440, 0x20 + uint64_t{xsflib::LoadBe32(apploader.data() + 0x14)} +
                                               xsflib::LoadBe32(apploader.data() + 0x18));
        const uint64_t dol_offset = uint64_t{xsflib::LoadBe32(out.boot.data() + kHeaderDolOffset)} << shift;
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

    auto files = std::make_shared<std::map<std::string, FileEntry>>(
        detail::ParseFileTable(out.fst, shift, DamagedMessage(options)));
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

    if (options.cpk_sound_archives)
    {
        const auto read_range = [game, files](const std::string& name, uint64_t offset, size_t size)
        {
            return game->Read(files->at(name).offset + offset, size);
        };
        AddCpkSoundArchives(out, read_range);
    }
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
    CheckConsole(out.boot.data(), options,
                 options.console == Console::kWii ? "sys/boot.bin isn't a Wii disc's header"
                                                  : "sys/boot.bin isn't a GameCube disc's header",
                 [&out] { return SystemFilesIntact(out.fst, out.dol); });

    // A folder may leave these files out. A Wii folder keeps its tmd.bin beside sys/. An extracted GameCube disc gives
    // its region in bi2.bin, with the magic word in its header or without it. A Wii partition's bi2.bin doesn't.
    ReadHostFile(sys / "bi2.bin", out.bi2);
    if (out.bi2.size() >= kBi2Region + 4 && DiscConsole(out.boot) != Console::kWii)
    {
        out.region = xsflib::LoadBe32(out.bi2.data() + kBi2Region);
    }

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

    if (options.cpk_sound_archives)
    {
        const auto read_range = [files](const std::string& name, uint64_t offset, size_t size)
        {
            return Source({files / xsflib::PathFromUtf8(name)}, "file").Read(offset, size);
        };
        AddCpkSoundArchives(out, read_range);
    }
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
            // Without GameOptions::titles, a folder without sys/main.dol has to hold the disc in DATA.
            const fs::path data = path / "DATA";
            if (fs::is_regular_file(path / "sys" / "main.dol", error))
            {
                LoadFolder(path, out, options);
            }
            else if (!options.titles || fs::is_regular_file(data / "sys" / "main.dol", error))
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
    }
    catch (const std::exception& e)
    {
        return e.what();
    }

    return std::nullopt;
}

} // namespace ntdlib::disc
