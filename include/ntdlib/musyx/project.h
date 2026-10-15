// SPDX-License-Identifier: MIT

// A MusyX project file: the groups of a game's sound data. A group names the objects that it loads from the pool and
// the samples that it loads from the sample directory. A song group maps MIDI programs to objects, and gives each song
// its channels' first settings. An effects group lists its sound effects. The layouts are those of MusyX 2 on the
// GameCube. The files are big-endian there.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::musyx
{

// What an object ID names. Bit 15 marks a layer and bit 14 a keymap. An ID with neither names a SoundMacro.
enum class ObjectKind
{
    kMacro,
    kKeymap,
    kLayer,
};

// Returns the kind of object that `id` names.
ObjectKind KindOf(uint16_t id);

// A MIDI program of a song group (a page): the object that plays its notes, the voices' priority and the most voices
// that the program plays at once.
struct Page
{
    uint16_t object = 0;
    uint8_t priority = 0;
    uint8_t max_voices = 0;
    uint8_t program = 0;
};

// The settings that a song gives a MIDI channel before it starts.
struct ChannelSetup
{
    uint8_t program = 0;
    uint8_t volume = 127; // CC7
    uint8_t pan = 64;     // CC10
    uint8_t reverb = 0;   // CC91
    uint8_t chorus = 0;   // CC93
};

// The settings of the 16 channels for one song. A song names its setup by the song's ID.
struct MidiSetup
{
    uint16_t song = 0;
    std::array<ChannelSetup, 16> channels{};
};

// A sound effect of an effects group. It plays one note of `object` at `key`, with `volume` as the note's velocity.
struct Effect
{
    uint16_t id = 0;
    uint16_t object = 0;
    uint8_t max_voices = 0;
    uint8_t priority = 0;
    uint8_t volume = 127;
    uint8_t pan = 64;
    uint8_t key = 60;
    uint8_t volume_group = 0;
};

// The kinds of group.
enum class GroupType : uint16_t
{
    kSongs = 0,
    kEffects = 1,
};

// A group of a project file.
struct Group
{
    uint16_t id = 0;
    GroupType type = GroupType::kSongs;

    // The IDs of the objects that the group loads. A keymap's ID includes bit 14, and a layer's includes bit 15. The
    // pool's IDs and the pages' objects include them too.
    std::vector<uint16_t> macros;
    std::vector<uint16_t> samples;
    std::vector<uint16_t> tables;
    std::vector<uint16_t> keymaps;
    std::vector<uint16_t> layers;

    // A song group's programs. Channel 10 plays the drum programs and the other channels play the pages. The MIDI
    // setups give each song's channels their first settings.
    std::vector<Page> pages;
    std::vector<Page> drum_pages;
    std::vector<MidiSetup> midi_setups;

    // An effects group's sound effects.
    std::vector<Effect> effects;
};

// Reads the groups of the project file `file`, in the file's order. Throws FormatError if the file is damaged.
std::vector<Group> ReadProject(std::span<const uint8_t> file);

} // namespace ntdlib::musyx
