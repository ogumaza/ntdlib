// SPDX-License-Identifier: MIT

// Tests of GameStarts (ntdlib/dse/game_starts.h): the levels that it reads from a game's image by the game's profile.
// Each test builds a small cartridge image with the profile's values at the profile's addresses: the immediates of the
// instructions, the calls, the tables, the packs with their indexes and the overlays of the events. The expected
// volumes follow the formulas in game_starts.cpp, worked out by hand.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/game_starts.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/testing/nds_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace dse = ntdlib::dse;
namespace nds_files = ntdlib::testing::nds_files;
using nds_files::Bytes;

constexpr uint32_t kArm9 = 0x02000000;

// The ARM9 binary of the tests: zeros, large enough for every address of the profiles.
Bytes Arm9()
{
    return Bytes(0x000F0000, 0);
}

void Put16(Bytes& arm9, uint32_t address, uint32_t value)
{
    xsflib::StoreLe16(&arm9[address - kArm9], static_cast<uint16_t>(value));
}

void Put32(Bytes& arm9, uint32_t address, uint32_t value)
{
    xsflib::StoreLe32(&arm9[address - kArm9], value);
}

// A table of levels at `address`: 8-byte entries of a number and a level, then the number -1.
void PutTable(Bytes& arm9, uint32_t address, const std::vector<std::pair<int32_t, uint8_t>>& entries)
{
    for (const auto& [number, level] : entries)
    {
        Put32(arm9, address, static_cast<uint32_t>(number));
        Put32(arm9, address + 4, level);
        address += 8;
    }
    Put32(arm9, address, 0xFFFFFFFF);
}

// A song file of 0x40 bytes: the header with `name` at 0x20.
Bytes Song(std::string_view name)
{
    Bytes song(0x40, 0);
    song[0] = 's';
    song[1] = 'm';
    song[2] = 'd';
    song[3] = 'l';
    xsflib::StoreLe32(&song[0x08], static_cast<uint32_t>(song.size()));
    xsflib::StoreLe16(&song[0x0C], 0x415);
    for (std::size_t i = 0; i < name.size(); i++)
    {
        song[0x20 + i] = static_cast<uint8_t>(name[i]);
    }

    return song;
}

// A pack of `names`, each entry a header of 0x20 bytes and the song, and its index: the offset of each entry, then the
// pack's size.
std::pair<Bytes, Bytes> Pack(const std::vector<std::string_view>& names)
{
    Bytes pack;
    Bytes index;
    for (const std::string_view name : names)
    {
        index.resize(index.size() + 4);
        xsflib::StoreLe32(&index[index.size() - 4], static_cast<uint32_t>(pack.size()));
        pack.resize(pack.size() + 0x20, 0xEE);
        const Bytes song = Song(name);
        pack.insert(pack.end(), song.begin(), song.end());
    }
    index.resize(index.size() + 4);
    xsflib::StoreLe32(&index[index.size() - 4], static_cast<uint32_t>(pack.size()));

    return {pack, index};
}

// Adds ARM9 overlays to `image`, an image that BuildImage made: entries after the overlay of BuildImage in the overlay
// table. The table has room for them before the ARM7 binary. Each overlay is the named file whose ID `overlays` gives
// with the overlay's ID, and it loads at `address`. The entries replace the padding after the table.
void AddArm9Overlays(Bytes& image, const std::vector<std::pair<uint32_t, uint32_t>>& overlays, uint32_t address)
{
    const uint32_t table = xsflib::LoadLe32(&image[0x50]);
    const uint32_t fat = xsflib::LoadLe32(&image[0x48]);
    uint32_t at = table + 32;
    for (const auto& [id, file_id] : overlays)
    {
        const uint32_t size =
            xsflib::LoadLe32(&image[fat + file_id * 8 + 4]) - xsflib::LoadLe32(&image[fat + file_id * 8]);
        std::fill_n(image.begin() + at, 32, uint8_t{0});
        nds_files::detail::Put32(image, at, id);
        nds_files::detail::Put32(image, at + 4, address);
        nds_files::detail::Put32(image, at + 8, size);
        nds_files::detail::Put32(image, at + 24, file_id);
        at += 32;
    }
    nds_files::detail::Put32(image, 0x54, at - table);
}

