// SPDX-License-Identifier: MIT

// Loads a title for ripping (see title.h).

#include "disc/title.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "disc/cpk_files.h"
#include "disc/encodings.h"
#include "disc/file_table.h"
#include "ntdlib/disc/cx.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/disc/source.h"
#include "ntdlib/dol.h"

namespace ntdlib::disc::detail
{
namespace
{

namespace fs = std::filesystem;

constexpr uint32_t kU8Magic = 0x55AA382D;
constexpr uint32_t kImetMagic = 0x494D4554; // "IMET"

// The message for a WAD.
std::string WadMessage(const GameOptions& options)
{
    return "it's a WAD. A WAD's contents are encrypted. " + options.program +
           " reads only decrypted contents. Use a folder with the title's TMD (.tmd) and decrypted contents (.app) "
           "instead. A WAD unpacker that decrypts the contents writes such a folder. An installed title's content "
           "folder from a Wii's NAND works too. Dolphin's Tools > Install WAD installs a WAD in Dolphin's NAND. The "
           "title's content folder is then under Wii/title/00010001 in Dolphin's user folder";
}

// How a folder names a title's contents: in 8 hex digits, with the extension .app. WAD unpackers name each content
// after its index in the TMD. The Wii's NAND names it after its content ID.
enum class Naming
{
    kIndex,
    kId,
};

// A content's record in the TMD.
struct ContentRecord
{
    uint32_t id;
    uint16_t index;
    uint64_t size;
    xsflib::Sha1::Digest hash; // of the decrypted content
};

// What the loader reads from a TMD.
struct Tmd
{
    uint64_t title_id;
    uint16_t boot_index;
    std::vector<ContentRecord> contents;
};

// A content that the folder holds.
struct Content
{
    const ContentRecord* record;
    std::string file;                           // the file's name, for messages
    std::shared_ptr<std::vector<uint8_t>> data; // decompressed if the title stores it compressed
};

// The contents that the folder holds, and how it names them.
struct Contents
{
    Naming naming;
    std::vector<Content> held;
};

// A file in one of the title's U8 archives.
struct TitleFile
{
    std::shared_ptr<const std::vector<uint8_t>> data; // the archive, or the file alone once decompressed
    size_t offset;
    size_t size;
};

std::string Hex8(uint32_t value)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string text(8, '0');
    for (size_t i = 8; i-- > 0; value >>= 4)
    {
        text[i] = kDigits[value & 15];
    }

    return text;
}

std::string ContentFile(const ContentRecord& record, Naming naming)
{
    return Hex8(naming == Naming::kIndex ? record.index : record.id) + ".app";
}

std::string NameOf(const fs::path& path)
{
    const std::u8string name = path.filename().u8string();

    return std::string(name.begin(), name.end());
}

// The folder's files whose names end in `extension`, by their names in lower case. AppleDouble files are left out.
// macOS writes one, named "._" and the file's name, beside each file that it copies to a FAT or exFAT drive.
std::map<std::string, fs::path> FilesEndingIn(const fs::path& folder, const std::string& extension)
{
    std::map<std::string, fs::path> files;
    std::error_code error;
    for (auto it = fs::directory_iterator(folder, error); !error && it != fs::directory_iterator(); it.increment(error))
    {
        std::error_code type_error;
        const std::string name = xsflib::AsciiLower(NameOf(it->path()));
        if (name.ends_with(extension) && !name.starts_with("._") && it->is_regular_file(type_error))
        {
            files[name] = it->path();
        }
    }

    return files;
}

// Reads a TMD. A TMD starts with its signature: the type (0x10001 for RSA-2048), the signature and padding. Its header
// follows at 0x140. The header gives the title ID at 0x18C, the number of contents at 0x1DE and the boot content's
// index at 0x1E0. The contents' records start at 0x1E4. Each takes 36 bytes: the content's ID, index, type, size and
// SHA-1.
Tmd ParseTmd(const std::vector<uint8_t>& data, const std::string& name)
{
    if (data.size() < 0x1E4 || xsflib::LoadBe32(data.data()) != 0x00010001)
    {
        throw Error(name + " isn't a TMD");
    }

    Tmd tmd{xsflib::LoadBe64(&data[0x18C]), xsflib::LoadBe16(&data[0x1E0]), {}};
    const size_t count = xsflib::LoadBe16(&data[0x1DE]);
    if (data.size() < 0x1E4 + count * 36)
    {
        throw Error(name + " is truncated");
    }

    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t* record = &data[0x1E4 + i * 36];
        ContentRecord content{xsflib::LoadBe32(record), xsflib::LoadBe16(record + 4), xsflib::LoadBe64(record + 8), {}};
        std::copy_n(record + 16, content.hash.size(), content.hash.begin());
        tmd.contents.push_back(content);
    }

