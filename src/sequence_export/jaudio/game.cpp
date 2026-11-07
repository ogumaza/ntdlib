// SPDX-License-Identifier: MIT

// A game's sound files (see game.h).

#include "ntdlib/sequence_export/jaudio/game.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"
#include "xsflib/sha1.h"

#include "ntdlib/disc/error.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/barc.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/z2_starts.h"
#include "ntdlib/jaudio/z2_states.h"
#include "ntdlib/sequence_export/jaudio/conductor.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

namespace fs = std::filesystem;

using xsflib::AsciiLower;
using xsflib::Utf8;

// The subfolder of `folder` named `name` in any capitals, or nothing.
std::optional<fs::path> Subfolder(const fs::path& folder, const std::string& name)
{
    std::error_code error;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
    {
        if (e.is_directory(error) && AsciiLower(Utf8(e.path().filename())) == AsciiLower(name))
        {
            return e.path();
        }
    }

    return std::nullopt;
}

// Give `seq` the state in which the game's code plays it.
void SetState(SequenceSound& seq, const SequenceState& state)
{
    seq.child_volumes = state.child_volumes;
    seq.tracks = state.tracks;
    seq.tempo_ratio = state.tempo_ratio;
    seq.game_ports = state.game_ports;
    seq.status_by_section = state.status_by_section;
    seq.fade_in = state.fade_in;
}

// The states in which The Wind Waker's code plays its music, and the other states that it gives the same pieces
// (GameRules::start_states and arrangements). The addresses are in the GameCube's main.dol (USA). The Wind Waker HD's
// executable has the same code: bgmStart at 0x0202204c, subBgmStart at 0x0201de00, bgmNowBattle at 0x0202879c, bgmMute
// at 0x0201dd18 and its table at 0x1018dcb8.
//
// JAIZelBasic::bgmStart (0x802a34a4) starts a piece with startSoundVec. For 13 pieces it then calls bgmMute
// (0x802abb18). bgmMute takes a mask from the table m_bgm_mute_state (0x8039b630), at the row of the piece's index and
// the column of a state, and gives each of the root track's 16 children a track volume of 1 or 0, by its bit. The game
// starts a piece in state 0 by the piece's ID. IDs from 0x80000105 to 0x80000152 start a piece in another state: a
// state of the piece's row, or once of the row before or after it. 0x80000125 also sets the tempo ratio to 1.3143.
// When the piece already plays, those IDs change its tracks over 5 or 60 frames.
//
// Three pieces follow the game's state. JA_BGM_SEA and JA_BGM_SEA_STORM play state 1, without children 9 to 15 (or 9
// to 14), while the boat's sail is down and its speed is under 0.2 (bgmStart, and shipCruiseSePlay at 0x802a92cc as the
// boat moves). Otherwise they play state 0, every child. JA_BGM_SEA_ENEMY plays state 0 while Link is on board, and
// state 1 otherwise (subBgmStart at 0x802a47b8, setLinkOnBoard at 0x802aba9c). The game's sound layer starts with the
// sail down, the boat still and Link off board (its constructor, 0x802a1c78). Those states start the pieces here.
//
// The battle music and the minibosses' music bring layers in and out as the fight goes (bgmNowBattle at 0x802a4eb8,
// mbossBgmNearByProcess at 0x802a591c, mbossBgmMuteProcess at 0x802a5818). JA_BGM_BATTLE_NORM starts in state 0 of
// its row, and bgmNowBattle gives child 9 a volume of 1 while Link's sword is sheathed. With the sword out, child 0
// plays in its place (SWORD_USING_TRACK, which nothing sets, so 0). An enemy within 30 units brings in children 4 and
// 10. JA_BGM_MBOSS and JA_BGM_MBOSS_S start in state 0 of their rows. An enemy within 30 units brings in child 9, and
// the sword child 2. The game fades those layers in over 5 or 10 frames. Here they play from the start. A spin attack
// brings in child 8 of the battle music, or child 15 of the minibosses', for a moment. Those moments aren't
// arrangements.
//
// The game writes port 9 of a piece that plays. JAIZelBasic::changeBgmStatus (0x802a5a40) writes it on the main music
// with setPortData, and changeSubBgmStatus (0x802a5a78) on the music that plays over it, with writePortApp. The values
// come from the actors. In The Wind Waker HD, changeBgmStatus writes 1, 2 and 9 (0x021554ac, 0x0215beac, 0x02255dd0,
// 0x02255df0), and changeSubBgmStatus 2 to 8 and 10 (0x02136258 to 0x02136efc). onEnemyDamage (0x802a57a4) writes 1
// to port 9 of JA_BGM_SEA_ENEMY, and to port 11 of JA_BGM_BATTLE_NORM, when Link hits an enemy. Each piece's root track
// clears port 9 in its first commands and then polls it. A piece converts once more for each of those values that
// changes what it plays, named after the port and the value.
void AddWindWakerMusicStates(GameRules& rules, bool gamecube)
{
    constexpr uint32_t kMusic = 0x80000000;

    // The volumes of a mask of the root track's children: 0 for each child whose bit is clear.
    const auto volumes = [](uint16_t children)
    {
        std::map<uint8_t, float> silent;
        for (uint8_t child = 0; child < 16; child++)
        {
            if ((children >> child & 1) == 0)
            {
                silent[child] = 0.0f;
            }
        }
        return silent;
    };

    // subBgmStart starts music without a fade for 13 pieces (14 in The Wind Waker HD), and fades every other piece in
    // over 60 frames (the choice at 0x802a4954; 0x0201e048 in HD). bgmNowBattle always fades the battle music in over
    // 60 frames (its startSoundVec at 0x802a519c). Of the pieces here, JA_BGM_BATTLE_NORM, JA_BGM_MBOSS_S,
    // JA_BGM_SEA_ENEMY and JA_BGM_P_GANON_2 fade in. Every call of bgmStart through mDoAud_bgmStart passes no fade
    // (0x025e18f4 in HD).
    constexpr uint16_t kStartFade = 60;
    const auto fade = [](uint32_t piece) -> uint16_t
    {
        return piece == 0x04 || piece == 0x1a || piece == 0x1c || piece == 0x47 ? kStartFade : 0;
    };
    const auto start = [&rules, &fade, &volumes](uint32_t piece, uint16_t children, float tempo = 1.0f)
    {
        rules.start_states[kMusic | piece] = SequenceState{volumes(children), tempo, {}, fade(piece)};
    };
    const auto arrange =
        [&rules, &fade, &volumes](uint32_t piece, std::string suffix, uint16_t children, float tempo = 1.0f)
    {
        rules.arrangements.push_back(
            {kMusic | piece, std::move(suffix), SequenceState{volumes(children), tempo, {}, fade(piece)}});
    };

    // State 0 of each piece that bgmStart mutes, from the table, and the other states that the game's IDs pick. A state
    // that plays what another does is left out. The Wind Waker HD's table differs in one row: JA_BGM_MJ_TOWER_BATTLE
    // (0x15) plays every track in state 0.
    start(0x05, 0xe7ff); // JA_BGM_KINDAN_BOSS
    arrange(0x05, "80000105", 0xfbbb);
    start(0x14, 0xaf55); // JA_BGM_DK_BATTLE
    arrange(0x14, "80000110", 0xffff);
    if (gamecube)
    {
        start(0x15, 0xffef); // JA_BGM_MJ_TOWER_BATTLE
        arrange(0x15, "80000115", 0xffff);
    }
    start(0x23, 0xcbcf); // JA_BGM_BST_BATTLE
    arrange(0x23, "80000120", 0xcfcc);
    arrange(0x23, "80000121", 0xcfc3);
    arrange(0x23, "80000122", 0xd6d0);
    arrange(0x23, "80000123", 0xe230); // state 3 of the row before
    start(0x28, 0xfbf7);               // JA_BGM_KAMI_TOWER
    arrange(0x28, "80000125", 0xffbf, 1.3143f);
    arrange(0x28, "80000126", 0xfbf6);
    start(0x42, 0x4801); // JA_BGM_D_GANON_1
    arrange(0x42, "80000130", 0x000f);
    arrange(0x42, "80000131", 0x1601);
    arrange(0x42, "80000132", 0x8071);
    arrange(0x42, "80000133", 0x2181); // state 3 of the row after
    start(0x48, 0xffdf);               // JA_BGM_BIG_POW
    arrange(0x48, "80000140", 0xfbff);
    start(0x49, 0xfffd); // JA_BGM_PAST_BIG_POW
    arrange(0x49, "80000150", 0xffff);
    arrange(0x4a, "80000151", 0xfe1b); // JA_BGM_PAST_BKM, whose state 0 plays every track

    // The pieces that follow the boat and Link.
    start(0x2e, 0x01ff); // JA_BGM_SEA
    arrange(0x2e, "SAILING", 0xffff);
    start(0x3c, 0x81ff); // JA_BGM_SEA_STORM
    arrange(0x3c, "SAILING", 0xffff);
    start(0x1c, 0xf47d); // JA_BGM_SEA_ENEMY
    arrange(0x1c, "ON_BOARD", 0xf5ff);

    // The layers of the fights.
    start(0x47, 0xffff); // JA_BGM_P_GANON_2: every track, with the fade
    start(0x04, 0xf266); // JA_BGM_BATTLE_NORM: state 0 (0xf066) and child 9
    arrange(0x04, "NEAR", 0xf676);
    arrange(0x04, "SWORD", 0xf067);
    arrange(0x04, "NEAR_SWORD", 0xf477);
    for (const uint32_t piece : {0x19u, 0x1au}) // JA_BGM_MBOSS and JA_BGM_MBOSS_S
    {
        start(piece, 0x7ddb);
        arrange(piece, "NEAR", 0x7fdb);
        arrange(piece, "SWORD", 0x7ddf);
        arrange(piece, "NEAR_SWORD", 0x7fdf);
    }

    // The values that the game writes on port 9 and port 11, where they change what a piece plays. changeBgmStatus
    // writes 1, 2 and 9 on the main music, here JA_BGM_JI_TRAINING and JA_BGM_GANON_BATTLE. changeSubBgmStatus writes 2
    // to 8 and 10 on the music over it, here JA_BGM_P_GANON_1 and JA_BGM_P_GANON_2. onEnemyDamage writes 1 on
    // JA_BGM_SEA_ENEMY's port 9 and JA_BGM_BATTLE_NORM's port 11. The other values play what the piece plays without
    // them, or what another value plays: 2 and 9 in JA_BGM_GANON_BATTLE, and every value of port 9 in
    // JA_BGM_BATTLE_NORM, JA_BGM_MBOSS and JA_BGM_MBOSS_S. JA_BGM_SEA_ENEMY plays the same with each value from 1
    // to 10.
    const auto port = [&rules](uint32_t piece, uint8_t number, uint16_t value)
    {
        const auto state = rules.start_states.find(kMusic | piece);
        SequenceState s = state != rules.start_states.end() ? state->second : SequenceState{};
        s.game_ports[number] = value;
        rules.arrangements.push_back(
            {kMusic | piece, "PORT" + std::to_string(number) + "_" + std::to_string(value), s});
    };
    for (const uint16_t value : std::array<uint16_t, 3>{1, 2, 9})
    {
        port(0x11, 9, value); // JA_BGM_JI_TRAINING
    }
    port(0x4d, 9, 1);                           // JA_BGM_GANON_BATTLE
    for (const uint32_t piece : {0x41u, 0x47u}) // JA_BGM_P_GANON_1 and JA_BGM_P_GANON_2
    {
        for (const uint16_t value : std::array<uint16_t, 8>{2, 3, 4, 5, 6, 7, 8, 10})
        {
            port(piece, 9, value);
        }
    }
    port(0x1c, 9, 1);  // JA_BGM_SEA_ENEMY
    port(0x04, 11, 1); // JA_BGM_BATTLE_NORM
}

// Give Twilight Princess's rules the states in which its code plays its music once a piece has started, and the start
// states that its scenes and its other starters give more pieces: `states` holds kTwilightPrincessStates,
// kTwilightPrincessWiiStates, kTwilightPrincessHdStates or, for Link's Crossbow Training, kCrossbowTrainingStates.
// Child m of track 0 is the handle's child m. A state's port 9 is a game port.
void AddTwilightPrincessMusicStates(std::span<const ntdlib::jaudio::MusicState> states, GameRules& rules)
{
    for (const ntdlib::jaudio::MusicState& music : states)
    {
        SequenceState state;
        for (uint8_t child = 0; child < 16; child++)
        {
            const float volume = music.Volume(child);
            if (volume != 1.0f)
            {
                state.child_volumes[child] = volume;
            }
        }
        if (music.status_port >= 0)
        {
            state.game_ports[9] = static_cast<uint16_t>(music.status_port);
        }
        for (const ntdlib::jaudio::SectionStatus& s : music.section_statuses)
        {
            if (s.status != 0)
            {
                state.status_by_section[s.section] = s.status;
            }
        }
        if (music.suffix.empty())
        {
            rules.start_states[music.sound] = state;
        }
        else
        {
            rules.arrangements.push_back({music.sound, std::string(music.suffix.substr(1)), state});
        }
    }
}

// Give a volume of 0 to the children of track 0 that a game mutes as it starts each piece of `starts`, through the
// sound's handle. Child m of track 0 is the handle's child m. A piece's second start becomes an arrangement, such as
// Hyrule Field's day music as Link rides. Its suffix starts with the "_" that the arrangement's name adds.
void AddStarts(std::span<const ntdlib::jaudio::MutedStart> starts, GameRules& rules)
{
    for (const ntdlib::jaudio::MutedStart& start : starts)
    {
        SequenceState state;
        for (uint8_t child = 0; child < 16; child++)
        {
            if ((start.muted_children >> child & 1) != 0)
            {
                state.child_volumes[child] = 0.0f;
            }
        }
        if (start.suffix.empty())
        {
            rules.start_states[start.sound] = state;
        }
        else
        {
            rules.arrangements.push_back({start.sound, std::string(start.suffix.substr(1)), state});
        }
    }
}

// A start-up archive that another file of a disc holds: its data, the path of that file and the folder that holds the
// game's other sound files, in lowercase and ending in '/'.
struct ContainedArchive
{
    std::vector<uint8_t> archive;
    std::string path;
    std::string folder;
};