// The image of Fuurai no Shiren 4's profile: the option's default `option` (ARM MOV), the option's levels, two songs in
// the table of sound/bgm.bin and a third that it doesn't list, and one short piece in the table of sound/me_bgm.bin.
nds_files::Image ShirenFour(uint32_t option_instruction)
{
    Bytes arm9 = Arm9();
    Put32(arm9, 0x02013E40, option_instruction);
    const uint32_t levels[] = {66, 80, 94, 108, 127};
    for (uint32_t i = 0; i < 5; i++)
    {
        Put16(arm9, 0x020C5214 + 2 * i, levels[i]);
    }
    PutTable(arm9, 0x020C535C, {{1, 110}, {0, 95}});
    PutTable(arm9, 0x020C52AC, {{0x5, 127}, {0x401, 104}, {0x400, 50}});
    Put16(arm9, 0x0200D86E, 0x207F); // movs r0, #0x7F
    Put16(arm9, 0x0200D89A, 0x2064); // movs r0, #100

    const auto [bgm, bgm_index] = Pack({"BGM_A", "BGM_B", "BGM_A"});
    const auto [me, me_index] = Pack({"ME_FANFARE", "ME_WIN"});

    return {.game_code = "B4FJ",
            .arm9 = arm9,
            .files = {{"sound/bgm.bin", bgm},
                      {"sound/bgm.ent", bgm_index},
                      {"sound/me_bgm.bin", me},
                      {"sound/me_bgm.ent", me_index}}};
}

// Shiren 4's songs take the level in their table, or the code's level of an unlisted number, times the level of the
// settings' option 3 (108) / 127: entry 0 95 × 108 / 127 = 80, entry 1 110 → 93, entry 2 (unlisted) 127 → 108. A
// short piece's number is 0x401 plus its entry: entry 0 104 → 88, entry 1 (unlisted, 100) → 85. A name that comes
// again in the pack gets _2, as FindFiles gives it. Each state's level gives DSE that volume, and a path outside the
// packs gets full level.
void TestTableGame()
{
    const auto [image, ids] = nds_files::BuildImage(ShirenFour(0xE3A00003)); // mov r0, #3
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);

    const dse::GameStarts starts(rom);
    const std::vector<dse::SongState> first = starts.StatesOf("sound/bgm.bin/BGM_A.smd");
    const std::vector<dse::SongState> second = starts.StatesOf("sound/bgm.bin/BGM_B.smd");
    const std::vector<dse::SongState> unlisted = starts.StatesOf("sound/bgm.bin/BGM_A_2.smd");
    const std::vector<dse::SongState> fanfare = starts.StatesOf("sound/me_bgm.bin/ME_FANFARE.smd");
    const std::vector<dse::SongState> win = starts.StatesOf("sound/me_bgm.bin/ME_WIN.smd");
    const std::vector<dse::SongState> other = starts.StatesOf("sound/se.bin/SE_A.smd");

    const auto volume = [](const std::vector<dse::SongState>& states)
    {
        return states.size() == 1 && states[0].suffix.empty() ? dse::StartOf(states[0]).volume : -1;
    };
    XSFLIB_CHECK(volume(first) == 80 && volume(second) == 93 && volume(unlisted) == 108);
    XSFLIB_CHECK(volume(fanfare) == 88 && volume(win) == 85);
    XSFLIB_CHECK(other == std::vector<dse::SongState>{dse::SongState{}});
    XSFLIB_CHECK(first[0].level == 162 && second[0].level == 188 && unlisted[0].level == 218);
}

