// SPDX-License-Identifier: MIT

// The catalogues of the MusyX games whose code keeps its sounds in layouts of its own. A catalogue holds the sets of
// MusyX's four files that hold a game's groups, the songs' SNG files, and the game's sounds. For each sound it gives
// what MusyX plays, the groups that the game pushes before it plays the sound, and the settings that the game's code
// gives the sound. ReadCatalogue reads a game's catalogue from the game's files and code. CatalogueTables gives the
// places of the code's tables. They differ between versions of a game. The source of each layout's reader says what the
// game's code does.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"

namespace ntdlib::musyx
{

// MusyX's four files for one or more groups: the project, the pool, the sample directory and the samples.
struct FileSet
{
    std::vector<uint8_t> project, pool, sdir, samples;
};

// A group that a game pushes before it plays a sound: the group's MusyX ID and the number of the file set that holds
// it.
struct PushedGroup
{
    uint16_t id = 0;
    std::size_t set = 0;
};

// A MIDI controller that a game sets once a sound plays. The game sets an effect's controller through sndFXCtrl, or
// with `fine` through sndFXCtrl14. That one takes a 14-bit value, such as the pitch wheel's. The game sets a song's
// controller through sndSeqCntrl, on the song's MIDI channel `channel`. MusyX numbers the pitch wheel 0x80 and the
// surround pan 0x83. A pitch wheel's value is 0x2000 plus the game's bend. The sum can fall outside 14 bits.
struct Control
{
    uint8_t controller = 0;
    bool fine = false;
    int32_t value = 0;
    uint8_t channel = 0;
};

// The settings of MusyX's high-quality reverb (SND_AUX_REVERBHI) in the order of its block: the coloration, the mix,
// the time, the damping, the pre-delay and the crosstalk.
struct ReverbSettings
{
    float coloration = 0, mix = 0, time = 0, damping = 0, pre_delay = 0, crosstalk = 0;
};

// The settings of MusyX's chorus (SND_AUX_CHORUS): its base delay, its variation and its period, in ms.
using ChorusSettings = std::array<uint32_t, 3>;

// The most voices that MusyX plays at once for music and for effects (sndSetMaxVoices).
struct VoiceLimits
{
    uint32_t music = 0;
    uint32_t effects = 0;
};

// How a module of a game's code sets up studio 0 and MusyX's voices for its sounds. Aux A takes the game's
// high-quality reverb, or with `standard_reverb` the game's standard reverb (SND_AUX_REVERBSTD). Aux B takes a chorus
// with `chorus`, or no effect. With `voices`, the module limits MusyX's voices.
struct Module
{
    bool standard_reverb = false;
    std::optional<ChorusSettings> chorus;
    std::optional<VoiceLimits> voices;
};

// The kinds of sound: a song that sndSeqPlayEx plays, or an effect that sndFXStartEx starts.
enum class SoundKind : uint8_t
{
    kSong,
    kEffect,
};

// The number of the song of a sound that plays no song.
inline constexpr std::size_t kNoSong = static_cast<std::size_t>(-1);

// A sound of a catalogue: its name, what MusyX plays, the groups that the game pushes first, in the game's order, and
// the settings that the game gives the sound.
struct Sound
{
    std::string name;
    SoundKind kind = SoundKind::kSong;
    uint16_t id = 0;            // MusyX's song ID or effect ID
    uint16_t song_group = 0;    // a song's group, which sndSeqPlayEx takes
    std::size_t song = kNoSong; // a song's SNG file: its index in Catalogue::songs
    std::vector<PushedGroup> groups;

    // The volume and the pan that the game starts the sound at. Without a volume, a song plays at MusyX's default. An
    // effect then takes the volume or the pan of its row in its group's table of effects.
    std::optional<uint8_t> volume;
    std::optional<uint8_t> pan;

    // The controllers that the game sets once the sound plays, in the game's order.
    std::vector<Control> controls;

    // The time over which the game raises a song's volume from 0 to `volume` as it starts the song, in ms. MusyX's
    // fader moves the volume (sndSeqVolume).
    uint32_t fade_in_ms = 0;

    // An effect that the game plays as its music, and a song that the game plays as a sound effect.
    bool music = false;
    bool sound_effect = false;

    // The game's pick of a reverb for aux A while the sound plays: the game's other reverb in place of the usual one,
    // or the settings that the game gives the usual one for this sound.
    bool other_reverb = false;
    std::optional<ReverbSettings> reverb;

    // The volume that the game gives every group after its start-up's sndVolume(127, 0, 0xFF), if it gives one. The
    // master volume that the game gives the sound's kind (sndMasterVolume), if it gives one.
    std::optional<uint8_t> group_volume;
    std::optional<uint8_t> master_volume;