// The RARC archives on a disc that hold a game's start-up archive, and the folder of the game's other sound files.
// Super Mario Sunshine mounts /data/nintendo.arc (0x802a7878 in the USA release's main.dol). Its loader reads the file
// compressed, as data/nintendo.szs. The function at 0x802a6dd0 takes mSound.aaf from the archive for the game's sound
// object. The sound library reads the other sound files from /AudioRes/Seqs/ and /AudioRes/Banks/ (0x80373ee4 and
// 0x80373ed0).
struct ArchiveContainer
{
    std::string_view path;
    std::string_view folder;
};
constexpr std::array<ArchiveContainer, 1> kArchiveContainers{{{"data/nintendo.szs", "audiores/"}}};

// The start-up archive that a file of `files` holds, read with `read`, or nothing. Donkey Kong Jungle Beat keeps its
// start-up archive beside its Seqs folder in a file that Yaz0 compresses (.szs). The game decodes it as it loads it.
// The archives of kArchiveContainers can hold one too. A file that can't be decoded doesn't count. Throws FormatError
// if a file can't be read.
std::optional<ContainedArchive> FindContainedArchive(
    const std::vector<const ntdlib::disc::GameFile*>& files,
    const std::function<std::vector<uint8_t>(const std::string&)>& read)
{
    // The folders whose Seqs folder holds a sequence archive.
    std::set<std::string> sound_folders;
    for (const ntdlib::disc::GameFile* f : files)
    {
        const std::string path = AsciiLower(f->path);
        const std::size_t seqs = path.rfind("seqs/");
        if (seqs != std::string::npos && (seqs == 0 || path[seqs - 1] == '/') && path.ends_with(".arc"))
        {
            sound_folders.insert(path.substr(0, seqs));
        }
    }

    for (const ntdlib::disc::GameFile* f : files)
    {
        const std::string path = AsciiLower(f->path);
        std::string folder = path.substr(0, path.rfind('/') + 1);
        if (!path.ends_with(".szs") || !sound_folders.contains(folder))
        {
            continue;
        }

        const std::vector<uint8_t> data = read(f->path);
        try
        {
            if (IsYaz0(data))
            {
                std::vector<uint8_t> decoded = DecodeYaz0(data);
                if (IsStartUpArchive(decoded))
                {
                    return ContainedArchive{std::move(decoded), f->path, std::move(folder)};
                }
            }
        }
        catch (const FormatError&)
        {
        }
    }

    for (const ArchiveContainer& container : kArchiveContainers)
    {
        const auto f =
            std::ranges::find_if(files, [&](const auto* g) { return AsciiLower(g->path) == container.path; });
        if (f == files.end())
        {
            continue;
        }

        const std::vector<uint8_t> data = read((*f)->path);
        try
        {
            std::vector<uint8_t> decoded;
            for (const ArchiveFile& member : ReadRarc(DecodeIfYaz0(data, decoded)))
            {
                if (IsStartUpArchive(member.data))
                {
                    return ContainedArchive{
                        {member.data.begin(), member.data.end()}, (*f)->path, std::string(container.folder)};
                }
            }
        }
        catch (const FormatError&)
        {
        }
    }

    return std::nullopt;
}

// The file or folder at `relative` below `folder`, with '/' between its parts, in any capitals, or nothing.
std::optional<fs::path> FindCaseless(const fs::path& folder, const std::string& relative)
{
    fs::path at = folder;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = relative.find('/', start);
        const std::string part = relative.substr(start, slash == std::string::npos ? slash : slash - start);
        std::optional<fs::path> next;
        std::error_code error;
        for (const fs::directory_entry& e : fs::directory_iterator(at, error))
        {
            if (AsciiLower(Utf8(e.path().filename())) == AsciiLower(part))
            {
                next = e.path();
                break;
            }
        }
        if (!next || slash == std::string::npos)
        {
            return next;
        }

        at = *next;
        start = slash + 1;
    }
}

// A field of a JMap table (BCSV). The table starts with its count of rows, its count of fields, the offset of its
// first row and the size of a row. 12 bytes then describe each field: the hash of its name, a mask, its offset in a
// row, a shift and its type. Type 0 is a 32-bit integer, and type 6 the offset of a string in the strings after the
// rows.
struct JMapField
{
    uint32_t mask = 0;
    uint16_t offset = 0;
    uint8_t shift = 0;
    uint8_t type = 0;
};

// The hash of a field's name: each character, as a signed char, added to 31 times the hash so far.
uint32_t JMapHash(std::string_view name)
{
    uint32_t hash = 0;
    for (const char c : name)
    {
        hash = hash * 31 + static_cast<uint32_t>(static_cast<signed char>(c));
    }

    return hash;
}

// The field of table `r` named `name`. Throws FormatError if the table has none.
JMapField FindJMapField(const xsflib::Reader& r, std::string_view name)
{
    const uint32_t fields = r.U32(4);
    for (uint32_t i = 0; i < fields; i++)
    {
        const std::size_t at = 16 + 12 * std::size_t{i};
        if (r.U32(at) == JMapHash(name))
        {
            return {.mask = r.U32(at + 4), .offset = r.U16(at + 8), .shift = r.U8(at + 10), .type = r.U8(at + 11)};
        }
    }

    throw FormatError("the table has no field " + std::string(name));
}

// Pikmin's sound files on a disc: the bank file (.bx) in a Banks folder, the sequence index in the disc's main.dol, and
// the file of sequences in the Seqs folder beside the Banks folder that the index names. The index gives the file's
// name in 16 bytes at +0x10.
struct BankFileGame
{
    const ntdlib::disc::GameFile* bank_file = nullptr;
    const ntdlib::disc::GameFile* sequences = nullptr;
    std::vector<uint8_t> index;
    std::string folder; // the folder of the Banks and Seqs folders, in lowercase and ending in '/'
};

std::optional<BankFileGame> FindBankFileGame(const std::vector<const ntdlib::disc::GameFile*>& files,
                                             std::span<const uint8_t> dol)
{
    constexpr std::size_t kNameAt = 0x10;
    constexpr std::size_t kNameSize = 16;
    constexpr std::size_t kHeaderSize = 0x20;
    constexpr std::size_t kEntrySize = 0x20;
    const std::optional<std::size_t> at = ntdlib::jaudio::FindBarc(dol);
    if (!at)
    {
        return std::nullopt;
    }

    const std::span<const uint8_t> index = dol.subspan(*at);
    const std::size_t count = std::size_t{index[12]} << 24 | std::size_t{index[13]} << 16 |
                              std::size_t{index[14]} << 8 | std::size_t{index[15]};
    const auto name_bytes = index.subspan(kNameAt, kNameSize);
    const std::string name = AsciiLower(std::string(name_bytes.begin(), std::ranges::find(name_bytes, uint8_t{0})));

    for (const ntdlib::disc::GameFile* f : files)
    {
        const std::string path = AsciiLower(f->path);
        const std::size_t banks = path.rfind("banks/");
        if (!path.ends_with(".bx") || banks == std::string::npos || (banks != 0 && path[banks - 1] != '/') ||
            path.find('/', banks + 6) != std::string::npos)
        {
            continue;
        }

        std::string folder = path.substr(0, banks);
        const auto sequences =
            std::ranges::find_if(files, [&](const auto* g) { return AsciiLower(g->path) == folder + "seqs/" + name; });
        if (sequences != files.end())
        {
            const auto size = static_cast<std::ptrdiff_t>(kHeaderSize + kEntrySize * count);
            return BankFileGame{f, *sequences, {index.begin(), index.begin() + size}, std::move(folder)};
        }
    }

    return std::nullopt;
}

} // namespace

std::map<std::string, uint8_t> ReadVolumeTable(std::span<const uint8_t> archive, const GameRules::VolumeTable& table)
{
    std::vector<uint8_t> decoded;
    const std::vector<ArchiveFile> files = ReadRarc(DecodeIfYaz0(archive, decoded));
    const auto file =
        std::ranges::find_if(files, [&](const ArchiveFile& f) { return AsciiLower(f.name) == AsciiLower(table.file); });
    if (file == files.end())
    {
        throw FormatError("the archive holds no " + table.file);
    }

    const xsflib::Reader r(file->data, true, {.check_string_offset = true});
    const uint32_t rows = r.U32(0);
    const std::size_t first = r.U32(8);
    const std::size_t size = r.U32(12);
    const JMapField name = FindJMapField(r, table.name_field);
    const JMapField volume = FindJMapField(r, table.volume_field);
    if (name.type != 6 || volume.type != 0 || volume.shift >= 32)
    {
        throw FormatError("the table's fields aren't a name and an integer");
    }

    std::map<std::string, uint8_t> volumes;
    const std::size_t strings = first + rows * size;
    for (uint32_t i = 0; i < rows; i++)
    {
        const std::size_t row = first + i * size;
        const uint32_t value = (r.U32(row + volume.offset) & volume.mask) >> volume.shift;
        volumes[r.CString(strings + r.U32(row + name.offset), 0x100)] = static_cast<uint8_t>(value & 0xff);
    }

    return volumes;
}