// The option's default comes from the instruction: MOV r0, #2 selects 94. Entry 1 then plays at 110 × 94 / 127 = 81. An
// image is refused for an instruction of another kind, for an option past the levels, or without the pack's index.
void TestTableGameRefusals()
{
    const auto [two_image, two_ids] = nds_files::BuildImage(ShirenFour(0xE3A00002));
    const auto [branch_image, branch_ids] = nds_files::BuildImage(ShirenFour(0xEA000000));
    const auto [past_image, past_ids] = nds_files::BuildImage(ShirenFour(0xE3A00005));
    nds_files::Image no_index = ShirenFour(0xE3A00003);
    no_index.files.erase(no_index.files.begin() + 1);
    const auto [no_index_image, no_index_ids] = nds_files::BuildImage(no_index);
    const ntdlib::ntr::Rom two = ntdlib::ntr::Rom::Parse(two_image);
    const ntdlib::ntr::Rom branch = ntdlib::ntr::Rom::Parse(branch_image);
    const ntdlib::ntr::Rom past = ntdlib::ntr::Rom::Parse(past_image);
    const ntdlib::ntr::Rom missing = ntdlib::ntr::Rom::Parse(no_index_image);

    const std::vector<dse::SongState> second = dse::GameStarts(two).StatesOf("sound/bgm.bin/BGM_B.smd");
    const std::string branch_error = ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{branch}; });
    const std::string past_error = ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{past}; });
    const std::string missing_error = ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{missing}; });

    XSFLIB_CHECK(second.size() == 1 && dse::StartOf(second[0]).volume == 81);
    XSFLIB_CHECK(branch_error == "no MOV of an immediate at 0x02013E40");
    XSFLIB_CHECK(past_error == "the option 5 has no level");
    XSFLIB_CHECK(missing_error == "the image has no sound/bgm.ent");
}

constexpr uint32_t kEvents = 0x021E5900;

// Thumb MOVS r0 of `value`.
uint32_t Movs(uint32_t value)
{
    return 0x2000 | value;
}

// The two halves of a Thumb BL at `address` to `target`.
void PutCall(Bytes& overlay, uint32_t address, uint32_t target)
{
    const uint32_t offset = target - (address + 4);
    xsflib::StoreLe16(&overlay[address - kEvents], static_cast<uint16_t>(0xF000 | (offset >> 12 & 0x7FF)));
    xsflib::StoreLe16(&overlay[address + 2 - kEvents], static_cast<uint16_t>(0xF800 | (offset >> 1 & 0x7FF)));
}

void PutMovs(Bytes& overlay, uint32_t address, uint32_t value)
{
    xsflib::StoreLe16(&overlay[address - kEvents], static_cast<uint16_t>(Movs(value)));
}

// The values of Shiren 5's events in the test image: the song and the volume of each start, and the volume and the
// frames of each change, at the addresses of the rows of the profile.
struct Events
{
    uint32_t change_226 = 102;
    uint32_t song_393 = 73;
    uint32_t volume_393 = 127;
    uint32_t start_233 = 0x02161DC4;
    uint32_t change_186 = 0x02161E44;
};

