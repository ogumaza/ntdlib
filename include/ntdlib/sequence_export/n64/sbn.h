// SPDX-License-Identifier: MIT

// The games on Intelligent Systems' sound engine, which keep their sound in an SBN archive. ntdlib's sbn.h gives the
// archive's form. A song holds up to four compositions, and the game's code chooses one. A composition plays phrases in
// turn. A phrase holds up to 16 tracks of notes and commands, and it ends when any of them ends.
//
// The engine's arithmetic:
//
// - The clock runs in frames of 184 samples. Each frame takes 1000 times the tempo from a counter. When the counter
//   reaches 0, it gains 60000 * 32000 / 184 / ticks a beat, and a tick runs. A song's header chooses its ticks a beat.
// - A note at key n plays a wave at 2^((100 n + the track's tunings - the instrument's key base) / 1200) times the
//   instrument's rate / 32000 of the output rate. Above 2, the note plays at 2.
// - A voice's volume is the product of the envelope, the velocity, the track's volume, the instrument's volume and the
//   master volume. The mixer squares it. Velocity, CC7 and CC11 act squared in SoundFont and SFZ too.
// - An envelope is a list of steps. Each holds a time, from a table in the game's code, and a level. The mixer moves in
//   a straight line of amplitude to the square of each level.
// - A track has up to four voices. A track of one voice cuts its note for the next. A track of more takes a free voice,
//   or else cuts the one that has been released, or else the note with the least time left.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/n64/sbn.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/n64/games.h"
#include "ntdlib/sequence_export/n64/instruments.h"

namespace ntdlib::sequence_export::n64
{

// What the game's code sets for a song.
struct SbnSettings
{
    uint32_t output_rate = 32006;
    int ticks_per_beat = 48;
    int variation = 0;
    int mix = 0;          // the value that the song's branches read
    bool linked = false;  // the linked tracks play in place of the tracks that they link to
    int proximity = 0x7F; // the level that the game's code gives a mix's tracks

    // A list of a track's number from 1 and a level. The game's code fades the tracks to these a few frames after it
    // starts the song. A number of 0 ends the list.
    std::span<const uint8_t> track_levels;

    // The game's table that turns the first byte of a custom envelope's step into an index of the table of times.
    std::span<const uint8_t> custom_times;
};

// What a voice's life takes from an instrument.
struct SbnInstrumentFacts
{
    bool exists = false;
    int key_base = 0; // in cents
    uint32_t sample_rate = 0;
    double samples = 0.0; // the wave's length, if it doesn't loop
    bool loops = false;
    double release_seconds = 0.0;
};

// Gives the facts of the instrument that a bank and a patch name, with the bank in the high byte.
using SbnFacts = std::function<SbnInstrumentFacts(uint16_t bank_patch)>;

// What a preset plays.
struct SbnPresetKey
{
    friend bool operator==(const SbnPresetKey&, const SbnPresetKey&) = default;

    bool drums = false;      // the preset holds a zone for each drum that the song plays
    uint16_t bank_patch = 0; // for an instrument
    int fine_cents = 0;
    bool cuts = false;          // a track of one voice: a new note cuts the one before
    std::vector<uint8_t> press; // a custom envelope's steps, or none for the instrument's
    int tremolo_depth = 0;      // a vibrato's depth in cents, or 0 for none
    int tremolo_hz_sixteenths = 0;
    int tremolo_delay_ms = 0;
};

// A recorded song, with what its presets play.
struct SbnPerformance
{
    Performance performance;
    std::vector<SbnPresetKey> presets;
    std::set<int> drum_keys;
};

// Plays a composition of `file`. `per` holds the game's drums for the keys below 72, and `prg` the game's instruments
// from number 128 on.
SbnPerformance PerformSbn(const ntdlib::n64::BgmFile& file, std::span<const ntdlib::n64::BgmDrum> per,
                          std::span<const ntdlib::n64::BgmInstrument> prg, const SbnFacts& facts,
                          const SbnSettings& settings, const PerformOptions& options);

// The compositions of `file` that play something, each once. Two that list the same phrases play the same.
std::vector<int> SbnVariations(const ntdlib::n64::BgmFile& file);

// An instrument's sound, as its zones take it. Each list runs to its end byte.
struct SbnSound
{
    int key_base = 0; // in cents
    uint32_t sample_rate = 0;
    std::span<const uint8_t> press;
    std::span<const uint8_t> release;
};

// The seconds that a release list takes.
double SbnReleaseSeconds(std::span<const uint8_t> release, std::span<const int32_t> times, uint32_t output_rate);

// The zones of an instrument's preset, without their wave. Keys past the resampler's limit play at the limit.
std::vector<Zone> SbnInstrumentZones(const SbnSound& sound, const SbnPresetKey& key, std::span<const int32_t> times,
                                     uint32_t output_rate);

// The zone of a drum at `key`, without its wave. `tune_cents` holds the track's tunings when the preset starts.
Zone SbnDrumZone(const SbnSound& sound, const ntdlib::n64::BgmDrum& drum, int key, int tune_cents,
                 std::span<const int32_t> times, uint32_t output_rate);

// A game on Intelligent Systems' engine.
class SbnGame
{
public:
    // Reads the game's archive from `image`, which stays valid while the game lives.
    static std::unique_ptr<SbnGame> Open(std::span<const uint8_t> image, const SbnProfile& profile);

    std::vector<ntdlib::sequence_export::Sequence> Sequences() const;
    Conversion Convert(std::size_t index, const PerformOptions& options);

private:
    // A version of a song that the game's code can play.
    struct Track
    {
        std::size_t song = 0;
        int variation = 0;
        int mix = 0;
        bool linked = false;
        bool near = false;
        std::size_t levels = 0;
        std::string name;
    };

    // An instrument and the bank that holds it.
    struct Found
    {
        const ntdlib::n64::BkFile* bank = nullptr;
        const ntdlib::n64::BkInstrument* instrument = nullptr;
        uint16_t file = 0;
    };

    SbnGame(std::span<const uint8_t> image, const SbnProfile& profile, ntdlib::n64::SbnArchive archive);

    const ntdlib::n64::BkFile& Bank(uint16_t file);
    std::optional<Found> Find(uint16_t bank_patch, const ntdlib::n64::SbnArchive::Song& song);
    SbnSound SoundOf(const Found& found, uint16_t bank_patch) const;
    std::shared_ptr<const Wave> WaveOf(const Found& found);

    std::span<const uint8_t> image_;
    const SbnProfile* profile_ = nullptr;
    ntdlib::n64::SbnArchive archive_;
    std::vector<int32_t> times_;
    std::vector<ntdlib::n64::BgmDrum> per_;
    std::vector<ntdlib::n64::BgmInstrument> prg_;
    std::array<std::array<uint16_t, 16>, 7> init_files_ = {}; // by the game's number of a set of banks, and by bank
    std::map<uint16_t, ntdlib::n64::BkFile> banks_;
    std::map<std::pair<uint16_t, uint32_t>, std::shared_ptr<const Wave>> waves_;
    std::vector<Track> tracks_;
};

} // namespace ntdlib::sequence_export::n64