GameRules RulesOf(const std::string& archive_sha1, const GameOptions& options)
{
    // Four Swords Adventures: the SHA-1s of the USA release's and the Japanese release's start-up archives.
    constexpr std::array<std::string_view, 2> kFourSwordsAdventures{"8c55097f2cc9f5800fd8ab96da4e3b09727e0a39",
                                                                    "aab07cea25ba957374524c4f29d36b72e6d36a67"};
    // The Wind Waker: the SHA-1s of the start-up archives of the USA and Japanese releases on the GameCube and of the
    // USA and Japanese releases of The Wind Waker HD. The European releases have the USA releases' archives.
    constexpr std::array<std::string_view, 4> kWindWaker{
        "65ea70655099a196e0707bf4e4ce61e7818553d9", "0a0f93a5bb97313d895e0a5a16604ad77f567547",
        "c66c0103a5c7c1292b5fc6c3371ed1458035cde7", "aa3077a9dafc178e46b80b4e5d975550cb28c517"};
    // Luigi's Mansion: the SHA-1 of the USA release's start-up archive.
    constexpr std::string_view kLuigisMansion = "4a60396b9c35333f3083848be1f49c82bf9ba4df";
    // Pikmin 2: the SHA-1s of the USA release's start-up archive on the GameCube and on the Wii.
    constexpr std::array<std::string_view, 2> kPikmin2{"a8b63d2f333c34aaf9cb859f37f23011149af917",
                                                       "6747c3c7bce964bc62f817e1c8c32711c905c4c3"};
    // Mario Kart: Double Dash!!: the SHA-1 of the USA release's start-up archive, AudioRes/GCKart.baa.
    constexpr std::string_view kDoubleDash = "a90e5f2b804e154a043f8b89e0bd9d33f4798a99";
    // Twilight Princess: the SHA-1 of the start-up archive on the GameCube. The USA, European and Japanese releases
    // share it.
    constexpr std::string_view kTwilightPrincessGameCube = "05b96ae88eb84f1f29fb606aad19b1be138223e8";
    // Twilight Princess: the SHA-1s of the start-up archive on the Wii. The USA release's third version, the Japanese
    // release and Twilight Princess HD hold the first file. The USA release's first version holds the second, and the
    // European release the third.
    constexpr std::array<std::string_view, 3> kTwilightPrincessWii{"f3e93983847ccbb9d801ad047faea0182ab466cc",
                                                                   "b17393f6f9a15d891c8aa02682c6e5ee08b3f0c4",
                                                                   "d7d9d7b67d6ef6334a31d1d71be87a80e012cf0a"};
    // Link's Crossbow Training: the SHA-1s of the start-up archive. The USA release (revision 1) and the European
    // release hold the first file, and the Japanese release the second.
    constexpr std::array<std::string_view, 2> kCrossbowTraining{"b93115de942c91600250e2742be35005708aad91",
                                                                "53f7fd5c6de729cd161e877ca1bbd5689e163cf1"};
    // Donkey Kong Jungle Beat: the SHA-1s of the USA release's start-up archive, decoded, on the GameCube and in New
    // Play Control! on the Wii.
    constexpr std::array<std::string_view, 2> kJungleBeat{"8cf5a451fc0533f8922e4cd0b922029a4d1f5129",
                                                          "b8c1a0440d328b6d197827b7fd16b04fe170fd71"};
    // Super Mario Sunshine: the SHA-1 of the USA release's start-up archive.
    constexpr std::string_view kSunshine = "00c63687e6963ad89e1d86f02070e1f552623632";
    // Super Mario Galaxy and Super Mario Galaxy 2: the SHA-1s of the USA releases' start-up archives
    // (AudioRes/SMR.szs), decoded.
    constexpr std::string_view kGalaxy = "01611ea8ef87b9a6fcec990ec7c9accfb7c00720";
    constexpr std::string_view kGalaxy2 = "037ac96ba485f943bdabef4e18b579df9a985f99";
    // Pikmin: the SHA-1s of the USA release's bank file (Game::OpenBankFile) on the GameCube and in New Play Control!
    // on the Wii.
    constexpr std::array<std::string_view, 2> kPikmin{"9ae6b40b8e9eacfda6ed0b5d59ff1e680373a018",
                                                      "9f42f65be7f7159faa14b003524476d624e1227f"};
    GameRules rules;
    if (archive_sha1 == kSunshine)
    {
        // The game's music start calls JAISound::setVolume (0x8030a57c in the USA release's main.dol) with the float at
        // 0x8040c1c0, 0.75, at 0x80016a40. No code writes that float.
        rules.sequence_volume = 0.75f;

        // The game's sequence callback (0x800158a8) mutes child 15 of a track that calls it with ID 0. 14 pieces have
        // a child 15 of the root track that does so. The game's code clears the mute of the music's child 15 while
        // Mario rides Yoshi (0x80016548, called from 0x802703c0 and 0x802704b0). The pieces play as they do without
        // Yoshi.
        rules.mute_child_15 = true;

        // MSound's constructor (0x80014e70) gives each category of effects that has sounds a volume, in the loop from
        // 0x80014fe4 to 0x80015070. The category's row of the table at 0x803ac8ec (16 bytes a row) holds a float at +8.
        // The constructor passes the whole part of 127 times that float to JAIBasic::setSeCategoryVolume (0x803029a4).
        // That function stores the volume divided by 127. The name file names the categories. The rows give the pump
        // (category 0) 0.76, objects (3) 0.81, system sounds (4) 0.84, the surroundings (5) 0.59, water (6) 0.9, other
        // characters (8) 0.76, and Mario (1), enemies (2) and Mario's voice (7) 1. Nine other calls of
        // setSeCategoryVolume, from 0x800143b8 to 0x80014c24, change the volumes later. Nobody has read when they run.
        constexpr std::array<std::pair<uint8_t, uint8_t>, 6> kCategoryVolumes{
            {{0, 96}, {3, 102}, {4, 106}, {5, 74}, {6, 114}, {8, 96}}};
        for (const auto& [category, volume] : kCategoryVolumes)
        {
            rules.category_volumes[category] = static_cast<float>(volume) / 127.0f;
        }
    }

    if (std::ranges::find(kJungleBeat, archive_sha1) != kJungleBeat.end())
    {
        // The game starts its music through its BGM player (0x80253a7c in the USA release's main.dol). The player
        // gives the piece its first states at once (0x80253d20): the first state of its row of the table at
        // 0x8035223c, then the first of its row of the table at 0x803533bc. The table at 0x80351e58 gives 44 pieces
        // their rows, by the low half of their IDs. A state holds a code for each child of the piece's track 0, a
        // nibble each, from child 0 in the high nibble (0x80253684). Codes 0 and 2 turn a child off: 0 sets its volume
        // to 0, and 2 mutes it too. Codes 1 and 3 turn it on, and code 7 leaves it. The game turns some children on
        // later. JA_BGM_OP_CEMEMONY's first state turns every child off, and the game turns them on as the piece goes
        // on. It plays here with the children of its next state on. The player then hands the game's rhythm the piece
        // whose entry in the sound table sets bit 0 of the halfword at +4 (0x80253444). 62 pieces set it. The rhythm
        // mutes child 0 of track 0, which writes the beat to port 0xe (0x80247e20). Each mask gives the children of
        // track 0 that play. The Wii release's code does the same. In its main.dol, the BGM player starts a piece at
        // 0x802796ec and gives it its first states at 0x80279940, from the tables at 0x803ab6b0, 0x803abac4 and
        // 0x803acc44. The player hands the piece to the rhythm at 0x80278fd0, and the rhythm mutes child 0 at
        // 0x8026f8a0. The Wii's tables give the same masks. Its sound table adds four pieces, 0x53 to 0x56, with
        // neither a row nor the rhythm's bit. JA_BGM_COURSE_SELECT gains a row there. Its first state turns child 0
        // off, as the rhythm does.
        constexpr std::array<std::pair<uint16_t, uint16_t>, 62> kStartingChildren{{
            {0x03, 0x37fe}, {0x04, 0x0ffe}, {0x05, 0x0ffe}, {0x06, 0xfffe}, {0x07, 0xfffe}, {0x08, 0xfffe},
            {0x0a, 0xfffe}, {0x0b, 0xfffe}, {0x0e, 0x01fa}, {0x0f, 0x000e}, {0x10, 0x1ffe}, {0x11, 0xfffe},
            {0x13, 0xfefe}, {0x14, 0xfffe}, {0x15, 0xfffe}, {0x16, 0xf1fe}, {0x17, 0x0ffe}, {0x18, 0x0ffe},
            {0x19, 0x0ffe}, {0x1a, 0x1dc0}, {0x1d, 0x1ffe}, {0x1e, 0x1ffe}, {0x1f, 0xfffe}, {0x20, 0x1ffe},
            {0x21, 0x1ffe}, {0x22, 0x3ffe}, {0x23, 0x1ffe}, {0x24, 0x1ffe}, {0x25, 0x1ffe}, {0x26, 0xfffe},
            {0x27, 0x3ffe}, {0x28, 0x1ffe}, {0x29, 0x3ffe}, {0x2a, 0x1ffe}, {0x2b, 0xfffe}, {0x2c, 0xfffe},
            {0x2f, 0x3ffe}, {0x30, 0x7ffe}, {0x31, 0xfffa}, {0x32, 0xfe0e}, {0x33, 0xfffa}, {0x34, 0xff8e},
            {0x35, 0xfffa}, {0x36, 0xfc0e}, {0x37, 0xfffe}, {0x38, 0xfffe}, {0x39, 0x1ffe}, {0x3a, 0x1ffe},
            {0x3f, 0x3ffe}, {0x41, 0x1ffe}, {0x42, 0x1ffe}, {0x43, 0xfffe}, {0x44, 0x07fe}, {0x45, 0xfffe},
            {0x46, 0x0ffe}, {0x47, 0xfffe}, {0x48, 0x07fe}, {0x49, 0x3ffe}, {0x4a, 0x803e}, {0x4b, 0x0ffe},
            {0x4c, 0xfffe}, {0x51, 0xfffe},
        }};
        for (const auto& [piece, children] : kStartingChildren)
        {
            TrackMask tracks = kEveryTrack;
            tracks[0] = children;
            rules.start_states[0x01000000u | piece].tracks = tracks;
        }

        // Some effects' sequences add a value at port 0xb to a key. The game's code writes the value as it starts the
        // effect. Most effects start by name with a count (0x8022f968 in the GameCube's main.dol, 0x80260f20 in the
        // Wii's). The audio manager's start of a sound with a count (0x8023f98c, 0x802678b0 on the Wii) writes the
        // count to port 0xb of some of them (0x802405ac, 0x802686b0). Some of the game's objects write a value there
        // after they start an effect (0x8024658c, 0x8026e2b0). The values below are the first that the game's code
        // gives the effects whose first value isn't 0. In both releases, JA_SE_BEAT_POINT_ADD (0x37) takes the points
        // added so far + 60 (0x802400c8, 0x80268124). They start at 0 (0x80240064, 0x802680b4). No code sets the
        // limit below which the points rise. The game therefore never reaches the start.
        // JA_SE_OBJ_SOUND_BLOCK_DISAPPEAR (0x4000f) takes the entry of a table (8, 4, 1, 0 at 0x803f09a0, 0x80500780)
        // for the block's step. The step starts at 0 (0x802438cc, 0x8026aa10).
        rules.ports[0x37] = {{0xb, 60}};
        rules.ports[0x4000f] = {{0xb, 8}};
        if (archive_sha1 == kJungleBeat[0])
        {
            // On the GameCube, JA_SE_PAUSE_MENU_CLAP_PARAM (0x8e) takes the pause menu's setting of the microphone
            // + 1 after each step of the setting (0x801c8964). A new save gives the setting 6 (0x8021ddf4, 0x801c87f4).
            // The menu checks a step down first (0x801c8de4), to 5.
            rules.ports[0x8e] = {{0xb, 6}};
        }
        else
        {
            // On the Wii, JA_SE_PAUSE_MENU_CLAP_PARAM (0x8e) takes the opening ceremony's count of hits + 1
            // (0x801cf75c). The count starts at 0 (0x801ced3c). JA_SE_STAGE_RESULT_DEC (0xcc) always takes 7
            // (0x801d53c0). JA_SE_BLACK_FAIRY_REFLECT (0x169) takes (the reflections + 1) * 2 (0x8013c4a0). They start
            // at 0 (0x8013a5b8). JA_SE_MOOSHIN_CONTINUOUS_JUMP_H (0x18a) takes a count of jumps from 8 (0x802123dc).
            // The count starts at 0 (0x801e4db4) and rises by 1 after each jump's effect (0x801e5370).
            rules.ports[0x8e] = {{0xb, 1}};
            rules.ports[0xcc] = {{0xb, 7}};
            rules.ports[0x169] = {{0xb, 2}};
            rules.ports[0x18a] = {{0xb, 8}};
        }
    }

    if (archive_sha1 == kWindWaker[2] || archive_sha1 == kWindWaker[3])
    {
        // The Wind Waker HD runs the GameCube's coefficients at the Wii U's rate.
        rules.filter_rate = 48000.0;
    }

    if (std::ranges::find(kWindWaker, archive_sha1) != kWindWaker.end())
    {
        // JAIZelBasic::init (0x802a9664 in the GameCube's main.dol) gives the eight categories of effects their
        // volumes, over 127 (JAIBasic::setSeCategoryVolume). The Japanese and European releases' code reads the same
        // table (0x803e9a64 and 0x803fdd84). So does The Wind Waker HD's, a byte at a time from 0x0201dc18 (0x1018dc97
        // in the USA release's cking.rpx, 0x1018dcb7 in the Japanese release's). The categories hold mostly system
        // sounds (0x70), Link's voice (0x7f), his movements (0x70), his footsteps (0x68), other characters' voices
        // (0x70) and movements (0x70), objects (0x68) and the surroundings (0x68).
        constexpr std::array<uint8_t, 8> kVolumes{0x70, 0x7f, 0x70, 0x68, 0x70, 0x70, 0x68, 0x68};
        for (std::size_t category = 0; category < kVolumes.size(); category++)
        {
            if (kVolumes[category] != 0x7f)
            {
                rules.category_volumes[static_cast<uint8_t>(category)] =
                    static_cast<float>(kVolumes[category]) / 127.0f;
            }
        }

        // Four effects add a random number to a base key (register op 0x90): se3-0049 a number below 3 to key 48,
        // se4-0254 one below 8 to 48, se4-0255 one below 8 to 36 and se4-0327 one below 4 to 24. The banks give some
        // of those keys a velocity region of volume 0. From the generator's first state, the first draw picks 49, 51,
        // 39 and 27, and all four play nothing. On the GameCube, the keys that make a sound are 48, 48 to 50, 36 to
        // 38 and 24 to 25.
        if (archive_sha1 == kWindWaker[0] || archive_sha1 == kWindWaker[1])
        {
            rules.random_keys[0x3831] = {{48, 1}};
            rules.random_keys[0x48fe] = {{48, 8}, {49, 5}, {50, 1}};
            rules.random_keys[0x48ff] = {{36, 8}, {37, 5}, {38, 1}};
            rules.random_keys[0x4947] = {{24, 3}, {25, 5}};
        }

        // The first two archives are the GameCube's. The Japanese release's table (0x8038eb58 in its main.dol) holds
        // the USA release's bytes (0x8039b630). The Japanese HD release's (0x1018dcd8) holds the USA HD release's
        // (0x1018dcb8).
        AddWindWakerMusicStates(rules, archive_sha1 == kWindWaker[0] || archive_sha1 == kWindWaker[1]);
    }

    if (std::ranges::find(kFourSwordsAdventures, archive_sha1) != kFourSwordsAdventures.end())
    {
        // The game plays its system sounds, category 0, at a volume of 0.7 in each of its sound modes. The other
        // categories keep a volume of 1. The function at 0x80458744 in the USA release's main.dol takes the value from
        // a table at 0x804b1e60 and passes it to JAISoundParamsMove::moveVolume (0x800262ac). The Japanese release has
        // the same table.
        rules.category_volumes[0] = 0.7f;

        // JASOscillator::release (0x8002d8d4) gives an oscillator with neither a release table nor a direct release a
        // direct release of 0x10. Twilight Princess's build stops such an oscillator at once.
        rules.default_release = 0x10;

        // At start-up, the function at 0x80459ae4 passes the FX lines in the table at 0x80491600 to JASDsp::setFXLine
        // (0x8002cf5c). A line delays an effect send by 64 sub-frames (5,120 samples). Its filter has one tap, of
        // 0x3fff. The filter's output feeds back into the line and goes to one side of the mix at 0x6000. A sound
        // therefore repeats every 64 sub-frames: the first time at 0.375 times its send, then at half the level each
        // time. A voice at a volume of 1 gets a mixer volume of 12000: JASDriver::getChannelLevel_dsp (0x8002c328)
        // returns the halfword at 0x80539bd0. The game mixes each voice at twice its samples times the mixer volume in
        // 32768ths.
        rules.echo = EchoLine{64, 0x3fff / 32768.0f, 0x6000 / 32768.0f, 2.0f * 12000.0f / 32768.0f};

        // When the game starts JA_BGM_SELECT (0x0100001a) as its music, the function at 0x80459674 in the USA
        // release's main.dol calls JASTrack::mute (0x80037f1c) on the children of the piece's track 0 just after
        // startSound. It mutes all of them but 0.2 and 0.7. 0x80458ff0 does the same. The Japanese release does it
        // too.
        TrackMask select = kEveryTrack;
        select[0] = 1u << 2 | 1u << 7;
        rules.start_states[0x0100001a].tracks = select;

        // The function at 0x80458408 switches the piece to one of two sets of tracks. With its flag set, it plays
        // every child of track 0 but 0.2. Without it, it plays only 0.2 and 0.7. The music start leaves the same
        // tracks playing. The game's file select and its choice of mode clear the flag (0x803d0264 and 0x803df810).
        // The screen where the players join and the map where they pick a stage set it (0x803a8884 and 0x8039027c).
        // The piece plays on from one screen to the next and changes its tracks there. The Japanese release has the
        // same function at 0x80079998.
        TrackMask map = kEveryTrack;
        map[0] = 0xffff & ~(1u << 2);
        rules.arrangements.push_back({.sound = 0x0100001a, .suffix = "map", .state = {.tracks = map}});
    }

    if (archive_sha1 == kLuigisMansion)
    {
        // The game starts LuigiSings (0x80000007) through 0x8018c274 in the USA release's main.dol. That function and
        // the game's frame (0x8018bd84) pick the root's children that play from the game's state. Children 4 to 7 hold
        // the tune. One of them plays, and 0x80198e4c mutes the others: child 4 while Luigi has more than 70 of his
        // 100 points of health, child 5 while he has more than 40, and child 6 below that. Child 7 plays while the
        // room's lights are on, unless the room is outdoors. The others then keep a volume of 0. Otherwise 0x80189488
        // gives the accompaniment a volume of 1 and the rest 0: children 0 to 3 and 11 in the hallways, child 9
        // outdoors, and children 8 and 10 in the other rooms. Rooms 19, 32 and 65 have none. The frame also sets the
        // piece's tempo: 0.6 + 0.4 x the health / 100. With the lights on, it scales that by the time between Luigi's
        // steps: 12 / 16 x 1.2 while he stands still. The game never plays the piece with every child. A state takes
        // the most health of its band.
        constexpr uint16_t kHallway = 0xf | 1u << 11;
        constexpr uint16_t kRoom = 1u << 8 | 1u << 10;
        constexpr uint16_t kOutdoors = 1u << 9;
        const std::array<std::pair<std::string_view, uint16_t>, 4> places{
            {{"hallway", kHallway}, {"room", kRoom}, {"outdoors", kOutdoors}, {"unaccompanied", 0}}};
        const std::array<std::pair<int, uint16_t>, 3> healths{{{100, 1u << 4}, {70, 1u << 5}, {40, 1u << 6}}};
        // The volumes of a state whose children 0 to 11 play only where `playing` has their bits. Children 12 to 15
        // play in every state.
        const auto silent_children = [](uint16_t playing)
        {
            std::map<uint8_t, float> volumes;
            for (uint8_t child = 0; child < 12; child++)
            {
                if ((playing >> child & 1) == 0)
                {
                    volumes[child] = 0.0f;
                }
            }
            return volumes;
        };
        for (const auto& [place, accompaniment] : places)
        {
            for (const auto& [health, tune] : healths)
            {
                rules.arrangements.push_back(
                    {.sound = 0x80000007,
                     .suffix = std::string(place) + "_" + std::to_string(health) + "_HP",
                     .state = {.child_volumes = silent_children(accompaniment | tune),
                               .tempo_ratio = 0.6f + 0.4f * static_cast<float>(health) / 100.0f}});
            }
        }
        rules.arrangements.push_back(
            {.sound = 0x80000007,
             .suffix = "lit_room",
             .state = {.child_volumes = silent_children(1u << 7),
                       .tempo_ratio = 12.0f / 16.0f * 1.2f * (0.6f + 0.4f * 100.0f / 100.0f)}});
        rules.only_arranged.push_back(0x80000007);

        // The game's sound class (its vtable at 0x8038ef60) replaces JAIBasic::setSeExtParameter (0x801908d8) with
        // 0x8018d714. That function gives sequences and streams the parameters of their entries in the sound table. It
        // returns at once for an effect. An effect then plays at full volume, whatever its entry holds. 217 of the
        // 1,264 effects give 255 there. The sound library starts each of the five categories at a volume of 1
        // (0x8018f270). The game's code changes only category 4's (0x80190694): to 80/127 in room 57 (0x80189af4), and
        // back to 1 in the other rooms. The rules keep 1.
        rules.effect_parameters = false;

        // The game's JASystem has no volume mode. Its TTrack::updateTrack (0x801a6a2c) gives the channels the track's
        // volume as it is, or 0 for a muted track (0x801a6ae4).
        rules.squared_track_volume = false;

        // The sequences' random numbers (register op 0x90, 0x801a5ee0) come from 0x801a35e0. That generator reads the
        // time base (0x801e13c4) at each draw. The time picks the order of the number's bytes and its sign, and it
        // moves the generator's state. A number therefore depends on the time at which the game draws it.
        rules.timed_random = true;

        // The game's oscroute (0x801a87a0) and simpleadsr (0x801aa1a0) differ from The Wind Waker's. Nobody has read
        // how its notes take the routes.
        rules.track_oscillator_routes = false;
    }

    if (std::ranges::find(kPikmin2, archive_sha1) != kPikmin2.end())
    {
        // The sound table's first sequence is a jingle. The game's sound layer starts se.bms as its effects sequence:
        // the function at 0x80335728 in the GameCube's main.dol makes the sequence's object with that name
        // (0x8051e134) at 0x803359c0.
        rules.effects_sequence = "se.bms";

        // Three effects draw a random number (JASPlayer::getRandomS32, 0x8009c6a4 in the GameCube's main.dol and
        // 0x8004e508 in the Wii's) and end at once on some draws. The first draw ends all three. Renders of a run of
        // the GameCube's code after 0 to 15 draws play each one first after 3, 1 and 1 draws. Each plays the same sound
        // on every draw that doesn't end it.
        rules.random_starts = {{0x5829, 3}, {0x58fc, 1}, {0x58fe, 1}};

        // The game's sound layer gives the sound table's sequences their data as it plays them. The files that the
        // table names hold other pieces. The name file would call c_result PSSQ_JUNGLE and comp_result PSSQ_FOREST.
        rules.sequence_names = false;

        // Each piece is an object of the game's sound layer, which answers the calls that the piece's tracks make
        // (PSGame::seqCpuSync, 0x803372e0). The game's sound library is a later JAudio1
        // (SequenceSound::later_jaudio1).
        rules.later_jaudio1 = true;

        // A cave's music is an AutoBgm: a conductor picks the phrases that its tracks play (SequenceSound::conductor).
        // PSM::SceneMgr::newAutoBgm takes the conductor from the floor's list (BgmList_*.txt). These are the first of
        // each family. Each of them plays on some floors. The game's code names caveconc_00_0.cnd itself, for a boss's
        // floor.
        // No code or list of the game starts a_iron, a_zawinul01 or caveboss. Their calls are those of the caves'
        // music. a_iron and a_zawinul01 play with iron.cnd, and caveboss with caveboss.cnd.
        for (const auto& [file, cnd] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"a_iron.bms", "iron.cnd"},
                 {"a_zawinul01.bms", "iron.cnd"},
                 {"caveboss.bms", "caveboss.cnd"},
                 {"caveconc.bms", "caveconc_00_0.cnd"},
                 {"caveglass.bms", "caveglass_00_0.cnd"},
                 {"cavekeyget.bms", "cavekeyget.cnd"},
                 {"cavemetal.bms", "cavemetal_00_0.cnd"},
                 {"caverelax.bms", "caverelax.cnd"},
                 {"cavesoil.bms", "cavesoil_00_0.cnd"},
                 {"cavetile.bms", "cavetile_00_0.cnd"},
                 {"cavetsumiki.bms", "cavetsumiki_00_0.cnd"},
             })
        {
            rules.conductors[file] = cnd;
        }
        for (int i = 0; i <= 8; i++)
        {
            rules.conductors["new_0" + std::to_string(i) + ".bms"] = "new_0" + std::to_string(i) + "_0.cnd";
        }

        // The other pieces' roots wait between polls for as many ticks as call 0x2e00 gives
        // (SeqTrackRoot::beatUpdate, 0x803428c8). The time is 60 ticks (0x80342368). PikSceneMgr::initMainBgm halves
        // it for last and yakushima (0x80336e38 and 0x80336ec4). The root of an area's or a boss's music asks for its
        // start point with call 0x4000 (JumpBgmSeq::getSeqStartPoint, 0x80333950). A boss starts its music at a point
        // that the boss gives (PSM::Scene_Cave::bossAppear, 0x8046a5c8): 9 for the middle bosses' m_boss, and 1 for
        // the final boss's l_boss. Call 0x4001 gives 0xffff while no jump waits (JumpBgmSeq::outputJumpRequest,
        // 0x80333888). Every other call of these pieces gives 0.
        const std::map<uint16_t, uint16_t> area{{0x2e00, 60}, {0x4000, 0}, {0x4001, 0xffff}};
        const std::map<uint16_t, uint16_t> slow_area{{0x2e00, 30}, {0x4000, 0}, {0x4001, 0xffff}};
        rules.sync_answers = {
            {"forest.bms", area},
            {"n_tutorial.bms", area},
            {"last.bms", slow_area},
            {"yakushima.bms", slow_area},
            {"l_boss.bms", {{0x2e00, 60}, {0x4000, 1}, {0x4001, 0xffff}}},
            {"m_boss.bms", {{0x2e00, 60}, {0x4000, 9}, {0x4001, 0xffff}}},
            {"battle_t.bms", {{0x2e00, 60}}},
            {"2pbattle.bms", {{0x2e00, 60}}},
        };

        // A piece's directors mute their tracks as the tracks start (TrackOnDirectorBase::onPlayInit, 0x8045732c). The
        // directors bring a track in when the game's state calls for it, such as an enemy nearby or a treasure carried.
        // In an area or a cave, the track maps in user/Totaka and user/Wakai list the directors' tracks after the
        // "basic" ones that always play (PSM::DirectorMgr_Scene::initTrackMap, 0x8046f620). A map names a cave's
        // conductor, or the piece where the conductor isn't in a map. battle_t and 2pbattle play with the two-player
        // battle's directors, from track 8 on (PSM::DirectorMgr_2PBattle, 0x80470484). l_boss plays with the bosses'
        // directors (PSM::DirectorMgr_Battle, 0x804701f0). They take tracks 14 and 15 while a big boss exists, and 10
        // and 11 otherwise (newDirector, 0x80470240, through PSM::SceneMgr::curSceneIsBigBossFloor, 0x8045a2d8). The
        // big boss's sound object sets the flag as it's made (0x8046083c). The order in which the game makes the boss
        // and the directors isn't known. The rule mutes 10 and 11, as the directors do without a big boss. From
        // l_boss's start point, the four tracks hold one note, on track 14.
        // m_boss's tracks start at point 9 and never meet the directors. a_iron, a_zawinul01 and caveboss play
        // without directors.
        rules.muted_children = {
            {"forest.bms", 0x0fc0},     {"n_tutorial.bms", 0x0fe0}, {"last.bms", 0xff80},
            {"yakushima.bms", 0x07e0},  {"battle_t.bms", 0xff00},   {"2pbattle.bms", 0x0f00},
            {"l_boss.bms", 0x0c00},     {"caveconc.bms", 0x03f0},   {"caveglass.bms", 0x1fc0},
            {"cavekeyget.bms", 0x0ff0}, {"cavemetal.bms", 0x07f0},  {"caverelax.bms", 0x1f00},
            {"cavesoil.bms", 0x07f0},   {"cavetile.bms", 0x0ff0},   {"cavetsumiki.bms", 0x3ff0},
            {"new_00.bms", 0xffc0},     {"new_01.bms", 0x3ff0},     {"new_02.bms", 0x7fe0},
            {"new_03.bms", 0xff80},     {"new_04.bms", 0x3ff0},     {"new_05.bms", 0x3ff0},
            {"new_06.bms", 0x3fe0},     {"new_07.bms", 0xff80},     {"new_08.bms", 0x1ff0},
        };

        // The sound layer plays each piece at the volume that user/Totaka/BgmList.txt gives it, out of 127
        // (PSSystem::SeqDataList::getSeqVolume, 0x80330e08). PSSystem::SeqBase::init and DirectedBgm::init write it
        // into the piece's sound information (0x80331740 and 0x80332d30). The sound table's volumes play no part. The
        // list pairs a file's name with a number, up to "endoffile". The first pair with the name wins, so new_00
        // plays at 50, not at the 60 of its second pair. Stream::readByte reads the number with sscanf's "%d"
        // (0x8041449c), so new_04's "50+5" gives 50. The Wii release's list is the same file.
        rules.piece_volumes = {
            {"2pbattle.bms", 85},    {"a_iron.bms", 50},         {"a_zawinul01.bms", 50},
            {"caveboss.bms", 50},    {"caveconc.bms", 50},       {"caveglass.bms", 40},
            {"new_00.bms", 50},      {"cavemetal.bms", 45},      {"caverelax.bms", 60},
            {"cavesoil.bms", 35},    {"cavetile.bms", 45},       {"cavetsumiki.bms", 50},
            {"comp_result.bms", 50}, {"c_menu.bms", 50},         {"c_result.bms", 50},
            {"ff_annihi.bms", 50},   {"ff_doped.bms", 50},       {"ff_doping.bms", 50},
            {"ff_keyget.bms", 50},   {"ff_playerdown.bms", 50},  {"ff_treasureget.bms", 50},
            {"forest.bms", 60},      {"f_result.bms", 50},       {"hiscore.bms", 50},
            {"id_test.bms", 50},     {"last.bms", 40},           {"m_boss.bms", 50},
            {"n_tutorial.bms", 35},  {"result.bms", 50},         {"u_result.bms", 50},
            {"worldmap.bms", 40},    {"yakushima.bms", 40},      {"cavekeyget.bms", 50},
            {"l_boss.bms", 50},      {"book.bms", 50},           {"battle_t.bms", 127},
            {"new_01.bms", 55},      {"new_02.bms", 50},         {"new_03.bms", 75},
            {"new_04.bms", 50},      {"new_05.bms", 50},         {"new_06.bms", 50},
            {"new_07.bms", 50},      {"new_08.bms", 60},         {"kuro_pre.bms", 50},
            {"kuro_post.bms", 72},   {"worldmap_intro.bms", 40}, {"n_tutorial_1stday.bms", 50},
        };
    }

    if (std::ranges::find(kPikmin, archive_sha1) != kPikmin.end())
    {
        // New Play Control! Pikmin on the Wii has the GameCube release's sound code at other addresses. The Wii's
        // main.dol names the same 17 sequences. Jac_PlayBgm is at 0x8001c200, Jac_ChangeBgmTrackVol at 0x8001c7c0, and
        // Jac_Orima_Formation at 0x8001b8c0. The tables of children's masks and volumes (0x80315d50 and 0x80315da0)
        // hold the GameCube's values. So do the first 0x200 bytes of the events' offsets (0x80315688). The Wii's
        // catalogue of effects has one more row (see below). The other addresses in these rules are in the GameCube's
        // main.dol.
        const bool wii = archive_sha1 == kPikmin[1];

        // The levels of the mix. Jac_Start (0x800171c0 in the GameCube's main.dol) calls Jac_SetMixerLevel (0x80005ec0)
        // at 0x800172d4 with a channel level of 1.1 (0x803e8390) and a DSP level of 2.0 (0x803e8394).
        // Channel_SetMixerLevel (0x80009400) stores 16384 times the channel level, 18022. UpdateEffecterParam
        // multiplies each channel's volume by that value at 0x8000a154. DsetMixerLevel (0x800088a0) stores 4096 times
        // the DSP level, 8192. DsyncFrame (0x800888e0) passes that value to the DSP program each frame. Super Mario
        // Sunshine's sound object sets 0.802 and 5.0 (0x80014f74 in the USA release's main.dol), with the same scales:
        // 13139 and 20480. The Wii's main.dol calls Jac_SetMixerLevel (0x80007780) at 0x8001a488 with 0.8 (0x8049f370)
        // and 1.5 (0x8049f374). Its scale of the channel level is 32767 (0x8000bc80): 26213 and 6144.
        constexpr float kSunshineMix = 13139.0f * 20480.0f;
        rules.mix_level = (wii ? 26213.0f * 6144.0f : 18022.0f * 8192.0f) / kSunshineMix;

        // Addresses are in the USA release's main.dol. At start-up, the sound library starts pikise.jam, the effects
        // sequence, and sysevent.jam, which plays the events' effects (0x800189c0, from the table at 0x80222278). The
        // game plays a piece through 0x80018c80. That function gives the piece's root track the bank of the piece's
        // number (0x80018df4).
        rules.effects_sequence = "pikise.jam";
        rules.bank_of_number = true;

        // The sound library also starts demobgm.jam at start-up, the music of the cutscenes. A cutscene picks its part
        // by writing the part's number to port 0 of the root track (0x8001a964). The table of cutscenes (0x802269b0)
        // names parts 1, 2, 5, 6, 7, 9 and 10. Part 8 plays when the player finds a part of the ship (0x80016de0).
        // Parts 3 and 4 play part 5's tracks.
        auto& parts = rules.parts["demobgm.jam"];
        for (const uint16_t part : std::array<uint16_t, 8>{1, 2, 5, 6, 7, 8, 9, 10})
        {
            parts.push_back({.suffix = "-" + std::to_string(part), .ports = {{0, part}}});
        }

        // The game plays each other piece through Jac_PlayBgm (0x80018c80). Jac_ChangeBgmTrackVol (0x80019200) gives
        // each child of the piece's first track a volume, through the child's external buffer. A row of the table at
        // 0x80226130 points to the piece's masks of children, by the game's mode. A row of the table at 0x80226180
        // points to its volumes. Both tables count from piece 2. A piece starts in mode 0 (0x80018da4). A child whose
        // bit of the first mask is set plays at the first volume, and any other child at 0. The game changes the mode
        // as play goes on (Jac_SetBgmModeFlag, 0x80018f40).
        const auto volumes = [](uint16_t mask, float volume)
        {
            std::map<uint8_t, float> children;
            for (uint8_t child = 0; child < 16; child++)
            {
                children[child] = (mask >> child & 1) != 0 ? volume : 0.0f;
            }
            return children;
        };
        for (const char* name :
             {"d_end2.jam", "jungle.jam", "boss2.jam", "map.jam", "boss3.jam", "select.jam", "char.jam", "fresult.jam"})
        {
            rules.child_volumes[name] = volumes(0xffff, 0.5f);
        }
        rules.child_volumes["tutorial.jam"] = volumes(0x103c, 0.5f);
        rules.child_volumes["play3.jam"] = volumes(0x0033, 0.5f);
        rules.child_volumes["yaku.jam"] = volumes(0xc00f, 0.5f);
        rules.child_volumes["cave.jam"] = volumes(0x0047, 0.5f);
        rules.child_volumes["flow.jam"] = volumes(0x057d, 0.55f);
        rules.child_volumes["cresult.jam"] = volumes(0xffff, 0.35f);

        // The game's catalogue of effects. The system's effects go to track 0x10009 of pikise.jam, child 9 of its
        // first track (0x80016a60), and the player's to child 10 (0x800183f8). Effect 0 of the system stops the others.
        // Effect 15 plays only after a pause (0x80016d78). Effect 18 plays part 8 of demobgm.jam (0x80016de0), and 32
        // a stream (0x80016df8). Effect 16 plays the player's effect 14 (0x80016bf8). The catalogue lacks the
        // system's effects 25, 30, 31 and 36, which the demo of a found part of the ship plays (0x8001b120 and
        // 0x8001a748), and the player's voice 0x800c, which the first scene's set-up picks (0x80019c58). An event's
        // effect goes to the track for the event in sysevent.jam (0x80017dcc).
        // The Wii's catalogue (0x80341868) adds row 309 for the system's effect 41, named "Change Piki". The rows that
        // follow it are the GameCube's rows 309 to 330.
        rules.jac_effects = GameRules::JacEffects{
            .dol_sha1 = wii ? "551306334183a633d0fde434a930b98809433b70" : "02204260b7efe8742d34572e58ba3dfecd92e4e9",
            .catalogue = wii ? 0x80341868u : 0x802b33d4u,
            .count = wii ? 332u : 331u,
            .event_offsets = wii ? 0x80315688u : 0x80225a68u,
            .events_sequence = "sysevent.jam",
            .system_track = 9,
            .player_track = 10,
            .left_out = {0, 15, 18, 32},
            .played_by_player = {{16, 14}},
            .other_system = {25, 30, 31, 36},
            .other_player = {0x800c},
        };

        // The sound of the formation. The game passes the player's move to Jac_Orima_Formation (0x80018200) as x and y
        // each frame. The function holds both to -127 to 127. It writes x to port 2 of the effects sequence's track
        // 0x10007, child 7 of its first track, and the move's length (x, y) to port 3. As a move starts, it writes 1 to
        // port 0. The track then plays the formation's sound, faster the longer the move. A full move straight ahead
        // gives x 0 and a length of 127.
        rules.jac_effects->port_effects = {
            {.name = "formation", .track = 7, .port = 0, .number = 1, .ports = {{2, 0}, {3, 127}}}};
    }

    if (archive_sha1 == kDoubleDash)
    {
        // The game reads its sound table by the first layouts. The function at 0x80120080 in the USA release's main.dol
        // compares an entry's type with 0x10 (an effect of the sequence collection), 0x20 (a sequence block) and 0x30
        // (a stream). For each, it divides the entry's ninth byte by 255 for the sound's volume.
        rules.first_layouts = true;

        // The game's main.dol names AudioRes/GCKart.baa and AudioRes/Waves, and no sequence archive.
        rules.sequence_archive = false;

        // The audio object's start (0x80114740) picks a factor by a mode: 1, 0.8, 0.7 or 0.65 for modes 0 to 3
        // (0x80114804 to 0x8011484c). The race set-up gives the mode (0x801ccfdc): a count in the race data at
        // 0x803b147e, less 1. The count is likely the number of players. 0x80116a60 then gives the five categories of
        // effects their volumes through JAISoundParamsMove (0x8008e370): 1.2, 1.75, 1.8, 1.6 and 1.4. All but category
        // 2's take the factor too. The rules take mode 0.
        rules.category_volumes = {{0, 1.2f}, {1, 1.75f}, {2, 1.8f}, {3, 1.6f}, {4, 1.4f}};

        // The start then calls 0x8011684c. It gives two of the sound manager's parts 1.3 x 1.5 = 1.95 (at +0x73c) and
        // 1.3 x 1.2 = 1.56 (at +0x7a0), times two factors of the audio object that start at 1. The game's music plays
        // at 1.95 times its entry's volume. The other part plays the streams, which the conversion leaves out.
        rules.sequence_volume = 1.95f;

        // The audio object's set-up registers a sequence callback, 0x80116d9c, at 0x80113e88. The callback answers 0
        // to every argument but 0xa. For 0xa, it answers 1 once the game has loaded the award ceremony's voices (its
        // checks from 0x80116dc0). Until then it counts its calls in the track's register 15 and answers 0, or 2 after
        // 384 calls. The 20 characters' JA_SE_LV_*_DEMO wait 480 ticks, then ask with 0xa until the answer is 1. They
        // are the only sounds that call the callback. The rules answer 1, as for a game that has loaded the voices.
        rules.callback = std::map<uint16_t, uint16_t>{{0xa, 1}};
    }

    // The Wii's first USA version and its European release hold other start-up archives. Their code gives the rules
    // below too. Runs on a CPU emulator of changeBgmStatus and changeSubBgmStatus (0x80294a90 and 0x8029660c in the
    // first version's main.dol, 0x802946bc and 0x80296238 in the European one) logged the same calls as runs of the
    // third version, for every piece and status. Z2AudioMgr::init passes the same volumes (0x802af710, 0x802af554).
    // JASDriver::getChannelLevel_dsp returns 12000 (0x8028342c, 0x80283030). With the linker's fields masked,
    // initDataArc, setFXLine, bgmStart, subBgmStart, sceneBgmStart and the battle and field functions match the code of
    // the third version, apart from changes that reach no start and no state. In both releases, the code that ends the
    // battle music differs (0x80298508 and 0x802980d8 in the third version). The first version also differs in a
    // volume of the whole piece that bgmStart and subBgmStart set for two pieces, in one scene's choice of piece and in
    // FX lines 2 and 3. Its Hyrule Field sequence differs as well. The conversion plays the file on the disc.
    const bool twilight_wii = std::ranges::find(kTwilightPrincessWii, archive_sha1) != kTwilightPrincessWii.end();
    if (archive_sha1 == kTwilightPrincessGameCube || twilight_wii)
    {
        // Z2AudioMgr::init (0x802cd34c in the USA release's main.dol) gives the game's ten categories of effects their
        // volumes at start-up. From 0x802cd458 on, it passes Z2Param's variables to JAISoundParamsMove::moveVolume
        // (0x802a2db4). The categories hold system sounds (0x804507f8), Link's voice (0x804507fc), his movements
        // (0x80450800), his footsteps (0x80450804), objects (0x80450810), other characters' voices (0x80450808) and
        // movements (0x8045080c), more voices (0x80450808), more objects (0x80450810) and the surroundings
        // (0x80450814). Link's voice keeps a volume of 1. Each of the other variables holds 120/127. A conversation and
        // the pause menu lower most categories for a while (Z2SeMgr::talkInSe at 0x802adb50, Z2SeMgr::menuInSe at
        // 0x802add58). Z2SeMgr::talkOutSe (0x802adc54) gives them their start-up volumes back. The European and
        // Japanese releases' Z2AudioMgr::init (0x802ce14c and 0x802cf7e8) gives the same volumes from the same
        // variables.
        //
        // The Wii's and HD's code gives the categories the same volumes. The Wii's Z2AudioMgr::init (0x802af124 in
        // the USA release's main.dol) passes the variables from 0x8051fbe8 to 0x8051fc04 to moveVolume (0x80287280)
        // from 0x802af240 on, in the GameCube's order. Link's voice reads 0x8051fbec. The Wii's sound library has an
        // eleventh category, and the init gives it a volume of 1 (0x80525cec). HD's init (0x02c066cc in its
        // executable) does the same from 0x02c06804 on. Its moveVolume is at 0x02b06690, its variables run from
        // 0x1012da20 to 0x1012da3c, Link's voice reads 0x1012da24, and the eleventh category's volume of 1 is at
        // 0x100cc9a8.
        for (uint8_t category = 0; category < 10; category++)
        {
            if (category != 1)
            {
                rules.category_volumes[category] = 120.0f / 127.0f;
            }
        }

        // As the game starts five pieces of music, it gives some children of their track 0 a volume of 0
        // (kTwilightPrincessStarts).
        AddStarts(ntdlib::jaudio::kTwilightPrincessStarts, rules);

        // HD shares the Wii's start-up archive. HD's code gives two pieces other states (kTwilightPrincessHdStates).
        // The Wii's code reaches one state through another status (kTwilightPrincessWiiStates).
        std::span<const ntdlib::jaudio::MusicState> states = ntdlib::jaudio::kTwilightPrincessStates;
        if (options.wii_u)
        {
            states = ntdlib::jaudio::kTwilightPrincessHdStates;
        }
        else if (twilight_wii)
        {
            states = ntdlib::jaudio::kTwilightPrincessWiiStates;
        }
        AddTwilightPrincessMusicStates(states, rules);

        // The audio set-up reads the archive's FX configurations. On the Wii, Z2AudioArcLoader::readBFCA (0x8028e060 in
        // the USA release's main.dol) reads them. Z2FxLineMgr::initDataArc (0x8029cc74) passes the first, default.bfc,
        // to JASDsp::setFXLine (0x802826e8) for all four FX lines. Each line delays an effect send by 64 sub-frames.
        // Its filter has one tap, of 0x3fff. The filter's output feeds back into the line and goes to one side of the
        // mix at 0x6000. Four Swords Adventures sets up the same lines. As a scene starts, setSceneFx (0x8029d2c8) sets
        // lines 2 and 3 from the scene's configuration. Those two lines don't reach a stereo mix. On the Wii,
        // JASDriver::getChannelLevel_dsp (0x80282bc8) returns 12000. On the GameCube, setFXLine (0x8029db78 in the USA
        // release's main.dol) gets the same four lines, from 0x802bad6c and 0x802bae04. There getChannelLevel_dsp
        // (0x8029e150) returns 16225 when the lines are set, although the main.dol holds 12000.
        //
        // HD mixes on the CPU at 48 kHz, in sub-frames of 80 samples. Its reader of the FX configurations (0x02c11398
        // in HD's executable, version 81) scales each length by 10/7. The delay of 64 becomes 91 sub-frames, 151.7 ms.
        // At start-up, HD's setFXLine (0x02b0c5ec) sets its six lines from the first configuration, with the Wii's tap
        // and output. Its mixer (0x02b08928) mixes them. HD's level (0x1012b72c) holds 12000. In these rules'
        // sub-frames, 151.7 ms is 61.
        const float level = archive_sha1 == kTwilightPrincessGameCube ? 16225.0f : 12000.0f;
        const uint32_t delay = options.wii_u ? 61 : 64;
        rules.echo = EchoLine{delay, 0x3fff / 32768.0f, 0x6000 / 32768.0f, 2.0f * level / 32768.0f};
    }

    if (std::ranges::find(kCrossbowTraining, archive_sha1) != kCrossbowTraining.end())
    {
        // The archive of the Japanese release differs from the USA one in the banks, the wave systems and the
        // sequences of the effects. The two share the sound table, the names and the FX configurations. The sequence
        // archive, ZapSoundSeqs.arc, is the same file in both. The Japanese main.dol links the functions below at
        // other addresses. Each function matches the USA one with the linker's fields masked. The Japanese init
        // (0x8016eee0) passes the same volumes, from variables at 0x803db540 to 0x803db55c.
        //
        // Link's Crossbow Training runs a later build of Twilight Princess's sound layer, Z2. Its Z2AudioMgr::init
        // (0x8016eba0 in the main.dol) passes Z2Param's variables to JAISoundParamsMove::moveVolume (0x80149884) from
        // 0x8016ecb0 on, in Twilight Princess's order. The variable for Link's voice (0x803df97c) holds 1. The one
        // for the surroundings (0x803df994) holds 100/127. The variables of the other eight categories hold 120/127
        // (0x803df978 to 0x803df990). The eleventh category gets a volume of 1 (0x803e3fb4).
        for (uint8_t category = 0; category < 10; category++)
        {
            if (category != 1)
            {
                rules.category_volumes[category] = (category == 9 ? 100.0f : 120.0f) / 127.0f;
            }
        }

        // The game's Z2SeqMgr::bgmStart (0x80154aa4) mutes the same children of track 0 as Twilight Princess's
        // bgmStart. From 0x80155018 on, it calls changeBgmStatus (0x80155f3c) with a state of 0 for the same four
        // pieces. Its fieldBgmStart (0x8015b12c) and fieldRidingMute (0x8015b238) work as in Twilight Princess. Only
        // the offset of Link's riding flag differs.
        AddStarts(ntdlib::jaudio::kTwilightPrincessStarts, rules);

        // The game's scenes and one boss give three pieces states (kCrossbowTrainingStates).
        AddTwilightPrincessMusicStates(ntdlib::jaudio::kCrossbowTrainingStates, rules);

        // Z2FxLineMgr::initDataArc (0x8015da80) passes the archive's first FX configuration to JASDsp::setFXLine
        // (0x801442fc) for all four FX lines, from 0x8015df98 and 0x8015e030. The lines are the same as in Twilight
        // Princess. JASDriver::getChannelLevel_dsp (0x8014494c) returns 12000.
        rules.echo = EchoLine{64, 0x3fff / 32768.0f, 0x6000 / 32768.0f, 2.0f * 12000.0f / 32768.0f};
    }

    if (archive_sha1 == kGalaxy || archive_sha1 == kGalaxy2)
    {
        // Each scene's start calls AudSystem::initSceneVolume (0x80027358 in Galaxy's main.dol, 0x800793c0 in Galaxy
        // 2's). The sequence manager plays the music. initSceneVolume moves its volume to AudParams::masterVolBgm, 1.2
        // (0x806a3570, 0x807d7408). It gives the effects' 16 categories the first set of AudParams::scCtgVolume
        // (0x8052e2a0, 0x806457a8) through setSeVolumeSetTrig (0x80029ed4, 0x8007bf30). A category is a group of the
        // sound table's effects. Both games have the same set.
        rules.sequence_volume = 1.2f;
        constexpr std::array<float, 16> kCategoryVolumes{1.2f, 1.2f, 1.2f, 1.3f, 1.2f, 1.2f, 1.2f, 1.2f,
                                                         1.2f, 1.2f, 1.2f, 1.2f, 1.0f, 1.5f, 1.0f, 1.0f};
        for (std::size_t category = 0; category < kCategoryVolumes.size(); category++)
        {
            if (kCategoryVolumes[category] != 1.0f)
            {
                rules.category_volumes[static_cast<uint8_t>(category)] = kCategoryVolumes[category];
            }
        }

        // The game starts its music through AudSingleBgm::start (0x8002e694 in Galaxy, 0x80080fb0 in Galaxy 2). It
        // turns every child of track 0 on but child 0 of a rhythm piece. That child stays as it is. A piece with
        // states then takes its first state at once (changeTrackMuteState, 0x8002e91c and 0x800814b0). AudBgmSetting
        // gives a piece the index of its states: Galaxy's table at 0x8052e4c8 by the piece's place in its group, Galaxy
        // 2's at 0x80645da0 by its ID. A state holds a nibble for each child of track 0, from child 0 in the high
        // nibble (0x8052e810, 0x80645f8c). Its low three bits pass to AudTrackController::setMuteState: 0 fades the
        // child out (at once at the start), 1 fades it in, 2 mutes it, 3 unmutes it, and 7 leaves it. Bit 3 mutes the
        // child once its volume reaches 0. JAISeq::getChildTrack (0x80498dac in Galaxy) gives child n of the root
        // track's first child. Each mask gives the children that play. A mask of 0 plays nothing: the game plays such a
        // piece's sequence for its beat, over a stream. AudBgmMgr::start (0x8002fd04, 0x80082e00) then hands the piece
        // to the rhythm (sendToRhythmSystem, 0x80030200 and 0x80083330). A piece whose switches set bit 0 is a rhythm
        // piece (AudSoundInfo::isRhythmBgm, 0x8002a444 and 0x8007c5b0). AudRhythmMeSystem::setSeq (0x8031bb4c,
        // 0x80428200) then mutes child 0 of its track 0. 73 of Galaxy's 105 pieces are rhythm pieces, and 83 of Galaxy
        // 2's 127.
        rules.rhythm_mutes_child_0 = true;
        using Starts = std::span<const std::pair<uint16_t, uint16_t>>;
        constexpr std::array<std::pair<uint16_t, uint16_t>, 18> kGalaxyStarts{{
            {0x05, 0x0000},
            {0x08, 0x3ffe},
            {0x0f, 0x0000},
            {0x15, 0x0000},
            {0x1a, 0x0000},
            {0x25, 0x0000},
            {0x28, 0x0000},
            {0x38, 0x0000},
            {0x3b, 0x0000},
            {0x3c, 0x0000},
            {0x45, 0x0000},
            {0x46, 0x0000},
            {0x4b, 0x07fe},
            {0x4f, 0x0000},
            {0x53, 0x0000},
            {0x54, 0x0000},
            {0x60, 0x0000},
            {0x63, 0x07fe},
        }};
        constexpr std::array<std::pair<uint16_t, uint16_t>, 41> kGalaxy2Starts{{
            {0x08, 0x05fe}, {0x09, 0x07fe}, {0x0a, 0xc73e}, {0x17, 0x019e}, {0x18, 0x0000}, {0x1d, 0x0000},
            {0x1e, 0x03fe}, {0x21, 0x0000}, {0x23, 0x0190}, {0x25, 0x0000}, {0x29, 0x0190}, {0x2c, 0x1ffe},
            {0x2d, 0x1c86}, {0x31, 0x0000}, {0x32, 0x0000}, {0x35, 0x0000}, {0x39, 0x003e}, {0x3a, 0x0190},
            {0x3c, 0x0000}, {0x45, 0x0000}, {0x46, 0x0000}, {0x47, 0x0000}, {0x53, 0x0000}, {0x54, 0x0000},
            {0x58, 0x0000}, {0x5a, 0x0000}, {0x5b, 0x0000}, {0x5d, 0x0000}, {0x60, 0x0000}, {0x61, 0x0000},
            {0x62, 0x0000}, {0x65, 0x0000}, {0x67, 0x0000}, {0x70, 0x003e}, {0x72, 0x0000}, {0x73, 0x0000},
            {0x74, 0x0000}, {0x76, 0x0000}, {0x77, 0x0000}, {0x78, 0x0000}, {0x7b, 0x0000},
        }};
        for (const auto& [piece, children] : archive_sha1 == kGalaxy ? Starts(kGalaxyStarts) : Starts(kGalaxy2Starts))
        {
            TrackMask tracks = kEveryTrack;
            tracks[0] = children;
            rules.start_states[0x01000000u | piece].tracks = tracks;
        }

        // The multi BGMs (MBGM_*, section 1, group 1) play a sequence and a stream together. AudBgmMgr::start
        // (0x8002fd04 in Galaxy's main.dol, 0x80082e00 in Galaxy 2's) hands an ID with bit 16 to AudMultiBgm. Galaxy
        // clears bits 16 and 24 first (0x8002fe1c). AudMultiBgm::prepare (0x8002f41c, 0x80082220) looks the ID up:
        // getSeqIdForMultiBgm (0x80030798, 0x80083b80) gives the sequence, a BGM_* piece, and getStreamIdForMultiBgm
        // (0x800307ac, 0x80083bb0) the stream. Galaxy's table is AudBgmSetting::cMultiBgmSet (0x8052ee10). Galaxy 2
        // reads AudioRes/Info/MultiBgmInfo.arc. prepare starts the piece with JAISeqMgr::startSound (0x8002f480,
        // 0x80082284). The game never plays the entry of a multi BGM as a sequence. Every such entry points at one
        // stand-in sequence: 62 entries in Galaxy, 76 in Galaxy 2.
        rules.unplayed_groups.insert(0x0101);

        // At start-up, AudEffector::initFxLine (0x80030ae0 in Galaxy's main.dol) passes the four FX lines in the
        // table at 0x80551b58 to JASDsp::setFXLine (0x804957b4). Galaxy 2 does the same at 0x80084254 with the table at
        // 0x8066b930 (setFXLine at 0x80592110). A line delays an effect send and passes it through a filter of eight
        // taps. The filter's output feeds back into the line and goes to the mix. Galaxy's lines delay by 64
        // sub-frames. Their filter has two taps, 0x1000 and 0x3fff. Each line goes to one side at 0x7000. Galaxy 2's
        // lines 0 and 1 delay by 69 sub-frames, and lines 2 and 3 by 100 and 85. Their filter's taps are 0x0625,
        // -0x0745, 0x0625, -0x0626, -0x0508, 0x1808, 0x28cd and -0x0f18. Lines 0 and 1 go to the two sides at 0x7fff,
        // and to two other buses at 0x30a1. The performer plays lines 0 and 1 as the left and the right line. It keeps
        // one gain for a filter: the filter's largest gain over the frequencies. That gain is 0.625 for Galaxy's, at
        // 0 Hz, and 0.514 for Galaxy 2's, at 10 kHz. No frequency of the game's echo then falls silent later than the
        // performer's echo. JASDriver::getChannelLevel_dsp (0x80495c9c, 0x80592680) gives a voice at a volume of 1 a
        // mixer volume of 12000 in both games.
        rules.echo = archive_sha1 == kGalaxy
                         ? EchoLine{64, 0x4fff / 32768.0f, 0x7000 / 32768.0f, 2.0f * 12000.0f / 32768.0f}
                         : EchoLine{69, 0.5135f, 0x7fff / 32768.0f, 2.0f * 12000.0f / 32768.0f};

        // When the player plays as Luigi, AudSceneMgr::setPlayerModeLuigi (0x80031fc8 in Galaxy's main.dol, 0x800858d0
        // in Galaxy 2's) sets the scene manager's player mode to 2. Once AudSystem::startSound (0x80026d54, 0x80078cc0)
        // has started a sound, it calls AudSystem::switchToLuigi (0x80027024, 0x80079000). In that mode, for a sound of
        // group 1, the function writes 20 to port 0xc of the sound's track (JASTrack::writePort). Group 1 of section 0
        // holds the player's voices (SE_PV_*). A voice that reads the port plays Luigi's waves. The scene manager loads
        // them in place of Mario's in that mode (AudSceneMgr::loadPlayerResource, 0x80031fd4 in Galaxy).
        // AudSystem::startLevelSound doesn't call switchToLuigi.
        rules.port_variants.push_back({.group = 0x0001, .suffix = "_Luigi", .ports = {{0xc, 20}}});
    }

    if (archive_sha1 == kGalaxy2)
    {
        // At start-up, AudSceneMgr::loadStaticResource (0x80085230) reads AudioRes/Info/BgmParam.arc (0x80089480). For
        // each row of BgmParam.bcsv, the reader finds the sound by the name in SoundIdName. It writes the low byte of
        // the row's Volume over the volume in the sound's entry of the sound table (+1), if the sound's type has bit
        // 0x40 (0x800895f0). 153 of the 177 rows change a piece's volume.
        rules.volume_table = GameRules::VolumeTable{"Info/BgmParam.arc", "BgmParam.bcsv", "SoundIdName", "Volume"};
    }

    return rules;
}