// The image of Fuurai no Shiren 5's profile: the option 4 (Thumb MOVS) of the levels {40, 60, 80, 100, 120, 127}, the
// song 73 at 100 in the table of sound/bgm.bin and the code's level 127 for the songs it doesn't list, the 80 songs of
// sound/bgm.bin and one short piece. The events' starts follow `events`. With the defaults, events 217 and 226 start
// the song 73 at 127 and lower it to 102 over 1 frame, and event 393 starts it at 127. Event 233 starts the song 59 at
// 127 and lowers it to 114 over 3 frames. Event 186 starts the song 72 at 127 and lowers it to 100 over 2 frames.
// `overlays` lists the events' overlays of the image.
Bytes ShirenFive(const Events& events, const std::vector<uint32_t>& overlays = {98, 106, 118})
{
    Bytes arm9 = Arm9();
    Put16(arm9, 0x02014E58, Movs(4));
    const uint32_t levels[] = {40, 60, 80, 100, 120, 127};
    for (uint32_t i = 0; i < 6; i++)
    {
        Put16(arm9, 0x020E01BC + 2 * i, levels[i]);
    }
    PutTable(arm9, 0x020E032C, {{73, 100}});
    PutTable(arm9, 0x020E025C, {});
    Put16(arm9, 0x0200EF7C, Movs(127));
    Put16(arm9, 0x0200EFC8, Movs(127));

    // The events' starts, each a call of the music start with MOVS of the song and the volume, and then a call of the
    // change with MOVS of its volume and frames.
    constexpr uint32_t kStart = 0x02161DC4;
    constexpr uint32_t kChange = 0x02161E44;
    Bytes first(0x3000, 0);
    Bytes second(0x3000, 0);
    Bytes third(0x3000, 0);
    PutCall(second, 0x021E6142, kStart);
    PutMovs(second, 0x021E613E, 73);
    PutMovs(second, 0x021E6138, 127);
    PutCall(second, 0x021E6150, kChange);
    PutMovs(second, 0x021E614E, 102);
    PutMovs(second, 0x021E5982, 1);
    PutCall(second, 0x021E7460, kStart);
    PutMovs(second, 0x021E745A, 73);
    PutMovs(second, 0x021E7456, 127);
    PutCall(second, 0x021E746E, kChange);
    PutMovs(second, 0x021E746C, events.change_226);
    PutMovs(second, 0x021E746A, 1);
    PutCall(first, 0x021E82C8, kStart);
    PutMovs(first, 0x021E82C4, events.song_393);
    PutMovs(first, 0x021E82BE, events.volume_393);
    PutCall(second, 0x021E8544, events.start_233);
    PutMovs(second, 0x021E8540, 59);
    PutMovs(second, 0x021E853A, 127);
    PutCall(second, 0x021E8552, kChange);
    PutMovs(second, 0x021E8550, 114);
    PutMovs(second, 0x021E854E, 3);
    PutCall(third, 0x021E88B4, kStart);
    PutMovs(third, 0x021E88B0, 72);
    PutMovs(third, 0x021E88AA, 127);
    PutCall(third, 0x021E88C2, events.change_186);
    PutMovs(third, 0x021E88C0, 100);
    PutMovs(third, 0x021E88BE, 2);

    std::vector<std::string> names;
    for (int i = 0; i < 80; i++)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "BGM_%02d", i);
        names.emplace_back(name);
    }
    const auto [bgm, bgm_index] = Pack({names.begin(), names.end()});
    const auto [me, me_index] = Pack({"ME_WIN"});
    nds_files::Image spec = {.game_code = "B5FJ",
                             .arm9 = arm9,
                             .files = {{"sound/bgm.bin", bgm},
                                       {"sound/bgm.ent", bgm_index},
                                       {"sound/me_bgm.bin", me},
                                       {"sound/me_bgm.ent", me_index}}};
    const std::pair<uint32_t, const Bytes*> code[] = {{98, &first}, {106, &second}, {118, &third}};
    for (const auto& [id, bytes] : code)
    {
        spec.files.emplace_back("ov/" + std::to_string(id), *bytes);
    }

    auto [image, ids] = nds_files::BuildImage(spec);
    std::vector<std::pair<uint32_t, uint32_t>> table;
    for (std::size_t i = 0; i < 3; i++)
    {
        if (std::ranges::find(overlays, code[i].first) != overlays.end())
        {
            table.emplace_back(code[i].first, ids[4 + i]);
        }
    }
    AddArm9Overlays(image, table, kEvents);

    return image;
}

