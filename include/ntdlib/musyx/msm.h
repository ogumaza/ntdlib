// SPDX-License-Identifier: MIT

// Hudson's sound manager over MusyX, in Mario Party 4. The sound manager reads one file of sound data. That file holds
// every MusyX group and every song. A group's data holds its pool, its project, its sample directory and the SNG files
// of its songs. The group's samples are in another part of the file. The sound manager's table of songs gives each
// song's group and volume. Its table of effects gives each effect's group and the settings that msmSePlay passes to
// MusyX.
//
// The game's sound test lists the songs and the streams by their numbers. Each entry also gives the ID of the sound's
// name in the game's messages. The game's list of voices has the same form and names some of the effects.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::musyx
{

// A song of the sound manager's table: the MusyX group that plays it, the song's ID in that group's MIDI setups, its
// volume in 127ths, and its SNG file.
struct MsmSong
{
    uint16_t group_id = 0;
    uint16_t song_id = 0;
    uint8_t volume = 127;
    std::span<const uint8_t> data;
};

// An effect of the sound manager's table of effects: its number in the table, the MusyX group that holds it, its ID in
// that group's table of effects, and the sound manager's settings for it. msmSePlay passes the settings to MusyX as
// the effect's controllers: the volume (CC7), the pan (CC10), the sends (CC91 and CC93) and the pitch wheel's offset
// from its centre.
struct MsmEffect
{
    uint32_t number = 0;
    uint16_t group_id = 0;
    uint16_t id = 0;
    uint8_t volume = 127;
    uint8_t pan = 64;
    int16_t pitch = 0;
    uint8_t reverb = 0;
    uint8_t chorus = 0;
};

// The four parts of a group. The pool, the project and the sample directory each run from the part's start to the end
// of the group's data. Their readers find each part's end in the part.
struct MsmGroup
{
    std::span<const uint8_t> pool;
    std::span<const uint8_t> project;
    std::span<const uint8_t> directory;
    std::span<const uint8_t> samples;
};

// The sound manager's file, read in place. `file` must outlive the object.
class MsmFile
{
public:
    // Reads the header of `file`. `path` is the file's path on the disc. The errors name it. Throws FormatError if the
    // file is too short to hold a header.
    MsmFile(std::span<const uint8_t> file, std::string path);

    // Song `number` of the table. Throws FormatError if the table has no such song or the song's data is outside the
    // file.
    MsmSong Song(uint32_t number) const;

    // The group whose MusyX ID is `id`. Throws FormatError if the file has no such group or the group's parts are
    // outside the file.
    MsmGroup Group(uint16_t id) const;

    // The place in the table of the first group whose MusyX ID is `id`, or nothing if the table has none. The table's
    // first entry holds no group. Throws FormatError if the table is outside the file.
    std::optional<std::size_t> GroupIndex(uint16_t id) const;

    // The effects of the table of effects, in its order. An entry whose ID is 0xffff holds no effect. The list leaves
    // it out. Throws FormatError if the table is outside the file.
    std::vector<MsmEffect> Effects() const;

    // The places in the table of the base groups, in the sound manager's order. The sound manager loads them first and
    // keeps them loaded. Throws FormatError if the settings are outside the file.
    std::vector<std::size_t> BaseGroupIndexes() const;

    // The base groups, in the sound manager's order. Throws FormatError if the settings name a group that the table
    // doesn't have, or a group's parts are outside the file.
    std::vector<MsmGroup> BaseGroups() const;

private:
    // A reader of the file. A read past the file's end throws FormatError with `cut_short_`.
    xsflib::Reader Reader() const;

    // The number of groups in the table.
    std::size_t GroupCount() const;

    // The parts of the table's group `index`. Throws FormatError if the table has no such group or the group's parts
    // are outside the file.
    MsmGroup PartsOf(std::size_t index) const;

    // The offset in the file of the data of the table's group `index`. Throws FormatError if the table has no such
    // group.
    std::size_t GroupData(std::size_t index) const;

    std::span<const uint8_t> file_;
    std::string path_;
    std::string cut_short_;
};

// The kinds of sound in the sound test that aren't songs: a stream and a sound effect.
inline constexpr uint16_t kSoundTestStream = 1;
inline constexpr uint16_t kSoundTestEffect = 28;

// An entry of the sound test: the sound's kind, the group set that the sound test loads for it, its number (in the
// sound manager's table of songs for a song), and its name.
struct SoundTestEntry
{
    uint16_t kind = 0;
    uint16_t group_set = 0;
    uint32_t number = 0;
    std::string name;
};

// The text of message `id` of the game's message file `file`. `path` is the file's path on the disc. The ID holds the
// bank in its top 16 bits and the message's number in the bank in its low 16 bits. With `japanese`, the message takes
// the glyphs of the game's Japanese font, and otherwise those of the English font. The text is UTF-8, with no spaces at
// either end. Throws FormatError if the file has no such message or the message runs past the file's end.
std::string MessageText(std::span<const uint8_t> file, uint32_t id, const std::string& path, bool japanese);

// The songs and the streams that the sound test lists, in its order. `module` is the file of the code module that
// holds the sound test: `count` entries from offset `offset`. `module_path` is that file's path on the disc. The
// game's message file `messages` holds the names. `messages_path` is its path. With `japanese`, the messages take the
// glyphs of the game's Japanese font. An unused entry and an effect's entry are left out. Throws FormatError if an
// entry or a name is outside its file.
std::vector<SoundTestEntry> ReadSoundTest(std::span<const uint8_t> module, const std::string& module_path,
                                          uint32_t offset, uint32_t count, std::span<const uint8_t> messages,
                                          const std::string& messages_path, bool japanese);

// The names of the effects that a list in the form of the sound test names, by the effects' numbers in the sound
// manager's table of effects. The game's list of voices is one. Of two entries for one effect, the later counts. The
// arguments are those of ReadSoundTest. Throws FormatError if an entry or a name is outside its file.
std::map<uint32_t, std::string> ReadSoundTestEffects(std::span<const uint8_t> module, const std::string& module_path,
                                                     uint32_t offset, uint32_t count, std::span<const uint8_t> messages,
                                                     const std::string& messages_path, bool japanese);

} // namespace ntdlib::musyx