    return tmd;
}

// True if `data`, a content's file, holds the content: the file has the size and the SHA-1 that the record gives. Up to
// 63 bytes of padding may follow the content.
bool Holds(const std::shared_ptr<std::vector<uint8_t>>& data, const ContentRecord& record)
{
    return data && data->size() >= record.size && data->size() - record.size < 64 &&
           xsflib::Sha1::Of({data->data(), static_cast<size_t>(record.size)}) == record.hash;
}

// A content as the title uses it: decompressed if the title stores it compressed. Data that only looks compressed stays
// as it is.
std::shared_ptr<std::vector<uint8_t>> UnpackContent(std::shared_ptr<std::vector<uint8_t>> data)
{
    if (!IsCx(*data))
    {
        return data;
    }

    try
    {
        return std::make_shared<std::vector<uint8_t>>(DecompressCx(*data, kMaxFileSize));
    }
    catch (const Error&)
    {
        return data;
    }
}

// Reads the contents that the folder holds. The folder's naming is the one under which more contents' files hold their
// contents. Only the contents whose two names differ count. Throws Error if a content's file doesn't hold the content.
Contents ReadContents(const fs::path& folder, const Tmd& tmd, const GameOptions& options)
{
    const std::map<std::string, fs::path> files = FilesEndingIn(folder, ".app");
    std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> read;
    const auto load = [&files, &read](const std::string& name) -> std::shared_ptr<std::vector<uint8_t>>
    {
        const auto file = files.find(name);
        if (file == files.end())
        {
            return nullptr;
        }

        std::shared_ptr<std::vector<uint8_t>>& data = read[name];
        if (!data)
        {
            data = std::make_shared<std::vector<uint8_t>>();
            if (!ReadHostFile(file->second, *data))
            {
                throw Error("can't read " + NameOf(file->second));
            }
        }

        return data;
    };

    int by_index = 0;
    int by_id = 0;
    for (const ContentRecord& record : tmd.contents)
    {
        if (record.index != record.id)
        {
            by_index += Holds(load(ContentFile(record, Naming::kIndex)), record);
            by_id += Holds(load(ContentFile(record, Naming::kId)), record);
        }
    }

    Contents contents{by_id > by_index ? Naming::kId : Naming::kIndex, {}};
    for (const ContentRecord& record : tmd.contents)
    {
        const std::string name = ContentFile(record, contents.naming);
        std::shared_ptr<std::vector<uint8_t>> data = load(name);
        if (!data)
        {
            continue;
        }
        if (!Holds(data, record))
        {
            throw Error(name + " doesn't match the title's TMD. The content is encrypted or damaged. " +
                        options.program + " requires decrypted contents");
        }

        data->resize(static_cast<size_t>(record.size));
        contents.held.push_back({&record, name, UnpackContent(std::move(data))});
    }

    return contents;
}

const Content* Held(const Contents& contents, uint16_t index)
{
    for (const Content& content : contents.held)
    {
        if (content.record->index == index)
        {
            return &content;
        }
    }

    return nullptr;
}

bool IsDol(const std::vector<uint8_t>& data)
{
    const std::optional<Dol> dol = Dol::Parse(data);

    return dol && !dol->Sections().empty();
}

// The title's main program. Every title but the System Menu boots the SDK's NAND Boot Program, its boot content. That
// program loads the main program from content 1. A compressed main program is decompressed first. A title whose content
// 1 isn't a DOL boots its main program itself.
std::vector<uint8_t> MainProgram(const Tmd& tmd, const Contents& contents)
{
    const auto record = std::find_if(tmd.contents.begin(), tmd.contents.end(),
                                     [](const ContentRecord& content) { return content.index == 1; });
    if (record != tmd.contents.end())
    {
        const Content* program = Held(contents, 1);
        if (!program)
        {
            throw Error("the folder lacks " + ContentFile(*record, contents.naming) +
                        ". It holds the title's main program");
        }
        if (IsDol(*program->data))
        {
            return *program->data;
        }
    }

    const Content* boot = Held(contents, tmd.boot_index);
    if (boot && IsDol(*boot->data))
    {
        return *boot->data;
    }

    throw Error("the title has no main program. Neither content 1 nor the boot content is a DOL");
}