    // For a game that loads its sounds with modules of code, the module that plays the sound: its index in
    // Catalogue::modules.
    std::size_t module = 0;
};

// A game's sounds, gathered from its files: the file sets, the songs' SNG files, the sounds, and the modules of a game
// whose modules of code play its sounds.
struct Catalogue
{
    std::vector<FileSet> sets;
    std::vector<std::vector<uint8_t>> songs;
    std::vector<Sound> sounds;
    std::vector<Module> modules;
};

// The layouts of the games' sounds.
enum class CatalogueLayout : uint8_t
{
    kRetro,            // Metroid Prime and Metroid Prime 2: Echoes: packs of resources
    kTales,            // Tales of Symphonia: a sound file for each group, in S/
    kStarFox,          // Star Fox Adventures: the songs in audio/midi.wad
    kRogueLeader,      // Star Wars Rogue Squadron II: Rogue Leader: banks in a packed audio/data
    kRebelStrike,      // Star Wars Rogue Squadron III: Rebel Strike: the same banks, for its effects
    kColosseum,        // Pokémon Colosseum: FSYS archives and a module's table of sounds
    kResidentEvil,     // Resident Evil: a song and its group in each file of bgm/
    kResidentEvilZero, // Resident Evil Zero: archives in bio0/sound/ and the cutscenes' archives
    kPaperMario,       // Paper Mario: The Thousand-Year Door: one project and a song library
    kPokemonChannel,   // Pokémon Channel: one project, whose effects play the music
    kPuzzleCollection, // Nintendo Puzzle Collection: modules of code that hold the sounds
    kNdDemo,           // the NDDEMO tech demo: a table of the songs' paths
};

// The places of a game's tables. They differ between versions of a game. Each layout's reader names the fields that it
// reads. An address is a load address in main.dol, unless a reader says otherwise.
struct CatalogueTables
{
    uint32_t songs = 0;      // the table of songs
    uint32_t song_count = 0; // its entries, for a table without an end marker
    uint32_t effects = 0;    // the table of effects or of sounds
    uint32_t effect_count = 0;
    uint32_t embedded_dol = 0; // a second DOL in a main.dol that loads it
    uint32_t song_reverbs = 0; // a byte for each song ID that picks aux A's reverb, or 0 for none
    uint32_t file_table = 0;   // a table of the songs' files
    uint32_t file_count = 0;
    uint32_t reverb_presets = 0; // a table of the reverb's presets
    uint32_t viewer_names = 0;   // the offsets of a sound viewer's names and of its table of effects in its script
    uint32_t viewer_effects = 0;

    // Songs that the game plays at a volume that no table gives, each with its group in the top half of the key and
    // its ID in the low half.
    std::span<const std::pair<uint32_t, uint8_t>> song_volumes = {};
};

// The catalogue of the game whose files `game` holds, in `layout`, with its tables at `tables`. Throws FormatError if a
// file is missing or damaged, or if a table is outside the game's code.
Catalogue ReadCatalogue(CatalogueLayout layout, const disc::GameFiles& game, const CatalogueTables& tables);

// The name of an effect that nothing names: `prefix`, a hyphen and the effect's ID in four digits, such as
// "snd_se-0857".
std::string EffectName(const std::string& prefix, uint16_t id);

// A sound that a module's table of sounds lists (Pokémon Colosseum's common_rel): the entry's index in the table, the
// sound's kind, the MusyX ID of its group and its song ID or effect ID. A song's entry also gives the reverb's preset
// and the FSYS ID of the song's file.
struct TableSound
{
    uint32_t sound = 0;
    SoundKind kind = SoundKind::kSong;
    uint16_t group = 0;
    uint16_t id = 0;
    uint8_t reverb = 0;
    uint32_t file = 0;
};

// The sounds that the table of `sound_count` sounds at offset `sounds` of `module` lists, in the table's order. The
// table of `file_count` files at offset `files` gives the songs' files. A sound's entry has 12 bytes. Its first byte
// has bit 7 set for an effect. The entry gives the sound's group at +1 and its ID in a big-endian half at +4. A song's
// entry gives its reverb's preset at +2 and its file's number in the table of files at +3. A file's entry has 8 bytes.
// Its first word is the file's FSYS ID. An ID of 0 names no file, and the list leaves out a song with such a file.
// `name` names the module in the errors. Throws FormatError if a table runs past the module's end, or if a song names
// a file past the table of files.
std::vector<TableSound> ReadSoundTable(std::span<const uint8_t> module, const std::string& name, uint32_t sounds,
                                       uint32_t sound_count, uint32_t files, uint32_t file_count);

// The paths that a table in `dol` lists: `count` words at `address`, each the address of a path. Throws FormatError if
// `dol` has no data where a word or a path should be.
std::vector<std::string> ReadSongPaths(const Dol& dol, uint32_t address, uint32_t count);

} // namespace ntdlib::musyx