Game Game::FromFolder(const fs::path& baa, const std::string& program, const GameOptions& options)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(baa, bytes))
    {
        throw FormatError("can't read it");
    }

    Game game;
    game.options_ = options;
    game.OpenArchive(std::move(bytes));

    // The files in `where` with the extension `extension`, in the order of their names. AppleDouble files don't count.
    // macOS writes one, named "._" and the file's name, beside a file that it copies to a FAT or exFAT drive.
    const auto files_in = [](const fs::path& where, const std::string& extension)
    {
        std::vector<fs::path> found;
        std::error_code error;
        for (const fs::directory_entry& e : fs::directory_iterator(where, error))
        {
            if (e.is_regular_file(error) && AsciiLower(Utf8(e.path().extension())) == extension &&
                !Utf8(e.path().filename()).starts_with("._"))
            {
                found.push_back(e.path());
            }
        }
        std::sort(found.begin(), found.end());

        return found;
    };

    // The sequence archive: the first .arc in the Seqs folder, or beside the start-up archive. A JAudio1 game without
    // one can keep its sequences in a table (.bdt).
    const fs::path folder = baa.parent_path().empty() ? fs::path(".") : baa.parent_path();
    if (game.rules_.volume_table)
    {
        std::optional<std::vector<uint8_t>> table;
        if (const std::optional<fs::path> path = FindCaseless(folder, game.rules_.volume_table->archive))
        {
            table.emplace();
            if (!xsflib::ReadFile(*path, *table))
            {
                table.reset();
            }
        }
        game.OpenVolumeTable(table);
    }
    std::vector<fs::path> archives;
    for (const std::string extension : {".arc", ".bdt"})
    {
        if (extension == ".bdt" && !game.jaudio1_)
        {
            break;
        }

        for (const fs::path& where : {Subfolder(folder, "Seqs").value_or(folder), folder})
        {
            archives = files_in(where, extension);
            if (!archives.empty())
            {
                break;
            }
        }
        if (!archives.empty())
        {
            game.sequence_table_ = extension == ".bdt";
            break;
        }
    }
    if (!archives.empty())
    {
        game.sequence_archive_name_ = Utf8(archives.front().filename());
        // A sequence archive that can't be read stays empty.
        xsflib::ReadFile(archives.front(), game.sequence_archive_);
        if (archives.size() > 1)
        {
            game.warnings_.push_back("several sequence archives: " + program + " reads " + game.sequence_archive_name_);
        }
    }

    // The wave archives' folder, Waves (JAudio2) or Banks (JAudio1), or the other one if the game has no such folder.
    std::optional<fs::path> waves = Subfolder(folder, game.jaudio1_ ? "Banks" : "Waves");
    if (!waves)
    {
        waves = Subfolder(folder, game.jaudio1_ ? "Waves" : "Banks");
    }
    if (waves)
    {
        game.wave_folders_.push_back(*waves);
    }
    game.wave_folders_.push_back(folder);
    if (!game.rules_.conductors.empty())
    {
        for (const fs::path& arc : files_in(folder, ".arc"))
        {
            if (AsciiLower(Utf8(arc.filename())) == "conductor.arc")
            {
                xsflib::ReadFile(arc, game.conductor_archive_);
            }
        }
    }
    if (game.jaudio1_)
    {
        const std::vector<fs::path> name_files = files_in(folder, ".asn");
        std::vector<uint8_t> names;
        if (!name_files.empty() && xsflib::ReadFile(name_files.front(), names))
        {
            game.ReadNames(names, Utf8(name_files.front().filename()));
        }
    }
    game.FindSequences();

    return game;
}