// The game ID: the title ID's last four bytes as characters, or as 8 hex digits if they aren't all printable.
std::string GameIdOf(uint64_t title_id)
{
    const auto low = static_cast<uint32_t>(title_id);
    std::string id;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const auto c = static_cast<uint8_t>(low >> shift);
        if (c <= 0x20 || c >= 0x7F)
        {
            return Hex8(low);
        }

        id += static_cast<char>(c);
    }

    return id;
}

// Converts UTF-16BE text, up to its first NUL, to UTF-8. Control characters become spaces. The spaces at either end are
// removed. A surrogate without its pair becomes U+FFFD.
std::string DecodeUtf16Be(std::span<const uint8_t> text)
{
    std::string out;
    for (size_t i = 0; i + 1 < text.size(); i += 2)
    {
        uint32_t code = xsflib::LoadBe16(&text[i]);
        if (code == 0)
        {
            break;
        }

        const uint32_t next = i + 3 < text.size() ? xsflib::LoadBe16(&text[i + 2]) : 0;
        if (code >= 0xD800 && code < 0xDC00 && next >= 0xDC00 && next < 0xE000)
        {
            code = 0x10000 + ((code - 0xD800) << 10) + (next - 0xDC00);
            i += 2;
        }
        else if (code >= 0xD800 && code < 0xE000)
        {
            code = 0xFFFD;
        }

        AppendUtf8(out, code < 0x20 ? ' ' : code);
    }

    const size_t first = out.find_first_not_of(' ');

    return first == std::string::npos ? "" : out.substr(first, out.find_last_not_of(' ') + 1 - first);
}

// The English name in a title's banner, its content 0, or nothing. The banner's IMET header starts at 0x80 in a title's
// content 0. The header's names start 0x1C bytes into it: 42 UTF-16BE characters in each of ten languages, English
// second.
std::string BannerName(const Contents& contents)
{
    constexpr size_t kImet = 0x80;
    constexpr size_t kName = 0x54;
    constexpr size_t kEnglish = kImet + 0x1C + kName;
    const Content* banner = Held(contents, 0);
    if (!banner || banner->data->size() < kEnglish + kName || xsflib::LoadBe32(&(*banner->data)[kImet]) != kImetMagic)
    {
        return "";
    }

    return DecodeUtf16Be(std::span(*banner->data).subspan(kEnglish, kName));
}

// The files of a U8 archive. The archive starts with the magic 0x55AA382D, the offset of its file system table (0x20),
// the table's size with its names, and the offset of the files' data. The table has the layout of a disc's file system
// table (see ParseFileTable). It gives each file's offset in bytes from the start of the archive.
std::map<std::string, FileEntry> ReadU8(std::span<const uint8_t> archive, const std::string& name)
{
    const std::string damaged = name + " holds a damaged U8 archive";
    if (archive.size() < 0x20)
    {
        throw Error(damaged);
    }

    const size_t table = xsflib::LoadBe32(&archive[4]);
    const size_t table_size = xsflib::LoadBe32(&archive[8]);
    if (table > archive.size() || table_size > archive.size() - table)
    {
        throw Error(damaged);
    }

    std::map<std::string, FileEntry> files = ParseFileTable(archive.subspan(table, table_size), 0, damaged);
    for (const auto& [path, file] : files)
    {
        if (file.offset > archive.size() || file.size > archive.size() - file.offset)
        {
            throw Error(damaged);
        }
    }

    return files;
}

// A sound archive (.brsar) that the title stores compressed, decompressed. The Home Button menu's sound archives are
// compressed. Any other file stays as it is.
TitleFile UnpackSoundArchive(const std::string& path, const TitleFile& file)
{
    const std::span<const uint8_t> data(file.data->data() + file.offset, file.size);
    if (!xsflib::AsciiLower(path).ends_with(".brsar") || !IsCx(data))
    {
        return file;
    }

    try
    {
        auto archive = std::make_shared<const std::vector<uint8_t>>(DecompressCx(data, kMaxFileSize));

        return {archive, 0, archive->size()};
    }
    catch (const Error&)
    {
        return file;
    }
}

