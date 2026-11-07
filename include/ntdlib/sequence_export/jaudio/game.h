// SPDX-License-Identifier: MIT

// A game's JAudio sound files, from a folder or from the files of a disc or a title, and the sequences to convert. A
// JAudio2 game has a start-up archive (BAA), a sequence archive and wave archives. A JAudio1 game, such as The Wind
// Waker, has another kind of start-up archive (AAF, ntdlib/jaudio/aaf.h), a sequence archive or table, wave archives
// and, on the Wii U, a file of its sounds' names.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"

namespace ntdlib::sequence_export::jaudio
{

// A state in which a game's code plays a sequence, through the sound's handle: the volumes of the tracks that the
// handle reaches, the tempo ratio, the ports that the code writes to the root track once the sequence runs, the
// fade-in that its start gives the sound, the tracks that the code mutes, and the statuses that it writes by section
// (SequenceSound::child_volumes, tempo_ratio, game_ports, fade_in, tracks and status_by_section). Index n of
// child_volumes is the root track's child n in a JAudio1 sequence, and JAISeq::getChildTrack(n) in a JAudio2 sequence:
// child n % 16 of the root's child n / 16.
struct SequenceState
{
    std::map<uint8_t, float> child_volumes = {};
    float tempo_ratio = 1.0f;
    std::map<uint8_t, uint16_t> game_ports = {};
    uint16_t fade_in = 0;
    TrackMask tracks = kEveryTrack;
    std::map<uint16_t, uint16_t> status_by_section = {};
};

// Another state in which the game plays a sequence of its sound table. The sequence converts once more in that state,
// right after the sequence itself, named after the sequence with "_" and `suffix` added.
struct Arrangement
{
    uint32_t sound = 0; // the sound's ID (SoundEntry::Id)
    std::string suffix;
    SequenceState state;
};

// What a game's code does with its sounds that its start-up archive doesn't say. An unknown game gets the defaults.
struct GameRules
{
    // The volumes, other than 1, that the game's code gives its effect categories. A category is a group of the sound
    // table. The sound library multiplies an effect's volume by the volume of the effect's category.
    std::map<uint8_t, float> category_volumes;

    // The volume, other than 1, that the game's code gives the sound library's sequences: the sound table's sounds that
    // play a file of the sequence archive or a sequence block. The library multiplies such a sound's volume by it.
    float sequence_volume = 1.0f;

    // The direct release that the sound library gives an oscillator with neither a release table nor a direct release
    // (OscillatorState::Release). With 0, such an oscillator stops at once when its note is released.
    uint16_t default_release = 0;

    // The echo that the game's code sets up, or nothing. A sequence that sends to it ends when the echo falls silent.
    std::optional<EchoLine> echo;

    // The level of the game's mix, against Super Mario Sunshine's. The sound library multiplies each channel's volume
    // by a channel level, and the DSP program multiplies its mix by a DSP level. The game's code sets both at start-up.
    // At 1, a conversion plays at the level of Super Mario Sunshine's mix. The performer multiplies every channel's
    // volume by this value.
    float mix_level = 1.0f;

    // The states in which the game's code starts its sequences, by the sounds' IDs (SoundEntry::Id). A sequence that
    // it doesn't list starts with every track, a tempo ratio of 1 and no ports.
    std::map<uint32_t, SequenceState> start_states;

    // The values that the game's code writes to the ports of some sounds as it starts them, by the sounds' IDs
    // (SoundEntry::Id). Each sound plays the values that the game's code gives it first (SequenceSound::ports).
    std::map<uint32_t, std::map<uint8_t, uint16_t>> ports;

    // The other states in which the game plays its sequences, in the order in which they convert.
    std::vector<Arrangement> arrangements;

    // The sounds that the game's code never plays as the sound table gives them, by their IDs. Only their arrangements
    // are listed.
    std::vector<uint32_t> only_arranged;

    // The JAudio1 effects that play a key that a random number picks, by their IDs, and the keys that make a sound in
    // the game. Each key comes with the draws from the generator's first state before the first draw that picks it
    // (SequenceSound::draws). Such an effect is listed once for each key, in its place. Its name then ends in "_key_"
    // and the key. The keys that make no sound in the game get no entry.
    struct RandomKey
    {
        int key = 0;
        uint32_t draws = 0;