std::optional<Game> Game::FromDiscFiles(std::shared_ptr<ntdlib::disc::GameFiles> disc, const std::string& program,
                                        const GameOptions& options)
{
    Game game;
    game.options_ = options;
    game.disc_ = std::move(disc);

    // The disc's files in the order of its file table.
    std::vector<const ntdlib::disc::GameFile*> files;
    for (const ntdlib::disc::GameFile& f : game.disc_->files)
    {
        files.push_back(&f);
    }
    std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->index < b->index; });

    // The start-up archive: a .baa file, with its Seqs and Waves folders beside it, or JAudio1's .aaf file, with its
    // Seqs and Banks folders.
    const ntdlib::disc::GameFile* baa = nullptr;
    for (const ntdlib::disc::GameFile* f : files)
    {
        if (AsciiLower(f->path).ends_with(".baa") || AsciiLower(f->path).ends_with(".aaf"))
        {
            if (baa != nullptr)
            {
                game.warnings_.push_back("several start-up archives on the disc: " + program + " reads " + baa->path);
                break;
            }
            baa = f;
        }
    }

    std::vector<uint8_t> archive;
    std::string folder; // the folder of the Seqs and Banks or Waves folders, in lowercase, ending in '/'
    if (baa != nullptr)
    {
        archive = game.ReadDiscFile(baa->path);
        game.disc_path_ = baa->path;
        folder = AsciiLower(baa->path.substr(0, baa->path.rfind('/') + 1));
    }
    else if (std::optional<ContainedArchive> found =
                 FindContainedArchive(files, [&game](const std::string& path) { return game.ReadDiscFile(path); }))
    {
        archive = std::move(found->archive);
        game.disc_path_ = found->path;
        folder = std::move(found->folder);
    }
    else if (std::optional<BankFileGame> pikmin = FindBankFileGame(files, game.disc_->dol))
    {
        game.disc_path_ = pikmin->bank_file->path;
        game.OpenBankFile(game.ReadDiscFile(pikmin->bank_file->path));
        game.sequence_index_ = std::move(pikmin->index);
        game.sequence_archive_ = game.ReadDiscFile(pikmin->sequences->path);
        game.sequence_archive_name_ = pikmin->sequences->path.substr(pikmin->sequences->path.rfind('/') + 1);
        folder = std::move(pikmin->folder);
    }
    else
    {
        return std::nullopt;
    }

    if (!game.archive_)
    {
        game.OpenArchive(std::move(archive));
    }

    // The wave archives are in the Waves folder (JAudio2) or the Banks folder (JAudio1), or in the other folder if that
    // one holds none. Donkey Kong Jungle Beat, a JAudio2 game, keeps them in Banks.
    std::map<std::string, std::string> other_waves;
    const std::string waves = game.jaudio1_ ? "banks/" : "waves/";
    const std::string other = game.jaudio1_ ? "waves/" : "banks/";
    for (const ntdlib::disc::GameFile* f : files)
    {
        const std::string path = AsciiLower(f->path);
        if (!path.starts_with(folder))
        {
            continue;
        }

        const std::string below = path.substr(folder.size());
        if (below.starts_with("seqs/") && (below.ends_with(".arc") || (game.jaudio1_ && below.ends_with(".bdt"))) &&
            game.sequence_archive_.empty())
        {
            game.sequence_archive_ = game.ReadDiscFile(f->path);
            game.sequence_archive_name_ = f->path.substr(f->path.rfind('/') + 1);
            game.sequence_table_ = below.ends_with(".bdt");
        }
        else if (below.starts_with(waves) && below.ends_with(".aw"))
        {
            game.disc_waves_.emplace(below.substr(waves.size()), f->path);
        }
        else if (below.starts_with(other) && below.ends_with(".aw"))
        {
            other_waves.emplace(below.substr(other.size()), f->path);
        }
        else if (below == "conductor.arc" && !game.rules_.conductors.empty())
        {
            game.conductor_archive_ = game.ReadDiscFile(f->path);
        }
        else if (game.jaudio1_ && below.find('/') == std::string::npos && below.ends_with(".asn") &&
                 game.names_.empty())
        {
            game.ReadNames(game.ReadDiscFile(f->path), f->path.substr(f->path.rfind('/') + 1));
        }
    }
    if (game.disc_waves_.empty())
    {
        game.disc_waves_ = std::move(other_waves);
    }
    if (game.rules_.volume_table)
    {
        const std::string table_path = folder + AsciiLower(game.rules_.volume_table->archive);
        const auto table = std::ranges::find_if(
            files, [&](const ntdlib::disc::GameFile* f) { return AsciiLower(f->path) == table_path; });
        std::optional<std::vector<uint8_t>> table_archive;
        if (table != files.end())
        {
            table_archive = game.ReadDiscFile((*table)->path);
        }
        game.OpenVolumeTable(table_archive);
    }

    game.FindSequences();

    return game;
}