// The files of the title's U8 archives, by path. The archives' files make one tree. When two archives hold a file at
// the same path, each archive's files go in a folder named after its content instead: content5 for content 5.
std::map<std::string, TitleFile> ArchiveFiles(const Contents& contents)
{
    struct Found
    {
        uint16_t index; // the content's index
        std::string path;
        TitleFile file;
    };

    std::vector<Found> found;
    std::set<std::string> paths;
    bool clash = false;
    for (const Content& content : contents.held)
    {
        const std::vector<uint8_t>& data = *content.data;
        if (data.size() < 4 || xsflib::LoadBe32(data.data()) != kU8Magic)
        {
            continue;
        }

        for (const auto& [path, entry] : ReadU8(data, content.file))
        {
            clash |= !paths.insert(path).second;
            found.push_back(
                {content.record->index, path, {content.data, static_cast<size_t>(entry.offset), entry.size}});
        }
    }

    std::map<std::string, TitleFile> files;
    for (const Found& file : found)
    {
        const std::string path = clash ? "content" + std::to_string(file.index) + "/" + file.path : file.path;
        files[path] = UnpackSoundArchive(path, file.file);
    }

    return files;
}

} // namespace

bool HoldsTitle(const std::filesystem::path& folder)
{
    return !FilesEndingIn(folder, ".tmd").empty() || !FilesEndingIn(folder, ".app").empty();
}

void LoadTitle(const std::filesystem::path& folder, GameFiles& out, const GameOptions& options)
{
    const std::map<std::string, fs::path> tmds = FilesEndingIn(folder, ".tmd");
    if (tmds.empty())
    {
        throw Error("the folder holds a title's contents (.app) but not its TMD (.tmd). The TMD lists the contents");
    }
    if (tmds.size() > 1)
    {
        throw Error("the folder holds more than one TMD (.tmd)");
    }

    const fs::path& tmd_path = tmds.begin()->second;
    std::vector<uint8_t> tmd_bytes;
    if (!ReadHostFile(tmd_path, tmd_bytes))
    {
        throw Error("can't read " + NameOf(tmd_path));
    }

    const Tmd tmd = ParseTmd(tmd_bytes, NameOf(tmd_path));
    const Contents contents = ReadContents(folder, tmd, options);
    if (contents.held.empty())
    {
        throw Error("the folder holds none of the contents that " + NameOf(tmd_path) + " lists");
    }

    out.source = "unpacked title";
    out.tmd = std::move(tmd_bytes);
    out.boot.clear();
    out.bi2.clear();
    out.apploader.clear();
    out.dol = MainProgram(tmd, contents);
    out.fst.clear();
    out.title_game_id = GameIdOf(tmd.title_id);
    const std::string name = BannerName(contents);
    out.title_name = name.empty() ? out.title_game_id : name;

    const auto files = std::make_shared<const std::map<std::string, TitleFile>>(ArchiveFiles(contents));
    out.files.clear();
    for (const auto& [path, file] : *files)
    {
        out.files.push_back({path, file.size, static_cast<uint32_t>(out.files.size())});
    }

    out.read_file = [files](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto file = files->find(path);
        if (file == files->end())
        {
            return false;
        }

        const uint8_t* start = file->second.data->data() + file->second.offset;
        data.assign(start, start + file->second.size);

        return true;
    };

    if (options.cpk_sound_archives)
    {
        const auto read_range = [files](const std::string& path, uint64_t offset, size_t size)
        {
            const TitleFile& file = files->at(path);
            if (offset > file.size || size > file.size - offset)
            {
                throw Error("the file is truncated");
            }

            const uint8_t* start = file.data->data() + file.offset + offset;
            return std::vector<uint8_t>(start, start + size);
        };
        AddCpkSoundArchives(out, read_range);
    }
}

void RejectTitleFile(const std::filesystem::path& path, const GameOptions& options)
{
    // An installable WAD starts with its header's size, 0x20, and its type, "Is" ("ib" for boot2). The System Menu
    // writes backup WADs to SD cards. A backup WAD starts with 0x70 and "Bk" (WiiBrew's "WAD files").
    uint8_t head[6] = {};
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(head), sizeof(head));
    const uint32_t header_size = xsflib::LoadBe32(head);
    const std::string type(reinterpret_cast<const char*>(head + 4), 2);
    const std::u8string extension_text = path.extension().u8string();
    const std::string extension = xsflib::AsciiLower(std::string(extension_text.begin(), extension_text.end()));
    if ((header_size == 0x20 && (type == "Is" || type == "ib")) || (header_size == 0x70 && type == "Bk") ||
        extension == ".wad")
    {
        throw Error(WadMessage(options));
    }
    if (extension == ".tmd" || extension == ".app")
    {
        throw Error("it's one of a title's files. Use the folder that holds the title's TMD and contents");
    }
}

} // namespace ntdlib::disc::detail