        bool operator==(const RandomKey&) const = default;
    };
    std::map<uint32_t, std::vector<RandomKey>> random_keys;

    // The JAudio1 effects that draw a random number and end at once on some draws, by their IDs. Each comes with the
    // draws from the generator's first state before the first draw on which it plays (SequenceSound::draws). The first
    // draw ends each of them.
    std::map<uint32_t, uint32_t> random_starts;

    // The sounds of a group that the game's code starts with other values at some ports at other times. Each sound of
    // the group is listed a second time with those values, after the sound and its arrangements. Its name then ends in
    // `suffix`. The variant counts only if the sound's sequence reads one of the ports (SequenceSound::variant_ports).
    struct PortVariant
    {
        uint16_t group = 0; // the section in the high byte and the group in the low byte, as for unplayed_groups
        std::string suffix;
        std::map<uint8_t, uint16_t> ports;
    };
    std::vector<PortVariant> port_variants;

    // The name of the file in the sequence archive that a JAudio1 game's code starts as its effects sequence. Empty
    // when the effects sequence is the sound table's first sequence.
    std::string effects_sequence;

    // Whether a JAudio1 game plays an effect at the volume and the pitch that the effect's entry in the sound table
    // gives (JAIBasic::setSeExtParameter). Luigi's Mansion's sound class replaces that function with one that gives
    // effects neither.
    bool effect_parameters = true;

    // Whether a JAudio1 game squares a track's volume in volume mode 0, as JASystem::TTrack::updateTrack does in The
    // Wind Waker (0x80281b80 in its main.dol). Luigi's Mansion's older JASystem plays the volume as it is (0x801a6ae8).
    bool squared_track_volume = true;

    // Whether a JAudio1 game's generator mixes the CPU's time base into each random number. Luigi's Mansion's does
    // (0x801a35e0). No state gives the game's numbers then. Such a game's sequences draw from JAudio2's generator, from
    // PerformOptions::seed (SequenceSound::timed_random).
    bool timed_random = false;

    // Whether a JAudio1 game's notes play the oscillators that their tracks route to them (oscroute), as
    // JASystem::TTrack does in The Wind Waker (0x802814ac in its main.dol). Pac-Man Vs. and Pikmin 2 have the same
    // commands. Luigi's Mansion's older JASystem keeps the routes as bytes (0x801a87a0). Its simpleadsr puts the last
    // value in a release table, not in the direct release (0x801aa1a0). The rules leave its routes out.
    bool track_oscillator_routes = true;

    // Whether the game's sound table has the first layouts (FirstLayoutOf). Mario Kart: Double Dash!!'s table has them.
    // The volumes are then in 255ths: the game divides an entry's ninth byte by 255 (0x80120080 in the USA release's
    // main.dol). The game's music plays the start-up archive's sequence blocks.
    bool first_layouts = false;

    // Whether the game's code reads the sequence archive in the start-up archive's Seqs folder. Mario Kart: Double
    // Dash!!'s disc holds AudioRes/Seqs/JaiSeqs.arc. The game's code never names it. The archive holds the start-up
    // archive's 12 sequence blocks.
    bool sequence_archive = true;

    // The answers of the sequence callback that the game's code registers, by the argument of JAudio2's command 0xf9.
    // The callback answers 0 to an argument that isn't here. Without a callback, the sound library's handler of the
    // command writes 0xffff. Four Swords Adventures registers none.
    std::optional<std::map<uint16_t, uint16_t>> callback;

    // Whether the game's code mutes child 15 of a track that calls it with ID 0 (SequenceSound::mute_child_15).
    bool mute_child_15 = false;

    // Whether a JAudio1 game's name file names the sound table's sequences. Without its names, each sequence takes the
    // name of its file in the sequence archive.
    bool sequence_names = true;

    // The groups of the sound table whose sounds the game's code never starts as sequences: the section in the high
    // byte and the group in the low byte. Such a sound's ID picks an entry of another table, and its sequence is a
    // stand-in. These sounds are left out. Their files count as played. Super Mario Galaxy's multi BGMs are one group.
    std::set<uint16_t> unplayed_groups;