void Game::OpenArchive(std::vector<uint8_t> bytes)
{
    // Donkey Kong Jungle Beat compresses its start-up archive with Yaz0. The game decodes it as it loads it.
    if (IsYaz0(bytes))
    {
        bytes = DecodeYaz0(bytes);
    }

    archive_sha1_ = xsflib::Sha1::Hex(xsflib::Sha1::Of(bytes));
    rules_ = RulesOf(archive_sha1_, options_);

    if (!ntdlib::jaudio::IsAaf(bytes))
    {
        ntdlib::jaudio::BaaOptions layouts;
        layouts.first_layouts = rules_.first_layouts;
        archive_ = std::make_unique<SoundArchive>(SoundArchive::Parse(std::move(bytes), layouts));
        return;
    }

    // JASystem::TOscillator::release gives an oscillator with neither a release table nor a direct release a direct
    // release of 0x10.
    jaudio1_ = true;
    aaf_ = bytes;
    rules_.default_release = 0x10;
    effects_file_ = ntdlib::jaudio::AafEffectsSequenceFile(bytes);
    effect_slot_counts_ = ntdlib::jaudio::AafEffectSlotCounts(bytes);
    archive_ = std::make_unique<SoundArchive>(SoundArchive::ParseAaf(std::move(bytes)));
}