// Shiren 5's songs take their levels as Shiren 4's do: the option 4 gives 120, the song 73 plays at 100 × 120 / 127 =
// 94 and an unlisted song at 120. The state of a song that the events start is the level of most events. The song 73
// starts at 94 (level 190) and goes to 94 × 102 / 127 = 75 (level 152) in events 217 and 226, and starts at 94 in event
// 393: the state is level 152, with its change. The song 59 starts at 120 (level 242) and goes to 120 × 114 / 127 = 107
// (level 216). The song 72 goes from 120 to 94. The other songs keep their table's levels.
void TestEvents()
{
    const Bytes image = ShirenFive({});
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    const dse::GameStarts starts(rom);

    XSFLIB_CHECK((starts.StatesOf("sound/bgm.bin/BGM_73.smd") ==
                  std::vector<dse::SongState>{{"", 152, dse::StartChange{190, 102, 1}}}));
    XSFLIB_CHECK((starts.StatesOf("sound/bgm.bin/BGM_59.smd") ==
                  std::vector<dse::SongState>{{"", 216, dse::StartChange{242, 114, 3}}}));
    XSFLIB_CHECK((starts.StatesOf("sound/bgm.bin/BGM_72.smd") ==
                  std::vector<dse::SongState>{{"", 190, dse::StartChange{242, 100, 2}}}));
    XSFLIB_CHECK((starts.StatesOf("sound/bgm.bin/BGM_10.smd") == std::vector<dse::SongState>{{"", 242}}));
    XSFLIB_CHECK((starts.StatesOf("sound/me_bgm.bin/ME_WIN.smd") == std::vector<dse::SongState>{{"", 242}}));
    XSFLIB_CHECK(dse::StartOf(starts.StatesOf("sound/bgm.bin/BGM_73.smd")[0]).volume == 75);

    // Event 393's start of the song 10 at 64 (120 × 64 / 127 = 60, level 121) has no change. Event 226's change to 90
    // (94 × 90 / 127 = 66, level 134) leaves the song 73 one event at each of three levels. The image is then refused.
    // Event 226's change to 127 leaves the song 73 at level 190 in events 226 and 393: the state takes the change of
    // the first of them.
    const Bytes alone_image = ShirenFive({.song_393 = 10, .volume_393 = 64});
    const Bytes tie_image = ShirenFive({.change_226 = 90});
    const Bytes raised_image = ShirenFive({.change_226 = 127});
    const ntdlib::ntr::Rom alone = ntdlib::ntr::Rom::Parse(alone_image);
    const ntdlib::ntr::Rom tie = ntdlib::ntr::Rom::Parse(tie_image);
    const ntdlib::ntr::Rom raised = ntdlib::ntr::Rom::Parse(raised_image);
    const std::string tie_error = ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{tie}; });

    XSFLIB_CHECK(
        (dse::GameStarts(alone).StatesOf("sound/bgm.bin/BGM_10.smd") == std::vector<dse::SongState>{{"", 121}}));
    XSFLIB_CHECK((dse::GameStarts(alone).StatesOf("sound/bgm.bin/BGM_73.smd") ==
                  std::vector<dse::SongState>{{"", 152, dse::StartChange{190, 102, 1}}}));
    XSFLIB_CHECK(tie_error == "the events start the song 73 at two levels in as many events");
    XSFLIB_CHECK((dse::GameStarts(raised).StatesOf("sound/bgm.bin/BGM_73.smd") ==
                  std::vector<dse::SongState>{{"", 190, dse::StartChange{190, 127, 1}}}));
}