    // Whether the game's rhythm mutes child 0 of a piece's track 0 as the game starts the piece: a piece whose switches
    // (SoundEntry::switches) set bit 0.
    bool rhythm_mutes_child_0 = false;

    // A table of volumes that the game's code reads at start-up and writes over the sound table's: a JMap table (BCSV)
    // in a RARC archive near the start-up archive. Each row gives a sound's name and its volume in 127ths. The volume
    // replaces that of a sound whose type has bit 0x40. Super Mario Galaxy 2's AudioRes/Info/BgmParam.arc is one.
    struct VolumeTable
    {
        std::string archive;      // the archive's path from the start-up archive's folder, with '/' between folders
        std::string file;         // the table's file in the archive
        std::string name_field;   // the field that holds a sound's name
        std::string volume_field; // the field that holds its volume
    };
    std::optional<VolumeTable> volume_table;

    // Pikmin's effects, which its code starts in two sequences without a sound table (EffectRequest::kPort). The
    // game's main.dol has a catalogue of effects at `catalogue`, `count` rows of 20 bytes: the effect's number at +4,
    // the address of its name at +8, and its kind at +16. Kind -2 is an effect of the system, which the effects
    // sequence's track `system_track` plays. Kind -1 is the player's, on `player_track`. A number of the player's from
    // 0x8000 goes to port 1 without its top bit, and any other to port 0. Any other kind is an event's type. The
    // sequence `events_sequence` plays the events' effects. Its first track's child 0 takes an effect's number at port
    // 0. The table at `event_offsets` gives each type's first action. The table of actions follows its 8 words. A row's
    // number counts from its type's first action. An action's 6 bytes give its effect's number at +4. An effect of
    // the events that several rows play comes once. The events' sequence isn't listed as music. The game's code can
    // also start an effect by writing ports of a track of the effects sequence (`port_effects`).
    struct PortEffect
    {
        std::string name{};
        uint8_t track = 0;                   // the child of the effects sequence's first track
        uint8_t port = 0;                    // the port that starts the effect
        uint16_t number = 0;                 // the value that the game's code writes to it
        std::map<uint8_t, uint16_t> ports{}; // the values that it writes to the track's other ports first
    };
    struct JacEffects
    {
        std::string dol_sha1; // the main.dol that holds the tables
        uint32_t catalogue = 0;
        uint32_t count = 0;
        uint32_t event_offsets = 0;
        std::string events_sequence;
        uint8_t system_track = 0;
        uint8_t player_track = 0;
        std::vector<uint16_t> left_out;                // effects of the system that aren't listed
        std::map<uint16_t, uint16_t> played_by_player; // effects of the system that play an effect of the player's
        std::vector<uint16_t> other_system;            // effects of the system that the catalogue lacks
        std::vector<uint16_t> other_player;            // and the player's
        std::vector<PortEffect> port_effects{};
    };
    std::optional<JacEffects> jac_effects;

    // Whether each file of the sequence archive that no sound plays starts with the bank of its number in the archive
    // (SequenceSound::bank).
    bool bank_of_number = false;

    // A file of the sequence archive that plays several pieces. The game's code picks one through the root track's
    // ports. The file is listed once for each part, with the part's ports (SequenceSound::ports). Its name then ends in
    // the part's suffix.
    struct Part
    {
        std::string suffix;
        std::map<uint8_t, uint16_t> ports;
    };
    std::map<std::string, std::vector<Part>> parts; // by the file's name in the archive

    // The volumes that the game's code gives children of the first track of a file of the sequence archive that no
    // sound plays (SequenceSound::child_volumes), by the file's name in the archive.
    std::map<std::string, std::map<uint8_t, float>> child_volumes;

    // The answers that the game's code gives the calls that the tracks of a file of the sequence archive make to the
    // game (command 0xe7), by the call's ID (SequenceSound::sync_answers), by the file's name in the archive.
    std::map<std::string, std::map<uint16_t, uint16_t>> sync_answers;

    // The children of the first track of a file of the sequence archive that the game's code mutes as they start, a bit
    // for each child (SequenceSound::muted_children), by the file's name in the archive.
    std::map<std::string, uint16_t> muted_children;

