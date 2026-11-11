// SPDX-License-Identifier: MIT

// The tracks that known 3DS games play when a sound starts. A game's code can quiet some of a sound's tracks through
// the sound's handle as the sound starts. The lists below find a sound by the SHA-1 of its sound archive and its name.
// They give every track for a sound that they leave out, and for every sound of an unknown archive. `archive_sha1` is
// the SHA-1 of the sound archive in 40 lowercase hex digits.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ntdlib::ctr::snd
{

// The tracks of stream sound `sound` that a known game plays when the sound starts, bit N for track N. The game's code
// sets the other tracks to a volume of 0 through the sound's handle. Engine::StartStreamSound takes the mask.
uint16_t StartingStreamTracks(std::string_view archive_sha1, std::string_view sound);

// A variable that a game sets for a sequence, through the sound's handle or through another sound that plays with it.
// Indices 0 to 15 are the sequence's player variables, and 16 to 31 the global variables. Indices 32 to 47 are track
// variables: the game sets track variable index - 32 of every track of the sequence.
struct SequenceVariable
{
    friend bool operator==(const SequenceVariable&, const SequenceVariable&) = default;

    // The index of an entry that sets no variable.
    static constexpr uint8_t kNone = 0xff;

    uint8_t index = kNone;
    int16_t value = -1;

    // The sound frame at whose start the variable takes the value, before the frame's ticks. Frame 0 is the frame of
    // the sequence's first tick: the variable has the value as the sequence starts.
    uint32_t frame = 0;
};

// The number of variables that a SequenceStart can set: 17. New Leaf's DJ interludes take the most. Each sets player
// variable 0 to start, and the 16 track variables that hold the town's tune.
constexpr std::size_t kStartVariables = 17;

// The item ID of an empty bank slot in SequenceStart::banks.
constexpr uint32_t kNoBank = 0xffffffff;

// The tracks of a sequence: 16.
constexpr std::size_t kSequenceTracks = 16;

// Every track at a volume of 1: the volumes that the sound library gives a sequence's tracks as the sound starts.
constexpr std::array<float, kSequenceTracks> kFullTrackVolumes = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                                  1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

// How a game starts a sequence: the tracks that it quiets, bit N for track N, the volumes that it gives the other
// tracks, the variables that it sets, the state of the random generator, the tempo, pitch and volume that it sets, the
// banks that it gives the sequence, and the sends and the effect that it gives the sequence on the aux buses. A track
// in neither mask plays at its usual volume times its entry in `track_volumes`.
struct SequenceStart
{
    friend bool operator==(const SequenceStart&, const SequenceStart&) = default;

    // The value of game_effect for a sequence that plays with the effects that the game runs for the whole archive.
    static constexpr uint8_t kArchiveEffects = 0xff;

    // The tracks that the game mutes (SequenceSoundHandle::SetTrackMute). They run their commands, and none of their
    // notes starts.
    uint16_t muted = 0;

    // The tracks that the game silences (SequenceSoundHandle::SetTrackSilence) or sets to a volume of 0. Their notes
    // start and take their voices, at a volume of 0.
    uint16_t silenced = 0;

    // The volumes other than 0 that the game gives the tracks (SequenceSoundHandle::SetTrackVolume), track 0 first, as
    // factors of amplitude. The driver multiplies each into the volume of the track's channels (UpdateChannelParam,
    // 0x31DE18 in Pokémon X's code.bin). A volume of 0 here plays as the track's bit in `silenced`.
    std::array<float, kSequenceTracks> track_volumes = kFullTrackVolumes;

    // The variables that the game sets as the sequence starts or plays, in the order of their frames. Entries with the
    // index kNone set nothing. A reader reads every entry and skips those.
    std::array<SequenceVariable, kStartVariables> variables = {};

    // The random generator's state at the sequence's first tick, for an effect that the game plays only on some draws.
    // The sequence's first random command draws the number after it. Without a value, the player's state holds.
    std::optional<uint32_t> seed = std::nullopt;

    // The tempo ratio that the game sets (SequenceSoundHandle::SetTempoRatio). The sequence's ticks come at its tempo
    // times the ratio.
    float tempo_ratio = 1.0f;

    // The pitch that the game sets (SoundHandle::SetPitch), as a ratio of frequencies. Every voice of the sequence
    // plays at its pitch times the ratio.
    float pitch = 1.0f;

    // The volume that the game sets (SoundHandle::SetVolume), as a factor of amplitude. It multiplies everything that
    // the sequence plays.
    float volume = 1.0f;

    // The banks that the game gives the sequence's four bank slots in place of the archive's, as item IDs: nw::snd's
    // SeqSoundInfo, which a StartInfo with flag 0x10 holds. The four replace all of the archive's slots, and kNoBank
    // leaves a slot empty. Without a value, the archive's banks hold.
    std::optional<std::array<uint32_t, 4>> banks = std::nullopt;

    // The sends on aux buses A and B that the game gives the sound: what the sound's handle (BasicSound::SetFxSend) and
    // its sound player (SoundPlayer::SetFxSend) set, added together (BasicSound's update, 0x52B7F8 to 0x52B818 in
    // Animal Crossing: New Leaf's code.bin). The driver adds them to each track's sends (SequenceTrack::
    // UpdateChannelParam: Pokémon X 0x31E03C to 0x31E068, New Leaf 0x533F84 to 0x533FB0).
    std::array<float, 2> fx_send = {};

    // The effect that the game runs on the aux buses while the sequence plays, by the game's number for it, or
    // kArchiveEffects. A tool looks the number up in its table of the game's effects.
    uint8_t game_effect = kArchiveEffects;
};

// How a known game starts sequence sound `sound`: the tracks that it quiets, the tracks' volumes, the variables that it
// sets, the random generator's state, the tempo, pitch and volume, the banks, and the sends and the effect on the aux
// buses. A player gives the sequence the banks as it starts it. It applies the masks, the tracks' volumes, the
// variables of frame 0, the tempo ratio, the pitch, the volume and the sends after the sequence's start and before its
// first tick.
SequenceStart StartingSequenceTracks(std::string_view archive_sha1, std::string_view sound);

// Another state of a sequence that a known game's code moves the sequence to as the game goes on. A rip writes the
// sequence once more for each state, after the sequence itself, with `start` in place of StartingSequenceTracks. The
// name of the state's file is the sequence's name followed by `suffix`.
struct SequenceArrangement
{
    std::string_view suffix;
    SequenceStart start;
};

// The later states of sequence sound `sound` in a known game, in the order in which the game plays them. An unlisted
// sound or archive has none.
std::span<const SequenceArrangement> SequenceArrangements(std::string_view archive_sha1, std::string_view sound);

// The states that a known game gives every sequence whose bank slot 0 holds `first_bank`, an item ID, in the order in
// which the game plays them. A game's code can start a sequence with other banks by the bank that the sequence names.
// Each state's start holds only the banks. A rip plays them with the rest of the sequence's start
// (StartingSequenceTracks), and writes these states after those of SequenceArrangements. An unlisted bank or archive
// has none.
std::span<const SequenceArrangement> BankArrangements(std::string_view archive_sha1, uint32_t first_bank);

} // namespace ntdlib::ctr::snd