void Game::OpenBankFile(std::vector<uint8_t> bytes)
{
    archive_sha1_ = xsflib::Sha1::Hex(xsflib::Sha1::Of(bytes));
    rules_ = RulesOf(archive_sha1_);
    jaudio1_ = true;
    jac_ = true;
    rules_.default_release = 0x10;
    archive_ = std::make_unique<SoundArchive>(SoundArchive::ParseBx(std::move(bytes)));
}

void Game::ReadNames(std::span<const uint8_t> names, const std::string& file)
{
    try
    {
        // The sound library reads only the type and the index of a sequence's ID. Super Mario Sunshine's and Pikmin
        // 2's name files also set category 16 in the ID of each piece of music. They give 0x80010001 for the sound
        // table's 0x80000001. A record names the sequence of its type and index. An effect's record names the effect
        // of its ID. A game whose name file doesn't name its sequences keeps the files' names
        // (GameRules::sequence_names).
        for (ntdlib::jaudio::AsnSound& sound : ntdlib::jaudio::ReadAsnSounds(names))
        {
            const uint32_t type = sound.id & ntdlib::jaudio::kAafTypeMask;
            if (type == ntdlib::jaudio::kAafSequence && !rules_.sequence_names)
            {
                continue;
            }
            const uint32_t id =
                type == ntdlib::jaudio::kAafEffect ? sound.id : type | (sound.id & ntdlib::jaudio::kAafIndexMask);
            names_.emplace(id, std::move(sound.name));
        }
    }
    catch (const FormatError& e)
    {
        warnings_.push_back(file + ": " + e.what() + ": its names are left out");
    }
}

std::vector<uint8_t> Game::ReadDiscFile(const std::string& path) const
{
    std::vector<uint8_t> data;
    try
    {
        disc_->read_file(path, data);
    }
    catch (const ntdlib::disc::Error& e)
    {
        throw FormatError(e.what());
    }

    return data;
}

void Game::OpenVolumeTable(const std::optional<std::vector<uint8_t>>& archive)
{
    if (!rules_.volume_table)
    {
        return;
    }

    const std::string& path = rules_.volume_table->archive;
    if (!archive)
    {
        warnings_.push_back(path + " not found: the sound table's volumes stay");
        return;
    }

    try
    {
        table_volumes_ = ReadVolumeTable(*archive, *rules_.volume_table);
    }
    catch (const FormatError& e)
    {
        warnings_.push_back(path + ": " + e.what() + ": the sound table's volumes stay");
    }
}

void Game::FindSequences()
{
    // A game whose code reads no sequence archive plays none of the files of one on its disc.
    if (!rules_.sequence_archive)
    {
        sequence_archive_.clear();
    }
    else if (sequence_archive_.empty())
    {
        warnings_.push_back("no sequence archive found: its music is skipped");
    }
    else
    {
        try
        {
            // Twilight Princess compresses its sequence archive with Yaz0.
            if (IsYaz0(sequence_archive_))
            {
                sequence_archive_ = DecodeYaz0(sequence_archive_);
            }

            // An earlier JAudio1, such as Luigi's Mansion's, keeps its sequences in one file that the AAF's sequence
            // index divides.
            std::optional<std::vector<ArchiveFile>> indexed;
            if (!sequence_index_.empty())
            {
                indexed = ntdlib::jaudio::ReadBarcSequenceFiles(sequence_index_, sequence_archive_);
            }
            else if (jaudio1_)
            {
                indexed = ntdlib::jaudio::ReadAafSequenceFiles(aaf_, sequence_archive_);
            }

            if (indexed)
            {
                sequence_files_ = std::move(*indexed);
            }
            else
            {
                sequence_files_ = sequence_table_ ? ntdlib::jaudio::ReadSequenceTable(sequence_archive_)
                                                  : ReadRarc(sequence_archive_);
            }

            // Super Mario Galaxy and Super Mario Galaxy 2 compress each file of the sequence archive with Yaz0. The
            // archive's entries give each file the type 0x95: bit 0x04 marks it compressed and bit 0x80 marks the
            // compression as Yaz0. The game's memory archive decodes such a file as it fetches it (0x804109fc and
            // 0x80410b3c in Super Mario Galaxy's main.dol).
            for (ArchiveFile& f : sequence_files_)
            {
                if (IsYaz0(f.data))
                {
                    decoded_sequences_.push_back(DecodeYaz0(f.data));
                    f.data = decoded_sequences_.back();
                }
            }
        }
        catch (const FormatError& e)
        {
            warnings_.push_back(sequence_archive_name_ + ": " + e.what() + ": its music is skipped");
        }
    }

    // A game's rules can name its effects sequence's file.
    if (jaudio1_ && !rules_.effects_sequence.empty())
    {
        const auto named = std::ranges::find(sequence_files_, rules_.effects_sequence, &ArchiveFile::name);
        if (named != sequence_files_.end())
        {
            effects_file_ = named->id;
        }
    }

    // A conductor that can't be read leaves its pieces without it.
    std::vector<ArchiveFile> conductors;
    if (!conductor_archive_.empty())
    {
        try
        {
            conductors = ReadRarc(conductor_archive_);
        }
        catch (const FormatError& e)
        {
            warnings_.push_back(std::string("Conductor.arc: ") + e.what() + ": the cave music plays without it");
        }
    }

    // The rules that the game gives a file of its sequence archive, by the file's name: the volume, the answers to its
    // tracks' calls, the children that start muted and the conductor.
    const auto file_rules = [&](SequenceSound& seq, const ArchiveFile& file)
    {
        const auto volume = rules_.piece_volumes.find(file.name);
        if (volume != rules_.piece_volumes.end())
        {
            seq.volume = static_cast<float>(volume->second) / 127.0f;
        }
        const auto answers = rules_.sync_answers.find(file.name);
        if (answers != rules_.sync_answers.end())
        {
            seq.sync_answers = answers->second;
        }
        const auto muted = rules_.muted_children.find(file.name);
        if (muted != rules_.muted_children.end())
        {
            seq.muted_children = muted->second;
        }
        const auto conductor = rules_.conductors.find(file.name);
        if (conductor != rules_.conductors.end())
        {
            const auto cnd = std::ranges::find(conductors, conductor->second, &ArchiveFile::name);
            try
            {
                if (cnd == conductors.end())
                {
                    throw FormatError("Conductor.arc has no " + conductor->second);
                }
                seq.conductor = ReadConductor(cnd->data);
            }
            catch (const FormatError& e)
            {
                warnings_.push_back(seq.name + ": " + e.what() + ": the piece plays without its conductor");
            }
        }
    };

    // The sound table's sequences. A sound of the collection starts at its offset in the collection. A sound of the
    // sequence archive starts at the start of its file. A sound of a sequence block starts at the block's start.
    std::vector<bool> played(sequence_files_.size());
    std::map<uint16_t, uint8_t> unnamed; // the volume of the first JAudio1 sound that plays each file
    const auto effects = std::find_if(sequence_files_.begin(), sequence_files_.end(),
                                      [&](const ArchiveFile& f) { return jaudio1_ && f.id == effects_file_; });
    std::vector<SequenceSound> effect_sounds; // they come last, and the other sequences keep their places
    for (const SoundEntry& s : archive_->Sounds())
    {
        // A JAudio1 game's effects play in its effects sequence.
        const bool effect = jaudio1_ && (s.Id() & ntdlib::jaudio::kAafTypeMask) == ntdlib::jaudio::kAafEffect;
        if (effect && options_.jaudio1_effects && effects != sequence_files_.end())
        {
            SequenceSound seq = EffectSound(s, effects->data);
            const auto start = rules_.random_starts.find(s.Id());
            if (start != rules_.random_starts.end())
            {
                seq.draws = start->second;
            }

            const auto keys = rules_.random_keys.find(s.Id());
            if (keys == rules_.random_keys.end())
            {
                effect_sounds.push_back(std::move(seq));
                continue;
            }

            for (const GameRules::RandomKey& key : keys->second)
            {
                SequenceSound variant = seq;
                variant.name += "_key_" + std::to_string(key.key);
                variant.draws = key.draws;
                effect_sounds.push_back(std::move(variant));
            }
            continue;
        }

        // A stream plays an audio file from the disc, not a sequence. A JAudio1 game's effects play in its effects
        // sequence. Without GameOptions::jaudio1_effects, or without that sequence, they're left out here.
        if ((jaudio1_ && s.source != SoundEntry::Source::kArchive) || (!jaudio1_ && s.stream))
        {
            continue;
        }

        SequenceSound seq;
        seq.name = s.name.empty() ? "SOUND_" + std::to_string(s.section) + "_" + std::to_string(s.group) + "_" +
                                        std::to_string(s.index)
                                  : s.name;
        // The game's table of volumes (GameRules::volume_table) replaces the sound table's.
        const auto table_volume = table_volumes_.find(s.name);
        const bool from_table = table_volume != table_volumes_.end() && (s.type & 0x40) != 0;
        seq.volume =
            static_cast<float>(from_table ? table_volume->second : s.volume) / (rules_.first_layouts ? 255.0f : 127.0f);
        seq.default_release = rules_.default_release;
        seq.echo = rules_.echo;
        seq.jaudio1 = jaudio1_;
        seq.squared_track_volume = rules_.squared_track_volume;
        seq.timed_random = rules_.timed_random;
        seq.track_oscillator_routes = rules_.track_oscillator_routes;
        seq.callback = rules_.callback;
        seq.jac = jac_;
        seq.later_jaudio1 = rules_.later_jaudio1;
        seq.mute_child_15 = rules_.mute_child_15;
        seq.filter_rate = rules_.filter_rate;
        const auto start_state = rules_.start_states.find(s.Id());
        if (start_state != rules_.start_states.end())
        {
            SetState(seq, start_state->second);
        }
        if (rules_.rhythm_mutes_child_0 && (s.switches & 1) != 0)
        {
            seq.tracks[0] &= 0xfffe;
        }
        const auto ports = rules_.ports.find(s.Id());
        if (ports != rules_.ports.end())
        {
            seq.ports = ports->second;
        }
        if (s.source == SoundEntry::Source::kCollection)
        {
            const std::optional<uint32_t> start = archive_->CollectionSequence(s.group, s.resource);
            if (!start)
            {
                continue;
            }

            seq.data = archive_->Collection();
            seq.start = *start;

            // The collection's sounds are effects. Each plays at the volume of its category too.
            const auto category = rules_.category_volumes.find(s.group);
            if (category != rules_.category_volumes.end())
            {
                seq.volume *= category->second;
            }
        }
        else if (s.source == SoundEntry::Source::kBlock)
        {
            const std::optional<std::span<const uint8_t>> block = archive_->SequenceBlock(s.resource);
            if (!block)
            {
                continue;
            }

            seq.data = *block;
            seq.start = 0;
            seq.volume *= rules_.sequence_volume;
        }
        else
        {
            const auto file = std::find_if(sequence_files_.begin(), sequence_files_.end(),
                                           [&](const ArchiveFile& f) { return f.id == s.resource; });
            if (file == sequence_files_.end())
            {
                continue;
            }

            played[static_cast<std::size_t>(file - sequence_files_.begin())] = true;
            seq.data = file->data;
            seq.start = 0;
            seq.volume *= rules_.sequence_volume;
            seq.zone_filters = true;

            // A JAudio1 sequence takes its name from the name file. Without one, it takes its file's name. A later
            // sound without a name that plays the same file at the same volume would give the same conversion. It's
            // left out. One at another volume keeps the name of its ID.
            if (jaudio1_)
            {
                const auto named = names_.find(s.Id());
                const auto [first, fresh] = unnamed.try_emplace(file->id, s.volume);
                if (named != names_.end())
                {
                    seq.name = named->second;
                }
                else if (!fresh && first->second == s.volume)
                {
                    continue;
                }
                else if (fresh && !file->name.empty())
                {
                    seq.name = file->name.substr(0, file->name.rfind('.'));
                }
            }
            file_rules(seq, *file);
        }
        // A sound that the game never starts as a sequence (GameRules::unplayed_groups) is left out. Its file still
        // counts as played.
        if (rules_.unplayed_groups.contains(static_cast<uint16_t>(s.Id() >> 16)))
        {
            continue;
        }
        if (std::ranges::find(rules_.only_arranged, s.Id()) == rules_.only_arranged.end())
        {
            sequences_.push_back(seq);
        }

        // The sequence's arrangements follow it.
        for (const Arrangement& arrangement : rules_.arrangements)
        {
            if (arrangement.sound == s.Id())
            {
                SequenceSound other = seq;
                other.name += "_" + arrangement.suffix;
                SetState(other, arrangement.state);
                sequences_.push_back(std::move(other));
            }
        }

        // Then its other values at ports (GameRules::port_variants).
        for (const GameRules::PortVariant& variant : rules_.port_variants)
        {
            if (variant.group == static_cast<uint16_t>(s.Id() >> 16))
            {
                SequenceSound again = seq;
                again.name += variant.suffix;
                for (const auto& [port, value] : variant.ports)
                {
                    again.ports[port] = value;
                    again.variant_ports = static_cast<uint16_t>(again.variant_ports | (1u << (port & 15)));
                }
                sequences_.push_back(std::move(again));
            }
        }
    }

    // The files of the sequence archive that no sound plays, but a JAudio1 game's effects sequences. A file of a
    // sequence table has no name. It takes its number.
    for (std::size_t i = 0; i < sequence_files_.size(); i++)
    {
        const ArchiveFile& file = sequence_files_[i];
        if (played[i] || file.data.empty() || (jaudio1_ && file.id == effects_file_) ||
            (rules_.jac_effects && file.name == rules_.jac_effects->events_sequence))
        {
            continue;
        }

        const std::string name =
            file.name.empty() ? "SEQUENCE_" + std::to_string(file.id) : file.name.substr(0, file.name.rfind('.'));
        SequenceSound seq{name, file.data, 0, 1.0f, rules_.default_release, rules_.echo};
        seq.jaudio1 = jaudio1_;
        seq.jac = jac_;
        seq.later_jaudio1 = rules_.later_jaudio1;
        seq.mute_child_15 = rules_.mute_child_15;
        seq.timed_random = rules_.timed_random;
        seq.track_oscillator_routes = rules_.track_oscillator_routes;
        seq.callback = rules_.callback;
        seq.zone_filters = true;
        seq.filter_rate = rules_.filter_rate;
        const auto volumes = rules_.child_volumes.find(file.name);
        if (volumes != rules_.child_volumes.end())
        {
            seq.child_volumes = volumes->second;
        }
        file_rules(seq, file);
        if (rules_.bank_of_number)
        {
            seq.bank = file.id;
        }
        const auto parts = rules_.parts.find(file.name);
        if (parts == rules_.parts.end())
        {
            sequences_.push_back(std::move(seq));
            continue;
        }

        for (const GameRules::Part& part : parts->second)
        {
            SequenceSound again = seq;
            again.name += part.suffix;
            again.ports = part.ports;
            sequences_.push_back(std::move(again));
        }
    }

    if (jac_ && options_.jaudio1_effects && rules_.jac_effects)
    {
        effect_sounds = JacEffectSounds();
    }
    std::ranges::move(effect_sounds, std::back_inserter(sequences_));

    for (SequenceSound& seq : sequences_)
    {
        seq.mix_level = rules_.mix_level;
    }
}