    // The conductors in AudioRes/Conductor.arc that play files of the sequence archive (SequenceSound::conductor): the
    // conductor's name in that archive, by the file's name.
    std::map<std::string, std::string> conductors;

    // The volumes, out of 127, at which the game's code plays files of the sequence archive, by the file's name. Each
    // takes the place of the volume of the sound that plays the file (SequenceSound::volume).
    std::map<std::string, uint8_t> piece_volumes;

    // Whether the sequences play under the later JAudio1 (SequenceSound::later_jaudio1).
    bool later_jaudio1 = false;

    // The rate at which the game runs its tracks' IIR filters, in Hz.
    double filter_rate = kOutputRate;
};

// The volumes that the table `table` gives, by the sounds' names, from its archive `archive` (compressed with Yaz0 or
// not). The game writes the low byte of each value. Throws FormatError if the archive or the table can't be read.
std::map<std::string, uint8_t> ReadVolumeTable(std::span<const uint8_t> archive, const GameRules::VolumeTable& table);

// How to open a game.
struct GameOptions
{
    // List a JAudio1 game's effects too, after the other sequences, in the sound table's order. The other sequences
    // keep their places. Each effect plays the effects sequence and starts in the first slot of its category
    // (SequenceSound::effect). The start-up archive's table of scenes gives each category its count of slots
    // (ntdlib::jaudio::AafEffectSlotCounts). Without the table, each category has the four slots of JAudio1's default
    // table of categories. An effect takes its name from the name file. Without one, it's named after its category and
    // index, as "se4-0123". It plays at its volume in the sound table, times its category's volume
    // (GameRules::category_volumes).
    bool jaudio1_effects = false;

    // The game runs on the Wii U, in a port of a GameCube or Wii game's sound code, such as Twilight Princess HD. A
    // port can share its start-up archive with the game that it ports. A port's rules follow the port's code where it
    // differs, such as Twilight Princess HD's echo (GameRules::echo).
    bool wii_u = false;
};

// The rules of the game whose start-up archive has the SHA-1 `archive_sha1`, as 40 lowercase hex digits, when the game
// opens with `options`. A compressed archive's SHA-1 is that of its decoded data (Game::ArchiveSha1).
GameRules RulesOf(const std::string& archive_sha1, const GameOptions& options = {});

class Game
{
public:
    // Open the start-up archive at `baa`, a BAA or an AAF, compressed with Yaz0 or not. The game keeps its sequence
    // archive in a Seqs folder beside it, and its wave archives in a Waves folder (JAudio2) or a Banks folder
    // (JAudio1), or in the other folder if it has no such folder. The reader also looks beside the archive itself. The
    // Wind Waker HD keeps its sequences in a sequence table (.bdt) in Seqs, and its sounds' names in a name file (.asn)
    // beside the archive. `program` is the name that the warnings give the program, such as "xyz2midi". `options` says
    // which sounds to list. Throw FormatError if the archive can't be read.
    static Game FromFolder(const std::filesystem::path& baa, const std::string& program,
                           const GameOptions& options = {});

    // Open the start-up archive among the files of a disc or a title that `disc` holds, with `program` and `options`
    // as for FromFolder. The archive is a .baa or .aaf file, a file compressed with Yaz0 (.szs) beside a Seqs folder,
    // or a file in one of the RARC archives that hold a game's start-up archive, such as Super Mario Sunshine's
    // data/nintendo.szs. Pikmin has no start-up archive. Its bank file (.bx) in a Banks folder takes the archive's
    // place, with the sequence index in the disc's main.dol. The index names the file of sequences in the Seqs folder
    // beside it. Return nothing if the disc has none of these. Throw FormatError if the archive can't be read.
    static std::optional<Game> FromDiscFiles(std::shared_ptr<ntdlib::disc::GameFiles> disc, const std::string& program,
                                             const GameOptions& options = {});

    const SoundArchive& Archive() const
    {
        return *archive_;
    }

    // The start-up archive's path among the files of a disc or a title (FromDiscFiles), or the path of the RARC
    // archive that holds it. It's empty for a game from FromFolder.
    const std::string& DiscPath() const
    {
        return disc_path_;
    }

    // The SHA-1 of the start-up archive as the game reads it, decoded if it's compressed, as 40 lowercase hex digits.
    // RulesOf takes it.
    const std::string& ArchiveSha1() const
    {
        return archive_sha1_;
    }

