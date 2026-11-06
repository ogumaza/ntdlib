// SPDX-License-Identifier: MIT

// The games on Nintendo EAD's sound engines. A sequence is a program in three levels: the sequence's script starts
// channels, a channel's script sets its sound and starts note layers, and a layer's script plays notes. The performer
// runs the scripts tick by tick, as the engines' players do. A note's level follows the engines' arithmetic:
//
// - A note plays at the square of its velocity, times the channel's volumes, times the envelope. EadLevels says what
// the
//   envelope and the volumes come to in each build. Velocity, CC7 and CC11 act squared in SoundFont and SFZ. CC7 and
//   CC11 hold the square root of the volumes where they act directly, and the volumes themselves where they act
//   squared.
// - An envelope is a list of straight lines. SoundFont's envelopes can't follow them exactly. A fit keeps each
// segment's
//   end and its energy.
// - A note's release is a straight line too, from where the note stands to nothing.
// - The mixer's pans are the sin/cos law of MIDI RP-036.
// - A note's pitch is the sample's tuning times two to the power of its key's distance from key 39, in twelfths.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/ead_later.h"
#include "ntdlib/n64/ead_script.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/n64/games.h"
#include "ntdlib/sequence_export/n64/instruments.h"

namespace ntdlib::sequence_export::n64
{

// What a preset of a converted sequence plays: a program of a bank, or its drums, with the envelope and the vibrato
// that the channel holds at the note.
struct EadPresetKey
{
    friend bool operator==(const EadPresetKey&, const EadPresetKey&) = default;

    int bank = 0;
    int program = -1;      // -1 for the bank's drums
    int envelope = -1;     // the offset in the sequence of the channel's envelope, or -1 for the instrument's
    int release_rate = -1; // the channel's release rate, or -1 for the instrument's
    int vibrato_rate = 0;  // the channel's vibrato: its rate, its depth and its delay, in the engine's units
    int vibrato_depth = 0;
    int vibrato_delay = 0;
    int drum_pan_weight = 128; // the share of a drum's pan that the channel takes over, out of 128
    int low_pass = 0;          // the row of the later engine's low-pass table that the channel holds, or 0
    int sample_gain = 0;       // the channel's gain on the samples, in sixteenths, or 0 for none
};

// A recorded sequence, and the key of each of its presets.
struct EadPerformance
{
    Performance performance;
    std::vector<EadPresetKey> presets;
};

// How a game runs the engine.
struct EadClock
{
    int updates_per_frame = 4;  // the engine's updates in each audio frame
    double frame_rate = 59.826; // audio frames a second: one for each video field
    double output_rate = 32006.0;
    double wave_rate = 32006.0; // a sample at a tuning of 1 plays this many samples a second
};

// What the performer and the instruments read for a sequence.
struct EadPerformSettings
{
    ntdlib::n64::EadEngine engine = ntdlib::n64::EadEngine::kEarly;
    EadLevels levels = EadLevels::kEarly;
    bool envelope_skip = false;     // an envelope's point with the delay kEnvelopeSkip takes no time
    std::size_t channel_layers = 4; // the layers that a channel holds in the later engine
    bool inverse_vibrato = false;   // a vibrato's scale divides by its curve, in the way of Pokémon Stadium 2's build
    std::span<const uint8_t> sequence;
    std::vector<uint8_t> bank_set;                              // as the table of the sequence's banks holds them
    const std::map<int, ntdlib::n64::EadBank>* banks = nullptr; // every bank of `bank_set`
    EadClock clock;
    double song_volume =
        -1.0; // what the game's code gives the player for the song, or below 0 to leave it to the script
    double player_scale = 1.0;    // the player's scale, until the script sets one
    int variation = -1;           // the value of the sequence's variation commands
    uint16_t silent_channels = 0; // the channels that the game's code fades to nothing
    int tempo_change = 0;         // the later engine's change of tempo that the game's code sets, in beats a minute

    // What the game's code writes to the later engine's ports before the sequence starts. -1 stands for nothing.
    std::array<int, 8> ports = {-1, -1, -1, -1, -1, -1, -1, -1};

    // Every sequence of the game and its banks, for the scripts that load or start another.
    std::span<const std::span<const uint8_t>> sequences;
    std::span<const std::vector<uint8_t>> bank_sets;
};

EadPerformance PerformEad(const EadPerformSettings& settings, const PerformOptions& options);

// The zones of a preset, except for their waves: each zone gets its sample's index in the bank in `sample_of_zone`, in
// the same order. `settings.sequence` holds a channel's envelope, if the key names one.
std::vector<Zone> EadZones(const EadPresetKey& key, const ntdlib::n64::EadBank& bank,
                           const EadPerformSettings& settings, std::vector<int32_t>& sample_of_zone);

// A game on one of EAD's engines.
class EadGame
{
public:
    // Reads the game's tables from `image`, which stays valid while the game lives. Returns nullptr for an image
    // without them.
    static std::unique_ptr<EadGame> Open(std::span<const uint8_t> image, const EadProfile& profile);

    std::vector<ntdlib::sequence_export::Sequence> Sequences() const;
    Conversion Convert(std::size_t index, const PerformOptions& options);

private:
    EadGame() = default;

    const ntdlib::n64::EadBank& BankAt(int number);
    Conversion ConvertTrack(const EadTrack& track, const std::string& name, const PerformOptions& options);
    std::shared_ptr<const Wave> WaveAt(int bank_number, int32_t sample, int gain);

    std::span<const uint8_t> image_;
    EadProfile profile_;
    std::optional<ntdlib::n64::EadLaterFiles> later_files_;
    std::vector<std::span<const uint8_t>> sequences_;
    std::vector<std::span<const uint8_t>> bank_files_;
    std::vector<std::span<const uint8_t>> sample_files_;
    std::vector<std::vector<uint8_t>> bank_sets_;
    std::map<int, ntdlib::n64::EadBank> banks_;
    std::map<std::tuple<int, int32_t>, std::shared_ptr<const Wave>> waves_;
    std::map<std::tuple<int, int32_t, int>, std::shared_ptr<const Wave>> gained_waves_;
};

} // namespace ntdlib::sequence_export::n64