std::vector<SequenceSound> Game::JacEffectSounds()
{
    const GameRules::JacEffects& rules = *rules_.jac_effects;
    if (!disc_ || xsflib::Sha1::Hex(xsflib::Sha1::Of(disc_->dol)) != rules.dol_sha1)
    {
        warnings_.push_back("main.dol isn't the release that the effects' tables come from: the effects are left out");
        return {};
    }

    const auto file_named = [this](const std::string& name) -> const ArchiveFile*
    {
        const auto f = std::ranges::find(sequence_files_, name, &ArchiveFile::name);
        return f == sequence_files_.end() ? nullptr : &*f;
    };
    const ArchiveFile* effects = file_named(rules_.effects_sequence);
    const ArchiveFile* events = file_named(rules.events_sequence);
    if (!effects || !events)
    {
        warnings_.push_back("the effects' sequences are missing: the effects are left out");
        return {};
    }

    constexpr std::size_t kRow = 20;
    constexpr std::size_t kEventTypes = 8;
    constexpr std::size_t kAction = 6;
    constexpr int32_t kSystem = -2;
    constexpr int32_t kPlayer = -1;
    constexpr uint16_t kPlayerPort1 = 0x8000;
    const ntdlib::Dol dol = ntdlib::Dol::ParseMain(disc_->dol);
    const std::span<const uint8_t> rows = dol.At(rules.catalogue);
    const std::span<const uint8_t> offsets = dol.At(rules.event_offsets);
    if (rows.size() < rules.count * kRow || offsets.size() < kEventTypes * 4)
    {
        warnings_.push_back("main.dol's catalogue of effects is damaged: the effects are left out");
        return {};
    }
    const std::span<const uint8_t> actions = offsets.subspan(kEventTypes * 4);
    const auto u16 = [](std::span<const uint8_t> b, std::size_t at)
    {
        return static_cast<uint16_t>(b[at] << 8 | b[at + 1]);
    };
    const auto u32 = [](std::span<const uint8_t> b, std::size_t at)
    {
        return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 | b[at + 2] << 8 | b[at + 3];
    };
    const auto name_at = [&dol](uint32_t address)
    {
        const std::span<const uint8_t> text = dol.At(address);
        return std::string(text.begin(), std::ranges::find(text, uint8_t{0}));
    };

    std::vector<SequenceSound> sounds;
    std::set<std::tuple<bool, uint8_t, uint8_t, uint16_t>> listed; // the requests listed already
    const auto add = [&](std::string name, bool event, uint8_t track, uint8_t port, uint16_t number,
                         std::map<uint8_t, uint16_t> ports = {})
    {
        if (!listed.insert({event, track, port, number}).second)
        {
            return;
        }

        SequenceSound seq;
        seq.name = std::move(name);
        seq.data = event ? events->data : effects->data;
        seq.default_release = rules_.default_release;
        seq.jaudio1 = true;
        seq.jac = true;
        seq.effect = EffectStart{.slot = track, .number = number, .request = EffectRequest::kPort, .port = port};
        seq.ports = std::move(ports);
        sounds.push_back(std::move(seq));
    };
    const auto add_player = [&](std::string name, uint16_t number)
    {
        if (number >= kPlayerPort1)
        {
            add(std::move(name), false, rules.player_track, 1, static_cast<uint16_t>(number & ~kPlayerPort1));
        }
        else
        {
            add(std::move(name), false, rules.player_track, 0, number);
        }
    };

    for (uint32_t i = 0; i < rules.count; i++)
    {
        const std::span<const uint8_t> row = rows.subspan(i * kRow, kRow);
        const auto number = static_cast<uint16_t>(u32(row, 4));
        std::string name = name_at(u32(row, 8));
        const auto kind = static_cast<int32_t>(u32(row, 16));
        if (kind == kSystem)
        {
            if (std::ranges::find(rules.left_out, number) != rules.left_out.end())
            {
                continue;
            }

            const auto player = rules.played_by_player.find(number);
            if (player != rules.played_by_player.end())
            {
                add_player(std::move(name), player->second);
            }
            else
            {
                add(std::move(name), false, rules.system_track, 0, number);
            }
        }
        else if (kind == kPlayer)
        {
            add_player(std::move(name), number);
        }
        else if (kind > 0 && static_cast<std::size_t>(kind) < kEventTypes)
        {
            const std::size_t entry = u32(offsets, 4 * static_cast<std::size_t>(kind)) + std::size_t{number};
            if ((entry + 1) * kAction <= actions.size())
            {
                add(std::move(name), true, 0, 0, u16(actions, entry * kAction + 4));
            }
        }
    }

    for (const uint16_t number : rules.other_system)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "system-%x", number);
        add(name, false, rules.system_track, 0, number);
    }
    for (const uint16_t number : rules.other_player)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "player-%x", number);
        add_player(name, number);
    }
    for (const GameRules::PortEffect& e : rules.port_effects)
    {
        add(e.name, false, e.track, e.port, e.number, e.ports);
    }

    return sounds;
}

SequenceSound Game::EffectSound(const SoundEntry& s, std::span<const uint8_t> sequence) const
{
    // The first slot of the effect's category. The effects manager numbers the slots of the categories in turn, from
    // category 0 (JAInter::SeMgr::checkPlayingSe, 0x800aede4 in Pikmin 2's main.dol). The AAF's table of scenes gives
    // each category its count. Without the table, each category has four (JAInter::Const::sCInfos_0). A category past
    // the table's row starts after the row's last slot. Only the slot's track plays.
    constexpr uint32_t kSlotsPerCategory = 4;
    const uint32_t category = ntdlib::jaudio::AafCategory(s.Id());
    uint32_t first_slot = kSlotsPerCategory * category;
    if (effect_slot_counts_)
    {
        const std::span<const uint8_t> counts = *effect_slot_counts_;
        const auto before = counts.first(std::min<std::size_t>(category, counts.size()));
        first_slot = std::accumulate(before.begin(), before.end(), 0u);
    }

    const auto slot = static_cast<uint8_t>(first_slot);

    // The name file gives some effects their IDs without bits 10 and 11. The game keeps such an effect going by
    // starting it again (kAafToEnd).
    constexpr uint32_t kLengthBits = 0xc00;
    SequenceSound seq;
    auto named = names_.find(s.Id());
    if (named == names_.end())
    {
        named = names_.find(s.Id() & ~kLengthBits);
    }
    if (named != names_.end())
    {
        seq.name = named->second;
    }
    else
    {
        std::array<char, 16> name{};
        std::snprintf(name.data(), name.size(), "se%u-%04u", category, s.Id() & ntdlib::jaudio::kAafIndexMask);
        seq.name = name.data();
    }

    seq.data = sequence;
    seq.volume = rules_.effect_parameters ? static_cast<float>(s.volume) / 127.0f : 1.0f;
    const auto volume = rules_.category_volumes.find(static_cast<uint8_t>(category));
    if (volume != rules_.category_volumes.end())
    {
        seq.volume *= volume->second;
    }
    seq.pitch = rules_.effect_parameters ? s.pitch : 1.0f;
    seq.default_release = rules_.default_release;
    seq.echo = rules_.echo;
    seq.jaudio1 = true;
    seq.squared_track_volume = rules_.squared_track_volume;
    seq.timed_random = rules_.timed_random;
    seq.track_oscillator_routes = rules_.track_oscillator_routes;
    seq.jac = jac_;
    seq.mute_child_15 = rules_.mute_child_15;
    seq.tracks.fill(0);
    seq.tracks[slot >> 4] = static_cast<uint16_t>(1u << (slot & 15));
    seq.effect = EffectStart{.slot = slot, .number = static_cast<uint16_t>(s.resource)};

    return seq;
}

std::optional<std::vector<uint8_t>> Game::ReadWaveArchive(const std::string& name) const
{
    if (disc_)
    {
        const auto f = disc_waves_.find(AsciiLower(name));
        if (f == disc_waves_.end())
        {
            return std::nullopt;
        }

        return ReadDiscFile(f->second);
    }

    std::error_code error;
    for (const fs::path& folder : wave_folders_)
    {
        for (const fs::directory_entry& e : fs::directory_iterator(folder, error))
        {
            if (e.is_regular_file(error) && AsciiLower(Utf8(e.path().filename())) == AsciiLower(name))
            {
                std::vector<uint8_t> data;
                return xsflib::ReadFile(e.path(), data) ? std::optional(std::move(data)) : std::nullopt;
            }
        }
    }

    return std::nullopt;
}

} // namespace ntdlib::sequence_export::jaudio