    // The sequences to convert: the sounds of the sound table that play sequences, in its order, then the files of the
    // sequence archive that no sound plays, named after their files. A JAudio1 game's effects play in its effects
    // sequence. The list leaves that sequence out. It leaves the effects out too, unless GameOptions::jaudio1_effects
    // lists them at its end. Its sequences take their names from the name file or their files.
    const std::vector<SequenceSound>& Sequences() const
    {
        return sequences_;
    }

    // Read a wave archive by name, or return nothing if it can't be found.
    std::optional<std::vector<uint8_t>> ReadWaveArchive(const std::string& name) const;

    // What the reader found missing or damaged, such as a missing sequence archive.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // Read the start-up archive in `bytes`, and look up its game's rules. Throw FormatError if the archive can't be
    // read.
    void OpenArchive(std::vector<uint8_t> bytes);

    // Read Pikmin's bank file in `bytes` (ntdlib/jaudio/bx.h), and look up its game's rules. Throw FormatError if the
    // file can't be read.
    void OpenBankFile(std::vector<uint8_t> bytes);

    // List the sequences once the archive and the sequence archive are read.
    void FindSequences();

    // The sequence of JAudio1 effect `s`, which plays in the effects sequence `sequence` (GameOptions).
    SequenceSound EffectSound(const SoundEntry& s, std::span<const uint8_t> sequence) const;

    // Pikmin's effects (GameRules::jac_effects), named after the catalogue. An effect that the catalogue lacks is
    // named "system-" or "player-" and its number in hex.
    std::vector<SequenceSound> JacEffectSounds();

    // Read a JAudio1 game's name file, `names`, named `file` in the warnings. A record of a sequence names the sequence
    // of its type and index, whatever its category bits.
    void ReadNames(std::span<const uint8_t> names, const std::string& file);

    // Read the disc's file at `path`. Throw FormatError if it can't be read.
    std::vector<uint8_t> ReadDiscFile(const std::string& path) const;

    // Read the game's table of volumes (GameRules::volume_table) from its archive, `archive`, or from nothing if the
    // game has none. A table that can't be read leaves the sound table's volumes, with a warning.
    void OpenVolumeTable(const std::optional<std::vector<uint8_t>>& archive);

    std::unique_ptr<SoundArchive> archive_;
    std::string archive_sha1_;
    GameRules rules_;
    GameOptions options_;
    bool jaudio1_ = false;
    bool jac_ = false;                     // Pikmin's sound library (SequenceSound::jac)
    std::optional<uint16_t> effects_file_; // a JAudio1 game's effects sequence, where its AAF or rules name one
    std::optional<std::vector<uint8_t>> effect_slot_counts_; // the effects sequence's slots (AafEffectSlotCounts)
    std::vector<uint8_t> aaf_; // a JAudio1 game's start-up archive, for its sequence index (ReadAafSequenceFiles)
    std::vector<uint8_t> sequence_index_; // Pikmin's sequence index from its executable (ReadBarcSequenceFiles)
    std::vector<uint8_t> sequence_archive_;
    std::string sequence_archive_name_;
    bool sequence_table_ = false;           // the sequences are in a table (ReadSequenceTable), not a RARC
    std::map<uint32_t, std::string> names_; // a JAudio1 game's names, by sound ID (ReadNames)
    std::vector<ArchiveFile> sequence_files_;
    std::vector<std::vector<uint8_t>> decoded_sequences_; // the files of the sequence archive that Yaz0 compresses
    std::map<std::string, uint8_t> table_volumes_;        // GameRules::volume_table's volumes, by the sounds' names
    std::vector<uint8_t> conductor_archive_; // Conductor.arc beside the start-up archive (GameRules::conductors)
    std::vector<SequenceSound> sequences_;
    std::vector<std::string> warnings_;

    // Where the wave archives are: the folders to look in, or the disc's files, their paths by lowercase name.
    std::vector<std::filesystem::path> wave_folders_;
    std::shared_ptr<ntdlib::disc::GameFiles> disc_;
    std::string disc_path_;
    std::map<std::string, std::string> disc_waves_;
};

} // namespace ntdlib::sequence_export::jaudio
