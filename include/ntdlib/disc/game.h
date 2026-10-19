// SPDX-License-Identifier: MIT

// A game's files for ripping: the system files and the file system of a GameCube disc, or of a Wii disc's game
// partition with the partition's TMD. They come from a disc image in any format that OpenImage reads, or from an
// extracted disc. A Wii game partition must be stored decrypted. NKit, RVZ and WIA images store it that way. An
// encrypted partition is rejected with a message that explains how to convert the image. A title for the Wii's NAND,
// such as a WiiWare game, can come from a folder with the title's TMD and its decrypted contents (GameOptions::titles).
// It provides the same files as an extracted disc.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/image.h"

namespace ntdlib::disc
{

// What LoadGame reads, and how its messages name the program.
struct GameOptions
{
    Console console;
    std::string program; // the program's name in messages, such as "xyz" in "xyz is for Wii discs"

    // How the image's container opens (ImageOptions). A program that reads GCZ, CISO, WIA and RVZ sets
    // image.open_container to disc_containers' OpenContainer.
    ImageOptions image{};

    // Messages for a program that words them otherwise. An empty one keeps the default, given here. Only an image's
    // checks use not_this_console and damaged_file_system. An extracted disc has fixed messages for its sys/boot.bin,
    // and LoadGame doesn't parse its sys/fst.bin.
    std::string not_this_console{};    // a disc of neither console: "it isn't a GameCube disc image"
    std::string other_console{};       // a disc of the other console: "it's a Wii disc, and xyz is for GameCube discs"
    std::string damaged_file_system{}; // "the disc's file system is damaged"

    // Checks of a plain GameCube image's size, for a program that reads only plain images. Without them, reads past the
    // image's end return zeros. `short_header` is the message for an image that ends before the disc header's 0x440
    // bytes. `file_past_end` returns the message for a file that ends past the image's end. With it, a file system that
    // does that is damaged.
    std::string short_header{};
    std::function<std::string(const std::string& path)> file_past_end{};

    // Whether LoadGame reads an image's system files besides the disc header and the file system: bi2.bin, the
    // apploader and main.dol. LoadGame reads an extracted disc's system files whatever the setting.
    bool system_files = true;

    // Whether LoadGame loads a title for the Wii's NAND from a folder that holds the title's TMD and decrypted
    // contents. With it, LoadGame rejects a WAD, a TMD or a content file with a message that says what to load instead.
    // A title's game ID is the last four characters of its title ID, and its title is the banner's English name. The
    // title's main program serves as main.dol. The files of the title's U8 archives make up its file system. WiiBrew
    // documents the TMD, U8 archives, the banner and the NAND Boot Program.
    bool titles = false;

    // Whether LoadGame lists the sound archives (.brsar) inside the game's CPK archives (.cpk, see cpk.h). A sound
    // archive inside a CPK is listed at the CPK's path, a slash and the archive's path in the CPK, such as
    // "data.cpk/SOUND/a.brsar". Its size is the size once decompressed. The CPK stays among the files too. A CPK
    // whose tables are damaged adds nothing. Neither does a CPK that lists its files by ID only. LoadGame reads only
    // each CPK's first chunk and its TOC. read_file reads a sound archive's bytes from the CPK. It decompresses the
    // bytes that the CPK stores with CRILAYLA.
    bool cpk_sound_archives = false;
};

// A file in the game's file system.
struct GameFile
{
    std::string path; // without a leading '/' ("Sound/sound.brsar")
    uint64_t size;

    // The file's entry in the file system table. The table lists the files in the disc's order. For an extracted disc
    // or a title, the file's place in `files`. A sound archive inside a CPK (GameOptions::cpk_sound_archives) takes
    // the CPK's index. The other files' indices stay the same with that option.
    uint32_t index = 0;
};

struct GameFiles
{
    // The game's ID and title. A disc's six-character ID ("RSPE01") and its title come from the disc's header (a Wii
    // game's partition header). Title returns the title in UTF-8. The header gives it in Shift-JIS on a Japanese disc,
    // and in Windows-1252 on any other. `region` shows whether the disc is Japanese. Without it, the ID's region letter
    // does: 'J' is Japan. A byte that doesn't decode becomes %XX. A title's four-character ID comes from the end of its
    // title ID, and its title from its banner.
    std::string GameId() const;
    std::string Title() const;

    // True for a title from a folder (GameOptions::titles) rather than a disc.
    bool IsTitle() const;

    // Paths of the sound archives (.brsar) in `files`.
    std::vector<std::string> SoundArchives() const;

    // The source of the files, for messages: the image format ("NKit ISO", "RVZ", ...), "extracted disc" or "unpacked
    // title".
    std::string source;

    std::vector<uint8_t> tmd;       // a Wii game partition's or title's TMD, or nothing if an extracted disc has none
    std::vector<uint8_t> boot;      // sys/boot.bin: the disc's or the partition's 0x440-byte header (a title has none)
    std::vector<uint8_t> bi2;       // sys/bi2.bin
    std::vector<uint8_t> apploader; // sys/apploader.img
    std::vector<uint8_t> dol;       // sys/main.dol, or a title's main program
    std::vector<uint8_t> fst;       // sys/fst.bin

    // The word in the disc's region field: at 0x18 in bi2.bin on a GameCube disc (YAGCD's "country code"), or at
    // 0x4E000 on a Wii disc, outside its partitions (WiiBrew's "Wii disc"). The values are 0 for Japan, 1 for the
    // Americas, and 2 for Europe and Australia. On the Wii, 0 includes Taiwan, and 4 is Korea. LoadGame reads the field
    // from every image, and from an extracted GameCube disc's bi2.bin. An extracted Wii disc and a title have none.
    std::optional<uint32_t> region;

    // Every file in the game's file system, sorted by path. The names on a disc are Shift-JIS. The paths are UTF-8,
    // with %XX for each byte that doesn't decode. A title's files are those of its U8 archives. With
    // GameOptions::cpk_sound_archives, the sound archives inside CPKs are listed too.
    std::vector<GameFile> files;

    // Reads a file by its path in `files`. Returns false when the file can't be found. Throws Error if the image's data
    // is damaged. The read shares the image's caches. Only one thread may call it at a time.
    std::function<bool(const std::string& path, std::vector<uint8_t>& data)> read_file;

    // A title's game ID and title. GameId and Title return them. Both are empty for a disc.
    std::string title_game_id;
    std::string title_name;
};

// Returns the console whose disc header `header` starts, from the header's magic word: the GameCube's word at 0x1C or
// the Wii's word at 0x18. Returns nothing for other data.
std::optional<Console> DiscConsole(std::span<const uint8_t> header);

// Returns the format of a GameCube or Wii disc image whose first bytes are `header`, by Image::Format's names: "ISO"
// for a disc header, or "GCZ", "CISO", "WBFS", "WDF", "WIA" or "RVZ". Returns nothing for other data. A disc header
// shows in 0x20 bytes.
std::optional<std::string> ImageFormat(std::span<const uint8_t> header);

// Loads a disc image or an extracted disc of the console that `options` names, or with GameOptions::titles a title's
// folder. An extracted disc is a folder with sys/main.dol and files/, either directly or in a DATA folder. Dolphin and
// Wiimm's tools extract discs in these layouts. Returns an error message on failure and nothing on success.
std::optional<std::string> LoadGame(const std::filesystem::path& path, GameFiles& out, const GameOptions& options);

} // namespace ntdlib::disc