// A call at a row's address that doesn't go to the profile's routine, an instruction other than a call there, or an
// image without the overlay of an event refuses the image.
void TestEventRefusals()
{
    const Bytes start_image = ShirenFive({.start_233 = 0x02161DC8});
    const Bytes change_image = ShirenFive({.change_186 = 0x02161DC4});
    const Bytes missing_image = ShirenFive({}, {98, 106});
    const ntdlib::ntr::Rom start = ntdlib::ntr::Rom::Parse(start_image);
    const ntdlib::ntr::Rom change = ntdlib::ntr::Rom::Parse(change_image);
    const ntdlib::ntr::Rom missing = ntdlib::ntr::Rom::Parse(missing_image);
    Bytes no_call_image = ShirenFive({});
    const ntdlib::ntr::Rom parsed = ntdlib::ntr::Rom::Parse(no_call_image);
    const auto overlay = std::ranges::find(parsed.Arm9Overlays(), 106u, &ntdlib::ntr::Overlay::id);
    const uint32_t call_at = parsed.Files()[overlay->file_id].start + (0x021E7460 - kEvents);
    xsflib::StoreLe16(&no_call_image[call_at], static_cast<uint16_t>(Movs(0)));
    const ntdlib::ntr::Rom no_call = ntdlib::ntr::Rom::Parse(no_call_image);

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{start}; }) ==
                 "the call at 0x021E8544 in the event 233 isn't the music start");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{change}; }) ==
                 "the call at 0x021E88C2 in the event 186 isn't the change of the music's volume");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{missing}; }) ==
                 "the image has no ARM9 overlay 118");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::GameStarts{no_call}; }) == "no Thumb call at 0x021E7460");
}

// The Inazuma Eleven games' player gives every song (127 × 127 × 1000 >> 7) / 1000 = 126, the level 254. Another
// revision of a profiled game and a game without a profile take SongStatesOf's rows.
void TestPlayerGame()
{
    Bytes arm9 = Arm9();
    Put32(arm9, 0x0202B554, 0xE3A0107F); // mov r1, #127
    Put32(arm9, 0x0202B568, 0xE3A01FFA); // mov r1, #1000
    const auto [ogre_image, ogre_ids] = nds_files::BuildImage({.game_code = "BOEJ", .arm9 = arm9});
    const auto [revision_image, revision_ids] =
        nds_files::BuildImage({.game_code = "BOEJ", .version = 1, .arm9 = arm9});
    const auto [sky_image, sky_ids] = nds_files::BuildImage({.game_code = "C2SE"});
    const ntdlib::ntr::Rom ogre = ntdlib::ntr::Rom::Parse(ogre_image);
    const ntdlib::ntr::Rom revision = ntdlib::ntr::Rom::Parse(revision_image);
    const ntdlib::ntr::Rom sky = ntdlib::ntr::Rom::Parse(sky_image);

    const std::vector<dse::SongState> song = dse::GameStarts(ogre).StatesOf("data_iz/sound/sound.pkb/P01.smd");
    const std::vector<dse::SongState> other_revision =
        dse::GameStarts(revision).StatesOf("data_iz/sound/sound.pkb/P01.smd");
    const std::vector<dse::SongState> peace = dse::GameStarts(sky).StatesOf("SOUND/BGM/bgm0086.smd");

    XSFLIB_CHECK((song == std::vector<dse::SongState>{{"", 254}}) && dse::StartOf(song[0]).volume == 126);
    XSFLIB_CHECK(other_revision == std::vector<dse::SongState>{dse::SongState{}});
    XSFLIB_CHECK(peace == dse::SongStatesOf("C2SE", "SOUND/BGM/bgm0086.smd"));
}

// LevelOfVolume gives the lowest level whose start has the volume, for every volume from 0 to 127.
void TestLevelOfVolume()
{
    bool every = true;
    for (int volume = 0; volume <= 127; volume++)
    {
        const int level = dse::LevelOfVolume(volume);
        const bool gives = dse::StartOf({.suffix = "", .level = level}).volume == volume;
        const bool lowest = level == 0 || dse::StartOf({.suffix = "", .level = level - 1}).volume < volume;
        every = every && gives && lowest;
    }

    XSFLIB_CHECK(every);
    XSFLIB_CHECK(dse::LevelOfVolume(127) == dse::kFullLevel && dse::LevelOfVolume(100) == 202);
}

} // namespace

int main()
{
    TestTableGame();
    TestTableGameRefusals();
    TestEvents();
    TestEventRefusals();
    TestPlayerGame();
    TestLevelOfVolume();

    return xsflib::testing::Summary("DSE game starts");
}
