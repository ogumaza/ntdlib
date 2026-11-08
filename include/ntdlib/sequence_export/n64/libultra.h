// SPDX-License-Identifier: MIT

// The games on libultra's sequence players (ALSeqPlayer and ALCSPlayer). A game keeps its songs in compressed or
// standard MIDI and its instruments in a bank file with a table of samples. The performer plays a song as the players
// do. The instruments follow libultra's synthesizer:
//
// - A sample plays at the output rate at its key map's base key, plus the key map's detune. The resampler stops at just
//   under an octave up.
// - A voice's volume is the product of the velocity, the envelope, the sample's volume, the channel's volume and the
//   player's volume. The envelope mixer squares it. Velocity, CC7 and CC11 act squared in SoundFont and SFZ too.
// - The envelope mixer's ramps are multiplicative: straight lines in decibels, ending at a target. A ramp to nothing
//   ends at an amplitude of 1/32768. A voice starts at that amplitude when it has an attack.
// - The mixer's pans are the sin/cos law of MIDI RP-036. A send to the auxiliary bus takes the same law from the dry
//   level.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/midi.h"
#include "ntdlib/n64/seq_file.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/n64/games.h"
#include "ntdlib/sequence_export/n64/instruments.h"

namespace ntdlib::sequence_export::n64
{

// What a sequence player reads for a song.
struct LibultraSong
{
    std::vector<std::vector<ntdlib::n64::MidiEvent>> tracks;
    int division = 48;
};

// What the game's code sets for a song.
struct LibultraSettings
{
    LibultraPlayer player = LibultraPlayer::kSdk;
    bool line_ramps = false; // the library's ramps are straight lines in amplitude
    int volume = 0x7FFF;     // the sequence player's volume
    bool oscillators = false;
    BusEffect effect = BusEffect::kNone;
    double rate = 1.0; // divides the time between the song's events
};

// A recorded song, and the index in the bank file's instruments of each of its presets.
struct LibultraPerformance
{
    Performance performance;
    std::vector<int32_t> instruments;
};

// Decodes a wave's `bytes`, with `book` for VADPCM or as 16-bit samples without one. `rate` plays it at its zone's
// tuning. A `loop` may be null.
std::shared_ptr<const Wave> DecodeWave(std::span<const uint8_t> bytes, const ntdlib::n64::VadpcmBook* book,
                                       const ntdlib::n64::BankLoop* loop, std::string name, uint32_t rate);

// Plays `song` on bank `bank` of `file`.
LibultraPerformance PerformLibultra(const LibultraSong& song, const ntdlib::n64::BankFile& file,
                                    const ntdlib::n64::Bank& bank, const LibultraSettings& settings,
                                    const PerformOptions& options);

// The zones of instrument `index` of `file`, except for their waves: each zone gets the index in `file.waves` of its
// wave in `wave_of_zone`, in the same order.
std::vector<Zone> LibultraZones(const ntdlib::n64::BankFile& file, int32_t index, const LibultraSettings& settings,
                                std::vector<int32_t>& wave_of_zone);

// A game on libultra's sequence players.
class LibultraGame
{
public:
    // Reads the game's sequences and banks from `image`, which stays valid while the game lives. Without a profile,
    // the files are found by their headers. Returns nullptr for an image without them.
    static std::unique_ptr<LibultraGame> Open(std::span<const uint8_t> image, const LibultraProfile* profile);

    std::vector<ntdlib::sequence_export::Sequence> Sequences() const;
    Conversion Convert(std::size_t index, const PerformOptions& options);

private:
    // A bank file, the place in the image of its samples, and its waves, each decoded once.
    struct Banks
    {
        ntdlib::n64::BankFile file;
        std::size_t samples = 0;
        std::map<int32_t, std::shared_ptr<const Wave>> waves;
    };

    LibultraGame() = default;

    // Reads the bank file at `offset` and finds its samples.
    Banks ReadBanks(std::size_t offset) const;

    // Decodes wave `index` of `banks`.
    std::shared_ptr<const Wave> WaveAt(Banks& banks, int32_t index) const;

    // The number of songs that `set` holds.
    std::size_t SetSize(const LibultraSongSet& set) const;

    std::span<const uint8_t> image_;
    LibultraProfile profile_;
    std::vector<std::vector<uint8_t>> songs_;
    std::vector<ntdlib::n64::HudsonSongRecord> records_; // each song's bank and volume, in Hudson's sequence bank
    std::vector<Banks> banks_;                           // the bank file of the profile, then of each other set
};

} // namespace ntdlib::sequence_export::n64
