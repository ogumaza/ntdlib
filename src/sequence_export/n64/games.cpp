// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/games.h"

#include <array>

namespace ntdlib::sequence_export::n64
{

namespace
{

constexpr std::array kChameleonTwistNames = {SongName{0x19, "title screen"}};

constexpr std::array kChameleonTwist2Names = {
    SongName{0x0D, "after the logos"},
    SongName{0x10, "menus"},
};

constexpr std::array kCruisnWorldNames = {
    SongName{0x00, "title screen"},
    SongName{0x01, "menus"},
    SongName{0x03, "race results"},
    SongName{0x04, "China, Japan"},
    SongName{0x05, "Mexico"},
    SongName{0x06, "Hawaii, Florida"},
    SongName{0x07, "Italy"},
    SongName{0x08, "New York, Russia, Australia, Moon in a championship"},
    SongName{0x09, "France, Moon in a cruise"},
    SongName{0x0A, "England"},
    SongName{0x0B, "Germany"},
    SongName{0x0C, "Egypt"},
    SongName{0x0D, "Kenya"},
};

constexpr std::array kGoldenEyeNames = {
    SongName{0x01, "short death"},
    SongName{0x02, "intro"},
    SongName{0x03, "Train"},
    SongName{0x04, "Depot"},
    SongName{0x05, "multiplayer 1"},
    SongName{0x06, "Citadel"},
    SongName{0x07, "Facility"},
    SongName{0x08, "Control"},
    SongName{0x09, "Dam"},
    SongName{0x0A, "Frigate"},
    SongName{0x0B, "Archives"},
    SongName{0x0C, "Silo"},
    SongName{0x0D, "multiplayer 2"},
    SongName{0x0E, "Streets"},
    SongName{0x0F, "Bunker I"},
    SongName{0x10, "Bunker II"},
    SongName{0x11, "Statue"},
    SongName{0x12, "Control, elevator"},
    SongName{0x13, "Cradle"},
    SongName{0x15, "Caverns, elevator"},
    SongName{0x16, "Egyptian"},
    SongName{0x17, "folders"},
    SongName{0x18, "watch"},
    SongName{0x19, "Aztec"},
    SongName{0x1A, "Caverns"},
    SongName{0x1B, "death"},
    SongName{0x1C, "Surface II"},
    SongName{0x1D, "Train, second track"},
    SongName{0x1F, "Facility, second track"},
    SongName{0x20, "Depot, second track"},
    SongName{0x21, "Control, second track"},
    SongName{0x22, "Caverns, second track"},
    SongName{0x23, "Dam, second track"},
    SongName{0x24, "Frigate, second track 1"},
    SongName{0x25, "Archives, second track"},
    SongName{0x26, "Silo, second track"},
    SongName{0x28, "Streets, second track"},
    SongName{0x29, "Bunker I, second track"},
    SongName{0x2A, "Bunker II, second track"},
    SongName{0x2B, "Jungle, second track"},
    SongName{0x2C, "first sound of the intro"},
    SongName{0x2D, "Statue, second track 1"},
    SongName{0x2E, "Aztec, second track"},
    SongName{0x2F, "Egyptian, second track"},
    SongName{0x30, "Cradle, second track"},
    SongName{0x31, "Cuba"},
    SongName{0x32, "Runway"},
    SongName{0x33, "Runway, second track 2"},
    SongName{0x34, "multiplayer 3"},
    SongName{0x35, "wind"},
    SongName{0x36, "guitar gliss"},
    SongName{0x37, "Jungle"},
    SongName{0x38, "Runway, second track 1"},
    SongName{0x39, "Surface I"},
    SongName{0x3A, "multiplayer death"},
    SongName{0x3B, "Surface II, second track 1"},
    SongName{0x3C, "Surface II, second track 2"},
    SongName{0x3D, "Statue, second track 2"},
    SongName{0x3E, "Frigate, second track 2"},
};

// The table of the game's code that gives each song a volume, as a run of the game's code holds it at 0x80024358. The
// image keeps it compressed.
constexpr std::array<uint16_t, 64> kGoldenEyeVolumes = {
    0x6665, 0x7332, 0x7332, 0x7998, 0x7332, 0x5998, 0x6665, 0x6665, 0x6665, 0x6665, 0x7332, 0x6665, 0x7332,
    0x6665, 0x6665, 0x7332, 0x7332, 0x6665, 0x3FFF, 0x6665, 0x6665, 0x3FFF, 0x6665, 0x6665, 0x6665, 0x6665,
    0x6665, 0x7998, 0x6665, 0x5998, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x5998, 0x6665,
    0x3332, 0x6665, 0x7332, 0x7332, 0x5998, 0x7332, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665,
    0x6CCB, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x6665, 0x7332, 0x6665, 0x7332, 0x7998,
};

// The sequences that hold only the games' sound effects.
constexpr std::array<std::size_t, 1> kFirstSequenceIsEffects = {0};

constexpr std::array kSuperMario64Names = {
    SongName{0x01, "star collected"},
    SongName{0x02, "title screen"},
    SongName{0x03, "grass levels"},
    SongName{0x04, "inside the castle"},
    SongName{0x06, "hot levels"},
    SongName{0x07, "Bowser's battles"},
    SongName{0x08, "snow levels"},
    SongName{0x09, "slides"},
    SongName{0x0B, "piranha plant's lullaby"},
    SongName{0x0D, "star select"},
    SongName{0x0E, "wing cap"},
    SongName{0x0F, "metal cap"},
    SongName{0x10, "Bowser's message"},
    SongName{0x11, "Bowser's roads"},
    SongName{0x12, "high score"},
    SongName{0x13, "merry-go-round"},
    SongName{0x14, "race fanfare"},
    SongName{0x15, "star appears"},
    SongName{0x16, "boss"},
    SongName{0x17, "key collected"},
    SongName{0x18, "endless stairs"},
    SongName{0x19, "final battle"},
    SongName{0x1A, "credits"},
    SongName{0x1B, "puzzle solved"},
    SongName{0x1C, "Toad's message"},
    SongName{0x1D, "Peach's message"},
    SongName{0x1E, "intro"},
    SongName{0x1F, "course clear"},
    SongName{0x20, "ending"},
    SongName{0x21, "file select"},
    SongName{0x22, "Lakitu's message"},
};

// The level scripts ask for the title screen's and the water levels' sequences with bit 7 set in two places: 0x269FF8
// and 0x46C2CC in the image. The game's call passes that bit to the sequences' variation commands. The table of each
// level's music dynamics fades sets of channels to nothing, by where Mario is.
constexpr std::array kSuperMario64Tracks = {
    EadTrack{.sequence = 0x02, .name = "title screen"},
    EadTrack{.sequence = 0x02, .name = "title screen, with the variation", .variation = -128},
    EadTrack{.sequence = 0x05, .name = "water levels, all channels"},
    EadTrack{.sequence = 0x05, .name = "water levels, without channels 6, 9, 10 and 11", .silent_channels = 0x0E40},
    EadTrack{
        .sequence = 0x05, .name = "water levels, without channels 0, 1, 6, 9, 10 and 11", .silent_channels = 0x0E43},
    EadTrack{.sequence = 0x05, .name = "water levels, with the variation", .variation = -128},
    EadTrack{.sequence = 0x05,
             .name = "water levels, with the variation, without channels 6, 9, 10 and 11",
             .variation = -128,
             .silent_channels = 0x0E40},
    EadTrack{.sequence = 0x05,
             .name = "water levels, with the variation, without channels 0, 1, 6, 9, 10 and 11",
             .variation = -128,
             .silent_channels = 0x0E43},
    EadTrack{.sequence = 0x0A, .name = "Big Boo's Haunt, indoors"},
    EadTrack{.sequence = 0x0A, .name = "Big Boo's Haunt, outdoors", .silent_channels = 0x0070},
    EadTrack{.sequence = 0x0C, .name = "underground levels, all channels"},
    EadTrack{.sequence = 0x0C, .name = "underground levels, without channel 8", .silent_channels = 0x0100},
    EadTrack{.sequence = 0x0C, .name = "underground levels, without channel 3", .silent_channels = 0x0008},
};

// Super Mario 64 (USA). The game's code plays a sequence at the volume that its table of 34 bytes at 0xEE188 gives. The
// script's volume command doesn't change it. The game's call gives a sequence a variation of 0 unless it asks for
// another.
constexpr EadProfile kSuperMario64 = {.engine = ntdlib::n64::EadEngine::kEarly,
                                      .output_rate = 32006.0,
                                      .wave_rate = 32006.0,
                                      .updates_per_frame = 4,
                                      .song_volumes = 0xEE188,
                                      .song_volumes_count = 34,
                                      .effect_sequences = kFirstSequenceIsEffects,
                                      .variation = 0,
                                      .tracks = kSuperMario64Tracks,
                                      .names = kSuperMario64Names};

// Mario Kart 64 (USA). The game asks for 26800 samples a second, and the video interface's divider gives 26807.
constexpr EadProfile kMarioKart64 = {.engine = ntdlib::n64::EadEngine::kMiddle,
                                     .output_rate = 26807.0,
                                     .wave_rate = 32000.0,
                                     .updates_per_frame = 3,
                                     .levels = EadLevels::kVolumesSquared,
                                     .effect_sequences = kFirstSequenceIsEffects};

// Wave Race 64 (USA, Rev 1). The function at 0x800C1504 that starts its music sets the player's scale to 0.55.
constexpr EadProfile kWaveRace64 = {.engine = ntdlib::n64::EadEngine::kMiddle,
                                    .output_rate = 32006.0,
                                    .wave_rate = 32000.0,
                                    .updates_per_frame = 4,
                                    .levels = EadLevels::kEnvelopeSquared,
                                    .player_volume = 0.55,
                                    .effect_sequences = kFirstSequenceIsEffects};

// The names of Ocarina of Time's and Majora's Mask's sequences.
constexpr std::array kOcarinaOfTimeNames = {
    SongName{0x01, "nature ambience"},
    SongName{0x02, "Hyrule Field"},
    SongName{0x03, "Hyrule Field, opening"},
    SongName{0x04, "Hyrule Field, part 1"},
    SongName{0x05, "Hyrule Field, part 2"},
    SongName{0x06, "Hyrule Field, part 3"},
    SongName{0x07, "Hyrule Field, part 4"},
    SongName{0x08, "Hyrule Field, part 5"},
    SongName{0x09, "Hyrule Field, part 6"},
    SongName{0x0A, "Hyrule Field, part 7"},
    SongName{0x0B, "Hyrule Field, part 8"},
    SongName{0x0C, "Hyrule Field, part 9"},
    SongName{0x0D, "Hyrule Field, part 10"},
    SongName{0x0E, "Hyrule Field, part 11"},
    SongName{0x0F, "Hyrule Field, enemies near, opening"},
    SongName{0x10, "Hyrule Field, enemies near, part 1"},
    SongName{0x11, "Hyrule Field, enemies near, part 2"},
    SongName{0x12, "Hyrule Field, enemies near, part 3"},
    SongName{0x13, "Hyrule Field, enemies near, part 4"},
    SongName{0x14, "Hyrule Field, standing still, part 1"},
    SongName{0x15, "Hyrule Field, standing still, part 2"},
    SongName{0x16, "Hyrule Field, standing still, part 3"},
    SongName{0x17, "Hyrule Field, standing still, part 4"},
    SongName{0x18, "Dodongo's Cavern and other caves"},
    SongName{0x19, "Kakariko Village, as an adult"},
    SongName{0x1A, "battle"},
    SongName{0x1B, "boss battle"},
    SongName{0x1C, "Inside the Deku Tree"},
    SongName{0x1D, "Market"},
    SongName{0x1E, "title screen"},
    SongName{0x1F, "houses"},
    SongName{0x20, "game over"},
    SongName{0x21, "boss defeated"},
    SongName{0x22, "item found"},
    SongName{0x23, "Ganondorf appears"},
    SongName{0x24, "Heart Container found"},
    SongName{0x25, "Prelude of Light"},
    SongName{0x26, "Inside Jabu-Jabu's Belly"},
    SongName{0x27, "Kakariko Village, as a child"},
    SongName{0x28, "Great Fairy's Fountain and file select"},
    SongName{0x29, "Castle Courtyard, Zelda"},
    SongName{0x2A, "Fire Temple"},
    SongName{0x2B, "treasure chest opens"},
    SongName{0x2C, "Forest Temple"},
    SongName{0x2D, "Castle Courtyard, guards"},
    SongName{0x2E, "Ganon's Tower"},
    SongName{0x2F, "Lon Lon Ranch"},
    SongName{0x30, "Goron City"},
    SongName{0x31, "Hyrule Field, morning"},
    SongName{0x32, "Spiritual Stone found"},
    SongName{0x33, "Bolero of Fire"},
    SongName{0x34, "Minuet of Forest"},
    SongName{0x35, "Serenade of Water"},
    SongName{0x36, "Requiem of Spirit"},
    SongName{0x37, "Nocturne of Shadow"},
    SongName{0x38, "mini-boss battle"},
    SongName{0x39, "small item found"},
    SongName{0x3A, "Temple of Time"},
    SongName{0x3B, "puzzle solved"},
    SongName{0x3C, "Kokiri Forest"},
    SongName{0x3D, "Fairy Ocarina found"},
    SongName{0x3E, "Lost Woods"},
    SongName{0x3F, "Spirit Temple"},
    SongName{0x40, "horse race"},
    SongName{0x41, "horse race won"},
    SongName{0x42, "Ingo"},
    SongName{0x43, "Medallion found"},
    SongName{0x44, "Saria's Song"},
    SongName{0x45, "Epona's Song"},
    SongName{0x46, "Zelda's Lullaby"},
    SongName{0x47, "Sun's Song"},
    SongName{0x48, "Song of Time"},
    SongName{0x49, "Song of Storms"},
    SongName{0x4A, "Navi's opening"},
    SongName{0x4B, "the Deku Tree speaks"},
    SongName{0x4C, "Windmill"},
    SongName{0x4D, "the story of Hyrule"},
    SongName{0x4E, "mini-games"},
    SongName{0x4F, "Sheik"},
    SongName{0x50, "Zora's Domain"},
    SongName{0x51, "someone appears"},
    SongName{0x52, "Link as an adult"},
    SongName{0x53, "Master Sword"},
    SongName{0x54, "Ganondorf's introduction"},
    SongName{0x55, "shops"},
    SongName{0x56, "Chamber of the Sages"},
    SongName{0x58, "Ice Cavern"},
    SongName{0x59, "Door of Time"},
    SongName{0x5A, "Kaepora Gaebora"},
    SongName{0x5B, "Shadow Temple"},
    SongName{0x5C, "Water Temple"},
    SongName{0x5D, "bridge to Ganon's Castle"},
    SongName{0x5E, "seal of the Sages"},
    SongName{0x5F, "Gerudo Valley"},
    SongName{0x60, "Potion Shop"},
    SongName{0x61, "Kotake and Koume"},
    SongName{0x62, "escape from Ganon's Castle"},
    SongName{0x63, "Inside Ganon's Castle"},
    SongName{0x64, "Ganondorf battle"},
    SongName{0x65, "Ganon battle"},
    SongName{0x66, "Ocarina of Time"},
    SongName{0x67, "staff roll, part 1"},
    SongName{0x68, "staff roll, part 2"},
    SongName{0x69, "staff roll, part 3"},
    SongName{0x6A, "staff roll, part 4"},
    SongName{0x6B, "Volvagia battle"},
    SongName{0x6C, "timed mini-game"},
    SongName{0x6D, "cutscene effects"},
};

constexpr std::array kMajorasMaskNames = {
    SongName{0x01, "nature ambience"},
    SongName{0x02, "Termina Field"},
    SongName{0x03, "chase"},
    SongName{0x04, "Majora's theme"},
    SongName{0x05, "Clock Tower"},
    SongName{0x06, "Stone Tower Temple"},
    SongName{0x07, "Stone Tower Temple, inverted"},
    SongName{0x08, "failure, part 1"},
    SongName{0x09, "failure, part 2"},
    SongName{0x0A, "Happy Mask Salesman"},
    SongName{0x0B, "Song of Healing"},
    SongName{0x0C, "Southern Swamp"},
    SongName{0x0D, "aliens' attack"},
    SongName{0x0E, "boat cruise"},
    SongName{0x0F, "Sharp's curse"},
    SongName{0x10, "Great Bay Coast"},
    SongName{0x11, "Ikana Canyon"},
    SongName{0x12, "Deku Palace"},
    SongName{0x13, "Mountain Village"},
    SongName{0x14, "Pirates' Fortress"},
    SongName{0x15, "Clock Town, first day"},
    SongName{0x16, "Clock Town, second day"},
    SongName{0x17, "Clock Town, third day"},
    SongName{0x18, "file select and Fairy Fountain"},
    SongName{0x19, "puzzle solved"},
    SongName{0x1A, "battle"},
    SongName{0x1B, "boss battle"},
    SongName{0x1C, "Woodfall Temple"},
    SongName{0x1D, "Clock Town, main sequence"},
    SongName{0x1E, "opening"},
    SongName{0x1F, "inside a house"},
    SongName{0x20, "game over"},
    SongName{0x21, "boss defeated"},
    SongName{0x22, "item found"},
    SongName{0x24, "Heart Container found"},
    SongName{0x25, "timed mini-game"},
    SongName{0x26, "Goron race"},
    SongName{0x27, "Music Box House"},
    SongName{0x29, "Zelda's Lullaby"},
    SongName{0x2A, "Rosa Sisters"},
    SongName{0x2B, "treasure chest opens"},
    SongName{0x2C, "Marine Research Lab"},
    SongName{0x2D, "the Giants"},
    SongName{0x2E, "Song of Storms"},
    SongName{0x2F, "Romani Ranch"},
    SongName{0x30, "Goron Village"},
    SongName{0x31, "Mayor's Office"},
    SongName{0x32, "Epona's Song"},
    SongName{0x33, "Sun's Song"},
    SongName{0x34, "Song of Time"},
    SongName{0x35, "Song of Storms, ocarina"},
    SongName{0x36, "Zora Hall"},
    SongName{0x37, "new mask found"},
    SongName{0x38, "mini-boss battle"},
    SongName{0x39, "small item found"},
    SongName{0x3A, "Astral Observatory"},
    SongName{0x3B, "caves"},
    SongName{0x3C, "Milk Bar"},
    SongName{0x3D, "Zelda appears"},
    SongName{0x3E, "Saria's Song"},
    SongName{0x3F, "Goron race won"},
    SongName{0x40, "horse race"},
    SongName{0x41, "horse race won"},
    SongName{0x42, "Gorman Bros."},
    SongName{0x43, "Potion Shop"},
    SongName{0x44, "shop"},
    SongName{0x45, "Kaepora Gaebora"},
    SongName{0x46, "shooting gallery"},
    SongName{0x47, "Song of Soaring, ocarina"},
    SongName{0x48, "Song of Healing, ocarina"},
    SongName{0x49, "Inverted Song of Time"},
    SongName{0x4A, "Song of Double Time"},
    SongName{0x4B, "Sonata of Awakening"},
    SongName{0x4C, "Goron Lullaby"},
    SongName{0x4D, "New Wave Bossa Nova"},
    SongName{0x4E, "Elegy of Emptiness"},
    SongName{0x4F, "Oath to Order"},
    SongName{0x50, "Swordsman's School"},
    SongName{0x51, "Goron Lullaby, introduction"},
    SongName{0x52, "new song learned"},
    SongName{0x53, "Bremen March"},
    SongName{0x54, "Ballad of the Wind Fish"},
    SongName{0x55, "Song of Soaring"},
    SongName{0x57, "final hours and Majora's lair"},
    SongName{0x58, "Mikau's riff"},
    SongName{0x59, "Mikau's finale"},
    SongName{0x5A, "Frog Song"},
    SongName{0x5B, "Sonata of Awakening, ocarina"},
    SongName{0x5C, "Goron Lullaby, ocarina"},
    SongName{0x5D, "New Wave Bossa Nova, ocarina"},
    SongName{0x5E, "Elegy of Emptiness, ocarina"},
    SongName{0x5F, "Oath to Order, ocarina"},
    SongName{0x62, "guitar and bass session, ocarina"},
    SongName{0x63, "piano session"},
    SongName{0x64, "The Indigo-Go's session"},
    SongName{0x65, "Snowhead Temple"},
    SongName{0x66, "Great Bay Temple"},
    SongName{0x67, "New Wave Bossa Nova, saxophone"},
    SongName{0x68, "New Wave Bossa Nova, vocal"},
    SongName{0x69, "Majora's Wrath battle"},
    SongName{0x6A, "Majora's Incarnation battle"},
    SongName{0x6B, "Majora's Mask battle"},
    SongName{0x6C, "bass session"},
    SongName{0x6D, "drums session"},
    SongName{0x6E, "piano session, part 2"},
    SongName{0x6F, "Ikana Castle"},
    SongName{0x70, "the Giants gather"},
    SongName{0x71, "Kamaro's dance"},
    SongName{0x72, "Cremia's carriage"},
    SongName{0x73, "Keaton's quiz"},
    SongName{0x74, "staff roll, part 1"},
    SongName{0x75, "opening, loop"},
    SongName{0x76, "title screen"},
    SongName{0x77, "a dungeon appears"},
    SongName{0x78, "Woodfall cleared"},
    SongName{0x79, "Snowhead cleared"},
    SongName{0x7B, "into the Moon"},
    SongName{0x7C, "goodbye to the Giants"},
    SongName{0x7D, "Tatl and Tael"},
    SongName{0x7E, "the Moon's destruction"},
    SongName{0x7F, "staff roll, part 2"},
};

// Sound effects, the nature sounds that the game's code runs through ports, and a list of effects for cutscenes.
constexpr std::array<std::size_t, 3> kOcarinaOfTimeEffects = {0x00, 0x01, 0x6D};

// The game's code tells Hyrule Field's script through a port what the player is doing. It sets channel volumes in Lon
// Lon Ranch's sequence by the player's distance from Malon: her voice on two channels and a melody on another.
//
// The file select starts sequence 0x57 with 1 for port 7. Sequence 0x57 points to sequence 0x28. With the port at 1,
// the sequence leaves out the harp's introduction. The Great Fairy's Fountain plays it with -1.
constexpr std::array kOcarinaOfTimeTracks = {
    EadTrack{.sequence = 0x02, .name = "Hyrule Field, moving", .ports = EadPorts(2, 0)},
    EadTrack{.sequence = 0x02, .name = "Hyrule Field, enemies near", .ports = EadPorts(2, 1)},
    EadTrack{.sequence = 0x02, .name = "Hyrule Field, standing still", .ports = EadPorts(2, 2)},
    EadTrack{.sequence = 0x2F, .name = "Lon Lon Ranch, Malon away"},
    EadTrack{.sequence = 0x2F, .name = "Lon Lon Ranch, far from Malon", .silent_channels = 0x0003},
    EadTrack{.sequence = 0x2F, .name = "Lon Lon Ranch, close to Malon", .silent_channels = 0x2000},
    EadTrack{.sequence = 0x28},
    EadTrack{.sequence = 0x28, .name = "file select, without the harp's introduction", .ports = EadPorts(7, 1)},
};

// Sound effects, the nature sounds, and a sequence that only waits for the game's code to name another.
constexpr std::array<std::size_t, 3> kMajorasMaskEffects = {0x00, 0x01, 0x7A};

// The game's code writes the day, from 0, to a port. Some sequences leave out channels by it. A port also starts some
// scripts at dawn, with a piece of their own. Romani Ranch's sequence works as Lon Lon Ranch's does.
//
// The function at 0x801A3000 starts a song's fanfare and writes a value for Link's form to port 7. Fourteen fanfares
// play another arrangement for Goron, Zora and Deku Link (0x5C, 0x5D and 0x5E). Link's value, 0x35, plays as the port's
// -1 does. The file select starts sequence 0x18 with 1 there. The sequence then leaves out the harp's introduction.
constexpr std::array kMajorasMaskTracks = {
    EadTrack{.sequence = 0x02, .name = "Termina Field"},
    EadTrack{.sequence = 0x02, .name = "Termina Field, at dawn", .ports = EadPorts(0, 1)},
    EadTrack{.sequence = 0x1D, .name = "Clock Town, dawn of the first day", .ports = EadPorts(0, 1, 4, 0)},
    EadTrack{.sequence = 0x1D, .name = "Clock Town, dawn of the second day", .ports = EadPorts(0, 1, 4, 1)},
    EadTrack{.sequence = 0x1D, .name = "Clock Town, dawn of the final day", .ports = EadPorts(0, 1, 4, 2)},
    EadTrack{.sequence = 0x2F, .name = "Romani Ranch, Romani away"},
    EadTrack{.sequence = 0x2F, .name = "Romani Ranch, far from Romani", .silent_channels = 0x0003},
    EadTrack{.sequence = 0x2F, .name = "Romani Ranch, close to Romani", .silent_channels = 0x2000},
    EadTrack{.sequence = 0x4D, .name = "New Wave Bossa Nova, first day", .ports = EadPorts(4, 0)},
    EadTrack{.sequence = 0x4D, .name = "New Wave Bossa Nova, later days", .ports = EadPorts(4, 1)},
    EadTrack{.sequence = 0x54, .name = "Ballad of the Wind Fish, first day", .ports = EadPorts(4, 0)},
    EadTrack{.sequence = 0x54, .name = "Ballad of the Wind Fish, second day", .ports = EadPorts(4, 1)},
    EadTrack{.sequence = 0x54, .name = "Ballad of the Wind Fish, final day", .ports = EadPorts(4, 2)},
    EadTrack{.sequence = 0x18},
    EadTrack{.sequence = 0x18, .name = "file select, without the harp's introduction", .ports = EadPorts(7, 1)},
    EadTrack{.sequence = 0x32},
    EadTrack{.sequence = 0x32, .name = "Epona's Song, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x32, .name = "Epona's Song, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x32, .name = "Epona's Song, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x33},
    EadTrack{.sequence = 0x33, .name = "Sun's Song, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x33, .name = "Sun's Song, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x33, .name = "Sun's Song, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x34},
    EadTrack{.sequence = 0x34, .name = "Song of Time, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x34, .name = "Song of Time, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x34, .name = "Song of Time, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x35},
    EadTrack{.sequence = 0x35, .name = "Song of Storms, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x35, .name = "Song of Storms, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x35, .name = "Song of Storms, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x47},
    EadTrack{.sequence = 0x47, .name = "Song of Soaring, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x47, .name = "Song of Soaring, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x47, .name = "Song of Soaring, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x48},
    EadTrack{.sequence = 0x48, .name = "Song of Healing, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x48, .name = "Song of Healing, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x48, .name = "Song of Healing, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x49},
    EadTrack{.sequence = 0x49, .name = "Inverted Song of Time, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x49, .name = "Inverted Song of Time, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x49, .name = "Inverted Song of Time, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x4A},
    EadTrack{.sequence = 0x4A, .name = "Song of Double Time, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x4A, .name = "Song of Double Time, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x4A, .name = "Song of Double Time, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x51},
    EadTrack{.sequence = 0x51, .name = "Goron Lullaby, introduction, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x51, .name = "Goron Lullaby, introduction, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x51, .name = "Goron Lullaby, introduction, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5B},
    EadTrack{.sequence = 0x5B, .name = "Sonata of Awakening, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5B, .name = "Sonata of Awakening, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5B, .name = "Sonata of Awakening, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5C},
    EadTrack{.sequence = 0x5C, .name = "Goron Lullaby, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5C, .name = "Goron Lullaby, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5C, .name = "Goron Lullaby, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5D},
    EadTrack{.sequence = 0x5D, .name = "New Wave Bossa Nova, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5D, .name = "New Wave Bossa Nova, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5D, .name = "New Wave Bossa Nova, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5E},
    EadTrack{.sequence = 0x5E, .name = "Elegy of Emptiness, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5E, .name = "Elegy of Emptiness, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5E, .name = "Elegy of Emptiness, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
    EadTrack{.sequence = 0x5F},
    EadTrack{.sequence = 0x5F, .name = "Oath to Order, ocarina, as Goron Link", .ports = EadPorts(7, 0x5C)},
    EadTrack{.sequence = 0x5F, .name = "Oath to Order, ocarina, as Zora Link", .ports = EadPorts(7, 0x5D)},
    EadTrack{.sequence = 0x5F, .name = "Oath to Order, ocarina, as Deku Link", .ports = EadPorts(7, 0x5E)},
};

constexpr EadProfile kOcarinaOfTime = {.engine = ntdlib::n64::EadEngine::kLater,
                                       .output_rate = 32006.0,
                                       .wave_rate = 32006.0,
                                       .updates_per_frame = 3,
                                       .levels = EadLevels::kVolumesSquared,
                                       .effect_sequences = kOcarinaOfTimeEffects,
                                       .tracks = kOcarinaOfTimeTracks,
                                       .names = kOcarinaOfTimeNames};

constexpr EadProfile kMajorasMask = {.engine = ntdlib::n64::EadEngine::kLaterMajora,
                                     .output_rate = 32006.0,
                                     .wave_rate = 32006.0,
                                     .updates_per_frame = 3,
                                     .levels = EadLevels::kVolumesSquared,
                                     .effect_sequences = kMajorasMaskEffects,
                                     .tracks = kMajorasMaskTracks,
                                     .names = kMajorasMaskNames};

constexpr std::array<std::size_t, 40> kStadium2Effects = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
    0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
};

// The game's music call at 0x80021ED8 changes the tempo of four sequences and masks channels of two others. It switches
// between the two versions of a masked sequence during play.
constexpr std::array kStadium2Tracks = {
    EadTrack{.sequence = 0x2E, .silent_channels = 0x3000},
    EadTrack{.sequence = 0x2E, .name = "second version", .silent_channels = 0x07FF},
    EadTrack{.sequence = 0x49},
    EadTrack{.sequence = 0x49, .name = "faster", .tempo_change = 17},
    EadTrack{.sequence = 0x5D},
    EadTrack{.sequence = 0x5D, .name = "faster", .tempo_change = 40},
    EadTrack{.sequence = 0x65, .silent_channels = 0xFD00},
    EadTrack{.sequence = 0x65, .name = "second version", .silent_channels = 0x021F},
    EadTrack{.sequence = 0x6D, .tempo_change = 4},
    EadTrack{.sequence = 0x73},
    EadTrack{.sequence = 0x73, .name = "faster", .tempo_change = 20},
};

// The game's table at 0x878C0 gives each sequence's banks. Its code gives each sequence from 0x28 on a level out of
// 127, from the table at 0x80090138. A value above 127 there names an entry of the table at 0x800917F8. The player's
// scale is 0.81 of that level.
constexpr EadProfile kStadium2 = {.engine = ntdlib::n64::EadEngine::kLaterMajora,
                                  .output_rate = 32006.0,
                                  .wave_rate = 32006.0,
                                  .updates_per_frame = 3,
                                  .levels = EadLevels::kVolumesSquared,
                                  .envelope_skip = true,
                                  .channel_layers = 6,
                                  .inverse_vibrato = true,
                                  .player_volume = 0.81,
                                  .player_levels = 0x90D38,
                                  .player_levels_indirect = 0x923F8,
                                  .first_player_level = 0x28,
                                  .player_level_count = 0x61,
                                  .effect_sequences = kStadium2Effects,
                                  .tracks = kStadium2Tracks};

constexpr std::array kPokemonSnapNames = {
    SongName{0x00, "Beach"},
    SongName{0x01, "Poké Flute, first song"},
    SongName{0x02, "Poké Flute, third song"},
    SongName{0x03, "Poké Flute, second song"},
    SongName{0x04, "Tunnel"},
    SongName{0x05, "River"},
    SongName{0x06, "Volcano"},
    SongName{0x07, "Valley"},
    SongName{0x0B, "staff roll"},
    SongName{0x0C, "new game"},
    SongName{0x0D, "Cave"},
    SongName{0x0E, "Rainbow Cloud"},
    SongName{0x0F, "Cave, ambience"},
    SongName{0x10, "photo check"},
    SongName{0x11, "course finished"},
    SongName{0x12, "Professor Oak's report, part 1"},
    SongName{0x14, "River and title screen, ambience"},
    SongName{0x15, "River, ambience"},
    SongName{0x17, "main menu"},
    SongName{0x18, "Cave, second ambience"},
    SongName{0x19, "title screen"},
    SongName{0x1B, "menu"},
    SongName{0x1C, "Mew"},
    SongName{0x1D, "Professor Oak's report, part 2"},
    SongName{0x1E, "Professor Oak's report, part 3"},
    SongName{0x1F, "photo check, ambience"},
    SongName{0x20, "Valley and Professor Oak's lab, ambience"},
    SongName{0x21, "Tunnel, ambience"},
    SongName{0x22, "gallery"},
    SongName{0x23, "main menu, ambience"},
    SongName{0x24, "staff roll, second part"},
};

constexpr std::array kPuzzleLeagueBanks = {
    LibmusBankPlace{.first_song = 0, .pointers = 0x17C2A80, .samples = 0x17CA780},
    LibmusBankPlace{.first_song = 0x47, .pointers = 0x1C60AD0, .samples = 0x1C63BE0},
};

// The game's music setting starts at 0.55 of the player's full volume. Its code gives each song its byte of the table.
constexpr LibmusProfile kPuzzleLeague = {
    .output_rate = 22047,
    .songs = 0xB5F30,
    .song_count = 72,
    .song_volumes = 0xB4BB0,
    .master_volume = 18021,
    .banks = kPuzzleLeagueBanks,
};

// The volumes that the game's code gives its songs, in a switch at 0x8004B658. Song 0x1D reads a word at 0x80078FB4,
// which holds 112.
constexpr std::array<uint8_t, 76> kStadiumVolumes = {
    152, 104, 52, 68,  56,  52, 56,  120, 160, 80,  96,  56,  56,  72,  100, 96,  104, 96,  46,
    104, 88,  96, 96,  96,  96, 88,  96,  88,  96,  112, 101, 112, 112, 80,  72,  80,  72,  80,
    64,  88,  72, 120, 96,  96, 112, 96,  84,  88,  100, 96,  96,  96,  96,  84,  56,  56,  96,
    56,  104, 60, 96,  136, 96, 96,  80,  96,  128, 128, 144, 96,  96,  136, 112, 104, 168, 96,
};

// The game's code keeps the songs and their bank compressed in a directory. Each song is a file of the library's early
// form. The game gives the songs a master volume of 0x6FFF. Its synthesizer runs at 32006 Hz with the SDK's big room on
// the auxiliary bus.
constexpr LibmusProfile kStadium = {
    .output_rate = 32006,
    .early_form = true,
    .directory = 0x15B5020,
    .song_volume_list = kStadiumVolumes,
    .master_volume = 0x6FFF,
};

constexpr std::array kMarioTennisBanks = {
    LibmusBankPlace{.first_song = 0, .pointers = 0x3C780, .samples = 0x55B20},
};

// The trophy scene in file 0x1CE scales three songs' tempos, at offsets 0x5BCC, 0x5EE8 and 0x5FB4. A run of the game's
// code shows the calls at 0x802ECBCC, four frames after each song starts: TRO-MARIO for character 2, TRO-WALUI for
// character 8 and TRO-BABYM for character 0x11. Character 6 plays TRO-BABYM without a change.
constexpr std::array kMarioTennisTempos = {
    LibmusTempoChange{.song = 0x2A, .scale = 0x8E, .beside = "faster"},
    LibmusTempoChange{.song = 0x2F, .scale = 0x7C},
    LibmusTempoChange{.song = 0x3A, .scale = 0x78},
};

// The game's code loads its sound code from file 0x13 to 0x802EC000. The function at 0x802ECC60 starts a song and gives
// its channels a volume of 0x32, the word at 0x802F4AB0. The master volume stays at 0x7FFF. The synthesizer runs at
// 32006 Hz.
constexpr LibmusProfile kMarioTennis = {
    .output_rate = 32006,
    .song_volume = 0x32,
    .banks = kMarioTennisBanks,
    .files = {.directory = 0x116C0, .table_file = 0x13, .table = 0x8ABC, .table_count = 66, .file_address = 0x802EC000},
    .tempo_changes = kMarioTennisTempos,
};

// The names in the game's "Play music" menu, in the table at 0x8012BCA0 of the overlay at 0xB3C70 in the image.
constexpr std::array kHeyYouPikachuNames = {
    SongName{0x00, "Yours ever"},   SongName{0x01, "Camp night"},    SongName{0x02, "Mamma mia!"},
    SongName{0x03, "Joyful"},       SongName{0x04, "Home at last"},  SongName{0x05, "Prairie wind"},
    SongName{0x06, "Wood music"},   SongName{0x07, "Hello!"},        SongName{0x08, "Daring days"},
    SongName{0x09, "Discovery"},    SongName{0x0A, "Play days"},     SongName{0x0B, "Misty lake"},
    SongName{0x0C, "My bedroom"},   SongName{0x0D, "Pi-pikachu!"},   SongName{0x0E, "Go shopping"},
    SongName{0x0F, "Quiz time"},    SongName{0x10, "Who am I?"},     SongName{0x11, "Feel lonely"},
    SongName{0x12, "Over the sea"}, SongName{0x13, "Gone fishing!"}, SongName{0x14, "Opening 1"},
    SongName{0x15, "Opening 2"},    SongName{0x16, "Success"},       SongName{0x17, "See you later"},
    SongName{0x18, "Picnic"},       SongName{0x19, "What's up?"},    SongName{0x1A, "Boo"},
    SongName{0x1B, "Call"},         SongName{0x1C, "Counting"},      SongName{0x1D, "A good thing"},
    SongName{0x1E, "Evolved"},      SongName{0x1F, "Evolving"},      SongName{0x20, "Eye icon"},
    SongName{0x21, "Eye mode"},     SongName{0x22, "Failure"},       SongName{0x23, "Harmonica 1"},
    SongName{0x24, "Harmonica 2"},  SongName{0x25, "Out of tune"},   SongName{0x26, "In tune"},
    SongName{0x27, "Fly around"},   SongName{0x28, "Correct"},       SongName{0x29, "Trumpet 1"},
    SongName{0x2A, "Trumpet 2"},    SongName{0x2B, "Sea noise"},     SongName{0x2C, "Doing well"},
};

// The game's scripts slow some cues through 0x8011D768, which passes a rate to 0x800231F4. A run of those areas in
// usf's core shows where each slide stops: 0.798 past an area's time limit, and 0.79 in the fishing areas. The rate
// reaches a cue through 0x8001FF84, which leaves out cue 2, Mamma mia!. Each track holds the rate where the slide
// stops.
constexpr std::array kHeyYouPikachuRates = {
    LibultraRateChange{.song = 5, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 6, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 11, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 14, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 18, .rate = 0.798, .beside = "slower"},
    LibultraRateChange{.song = 19, .rate = 0.79, .beside = "slower"},
    LibultraRateChange{.song = 24, .rate = 0.798, .beside = "slower"},
};

// Where Paper Mario plays each song. The game has no sound test.
constexpr std::array kPaperMarioNames = {
    SongName{0x00, "Toad Town"},
    SongName{0x01, "Goomba Village"},
    SongName{0x02, "Normal Battle"},
    SongName{0x03, "Special Battle"},
    SongName{0x04, "Jr. Troopa Battle"},
    SongName{0x05, "Final Bowser Battle"},
    SongName{0x07, "Goomba King Battle"},
    SongName{0x08, "Koopa Bros Battle"},
    SongName{0x09, "Fake Bowser Battle"},
    SongName{0x0A, "Tutankoopa Battle"},
    SongName{0x0B, "Tubba Blubba Battle"},
    SongName{0x0C, "General Guy Battle"},
    SongName{0x0D, "Lava Piranha Battle"},
    SongName{0x0E, "Huff N. Puff Battle"},
    SongName{0x0F, "Crystal King Battle"},
    SongName{0x11, "Pleasant Path"},
    SongName{0x12, "Fuzzy Attack"},
    SongName{0x13, "Koopa Village"},
    SongName{0x14, "Koopa Fortress"},
    SongName{0x15, "Dry Dry Outpost"},
    SongName{0x16, "Mt. Rugged"},
    SongName{0x17, "Dry Dry Desert"},
    SongName{0x18, "Dry Dry Ruins"},
    SongName{0x19, "Ruins Basement"},
    SongName{0x1A, "Forever Forest"},
    SongName{0x1B, "Boo's Mansion"},
    SongName{0x1C, "Cheerful Boo's Mansion"},
    SongName{0x1D, "Gusty Gulch"},
    SongName{0x1E, "Tubba's Manor"},
    SongName{0x1F, "Tubba Escape"},
    SongName{0x20, "Shy Guy Toybox"},
    SongName{0x21, "Toybox Train"},
    SongName{0x22, "Creepy Toybox"},
    SongName{0x24, "Jade Jungle"},
    SongName{0x25, "Deep Jungle"},
    SongName{0x26, "Yoshi's Village"},
    SongName{0x27, "Yoshi's Panic"},
    SongName{0x28, "Raphael Raven"},
    SongName{0x29, "Mt. Lavalava"},
    SongName{0x2A, "Volcano Escape"},
    SongName{0x2B, "Star Way Opens"},
    SongName{0x2C, "Master Battle"},
    SongName{0x2D, "Radio Island Sounds"},
    SongName{0x2E, "Radio Hot Hits"},
    SongName{0x2F, "Radio Golden Oldies"},
    SongName{0x30, "Flower Fields Cloudy"},
    SongName{0x31, "Flower Fields Sunny"},
    SongName{0x32, "Cloudy Climb"},
    SongName{0x33, "Puff Puff Machine"},
    SongName{0x34, "Sun Tower Cloudy"},
    SongName{0x35, "Sun Tower Sunny"},
    SongName{0x37, "Crystal Palace"},
    SongName{0x38, "Shiver City"},
    SongName{0x39, "Penguin Mystery"},
    SongName{0x3A, "Shiver Snowfield"},
    SongName{0x3B, "Shiver Mountain"},
    SongName{0x3C, "Starborn Valley"},
    SongName{0x3D, "Merlar Theme"},
    SongName{0x3E, "Mail Call"},
    SongName{0x3F, "Peach's Castle Party"},
    SongName{0x40, "Chapter End"},
    SongName{0x41, "Chapter Start"},
    SongName{0x42, "Item Upgrade"},
    SongName{0x44, "Phonograph Music"},
    SongName{0x45, "Tutankoopa Theme"},
    SongName{0x46, "Kammy Koopa Theme"},
    SongName{0x47, "Jr. Troopa Theme"},
    SongName{0x48, "Bullet Bill Assault"},
    SongName{0x49, "Monty Mole Assault"},
    SongName{0x4A, "Shy Guy Invasion"},
    SongName{0x4B, "Toad Town Tunnels"},
    SongName{0x4C, "Whale Theme"},
    SongName{0x4D, "Forever Forest Warning"},
    SongName{0x4E, "Yoshi Kids Found"},
    SongName{0x4F, "Unused Fanfare"},
    SongName{0x50, "Goomba King Theme"},
    SongName{0x51, "Koopa Bros Interlude"},
    SongName{0x52, "Koopa Bros Theme"},
    SongName{0x53, "Tutankoopa Warning"},
    SongName{0x54, "Tutankoopa Revealed"},
    SongName{0x55, "Tubba Blubba Theme"},
    SongName{0x56, "General Guy Theme"},
    SongName{0x57, "Lava Piranha Theme"},
    SongName{0x58, "Huff N. Puff Theme"},
    SongName{0x59, "Crystal King Theme"},
    SongName{0x5A, "Blooper Theme"},
    SongName{0x5B, "Miniboss Battle"},
    SongName{0x5C, "Monstar Theme"},
    SongName{0x5D, "Club 64"},
    SongName{0x5E, "Unused Opening"},
    SongName{0x5F, "Bowser's Castle Falls"},
    SongName{0x60, "Star Haven"},
    SongName{0x61, "Shooting Star Summit"},
    SongName{0x62, "Starship Theme"},
    SongName{0x63, "Star Sanctuary"},
    SongName{0x64, "Bowser's Castle"},
    SongName{0x65, "Bowser's Castle Caves"},
    SongName{0x66, "Bowser Theme"},
    SongName{0x67, "Bowser Battle"},
    SongName{0x68, "Peach Wishes"},
    SongName{0x69, "File Select"},
    SongName{0x6A, "Main Theme"},
    SongName{0x6B, "Bowser Attacks"},
    SongName{0x6C, "Mario Falls"},
    SongName{0x6D, "Peach Appears"},
    SongName{0x6E, "The End"},
    SongName{0x6F, "Recovered Star Rod"},
    SongName{0x70, "Twink Theme"},
    SongName{0x71, "Stirring Cake"},
    SongName{0x72, "Gourmet Guy Freakout"},
    SongName{0x73, "Prisoner Peach Theme"},
    SongName{0x74, "Peach Mission"},
    SongName{0x75, "Peach Sneaking"},
    SongName{0x76, "Peach Caught"},
    SongName{0x77, "Peach Quiz Intro"},
    SongName{0x78, "Star Spirit Theme"},
    SongName{0x79, "Penguin Whodunit"},
    SongName{0x7A, "Penguin Wakes Up"},
    SongName{0x7B, "Magic Beanstalk"},
    SongName{0x7C, "Merlee Spell"},
    SongName{0x7D, "Lakilester Theme"},
    SongName{0x7E, "Goomba Bros Retreat"},
    SongName{0x7F, "Sunshine Returns"},
    SongName{0x80, "Riding the Rails"},
    SongName{0x81, "Riding the Whale"},
    SongName{0x82, "New Partner"},
    SongName{0x83, "Dry Dry Ruins Appear"},
    SongName{0x84, "Candy Canes"},
    SongName{0x85, "Playroom"},
    SongName{0x86, "Moustafa Theme"},
    SongName{0x87, "Game Over"},
    SongName{0x88, "Taking Rest"},
    SongName{0x89, "Flower NPC Theme"},
    SongName{0x8A, "Flower Gate Appears"},
    SongName{0x8B, "Battle End"},
    SongName{0x8C, "Pop Diva Song"},
    SongName{0x8D, "Boo Minigame"},
    SongName{0x8E, "Level Up"},
    SongName{0x90, "Parade Day"},
    SongName{0x91, "Parade Night"},
    SongName{0x94, "Mario Bros House"},
    SongName{0x95, "Intro Story"},
    SongName{0x96, "New Partner (Japanese)"},
};

// Lists of a track's number and a level. The game's code at 0x80055EB4 gives them by number.
constexpr std::size_t kPaperMarioVillageLevels = 0x541B8;
constexpr std::size_t kPaperMarioTunnelLevels = 0x541CC;
constexpr std::size_t kPaperMarioCastleFirst = 0x541F8;
constexpr std::size_t kPaperMarioCastleMiddle = 0x54214;
constexpr std::size_t kPaperMarioCastleFinal = 0x54230;

// The game's map scripts change five songs while they play. Toad Town's triggers pick a mix by the player's position,
// at the addresses below in the image. No trigger holds mix 6. Each script that uses them starts the song's first
// version: the calls at 0x7F9138, 0x809130, 0x831418 and 0x8520C0. The code at 0x80056044 gives a mix its near level,
// and the code at 0x80056068 its full level. A building's script gives the mixes with names their full level. The code
// at 0x954004 swaps Dry Dry Desert's linked tracks at the oasis. The code at 0xB2C47C and at 0x892A2C and 0x89FECC sets
// track levels for Yoshi's Village and two rooms of the tunnels. Bowser's Castle sets them in scripts at 0xA4C178 to
// 0xAA9768.
constexpr std::array kPaperMarioChanges = {
    SbnChange{.song = 0x00, .mix = 1, .name = "Tayce T.'s House"}, // 0x8313E0
    SbnChange{.song = 0x00, .mix = 2, .name = "Merlon's House"},   // 0x8090E0
    SbnChange{.song = 0x00, .mix = 3, .name = "the Dojo"},         // 0x7F9118
    SbnChange{.song = 0x00, .mix = 4, .near = true},               // 0x8313F8
    SbnChange{.song = 0x00, .mix = 5, .near = true},               // 0x809110
    SbnChange{.song = 0x00, .mix = 7, .name = "Russ T.'s House"},  // 0x7F9100
    SbnChange{.song = 0x00, .mix = 8, .near = true},               // 0x8090F8
    SbnChange{.song = 0x00, .mix = 9, .name = "the Warehouse"},    // 0x8520A0
    SbnChange{.song = 0x17, .linked = true, .name = "the Oasis"},
    SbnChange{.song = 0x26, .levels = kPaperMarioVillageLevels, .name = "full levels"},
    SbnChange{.song = 0x4B, .levels = kPaperMarioTunnelLevels, .name = "cold rooms"},
    SbnChange{.song = 0x64, .levels = kPaperMarioCastleFirst, .name = "first areas"},
    SbnChange{.song = 0x64, .levels = kPaperMarioCastleMiddle, .name = "middle areas"},
    SbnChange{.song = 0x64, .levels = kPaperMarioCastleFinal, .name = "final areas"},
};

// Paper Mario's tables, in its image.
constexpr SbnProfile kPaperMario = {.output_rate = 32006,
                                    .ticks_per_beat = 0x53910,
                                    .envelope_times = 0x539A0,
                                    .envelope_time_count = 95,
                                    .custom_times = 0x53958,
                                    .custom_time_count = 40,
                                    .default_envelope = 0x5394C,
                                    .names = kPaperMarioNames,
                                    .changes = kPaperMarioChanges};

const std::array kProfiles = {
    // Chameleon Twist (USA, Rev 1). The game's code gives the sequence player its volume from a table of 8 bytes for
    // each song at 0x800FB0B0 (0xD64B0 in the image), through 0x8008B4F4. Its player has the SDK's oscillators at
    // 0x80084CA0. Its synthesizer has the SDK's echo on the auxiliary bus.
    GameProfile{.sha1 = "4ca6d563131b4809fe1748335182816a024999d4",
                .title = "Chameleon Twist",
                .libultra = {.output_rate = 44095,
                             .sequences = 0x27E930,
                             .bank = 0x143210,
                             .volumes = {.offset = 0xD64B2, .stride = 8, .count = 30},
                             .oscillators = true,
                             .effect = BusEffect::kEcho,
                             .names = kChameleonTwistNames}},

    // Chameleon Twist 2 (USA). A run of the game's code gives its player a volume of 0x3FFF for every song. Its player
    // has no oscillators. Its synthesizer has the SDK's big room on the auxiliary bus. The game's code at 0x800CD70C
    // sets every program's bend range to 1216 cents.
    GameProfile{.sha1 = "9fa379d66b218228da3cbf386c91d857c677f489",
                .title = "Chameleon Twist 2",
                .libultra = {.output_rate = 22047,
                             .sequences = 0x120190,
                             .bank = 0xD7B90,
                             .line_ramps = true,
                             .volume = 0x3FFF,
                             .program_bend_range = 1216,
                             .effect = BusEffect::kBigRoom,
                             .names = kChameleonTwist2Names}},

    // Cruis'n World (USA). A run of the game's code gives its player a volume of 0x42D7 for every song. Its player has
    // no oscillators. Its synthesizer has the SDK's small room on the auxiliary bus.
    GameProfile{.sha1 = "6da1a6a2bda687d50e798d80c342948ad1738202",
                .title = "Cruis'n World",
                .libultra = {.output_rate = 22047,
                             .sequences = 0x82CF10,
                             .bank = 0x835C10,
                             .volume = 0x42D7,
                             .effect = BusEffect::kSmallRoom,
                             .names = kCruisnWorldNames}},

    // GoldenEye 007 (USA). The game's code gives its player a volume for each song, from a table in its data. Its
    // players have no oscillators. Its synthesizer has a custom effect of six sections on the auxiliary bus. The
    // stages'
    // second tracks play on a second player at the same time, when the game turns them up.
    GameProfile{.sha1 = "abe01e4aeb033b6c0836819f549c791b26cfde83",
                .title = "GoldenEye 007",
                .libultra = {.output_rate = 22047,
                             .bank = 0x3B4450,
                             .rare_sequences = true,
                             .line_ramps = true,
                             .volume_list = kGoldenEyeVolumes,
                             .effect = BusEffect::kCustom,
                             .names = kGoldenEyeNames}},

    // Pokémon Snap (USA). The game's code gives each player a volume of 0x7F00 at the start of a scene, and fades it
    // from there. HAL's player has the SDK's oscillators at 0x80020364, and its volume arithmetic at 0x8003144C. Its
    // synthesizer has a custom effect of eight sections.
    GameProfile{.sha1 = "edc7c49cc568c045fe48be0d18011c30f393cbaf",
                .title = "Pokémon Snap",
                .libultra = {.output_rate = 32006,
                             .sequences = 0xAEFC10,
                             .bank = 0xAFEEE0,
                             .player = LibultraPlayer::kHal,
                             .volume = 0x7F00,
                             .oscillators = true,
                             .effect = BusEffect::kCustom,
                             .names = kPokemonSnapNames}},

    // Hey You, Pikachu! (USA). The game's code at 0x800059E4 reads its 45 standard MIDI files on a player of its
    // developer. The synthesizer's bus has a custom effect of six sections.
    GameProfile{.sha1 = "da1a1e47a86720f9d54fb2d2d247480041bda824",
                .title = "Hey You, Pikachu!",
                .libultra = {.output_rate = 32006,
                             .sequences = 0xB1422C,
                             .bank = 0x84F5F0,
                             .midi_files = 45,
                             .player = LibultraPlayer::kAmbrella,
                             .line_ramps = true,
                             .effect = BusEffect::kCustom,
                             .names = kHeyYouPikachuNames,
                             .rate_changes = kHeyYouPikachuRates}},
    GameProfile{.sha1 = "8173866fc8c7652abd44c48efcab85441c6806a1",
                .title = "Pok\u00e9mon Puzzle League",
                .libmus = &kPuzzleLeague},

    // Pokémon Stadium (USA, Rev 2).
    GameProfile{.sha1 = "0d3b1d740c6ee6da6923550bf24ad827262fd8c0", .title = "Pokémon Stadium", .libmus = &kStadium},

    // Mario Tennis (USA).
    GameProfile{.sha1 = "999047f07cec931ffbdcc7b33b8502ef602807ee", .title = "Mario Tennis", .libmus = &kMarioTennis},

    // Paper Mario (USA).
    GameProfile{.sha1 = "3837f44cda784b466c9a2d99df70d77c322b97a0", .title = "Paper Mario", .sbn = &kPaperMario},

    // Pokémon Stadium 2 (USA).
    GameProfile{.sha1 = "d8343e69a7dc63b869cf6361d87cde64444281d3", .title = "Pokémon Stadium 2", .ead = &kStadium2},
    GameProfile{.sha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce", .title = "Super Mario 64", .ead = &kSuperMario64},
    GameProfile{.sha1 = "579c48e211ae952530ffc8738709f078d5dd215e", .title = "Mario Kart 64", .ead = &kMarioKart64},
    GameProfile{.sha1 = "41b3bdc48d98c48529219919015a1af22f5057c2",
                .title = "The Legend of Zelda: Ocarina of Time",
                .ead = &kOcarinaOfTime},
    GameProfile{.sha1 = "d6133ace5afaa0882cf214cf88daba39e266c078",
                .title = "The Legend of Zelda: Majora's Mask",
                .ead = &kMajorasMask},
    GameProfile{.sha1 = "508dfc2d4caa42b6f6de5263d0aed5e44ac7966a", .title = "Wave Race 64", .ead = &kWaveRace64},
};

} // namespace

const GameProfile* FindProfile(const std::string& sha1)
{
    for (const GameProfile& profile : kProfiles)
    {
        if (sha1 == profile.sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

} // namespace ntdlib::sequence_export::n64
