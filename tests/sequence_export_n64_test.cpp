// SPDX-License-Identifier: MIT

// Unit tests for the conversion of the Nintendo 64's sound data, without game data. The tests build small sound files
// in memory. Their expected values follow from the arithmetic of libultra's sequence players and synthesizer, and of
// EAD's sequence players, worked out by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/zip_archive.h"

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/audio_rsp.h"
#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/ead_later.h"
#include "ntdlib/n64/midi.h"
#include "ntdlib/sequence_export/n64/ead.h"
#include "ntdlib/sequence_export/n64/games.h"
#include "ntdlib/sequence_export/n64/image.h"
#include "ntdlib/sequence_export/n64/input.h"
#include "ntdlib/sequence_export/n64/libmus.h"
#include "ntdlib/sequence_export/n64/libultra.h"
#include "ntdlib/sequence_export/n64/resampler_low_pass.h"
#include "ntdlib/sequence_export/n64/sbn.h"

namespace
{

namespace n64 = ntdlib::n64;
namespace midi = seqlib::midi;
using ntdlib::sequence_export::n64::Image;
using ntdlib::sequence_export::n64::LibultraPerformance;
using ntdlib::sequence_export::n64::LibultraSettings;
using ntdlib::sequence_export::n64::LibultraSong;
using ntdlib::sequence_export::n64::PerformLibultra;
using ntdlib::sequence_export::n64::PerformOptions;
using ntdlib::sequence_export::n64::Zone;
using xsflib::testing::Check;
using xsflib::testing::StoredEntry;
using xsflib::testing::ZipArchive;

bool Near(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) <= tolerance;
}

// A bank of one instrument: two sounds on keys 48 to 84, split at velocity 64. The first has the envelope `envelope`.
n64::BankFile SmallBank(const n64::BankEnvelope& envelope)
{
    n64::BankFile file;
    file.envelopes.push_back(envelope);
    file.key_maps.push_back({.velocity_min = 0, .velocity_max = 63, .key_min = 48, .key_max = 84, .key_base = 60});
    file.key_maps.push_back({.velocity_min = 64, .velocity_max = 127, .key_min = 48, .key_max = 84, .key_base = 60});
    file.waves.push_back({.base = 0, .length = 9});
    file.sounds.push_back({.envelope = 0, .key_map = 0, .wave = 0, .pan = 64, .volume = 127});
    file.sounds.push_back({.envelope = 0, .key_map = 1, .wave = 0, .pan = 64, .volume = 127});
    file.instruments.push_back({.volume = 100, .pan = 32, .bend_range = 200, .sounds = {0, 1}});
    file.banks.push_back({.instruments = {0}});

    return file;
}

constexpr n64::BankEnvelope kHeldEnvelope = {.attack_volume = 127, .decay_volume = 127};

n64::MidiEvent NoteOn(uint32_t delta, int key, int velocity, uint32_t duration)
{
    return {.delta = delta,
            .status = 0x90,
            .data1 = static_cast<uint8_t>(key),
            .data2 = static_cast<uint8_t>(velocity),
            .duration = duration};
}

n64::MidiEvent Controller(uint32_t delta, int controller, int value)
{
    return {.delta = delta,
            .status = 0xB0,
            .data1 = static_cast<uint8_t>(controller),
            .data2 = static_cast<uint8_t>(value)};
}

n64::MidiEvent EndOfTrack(uint32_t delta)
{
    return {.delta = delta, .status = n64::kMidiMeta, .type = n64::kMidiMetaEndOfTrack};
}

LibultraPerformance Play(const std::vector<n64::MidiEvent>& track, const n64::BankFile& file,
                         const PerformOptions& options = {}, const LibultraSettings& settings = {})
{
    LibultraSong song;
    song.tracks.push_back(track);

    return PerformLibultra(song, file, file.banks[0], settings, options);
}

std::vector<midi::TrackEvent> EventsOfKind(const ntdlib::sequence_export::n64::Performance& performance,
                                           midi::EventKind kind)
{
    std::vector<midi::TrackEvent> events;
    std::ranges::copy_if(performance.tracks[0], std::back_inserter(events),
                         [kind](const midi::TrackEvent& event) { return event.kind == kind; });

    return events;
}

void TestImageOrders()
{
    std::vector<uint8_t> console(0x40, 0);
    console[0] = 0x80;
    console[1] = 0x37;
    console[2] = 0x12;
    console[3] = 0x40;
    const std::string name = "A GAME";
    std::ranges::copy(name, console.begin() + 0x20);
    std::fill(console.begin() + 0x26, console.begin() + 0x34, ' ');

    std::vector<uint8_t> halfwords = console;
    for (std::size_t i = 0; i + 1 < halfwords.size(); i += 2)
    {
        std::swap(halfwords[i], halfwords[i + 1]);
    }

    std::vector<uint8_t> words = console;
    for (std::size_t i = 0; i + 3 < words.size(); i += 4)
    {
        std::swap(words[i], words[i + 3]);
        std::swap(words[i + 1], words[i + 2]);
    }

    const Image first(console);
    Check(first.HeaderName() == name, "the header's name loses its trailing spaces");
    Check(Image(halfwords).Sha1() == first.Sha1(), "a .v64 image comes back in the console's order");
    Check(Image(words).Sha1() == first.Sha1(), "a .n64 image comes back in the console's order");

    bool threw = false;
    try
    {
        const Image image(std::vector<uint8_t>(0x40, 0x55));
        Check(image.Sha1().empty(), "bytes that start no image have no hash");
    }
    catch (const xsflib::FormatError&)
    {
        threw = true;
    }

    Check(threw, "bytes that start no image throw");
}

// The message that Input::Open throws for `zip`, written to a file.
std::string OpenMessage(const std::vector<uint8_t>& zip)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ntdlib_sequence_export_n64_test.zip";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));

    std::string message;
    try
    {
        ntdlib::sequence_export::n64::Input::Open(path, "test");
    }
    catch (const std::exception& e)
    {
        message = e.what();
    }

    std::error_code error;
    std::filesystem::remove(path, error);

    return message;
}

// A cartridge image in a ZIP archive reads as the image. Images are often kept that way. The image's name in the
// archive ends in .z64, .v64 or .n64, in capitals or not. An archive with two images still reads as a ROM set.
void TestZippedImage()
{
    std::vector<uint8_t> image(0x40, 0);
    image[0] = 0x80;
    image[1] = 0x37;
    image[2] = 0x12;
    image[3] = 0x40;

    const std::string one =
        OpenMessage(ZipArchive({StoredEntry("Game.Z64", image), StoredEntry("readme.txt", {'h', 'i'})}));
    const std::string two = OpenMessage(ZipArchive({StoredEntry("a.z64", image), StoredEntry("b.n64", image)}));

    Check(one == "the converter doesn't know this game's sound engine", "an archive's one image reads: " + one);
    Check(!two.empty() && two != one, "an archive with two images reads as a set: " + two);
}

void TestNotesAndTempo()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 48),
        NoteOn(96, 62, 127, 0),
        NoteOn(0, 100, 127, 24), // past the key map: plays nothing
        EndOfTrack(48),
    };

    const LibultraPerformance played = Play(track, file);
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;

    // The player keeps a whole number of microseconds a tick: 500000 / 48 is 10416.
    Check(performance.tempo.size() == 1, "one tempo");
    Check(Near(performance.tempo[0].bpm, 60000000.0 / (10416.0 * 48.0)), "the player's tempo is truncated");

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = EventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 2, "a note that the lookup doesn't find plays nothing");
    Check(offs.size() == 2, "every note ends");
    Check(ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 100, "the first note");
    Check(offs[0].tick == 48 && offs[0].key == 60, "a compressed note's duration gives its note off");
    Check(ons[1].tick == 96 && ons[1].key == 62, "the second note");
    Check(offs[1].tick == 144 && offs[1].key == 62, "a note without a duration stops at the end");

    // A program goes out with its channel's first note, with the instrument's volume and pan before it.
    Check(played.instruments.size() == 1, "one preset");
    const std::vector<midi::TrackEvent> volumes = EventsOfKind(performance, midi::EventKind::kVolume);
    const std::vector<midi::TrackEvent> pans = EventsOfKind(performance, midi::EventKind::kPan);
    Check(!volumes.empty() && volumes[0].value == 100, "the instrument gives the channel's volume");
    Check(!pans.empty() && pans[0].value == 32, "the instrument gives the channel's pan");
}

// The rate divides the time between a song's events. The ticks stay as they are.
void TestLibultraRate()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 48),
        NoteOn(96, 62, 100, 48),
        EndOfTrack(48),
    };

    LibultraSettings settings;
    settings.rate = 0.5;
    const LibultraPerformance plain = Play(track, file);
    const LibultraPerformance slower = Play(track, file, {}, settings);

    // 500000 / 48 is 10416 microseconds a tick, and a rate of 0.5 takes that to 20832.
    Check(slower.performance.tempo.size() == 1, "one tempo at a rate");
    Check(Near(slower.performance.tempo[0].bpm, 60000000.0 / (20832.0 * 48.0)), "a rate of 0.5 halves the tempo");
    Check(Near(plain.performance.tempo[0].bpm, 60000000.0 / (10416.0 * 48.0)), "the default rate keeps the tempo");

    const std::vector<midi::TrackEvent> plain_ons = EventsOfKind(plain.performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> slower_ons = EventsOfKind(slower.performance, midi::EventKind::kNoteOn);
    Check(slower_ons.size() == 2 && plain_ons.size() == 2, "a rate keeps the notes");
    Check(slower_ons[1].tick == plain_ons[1].tick && slower_ons[1].key == plain_ons[1].key, "a rate keeps the ticks");
    Check(Near(slower.performance.seconds, 2.0 * plain.performance.seconds), "a rate of 0.5 takes twice the time");
}

void TestUnusedInstrumentsStayOut()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const LibultraPerformance played = Play({EndOfTrack(48)}, file);
    Check(played.instruments.empty(), "an instrument that plays no note gets no preset");
}

void TestForeverLoop()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    n64::MidiEvent start = {.status = n64::kMidiMeta, .type = n64::kCompressedLoopStart, .payload = {0, 0xFF}};
    n64::MidiEvent end = {.delta = 48,
                          .status = n64::kMidiMeta,
                          .type = n64::kCompressedLoopEnd,
                          .payload = {0xFF, 0xFF, 0, 0, 0, 0},
                          .loop_start = 1};
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 24), start, NoteOn(24, 62, 100, 12), end, EndOfTrack(0),
    };

    PerformOptions options;
    options.loops = 2;
    const LibultraPerformance played = Play(track, file, options);
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;

    // Each pass runs from tick 0 for 72 ticks. The stop comes after two passes.
    Check(performance.loop.has_value(), "the song loops");
    Check(performance.loop && performance.loop->start == 0 && performance.loop->end == 72, "the loop's ticks");
    Check(performance.end_tick == 144, "the song ends after its passes");
    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 3, "the first note plays once and the loop's note twice");
}

void TestCountedLoop()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    n64::MidiEvent start = {.status = n64::kMidiMeta, .type = n64::kCompressedLoopStart, .payload = {0, 0xFF}};
    n64::MidiEvent end = {.delta = 48,
                          .status = n64::kMidiMeta,
                          .type = n64::kCompressedLoopEnd,
                          .payload = {2, 2, 0, 0, 0, 0},
                          .loop_start = 0};
    const std::vector<n64::MidiEvent> track = {start, NoteOn(0, 62, 100, 12), end, EndOfTrack(0)};
    const LibultraPerformance played = Play(track, file);

    // A count of 2 repeats twice more: three passes of 48 ticks.
    Check(!played.performance.loop.has_value(), "a counted loop isn't the song's loop");
    Check(EventsOfKind(played.performance, midi::EventKind::kNoteOn).size() == 3, "three passes");
}

void TestControllers()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);

    LibultraSettings settings;
    settings.effect = ntdlib::sequence_export::n64::BusEffect::kEcho;

    const std::vector<n64::MidiEvent> track = {
        Controller(0, 91, 64),
        {.status = 0xE0, .data1 = 0x7F, .data2 = 0x7F},
        NoteOn(0, 60, 100, 24),
        EndOfTrack(24),
    };

    const LibultraPerformance played = Play(track, file, {}, settings);
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;

    // The dry level at a send of 64 is cos(pi / 2 * 64 / 127), 0.695. CC11 acts squared: 127 * sqrt(0.695) is 106.
    const std::vector<midi::TrackEvent> sends = EventsOfKind(performance, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> expressions = EventsOfKind(performance, midi::EventKind::kExpression);
    Check(sends.size() == 1 && sends[0].value == 64, "the send reaches the reverb");
    Check(expressions.size() == 1 && expressions[0].value == 106, "the send takes its share from the dry level");

    // A bend of 8191 of 8192 on a range of 200 cents truncates to 199 cents.
    const std::vector<midi::TrackEvent> bends = EventsOfKind(performance, midi::EventKind::kPitch);
    Check(!bends.empty() && Near(bends[0].semitones, 1.99, 1e-5), "a bend's cents are a whole number");
}

const Zone* ZoneAt(const std::vector<Zone>& zones, int key, int velocity)
{
    for (const Zone& zone : zones)
    {
        if (key >= zone.low_key && key <= zone.high_key && velocity >= zone.low_velocity &&
            velocity <= zone.high_velocity)
        {
            return &zone;
        }
    }

    return nullptr;
}

void TestZones()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    std::vector<int32_t> waves;
    LibultraSettings settings;
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    Check(zones.size() == waves.size(), "each zone has a wave");

    Check(ZoneAt(zones, 47, 100) == nullptr, "a key below the key map has no zone");
    Check(ZoneAt(zones, 85, 100) == nullptr, "a key above the key map has no zone");
    const Zone* low = ZoneAt(zones, 60, 10);
    const Zone* high = ZoneAt(zones, 60, 100);
    Check(low != nullptr && high != nullptr && low != high, "the velocities split between the sounds");
    if (high == nullptr)
    {
        return;
    }

    // With every factor at its top, the player's product is 31507/32768, and the mixer squares it.
    const double full = (31507.0 / 32768.0) * (31507.0 / 32768.0);
    Check(Near(high->gain, full), "the level at the top of every factor");
    Check(high->root_key == 60 && high->follows_key, "the key map's base key");

    // The resampler stops an octave up.
    const Zone* limit = ZoneAt(zones, 72, 100);
    Check(limit != nullptr && !limit->follows_key && limit->tune_cents == 1200.0, "keys past the limit play at it");

    // The player's volume acts squared too.
    settings.volume = 0x3FFF;
    const std::vector<Zone> quieter = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone* half = ZoneAt(quieter, 60, 100);
    const double ratio = 16383.0 / 32767.0;
    Check(half != nullptr && Near(half->gain, full * ratio * ratio), "the player's volume");
}

void TestEnvelope()
{
    const n64::BankEnvelope falling = {
        .attack_time = 100000, .decay_time = 200000, .release_time = 50000, .attack_volume = 127, .decay_volume = 0};
    std::vector<int32_t> waves;
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, {}, waves);
    const Zone* zone = ZoneAt(zones, 60, 100);
    Check(zone != nullptr, "a zone");
    if (zone == nullptr)
    {
        return;
    }

    // At the nominal velocity of 100, the top stands above the floor of 1/32768 by:
    const double gain = (31507.0 / 32768.0) * (31507.0 / 32768.0);
    const double top_db = 20.0 * std::log10(gain * (100.0 / 127.0) * (100.0 / 127.0)) - 3.01;
    const double range_db = 90.31 + top_db;

    // A rise in decibels from the floor has the energy of a straight line in amplitude over T * 30 / (R * ln 10).
    const double line_ms = 100.0 * 30.0 / (range_db * std::numbers::ln10);
    Check(Near(zone->attack_ms, line_ms), "the attack's line");
    Check(Near(zone->delay_ms, 100.0 - line_ms), "the delay before the line ends with the rise");

    // SoundFont counts a decay's time for a fall of 100 dB.
    Check(Near(zone->decay_ms, 200.0 * 100.0 / range_db), "a fall to the floor");
    Check(Near(zone->sustain_db, 100.0), "a fall to the floor sustains at nothing");
    Check(Near(zone->release_ms, 50.0 * 100.0 / range_db), "a release of a falling sound runs from the top");

    const n64::BankEnvelope sustaining = {
        .decay_time = 100000, .release_time = 40000, .attack_volume = 127, .decay_volume = 90};
    const std::vector<Zone> other = ntdlib::sequence_export::n64::LibultraZones(SmallBank(sustaining), 0, {}, waves);
    const Zone* second = ZoneAt(other, 60, 100);
    const double fall_db = 40.0 * std::log10(127.0 / 90.0);
    Check(second != nullptr && Near(second->sustain_db, fall_db), "the mixer squares the decay's volume");
    Check(second != nullptr && Near(second->release_ms, 40.0 * 100.0 / (range_db - fall_db)),
          "a release runs from the sustain to the floor");
}

// A library's ramps as straight lines in amplitude. The game's output on the exact RSP follows such lines. The fit
// meets them at half of a ramp's time and at 9/10 of it.
void TestLineRampEnvelope()
{
    LibultraSettings settings;
    settings.line_ramps = true;

    // The level in decibels of a zone's decay, `ms` after its attack.
    const auto level_at = [](const Zone& zone, double ms)
    {
        return -std::max(ms - zone.hold_ms, 0.0) * 100.0 / zone.decay_ms;
    };

    std::vector<int32_t> waves;
    const n64::BankEnvelope falling = {
        .attack_time = 100000, .decay_time = 200000, .release_time = 50000, .attack_volume = 127, .decay_volume = 0};
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, settings, waves);
    const Zone* zone = ZoneAt(zones, 60, 100);
    Check(zone != nullptr, "the zone of a falling sound");
    if (zone == nullptr)
    {
        return;
    }

    Check(Near(zone->attack_ms, 100.0) && Near(zone->delay_ms, 0.0), "an attack is the same line");
    Check(Near(level_at(*zone, 100.0), 20.0 * std::log10(0.5), 1e-9), "a fall to nothing at half of its time");
    Check(Near(level_at(*zone, 180.0), 20.0 * std::log10(0.1), 1e-9), "a fall to nothing at 9/10 of its time");
    Check(Near(zone->sustain_db, 100.0), "a fall to nothing sustains at nothing");
    Check(Near(zone->release_ms, 50.0 * 5.0), "a release falls 20 dB in its time");

    // The level doesn't change the times: a line falls to nothing in its time from any level.
    settings.volume = 0x3FFF;
    const std::vector<Zone> quieter =
        ntdlib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, settings, waves);
    const Zone* quiet = ZoneAt(quieter, 60, 100);
    Check(quiet != nullptr && Near(quiet->decay_ms, zone->decay_ms) && Near(quiet->release_ms, zone->release_ms),
          "the times don't depend on the level");
    settings.volume = 0x7FFF;

    const n64::BankEnvelope sustaining = {
        .decay_time = 100000, .release_time = 40000, .attack_volume = 127, .decay_volume = 30};
    const std::vector<Zone> other =
        ntdlib::sequence_export::n64::LibultraZones(SmallBank(sustaining), 0, settings, waves);
    const Zone* second = ZoneAt(other, 60, 100);
    Check(second != nullptr, "the zone of a sustaining sound");
    if (second == nullptr)
    {
        return;
    }

    // The mixer squares the volume, and the line runs from 1 to the square of 30/127.
    const double end = (30.0 / 127.0) * (30.0 / 127.0);
    Check(Near(second->sustain_db, -20.0 * std::log10(end), 1e-9), "a fall to a level ends at the level");
    Check(Near(level_at(*second, 50.0), 20.0 * std::log10(1.0 - (1.0 - end) * 0.5), 1e-9), "a fall to a level at half");
    Check(Near(level_at(*second, 90.0), 20.0 * std::log10(1.0 - (1.0 - end) * 0.9), 1e-9), "a fall to a level at 9/10");
    Check(Near(second->release_ms, 40.0 * 5.0), "a release from a sustain takes the same time");

    // A library that multiplies keeps its fit.
    settings.line_ramps = false;
    const std::vector<Zone> standard =
        ntdlib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, settings, waves);
    const Zone* multiplying = ZoneAt(standard, 60, 100);
    Check(multiplying != nullptr && Near(multiplying->hold_ms, 0.0) && multiplying->delay_ms > 0.0,
          "the fit of ramps in decibels holds nothing");
}

void TestOscillators()
{
    n64::BankFile file = SmallBank(kHeldEnvelope);
    n64::BankInstrument& instrument = file.instruments[0];
    instrument.vibrato_type = 0x80;
    instrument.vibrato_rate = 3;
    instrument.vibrato_depth = 100;
    instrument.vibrato_delay = 10;
    instrument.tremolo_type = 1;
    instrument.tremolo_rate = 59;
    instrument.tremolo_depth = 64;

    std::vector<int32_t> waves;
    LibultraSettings settings;
    const std::vector<Zone> without = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone* plain = ZoneAt(without, 60, 100);
    Check(plain != nullptr && !plain->vibrato && !plain->tremolo, "a player without oscillators ignores them");

    settings.oscillators = true;
    const std::vector<Zone> with = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone* zone = ZoneAt(with, 60, 100);
    Check(zone != nullptr && zone->vibrato && zone->tremolo, "a player with oscillators applies them");
    if (zone == nullptr || !zone->vibrato || !zone->tremolo)
    {
        return;
    }

    // A sine has 259 - rate steps of 16 ms, and a delay's unit is 16.384 ms.
    Check(Near(zone->vibrato->hz, 1000.0 / (16.0 * 256.0)), "the vibrato's rate");
    Check(Near(zone->vibrato->delay_ms, 163.84), "the vibrato's delay");
    Check(Near(zone->vibrato->depth, std::pow(1.03099303, 100.0)), "the vibrato's depth");
    Check(Near(zone->tremolo->hz, 1000.0 / (16.0 * 200.0)), "the tremolo's rate");
    Check(Near(zone->tremolo->depth, 20.0 * std::log10(127.0 / 63.0)), "the tremolo's depth");
}

// A song file of the N64 Music Tools. Each list starts at its offset. A channel's volume and bend lists may be empty.
struct MusChannel
{
    std::vector<uint8_t> data;
    std::vector<uint8_t> volume = {};
    std::vector<uint8_t> bend = {};
};

std::vector<uint8_t> MusSong(const std::vector<MusChannel>& channels, const std::vector<uint8_t>& master,
                             const std::vector<uint8_t>& envelopes = {}, const std::vector<uint8_t>& drums = {},
                             const std::vector<uint16_t>& waves = {0, 1})
{
    constexpr uint32_t kLists = 0x2C;
    const uint32_t count = static_cast<uint32_t>(channels.size());
    const auto put = [](std::vector<uint8_t>& bytes, std::size_t at, uint32_t value)
    {
        bytes[at] = static_cast<uint8_t>(value >> 24);
        bytes[at + 1] = static_cast<uint8_t>(value >> 16);
        bytes[at + 2] = static_cast<uint8_t>(value >> 8);
        bytes[at + 3] = static_cast<uint8_t>(value);
    };

    std::vector<uint8_t> song(kLists + 12 * count);
    const auto append = [&](const std::vector<uint8_t>& bytes)
    {
        const uint32_t at = static_cast<uint32_t>(song.size());
        song.insert(song.end(), bytes.begin(), bytes.end());

        return at;
    };

    put(song, 0x04, count);
    put(song, 0x08, static_cast<uint32_t>(waves.size()));
    put(song, 0x0C, kLists);
    put(song, 0x10, kLists + 4 * count);
    put(song, 0x14, kLists + 8 * count);
    for (uint32_t i = 0; i < count; i++)
    {
        const MusChannel& channel = channels[i];
        put(song, kLists + 4 * i, append(channel.data));
        put(song, kLists + 4 * count + 4 * i, channel.volume.empty() ? 0 : append(channel.volume));
        put(song, kLists + 8 * count + 4 * i, channel.bend.empty() ? 0 : append(channel.bend));
    }

    put(song, 0x18, append(envelopes));
    put(song, 0x1C, append(drums));
    std::vector<uint8_t> wave_bytes;
    for (const uint16_t wave : waves)
    {
        wave_bytes.push_back(static_cast<uint8_t>(wave >> 8));
        wave_bytes.push_back(static_cast<uint8_t>(wave));
    }

    put(song, 0x20, append(wave_bytes));
    put(song, 0x24, append(master));

    return song;
}

// A master track that sets a script tempo of 120 and rests for `ticks`, in an endless loop.
std::vector<uint8_t> MusMaster(uint8_t high, uint8_t low)
{
    return {0x85, 120, 0x9A, 0x95, 0xFF, 96, static_cast<uint8_t>(high | 0x80), low, 0x96};
}

constexpr std::array<double, 2> kMusDetunes = {-48.0, -47.8};

void TestLibmusNote()
{
    // velon, an endit of 1, a loop of a note of 96 ticks and a rest of 96.
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0x87, 1, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;

    // 120 * 256 * 96 / 120 / 60 falls to 409, with 256 for a tick. A callback's 16666 microseconds come to 367 samples
    // at the rate of 22050.
    const double ticks_per_second = 409.0 * (22050.0 / 367.0) / 256.0;
    Check(Near(played.ticks_per_second, ticks_per_second), "the ticks a second");
    Check(performance.tempo.size() == 1 && Near(performance.tempo[0].bpm, ticks_per_second * 60.0 / 48.0), "the tempo");
    Check(performance.timebase == 48, "the file's ticks");
    Check(performance.loop.has_value() && performance.loop->start == 0 && performance.loop->end == 192, "the loop");
    Check(performance.end_tick == 192, "the end of the pass");

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 100, "the note");
    const std::vector<midi::TrackEvent> offs = EventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 1 && offs[0].tick == 95, "the release starts an endit before the note's end");
    Check(played.presets.size() == 1 && played.presets[0].wave == 0, "the wave");
}

void TestLibmusVelocity()
{
    // A velocity of 0x80 or more becomes the default, and the notes after it have no velocity byte.
    {
        const std::vector<uint8_t> song =
            MusSong({{.data = {0x99, 0x95, 0xFF, 48, 90, 48, 50, 0xC0, 48, 52, 48, 0x96}}}, MusMaster(0, 144));
        const ntdlib::sequence_export::n64::LibmusPerformance played =
            ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
        const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(ons.size() == 3 && ons[0].value == 90 && ons[1].value == 0x40 && ons[2].value == 0x40,
              "the default velocity");
    }

    // A voice's volume stops at 32767. 2^28 / (127 * 127 * 240) falls to 69.
    {
        const std::vector<uint8_t> song = MusSong({{.data = {0x99, 0x95, 0xFF, 48, 100, 48, 48, 60, 48, 0x96}}},
                                                  MusMaster(0, 96), {1, 127, 1, 127, 1, 127, 10});
        const ntdlib::sequence_export::n64::LibmusPerformance played =
            ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {.song_volume = 240}, {});
        const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(ons.size() == 2 && ons[0].value == 69 && ons[1].value == 60, "the top of the volume");
    }

    // Note 61 on a detune of -48 is over an octave above the output's rate. The player silences it, and a velocity of 0
    // says the same. Each of them still cuts the note before.
    {
        const std::vector<uint8_t> song =
            MusSong({{.data = {0x99, 0x95, 0xFF, 61, 100, 48, 60, 100, 48, 48, 0, 48, 0x96}}}, MusMaster(0, 144));
        const ntdlib::sequence_export::n64::LibmusPerformance played =
            ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
        const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(ons.size() == 3 && ons[0].value == 1 && ons[1].value == 100 && ons[2].value == 1, "silent notes");
    }
}

void TestLibmusDrums()
{
    // The table's entries are a wave, an envelope, a pan and a pitch. The song's wave table leads to the bank's wave.
    const std::vector<uint8_t> envelopes = {1, 127, 1, 127, 1, 127, 10, 1, 90, 1, 90, 1, 90, 20};
    const std::vector<uint8_t> drums = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 100, 50};
    const std::vector<uint8_t> song = MusSong(
        {{.data = {0x99, 0x9E, 0, 0x95, 0xFF, 1, 100, 48, 0x9F, 0x9C, 40, 0x81, 0, 0x90, 0, 48, 100, 48, 0x96}}},
        MusMaster(0, 96), envelopes, drums, {7, 1});
    const std::array<double, 8> detunes = {-48.0, -47.8, -48.0, -48.0, -48.0, -48.0, -48.0, -48.0};
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        ntdlib::sequence_export::n64::PerformLibmus(song, detunes, {}, {});
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 2 && ons[0].key == 62 && ons[1].key == 60, "the drum's pitch and the note");
    const std::vector<midi::TrackEvent> pans = EventsOfKind(performance, midi::EventKind::kPan);
    Check(pans.size() == 2 && pans[0].value == 50 && pans[1].value == 20, "half of each pan");
    Check(played.presets.size() == 2, "a preset for each sound");
    Check(played.presets[0].wave == 1 && played.presets[0].envelope[1] == 90, "the drum's sound");
    Check(played.presets[1].wave == 7 && played.presets[1].envelope[1] == 127, "the note's sound");
}

void TestLibmusLists()
{
    // The list's values come a tick apart. Above 127, a count follows: the value lasts for the count plus 2 ticks.
    const std::vector<uint8_t> song = MusSong({{.data = {0x99, 0x95, 0xFF, 48, 100, 8, 0x96},
                                                .volume = {100, 0x80 | 90, 1, 0x80 | 80, 20},
                                                .bend = {64, 0x80 | 32, 1, 0x80 | 96, 20}}},
                                              MusMaster(0, 8));
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;

    std::vector<std::pair<uint32_t, int>> volumes;
    for (const midi::TrackEvent& event : EventsOfKind(performance, midi::EventKind::kVolume))
    {
        volumes.emplace_back(event.tick, event.value);
    }

    const std::vector<std::pair<uint32_t, int>> expected_volumes = {{0, 100}, {1, 90}, {4, 80}};
    Check(volumes == expected_volumes, "the volume list");

    std::vector<std::pair<uint32_t, float>> bends;
    for (const midi::TrackEvent& event : EventsOfKind(performance, midi::EventKind::kPitch))
    {
        bends.emplace_back(event.tick, event.semitones);
    }

    // (value - 64) * 2 / 64 semitones.
    const std::vector<std::pair<uint32_t, float>> expected_bends = {{1, -1.0F}, {4, 1.0F}};
    Check(bends == expected_bends, "the bend list");
}

void TestLibmusLoops()
{
    // The channels enter their endless loops at different ticks, with different lengths. The main loop starts when the
    // last is in, and it lasts until they meet.
    const std::vector<uint8_t> song = MusSong({{.data = {0x99, 0x95, 0xFF, 48, 100, 96, 0x96}},
                                               {.data = {0x99, 48, 100, 48, 0x95, 0xFF, 50, 100, 0x80, 144, 0x96}}},
                                              MusMaster(0, 96));
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.loop.has_value() && performance.loop->start == 48 && performance.loop->end == 48 + 288,
          "the main loop");

    // The first channel plays at 0, 96, 192 and 288, and the second at 0, 48 and 192. The pass ends before 336.
    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 4 && ons[3].tick == 288, "the first channel's notes");
    std::vector<uint32_t> second;
    for (const midi::TrackEvent& event : performance.tracks[1])
    {
        if (event.kind == midi::EventKind::kNoteOn)
        {
            second.push_back(event.tick);
        }
    }

    Check(second == std::vector<uint32_t>{0, 48, 192}, "the second channel's notes");
}

void TestLibmusZone()
{
    ntdlib::sequence_export::n64::LibmusSettings settings;
    settings.song_volume = 128;
    settings.master_volume = 18021;
    const ntdlib::sequence_export::n64::LibmusPresetKey key = {.wave = 0, .envelope = {1, 100, 6, 127, 28, 89, 54}};
    const Zone zone = ntdlib::sequence_export::n64::LibmusZone(key, -47.8, settings, 96.0);

    // Key 60 plays note 48, at 0.2 semitones above the output's rate.
    Check(zone.root_key == 60 && Near(zone.tune_cents, 20.0), "the tuning");
    Check(zone.exclusive_class == 1, "a new note cuts the last");

    // The rise's energy, for a square from 100/127 to 1 over 6 ticks at 96 a second.
    const double first = 100.0 / 127.0;
    const double energy = (1.0 + first + first * first) / 3.0;
    Check(Near(zone.attack_ms, 2.0 * (1.0 - energy) * 6.0 * 1000.0 / 96.0, 1e-6), "the attack");
    Check(Near(zone.delay_ms, 0.0), "no delay");
    Check(Near(zone.sustain_db, -20.0 * std::log10((89.0 / 127.0) * (89.0 / 127.0)), 1e-6), "the sustain");

    // A release in a straight line, squared, from any level to nothing in 54 ticks.
    Check(Near(zone.release_ms, 2.0 * std::numbers::ln10 * 54.0 * 1000.0 / 96.0, 1e-6), "the release");

    // 127^3 * 128 / 2^13 of 32768, times the master's 18021 of 32768, squared by the mixer.
    const double full = 127.0 * 127.0 * 127.0 * 128.0 / 8192.0 / 32768.0 * (18021.0 / 32768.0);
    Check(Near(zone.gain, full * full, 1e-9), "the gain");
}

void TestLibmusSharedTrack()
{
    // Seventeen channels with notes: two share a track. The two that never sound together share.
    std::vector<MusChannel> channels;
    channels.push_back({.data = {0x99, 0x95, 0xFF, 48, 100, 48, 96, 0, 48, 0x96}});
    channels.push_back({.data = {0x99, 0x95, 0xFF, 96, 0, 48, 50, 100, 48, 0x96}});
    for (int i = 2; i < 17; i++)
    {
        channels.push_back({.data = {0x99, 0x95, 0xFF, 52, 100, 96, 0x96}});
    }

    const std::vector<uint8_t> song = MusSong(channels, MusMaster(0, 96));
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.tracks[0].size() > performance.tracks[1].size(), "the first track holds both");
    std::vector<int> keys;
    for (const midi::TrackEvent& event : performance.tracks[0])
    {
        if (event.kind == midi::EventKind::kNoteOn)
        {
            keys.push_back(event.key);
        }
    }

    Check(keys == std::vector<int>{60, 62}, "their notes");
    Check(!performance.approximations.empty(), "the report says so");
}

// A track of a BGM file: its commands, and its word's flags. A word's polyphony lies in bits 13 to 15.
struct BgmTestTrack
{
    std::vector<uint8_t> commands;
    uint32_t flags = 1U << 13;
};

constexpr uint32_t kBgmTwoVoices = 5U << 13;
constexpr uint32_t kBgmPlayPhrase = 1U << 28;
constexpr uint32_t kBgmStartLoop = 3U << 28;
constexpr uint32_t kBgmEndLoop = 5U << 28;

// Builds a BGM file of one composition. A word of `composition` with kBgmPlayPhrase names a phrase by its index, and
// any other word goes in as it stands. `tail` comes after the commands, and `tail_at` gets its place.
std::vector<uint8_t> BgmTestFile(const std::vector<std::vector<BgmTestTrack>>& phrases,
                                 const std::vector<uint32_t>& composition, const std::vector<uint8_t>& tail = {},
                                 std::size_t* tail_at = nullptr)
{
    constexpr std::size_t kHeaderSize = 0x24;
    constexpr std::size_t kPhraseSize = 0x40;
    std::vector<uint8_t> bytes(kHeaderSize, 0);
    std::copy_n("BGM ", 4, bytes.begin());
    std::copy_n("TEST", 4, bytes.begin() + 8);
    bytes[0x14] = 0;
    bytes[0x15] = kHeaderSize / 4;

    const auto add_word = [&bytes](uint32_t word)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bytes.push_back(static_cast<uint8_t>(word >> shift));
        }
    };

    const std::size_t phrases_at = kHeaderSize + (composition.size() + 1) * 4;
    for (const uint32_t word : composition)
    {
        const bool plays = (word & kBgmPlayPhrase) != 0 && (word >> 28) == 1;
        add_word(plays ? kBgmPlayPhrase |
                             static_cast<uint32_t>((phrases_at + (word & 0xFFFF) * kPhraseSize - kHeaderSize) / 4)
                       : word);
    }

    add_word(0);

    std::vector<uint8_t> commands;
    std::size_t commands_at = phrases_at + phrases.size() * kPhraseSize;
    for (std::size_t p = 0; p < phrases.size(); p++)
    {
        const std::size_t phrase_at = phrases_at + p * kPhraseSize;
        for (std::size_t t = 0; t < kPhraseSize / 4; t++)
        {
            if (t >= phrases[p].size())
            {
                add_word(0);
                continue;
            }

            add_word(static_cast<uint32_t>(commands_at + commands.size() - phrase_at) << 16 | phrases[p][t].flags);
            commands.insert(commands.end(), phrases[p][t].commands.begin(), phrases[p][t].commands.end());
        }
    }

    bytes.insert(bytes.end(), commands.begin(), commands.end());
    if (tail_at != nullptr)
    {
        *tail_at = bytes.size();
    }

    bytes.insert(bytes.end(), tail.begin(), tail.end());
    bytes.resize((bytes.size() + 3) / 4 * 4, 0);
    const auto size = static_cast<uint32_t>(bytes.size());
    for (std::size_t i = 0; i < 4; i++)
    {
        bytes[4 + i] = static_cast<uint8_t>(size >> (24 - 8 * i));
    }

    return bytes;
}

ntdlib::sequence_export::n64::SbnPerformance PlayBgm(const std::vector<uint8_t>& bytes,
                                                     ntdlib::sequence_export::n64::SbnSettings settings = {},
                                                     std::span<const n64::BgmInstrument> prg = {})
{
    const n64::BgmFile file = n64::BgmFile::Parse(bytes);
    const ntdlib::sequence_export::n64::SbnFacts facts = [](uint16_t)
    {
        return ntdlib::sequence_export::n64::SbnInstrumentFacts{.exists = true,
                                                                .key_base = 6000,
                                                                .sample_rate = 32000,
                                                                .samples = 0.0,
                                                                .loops = true,
                                                                .release_seconds = 0.1};
    };

    return ntdlib::sequence_export::n64::PerformSbn(file, {}, prg, facts, settings, {});
}

std::vector<midi::TrackEvent> AllEventsOfKind(const ntdlib::sequence_export::n64::Performance& performance,
                                              midi::EventKind kind)
{
    std::vector<midi::TrackEvent> events;
    for (const auto& track : performance.tracks)
    {
        std::ranges::copy_if(track, std::back_inserter(events),
                             [kind](const midi::TrackEvent& event) { return event.kind == kind; });
    }

    return events;
}

// Sets an instrument. Each note holds a key's command, a velocity and a length.
constexpr std::array<uint8_t, 3> kBgmInstrument = {0xE8, 0x30, 0x00};

std::vector<uint8_t> BgmCommands(std::initializer_list<uint8_t> commands)
{
    std::vector<uint8_t> out(kBgmInstrument.begin(), kBgmInstrument.end());
    out.insert(out.end(), commands);

    return out;
}

void TestSbnClock()
{
    // A tempo of 200 at 48 ticks a beat runs 160 ticks a second at the game's nominal rate. The game's interval of
    // 10434782 / 48 drops its fraction, and that adds a few parts in a million.
    const ntdlib::sequence_export::n64::SbnPerformance played =
        PlayBgm(BgmTestFile({{{BgmCommands({0xE0, 0x00, 200, 0xBC, 100, 10, 20, 0x00})}}}, {kBgmPlayPhrase}));
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.timebase == 48, "a tick is a 48th of a beat");
    Check(Near(performance.tempo.back().bpm, 200.0 * 32006.0 / 32000.0, 1e-3), "the tempo at the output's real rate");
    Check(performance.end_tick == 20, "a phrase ends with its track");
    Check(Near(performance.seconds, 20.0 / 160.0, 0.02), "the seconds of the ticks");

    const std::vector<midi::TrackEvent> ons = AllEventsOfKind(performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = AllEventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 1 && ons[0].key == 60 && ons[0].value == 100, "a note's key and velocity");
    Check(offs.size() == 1 && offs[0].tick == 10, "a note's length");
}

void TestSbnShortNote()
{
    // A note of 1 tick starts no voice, but a track of one voice has cut the note before.
    const ntdlib::sequence_export::n64::SbnPerformance played =
        PlayBgm(BgmTestFile({{{BgmCommands({0xBC, 100, 50, 5, 0xC0, 100, 1, 20, 0x00})}}}, {kBgmPlayPhrase}));
    const std::vector<midi::TrackEvent> ons = AllEventsOfKind(played.performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 1 && ons[0].key == 60, "a note of 1 tick doesn't sound");
    Check(offs.size() == 1 && offs[0].tick == 5, "a note of 1 tick cuts the one before");
}

void TestSbnVoices()
{
    {
        // A track of one voice cuts its note for the next.
        const ntdlib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xBC, 100, 50, 5, 0xC0, 100, 50, 60, 0x00})}}}, {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
        Check(offs.at(0).tick == 5 && offs.at(0).key == 60, "one voice cuts its note");
        Check(played.presets.at(0).cuts, "one voice's preset cuts");
    }

    {
        // With two voices held, the note with the least time left gives up its voice.
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile({{{BgmCommands({0xBC, 100, 100, 0xC0, 100, 20, 5, 0xC4, 100, 50, 120, 0x00}), kBgmTwoVoices}}},
                        {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
        Check(offs.at(0).tick == 5 && offs.at(0).key == 64, "the shortest note gives up its voice");
        Check(offs.at(1).tick == 55, "the new note's length");
        Check(offs.at(2).tick == 100 && offs.at(2).key == 60, "the longest note keeps its voice");
        Check(!played.presets.at(0).cuts, "two voices' preset doesn't cut");
    }

    {
        // A voice stays busy through its release. A note that has ended gives up its voice first.
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile({{{BgmCommands({0xBC, 100, 5, 0xC0, 100, 100, 10, 0xC4, 100, 50, 120, 0x00}), kBgmTwoVoices}}},
                        {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
        Check(offs.at(0).key == 60 && offs.at(0).tick == 5, "a note that reaches its end");
        Check(offs.at(1).key == 68 && offs.at(1).tick == 60, "the third note takes the released voice");
        Check(offs.at(2).key == 64 && offs.at(2).tick == 100, "the held note keeps its voice");
    }
}

void TestSbnComposition()
{
    const std::vector<std::vector<BgmTestTrack>> phrases = {{{BgmCommands({0xBC, 100, 5, 10, 0x00})}},
                                                            {{BgmCommands({0xC0, 100, 5, 20, 0x00})}}};
    {
        // An endless loop's end ends the performance, and its label gives the start.
        const ntdlib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile(phrases, {kBgmPlayPhrase, kBgmStartLoop | 1, kBgmPlayPhrase | 1, kBgmEndLoop | 1}));
        const ntdlib::sequence_export::n64::Performance& performance = played.performance;
        Check(performance.loop && performance.loop->start == 10 && performance.loop->end == 30, "an endless loop");
        Check(performance.end_tick == 30, "the end of an endless loop");
    }

    {
        // A loop with a count of 2 plays three passes.
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile(phrases, {kBgmPlayPhrase, kBgmStartLoop | 1, kBgmPlayPhrase | 1, kBgmEndLoop | (2U << 5) | 1}));
        const ntdlib::sequence_export::n64::Performance& performance = played.performance;
        Check(!performance.loop && performance.end_tick == 70, "a counted loop");
        Check(AllEventsOfKind(performance, midi::EventKind::kNoteOn).size() == 4, "a counted loop's notes");
    }

    {
        // The end of any track ends the phrase for all of them.
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(BgmTestFile(
            {{{BgmCommands({0xBC, 100, 50, 20, 0x00})}, {BgmCommands({0xC0, 100, 50, 60, 0xC4, 100, 5, 5, 0x00})}},
             {{BgmCommands({0xC8, 100, 5, 5, 0x00})}}},
            {kBgmPlayPhrase, kBgmPlayPhrase | 1}));
        const std::vector<midi::TrackEvent> ons = AllEventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(played.performance.end_tick == 25, "a phrase ends with its shortest track");
        Check(std::ranges::none_of(ons, [](const midi::TrackEvent& on) { return on.key == 68; }),
              "a later note stays out");
        Check(std::ranges::any_of(ons, [](const midi::TrackEvent& on) { return on.key == 72 && on.tick == 20; }),
              "the next phrase starts in the tick of the end");
    }
}

void TestSbnMix()
{
    // A branch reads its table by the song's mix. Its tail holds a place and a drum flag for each value.
    const auto file_of = [](std::size_t table)
    {
        const auto hi = static_cast<uint8_t>(table >> 8);
        const auto lo = static_cast<uint8_t>(table);
        return BgmCommands({0xFC, hi, lo, 2, 0, 1, 0x00});
    };

    const std::vector<uint8_t> branch_a = {0xBC, 100, 5, 10, 0x00};
    const std::vector<uint8_t> branch_b = {0xC8, 100, 5, 10, 0x00};
    const std::size_t tail_size = 6;
    std::size_t tail_at = 0;
    std::vector<uint8_t> tail(tail_size + branch_a.size() + branch_b.size(), 0);
    BgmTestFile({{{file_of(0)}}}, {kBgmPlayPhrase}, tail, &tail_at);

    const std::size_t a_at = tail_at + tail_size;
    const std::size_t b_at = a_at + branch_a.size();
    std::vector<uint8_t> filled = {static_cast<uint8_t>(a_at >> 8), static_cast<uint8_t>(a_at), 0,
                                   static_cast<uint8_t>(b_at >> 8), static_cast<uint8_t>(b_at), 0};
    filled.insert(filled.end(), branch_a.begin(), branch_a.end());
    filled.insert(filled.end(), branch_b.begin(), branch_b.end());
    const std::vector<uint8_t> bytes = BgmTestFile({{{file_of(tail_at)}}}, {kBgmPlayPhrase}, filled);

    ntdlib::sequence_export::n64::SbnSettings settings;
    const auto first_key = [&](int mix)
    {
        settings.mix = mix;
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(bytes, settings);
        return AllEventsOfKind(played.performance, midi::EventKind::kNoteOn).at(0).key;
    };

    Check(first_key(0) == 60, "the first of a branch's places");
    Check(first_key(1) == 72, "a mix's place");
    Check(first_key(7) == 60, "a mix past the table takes the first place");
}

void TestSbnLinked()
{
    // Track 2 links to track 1. The game's code chooses which of them sounds.
    constexpr uint32_t kLinkedToFirst = 1U << 13 | 1U << 9;
    const std::vector<uint8_t> bytes =
        BgmTestFile({{{BgmCommands({0xBC, 100, 5, 10, 0x00})}, {BgmCommands({0xC8, 100, 5, 20}), kLinkedToFirst}}},
                    {kBgmPlayPhrase});
    const auto keys = [&](bool linked)
    {
        ntdlib::sequence_export::n64::SbnSettings settings;
        settings.linked = linked;
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(bytes, settings);
        const std::vector<midi::TrackEvent> ons = AllEventsOfKind(played.performance, midi::EventKind::kNoteOn);
        return ons.size() == 1 ? ons[0].key : -1;
    };

    Check(keys(false) == 60, "a linked track starts muted");
    Check(keys(true) == 72, "the linked track plays in place of the other");
}

void TestSbnChanges()
{
    // The game's code fades tracks to a list of levels a few frames after it starts the song. The note runs past that.
    const std::vector<uint8_t> held = BgmTestFile(
        {{{BgmCommands({0xE0, 0x00, 200, 0xBC, 100, 0x70, 0x77, 0x77, 0x77, 0x77, 0x00})}}}, {kBgmPlayPhrase});
    const auto last_level =
        [](const std::vector<uint8_t>& bytes, const ntdlib::sequence_export::n64::SbnSettings& settings)
    {
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(bytes, settings);
        return AllEventsOfKind(played.performance, midi::EventKind::kExpression).back();
    };

    const std::array<uint8_t, 4> levels = {1, 64, 0, 0};
    const std::array<uint8_t, 4> silence = {1, 0, 0, 0};
    ntdlib::sequence_export::n64::SbnSettings settings;
    settings.track_levels = levels;
    const midi::TrackEvent faded = last_level(held, settings);
    Check(faded.value == 64, "a list's level");

    // 200 beats a minute at 48 ticks a beat is 160 ticks a second, so 0.2 s is about 32 ticks. The song's first tick
    // runs at the tempo before its first command.
    Check(faded.tick >= 26 + 48 && faded.tick <= 34 + 48, "a list's level takes 48 ticks from about 0.2 s");

    settings.track_levels = silence;
    Check(last_level(held, settings).value == 0, "a list can silence a track");

    // A mix's tracks take the game's level at their first branch. The table sends both values to one place.
    const auto branching = [](std::size_t table)
    {
        const auto hi = static_cast<uint8_t>(table >> 8);
        const auto lo = static_cast<uint8_t>(table);
        return BgmCommands({0xFC, hi, lo, 2, 0, 1, 0xBC, 100, 5, 10, 0x00});
    };

    std::size_t tail_at = 0;
    BgmTestFile({{{branching(0)}}}, {kBgmPlayPhrase}, std::vector<uint8_t>(11, 0), &tail_at);
    const std::size_t place = tail_at + 6;
    const auto hi = static_cast<uint8_t>(place >> 8);
    const auto lo = static_cast<uint8_t>(place);
    const std::vector<uint8_t> mixed =
        BgmTestFile({{{branching(tail_at)}}}, {kBgmPlayPhrase}, {hi, lo, 0, hi, lo, 0, 0xBC, 100, 5, 10, 0x00});
    settings = {};
    settings.mix = 1;
    settings.proximity = 0x57;
    Check(last_level(mixed, settings).value == 0x57, "a mix at the near level");
    settings.proximity = 0x7F;
    Check(last_level(mixed, settings).value == 0x7F, "a mix at the full level");
    settings.mix = 0;
    settings.proximity = 0x57;
    Check(last_level(mixed, settings).value == 0x7F, "the first mix keeps its level");

    // A proximity command with values stores them. One without values gives them to every track, by the level.
    const std::vector<uint8_t> proximity =
        BgmTestFile({{{BgmCommands({0xFF, 6, 100, 50, 0xBC, 100, 20, 30, 0x00})},
                      {BgmCommands({0x02, 0xFF, 6, 0, 0, 0xC8, 100, 20, 30, 0x00})}}},
                    {kBgmPlayPhrase});
    const auto first_track_level = [&](int level)
    {
        ntdlib::sequence_export::n64::SbnSettings proximity_settings;
        proximity_settings.mix = 1;
        proximity_settings.proximity = level;
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(proximity, proximity_settings);
        const auto& track = played.performance.tracks.at(0);
        int value = -1;
        for (const midi::TrackEvent& event : track)
        {
            if (event.kind == midi::EventKind::kExpression)
            {
                value = event.value;
            }
        }

        return value;
    };

    Check(first_track_level(0x7F) == 100, "a proximity command at the full level");
    Check(first_track_level(0x57) == 50, "a proximity command at the near level");

    // A composition without a phrase plays nothing, and one that lists the same phrases plays the same.
    std::vector<uint8_t> one = BgmTestFile({{{BgmCommands({0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase});
    const auto variations = [](const std::vector<uint8_t>& bytes)
    {
        return ntdlib::sequence_export::n64::SbnVariations(n64::BgmFile::Parse(bytes));
    };

    Check(variations(one) == std::vector<int>{0}, "one composition");
    one[0x17] = one[0x15];
    Check(variations(one) == std::vector<int>{0}, "a composition that lists the same phrases");
    one[0x19] = static_cast<uint8_t>(one[0x15] + 1);
    Check(variations(one) == std::vector<int>{0}, "a composition without a phrase");

    std::vector<uint8_t> two =
        BgmTestFile({{{BgmCommands({0xBC, 100, 5, 10, 0x00})}}, {{BgmCommands({0xC8, 100, 5, 10, 0x00})}}},
                    {kBgmPlayPhrase, kBgmPlayPhrase | 1});
    two[0x17] = static_cast<uint8_t>(two[0x15] + 1);
    Check(variations(two) == std::vector<int>({0, 1}), "a composition with other phrases");
}

void TestSbnLevels()
{
    {
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile({{{BgmCommands({0xEC, 100, 0xE9, 64, 0xE1, 127, 0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase}));
        const ntdlib::sequence_export::n64::Performance& performance = played.performance;
        Check(AllEventsOfKind(performance, midi::EventKind::kVolume).at(0).value == 100, "a track's volume is CC7");
        Check(AllEventsOfKind(performance, midi::EventKind::kExpression).at(0).value == 64,
              "an instrument's volume is CC11");
    }

    {
        // The master volume multiplies in, and the send takes its share from the dry level on the sin/cos law.
        const ntdlib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xE1, 64, 0xEB, 40, 0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase}));
        const int expected =
            static_cast<int>(std::lround(64.0 * std::sqrt(std::cos(std::numbers::pi / 2.0 * 40.0 / 127.0))));
        const std::vector<midi::TrackEvent> levels = AllEventsOfKind(played.performance, midi::EventKind::kExpression);
        Check(levels.at(0).value == expected, "the master volume and the send in CC11");
        Check(AllEventsOfKind(played.performance, midi::EventKind::kReverb).at(0).value == 40, "a send is CC91");
    }

    {
        // An instrument's volume fades in steps, one for each tick.
        const ntdlib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xBC, 100, 50, 0xF6, 0, 10, 27, 20, 0x00})}}}, {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> levels = AllEventsOfKind(played.performance, midi::EventKind::kExpression);
        Check(levels.size() > 5 && levels.back().value == 27 && levels.back().tick == 10, "a fade's end");
    }

    {
        // A track's detune is a pitch bend. A coarse tune moves the key.
        const ntdlib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xED, 2, 0xEF, 0, 50, 0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase}));
        Check(AllEventsOfKind(played.performance, midi::EventKind::kNoteOn).at(0).key == 62, "a coarse tune");
        Check(!AllEventsOfKind(played.performance, midi::EventKind::kPitch).empty(), "a detune's bend");
    }
}

void TestSbnMasterAndInstrument()
{
    {
        // 0xE5 fades the master volume to 0 over 100 ticks. 0xE1 at tick 10 sets it back to full, and the fade ends
        // there.
        const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(BgmTestFile(
            {{{BgmCommands({0xE5, 0x00, 100, 0, 0xBC, 100, 0x70, 10, 0xE1, 127, 0x77, 0x00})}}}, {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> levels = AllEventsOfKind(played.performance, midi::EventKind::kExpression);
        Check(!levels.empty() && levels.back().tick == 10, "0xE1 ends the master volume's fade");
        Check(!levels.empty() && levels.back().value == levels.front().value, "at the level that 0xE1 sets");
    }

    {
        // 0xF4 gives the track a random pan, and 0xF5 then takes an instrument at pan 64. The notes keep the random
        // pan.
        const std::array<n64::BgmInstrument, 1> prg = {{{.bank_patch = 0x3000, .volume = 100, .pan = 64}}};
        std::vector<uint8_t> commands = BgmCommands({0xF4, 64, 127, 0xF5, 0x80});
        for (int note = 0; note < 4; note++)
        {
            commands.insert(commands.end(), {0xBC, 100, 5, 10});
        }

        commands.push_back(0x00);
        const ntdlib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{commands}}}, {kBgmPlayPhrase}), {}, prg);
        const std::vector<midi::TrackEvent> pans = AllEventsOfKind(played.performance, midi::EventKind::kPan);
        Check(std::ranges::any_of(pans, [](const midi::TrackEvent& event) { return event.value != 64; }),
              "0xF5 keeps the random pan");
    }
}

void TestSbnPresets()
{
    // A tremolo's rate of 16 goes round in 16 ticks. Its depth comes in cents.
    const ntdlib::sequence_export::n64::SbnPerformance played = PlayBgm(
        BgmTestFile({{{BgmCommands({0xEE, 7, 0xF0, 0, 16, 100, 0xBC, 100, 5, 5, 0xF3, 0xBC, 100, 5, 10, 0x00})}}},
                    {kBgmPlayPhrase}));
    Check(played.presets.size() == 2, "a tremolo has a preset of its own");
    const ntdlib::sequence_export::n64::SbnPresetKey& key = played.presets.at(0);
    Check(key.bank_patch == 0x3000 && key.fine_cents == 7, "a preset's instrument and fine tune");
    Check(key.tremolo_depth == 100, "a tremolo's depth");

    // 156 beats a minute at 48 ticks a beat is about 124.8 ticks a second.
    Check(Near(key.tremolo_hz_sixteenths / 16.0, 124.8 / 16.0, 0.1), "a tremolo's rate");
    Check(played.presets.at(1).tremolo_depth == 0, "a stopped tremolo");
}

// The engine's table of an envelope's times stands in here with a few values in microseconds.
std::vector<int32_t> SbnTestTimes()
{
    std::vector<int32_t> times(95, 0);
    times[0x4D] = 1000000;
    times[0x50] = 250000;
    times[0x2B] = 500000;

    return times;
}

void TestSbnZones()
{
    const std::vector<int32_t> times = SbnTestTimes();
    constexpr uint32_t kOutput = 32000;
    const std::array<uint8_t, 3> press = {0x5E, 0x7F, 0xFF};
    const std::array<uint8_t, 3> release = {0x4D, 0x00, 0xFF};
    ntdlib::sequence_export::n64::SbnSound sound;
    sound.key_base = 6000;
    sound.sample_rate = 16000;
    sound.press = press;
    sound.release = release;

    const std::vector<Zone> zones = ntdlib::sequence_export::n64::SbnInstrumentZones(sound, {}, times, kOutput);

    // At 16000 Hz and a key base of 60, key 72 plays the wave at its rate.
    Check(zones.at(0).root_key == 72 && Near(zones.at(0).tune_cents, 0.0, 1e-6), "a zone's root");
    Check(Near(zones.at(0).attack_ms, 0.0) && Near(zones.at(0).decay_ms, 0.0), "a list of one step");

    // The factors at their top reach 31972 of 32768, and the envelope reaches 127 of 128. The mixer squares both.
    const double full = (31972.0 / 32768.0) * (127.0 / 128.0);
    Check(Near(zones.at(0).gain, full * full, 1e-9), "a zone's level");

    // A straight line of amplitude to 0 in a second has the energy of a fall of 100 dB in 10/3 ln(10) seconds.
    Check(Near(zones.at(0).release_ms, 10.0 / 3.0 * std::numbers::ln10 * 1000.0, 1e-6), "a release's time");
    Check(Near(ntdlib::sequence_export::n64::SbnReleaseSeconds(release, times, kOutput), 1.0), "a voice's release");

    // Keys past the resampler's limit of 2 hold the limit: at key 84 the wave would play at twice its rate.
    Check(zones.size() == 2 && zones[1].low_key == 84 && !zones[1].follows_key, "the resampler's limit");
    Check(zones[0].high_key == 83, "keys below the limit");

    // A release in steps relative to the level at the key's release: four lines of a quarter second each. The mixer
    // moves in a straight line of amplitude to the square of each level.
    const std::array<uint8_t, 9> steps = {0x50, 0xDF, 0x50, 0xBF, 0x50, 0x9F, 0x50, 0x80, 0xFF};
    sound.release = steps;
    const std::vector<Zone> stepped = ntdlib::sequence_export::n64::SbnInstrumentZones(sound, {}, times, kOutput);
    double energy = 0.0;
    double before = 1.0;
    for (const double level : {95.0 / 128.0, 63.0 / 128.0, 31.0 / 128.0, 0.0})
    {
        const double next = level * level;
        energy += 0.25 * (before * before + before * next + next * next) / 3.0;
        before = next;
    }

    Check(Near(stepped.at(0).release_ms, 10.0 * std::numbers::ln10 * energy * 1000.0, 1e-6), "a release in steps");
    Check(Near(ntdlib::sequence_export::n64::SbnReleaseSeconds(steps, times, kOutput), 1.0), "a list's whole time");

    // A press list with a decay.
    const std::array<uint8_t, 7> decays = {0x5E, 0x7F, 0x2B, 0x7F, 0x4D, 0x00, 0xFF};
    sound.press = decays;
    sound.release = release;
    const std::vector<Zone> decaying = ntdlib::sequence_export::n64::SbnInstrumentZones(sound, {}, times, kOutput);
    Check(decaying.at(0).decay_ms > 0.0 && Near(decaying.at(0).sustain_db, 100.0), "a list that falls to nothing");

    // The first list that a song writes replaces the instrument's.
    ntdlib::sequence_export::n64::SbnPresetKey custom;
    const std::vector<uint8_t> custom_press = {0x4D, 0x7F, 0xFF, 0x00};
    custom.press = custom_press;
    const std::vector<Zone> rising = ntdlib::sequence_export::n64::SbnInstrumentZones(sound, custom, times, kOutput);
    Check(Near(rising.at(0).attack_ms, 1000.0), "a custom press list");

    // A track of one voice cuts by an exclusive class.
    ntdlib::sequence_export::n64::SbnPresetKey cuts;
    cuts.cuts = true;
    Check(ntdlib::sequence_export::n64::SbnInstrumentZones(sound, cuts, times, kOutput).at(0).exclusive_class != 0,
          "a class for one voice");
}

void TestSbnDrumZone()
{
    const std::vector<int32_t> times = SbnTestTimes();
    constexpr uint32_t kOutput = 32000;
    const std::array<uint8_t, 3> press = {0x5E, 0x7F, 0xFF};
    const std::array<uint8_t, 3> release = {0x4D, 0x00, 0xFF};
    ntdlib::sequence_export::n64::SbnSound sound;
    sound.key_base = 4800;
    sound.sample_rate = 32000;
    sound.press = press;
    sound.release = release;

    n64::BgmDrum drum;
    drum.key_base = 4900;
    drum.volume = 64;
    drum.pan = 127;
    const Zone zone = ntdlib::sequence_export::n64::SbnDrumZone(sound, drum, 40, 0, times, kOutput);
    Check(zone.low_key == 40 && zone.high_key == 40 && zone.root_key == 40, "a drum's key");
    Check(!zone.follows_key && Near(zone.tune_cents, 100.0, 1e-6), "a drum's pitch");
    Check(Near(zone.pan, 1.0), "a drum's pan");

    const double full = (31972.0 / 32768.0) * (127.0 / 128.0) * (64.0 / 128.0);
    Check(Near(zone.gain, full * full, 1e-9), "a drum's level");

    // A drum's reverb takes its share from the dry level in the zone.
    drum.reverb = 127;
    Check(Near(ntdlib::sequence_export::n64::SbnDrumZone(sound, drum, 40, 0, times, kOutput).gain, 0.0, 1e-9),
          "a drum's send");
}

void TestSbnDrumNote()
{
    // Keys below 72 name the game's drums, and the others the song's.
    n64::BgmDrum drum;
    drum.bank_patch = 0x30B0;
    drum.key_base = 4800;
    drum.volume = 127;
    drum.reverb = 25;
    const std::vector<n64::BgmDrum> per(72, drum);
    constexpr uint32_t kDrums = 1U << 13 | 0x80;
    const std::vector<uint8_t> bytes = BgmTestFile({{{{0x80 + 40, 100, 5, 10, 0x00}, kDrums}}}, {kBgmPlayPhrase});
    const n64::BgmFile file = n64::BgmFile::Parse(bytes);
    const ntdlib::sequence_export::n64::SbnFacts facts = [](uint16_t bank_patch)
    {
        Check(bank_patch == 0x30B0, "a drum's instrument");
        return ntdlib::sequence_export::n64::SbnInstrumentFacts{
            .exists = true, .key_base = 4800, .sample_rate = 32000, .loops = true};
    };

    const ntdlib::sequence_export::n64::SbnPerformance played =
        ntdlib::sequence_export::n64::PerformSbn(file, per, {}, facts, {}, {});
    Check(played.presets.at(0).drums && played.drum_keys.count(40) == 1, "a drum's preset");
    Check(AllEventsOfKind(played.performance, midi::EventKind::kNoteOn).at(0).key == 40, "a drum's key");
    Check(AllEventsOfKind(played.performance, midi::EventKind::kReverb).at(0).value == 25, "a drum's send");
}

void TestUnpackDirectoryFile()
{
    const auto expect_throw = [](std::vector<uint8_t> bytes, const char* what)
    {
        bool thrown = false;
        try
        {
            ntdlib::sequence_export::n64::UnpackDirectoryFile(bytes);
        }
        catch (const xsflib::FormatError&)
        {
            thrown = true;
        }

        Check(thrown, what);
    };

    // Four bytes, then a copy of eight from a distance of 4, then the end.
    const std::vector<uint8_t> short_copy = {1, 0, 0, 12, 0x0C, 'A', 'B', 'C', 'D', 0x07, 0x04, 0, 0};
    const std::vector<uint8_t> abcd = ntdlib::sequence_export::n64::UnpackDirectoryFile(short_copy);
    Check(std::string(abcd.begin(), abcd.end()) == "ABCDABCDABCD", "a short copy");

    // A third byte of 3 gives a length of 20.
    const std::vector<uint8_t> run =
        ntdlib::sequence_export::n64::UnpackDirectoryFile(std::vector<uint8_t>{1, 0, 0, 21, 0x60, 'A', 0, 1, 3, 0, 0});
    Check(run.size() == 21 && run.back() == 'A', "a long copy");

    // The first byte's high nibble holds the distance's top bits: 0x112 reaches back to the start here.
    const std::vector<uint8_t> far = ntdlib::sequence_export::n64::UnpackDirectoryFile(
        std::vector<uint8_t>{1, 0, 1, 0x14, 0x38, 'Q', 'X', 0, 1, 0xFF, 0x11, 0x12, 0, 0});
    Check(far.size() == 0x114 && far[0x112] == 'Q' && far[0x113] == 'X', "a far copy");

    // A group of eight bytes with no flags set, then a second group.
    const std::vector<uint8_t> groups = ntdlib::sequence_export::n64::UnpackDirectoryFile(
        std::vector<uint8_t>{1, 0, 0, 9, 0, '1', '2', '3', '4', '5', '6', '7', '8', 0x40, '9', 0, 0});
    Check(std::string(groups.begin(), groups.end()) == "123456789", "two groups");

    expect_throw({0, 0, 0, 1, 0x40, 'A', 0, 0}, "a file of another kind");
    expect_throw({1, 0, 0, 2, 0x40, 'A', 0, 0}, "a size that isn't the file's");
    expect_throw({1, 0, 0, 2, 0x40, 'A', 0x01, 0x05}, "a copy from before the start");
    expect_throw({1, 0, 0, 1, 0x00, 'A'}, "a file with no end");
}

void TestLibmusTempoScale()
{
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0x87, 1, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));

    // The script's tempo of 120 comes to 409. The library multiplies that by 0x78 / 128 in whole numbers: 383.
    const double scaled = 383.0 * (22050.0 / 367.0) / 256.0;
    ntdlib::sequence_export::n64::LibmusSettings settings;
    settings.tempo_scale = 0x78;
    const ntdlib::sequence_export::n64::LibmusPerformance whole =
        ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});
    Check(Near(whole.ticks_per_second, scaled), "a whole song's scale reaches its envelopes' times");
    Check(whole.performance.tempo.size() == 1 && Near(whole.performance.tempo[0].bpm, scaled * 60.0 / 48.0),
          "a whole song's scale");
}

void TestLibmusRepeats()
{
    // An image with a bank of no waves and four songs. The first two have the same data and volume. The third has
    // another volume. The fourth has the first's data and volume in another bank.
    constexpr std::size_t kSongs = 0x100;
    constexpr std::size_t kVolumes = 0x140;
    constexpr std::size_t kData = 0x180;
    constexpr std::size_t kFirstBank = 0x200;
    constexpr std::size_t kSecondBank = 0x300;
    std::vector<uint8_t> image(0x400, 0);
    const auto put_word = [&image](std::size_t at, uint32_t word)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            image[at + i] = static_cast<uint8_t>(word >> (24 - 8 * i));
        }
    };

    for (std::size_t song = 0; song < 4; song++)
    {
        put_word(kSongs + song * 8, kData);
        put_word(kSongs + song * 8 + 4, kData + 16);
        put_word(kVolumes + song * 4, song == 2 ? 0x00728080 : 0x00808080);
    }

    namespace n64_export = ntdlib::sequence_export::n64;
    const std::array banks = {
        n64_export::LibmusBankPlace{.first_song = 0, .pointers = kFirstBank, .samples = kFirstBank + 0x80},
        n64_export::LibmusBankPlace{.first_song = 3, .pointers = kSecondBank, .samples = kSecondBank + 0x80},
    };
    const n64_export::LibmusProfile profile = {
        .songs = kSongs, .song_count = 4, .song_volumes = kVolumes, .banks = banks};
    const auto game = n64_export::LibmusGame::Open(image, profile);
    std::vector<std::size_t> indices;
    for (const auto& sequence : game->Sequences())
    {
        indices.push_back(sequence.index);
    }

    Check(indices == std::vector<std::size_t>({0, 2, 3}), "a song that repeats an earlier one is left out");
}

void TestLibmusSongTables()
{
    // Three songs of 16 bytes, each with another first byte, and a bank of no waves.
    constexpr std::size_t kTable = 0x100;
    constexpr std::size_t kSongFiles = 0x140;
    constexpr std::size_t kEnds = 0x160;
    constexpr std::size_t kBase = 0x180;
    constexpr std::size_t kBank = 0x200;
    std::vector<uint8_t> image(0x300, 0);
    const auto put_word = [&image](std::size_t at, uint32_t word)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            image[at + i] = static_cast<uint8_t>(word >> (24 - 8 * i));
        }
    };

    for (std::size_t song = 0; song < 3; song++)
    {
        image[kBase + 16 * song] = static_cast<uint8_t>(song + 1);
    }

    namespace n64_export = ntdlib::sequence_export::n64;
    const std::array kBanks = {n64_export::LibmusBankPlace{.pointers = kBank, .samples = kBank + 0x80}};

    const auto indices = [](const n64_export::LibmusGame& game)
    {
        std::vector<std::size_t> found;
        for (const auto& sequence : game.Sequences())
        {
            found.push_back(sequence.index);
        }

        return found;
    };

    // A table of files: file 0 is no song, and files 1 to 3 start at 0x00, 0x10 and 0x20 from the base. The game's
    // songs 1 to 4 play files 3, 1, 3 and 2. Song 3 repeats song 1.
    for (std::size_t file = 1; file < 4; file++)
    {
        put_word(kTable + 8 * file, static_cast<uint32_t>(16 * (file - 1)));
        put_word(kTable + 8 * file + 4, 16);
    }

    const std::array<uint16_t, 5> kNumbers = {0, 3, 1, 3, 2};
    for (std::size_t i = 0; i < kNumbers.size(); i++)
    {
        image[kSongFiles + 2 * i] = static_cast<uint8_t>(kNumbers[i] >> 8);
        image[kSongFiles + 2 * i + 1] = static_cast<uint8_t>(kNumbers[i]);
    }

    const n64_export::LibmusProfile files = {
        .song_count = 4,
        .cartridge_files = {.table = kTable, .base = kBase, .song_files = kSongFiles, .first_song = 1},
        .banks = kBanks};

    Check(indices(*n64_export::LibmusGame::Open(image, files)) == std::vector<std::size_t>({1, 2, 4}),
          "the songs of a table of files keep their numbers in the game");

    // A table of starts and a table of ends.
    for (std::size_t song = 0; song < 3; song++)
    {
        put_word(kTable + 4 * song, static_cast<uint32_t>(kBase + 16 * song));
        put_word(kEnds + 4 * song, static_cast<uint32_t>(kBase + 16 * song + 16));
    }

    const n64_export::LibmusProfile ends = {.songs = kTable, .song_count = 3, .song_ends = kEnds, .banks = kBanks};

    Check(indices(*n64_export::LibmusGame::Open(image, ends)) == std::vector<std::size_t>({0, 1, 2}),
          "a table of starts and a table of ends");
}

// A profile whose table runs past the image throws FormatError. The other reads throw it too. The tables are those of
// the starts and the ends, of the ends, of the songs' files, and the directory.
void TestLibmusTablesOutsideImage()
{
    namespace n64_export = ntdlib::sequence_export::n64;
    std::vector<uint8_t> image(0x100, 0);
    image[0xFF] = 0x40; // the song's start in a table at 0xFC
    const std::array kBanks = {n64_export::LibmusBankPlace{.pointers = 0x80, .samples = 0xC0}};
    const auto error_of = [&image](const n64_export::LibmusProfile& profile)
    {
        try
        {
            n64_export::LibmusGame::Open(image, profile);
        }
        catch (const xsflib::FormatError& e)
        {
            return std::string(e.what());
        }

        return std::string();
    };

    const std::string outside = "a table of the game's profile isn't in the image";

    Check(error_of({.songs = 0xFC, .song_count = 1, .banks = kBanks}) == outside, "a table of starts and ends");
    Check(error_of({.songs = 0xF0, .song_count = 1, .song_ends = 0x100, .banks = kBanks}) == outside,
          "a table of ends");
    Check(
        error_of({.song_count = 1, .cartridge_files = {.table = 0x10, .song_files = 0xFF}, .banks = kBanks}) == outside,
        "a table of the songs' files");
    Check(error_of({.directory = 0x100, .banks = kBanks}) == outside, "a directory");
}

void TestLibmusSend()
{
    // A send of 60 goes to CC91. The mixer takes its share from the dry level, and CC11 acts squared.
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0xA2, 60, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const std::vector<midi::TrackEvent> sends = EventsOfKind(played.performance, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> levels = EventsOfKind(played.performance, midi::EventKind::kExpression);
    Check(!sends.empty() && sends.back().value == 60, "a send");
    const double dry = std::cos(std::numbers::pi / 2.0 * 60.0 / 127.0);
    Check(!levels.empty() && levels.back().value == std::lround(127.0 * std::sqrt(dry)), "the dry level of a send");
}

// The resampler's mean response at `fraction` of the rate: the root mean square over its 64 filters of each filter's
// magnitude, from the microcode's table.
double ResamplerResponse(double fraction)
{
    const std::array<int16_t, 256>& table = n64::AudioResampleTable();
    double power = 0.0;
    for (std::size_t phase = 0; phase < 64; phase++)
    {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t tap = 0; tap < 4; tap++)
        {
            const double w = 2.0 * std::numbers::pi * fraction * static_cast<double>(tap);
            re += table[phase * 4 + tap] / 32768.0 * std::cos(w);
            im -= table[phase * 4 + tap] / 32768.0 * std::sin(w);
        }

        power += (re * re + im * im) / 64.0;
    }

    return std::sqrt(power);
}

double Db(double amplitude)
{
    return 20.0 * std::log10(amplitude);
}

void TestResamplerLowPassResponse()
{
    const std::array<int16_t, 5>& taps = ntdlib::sequence_export::n64::ResamplerLowPass();

    Check(taps[0] + 2 * (taps[1] + taps[2] + taps[3] + taps[4]) == 32768, "the filter passes a constant level at 1.0");
    Check(Near(Db(ResamplerResponse(0.25)), -2.26, 0.01) && Near(Db(ResamplerResponse(0.45)), -6.96, 0.01) &&
              Near(Db(ResamplerResponse(0.5)), -7.43, 0.01),
          "the resampler's mean response");
    for (const double fraction : {0.05, 0.1, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45})
    {
        double response = taps[0];
        for (std::size_t tap = 1; tap < taps.size(); tap++)
        {
            response += 2.0 * taps[tap] * std::cos(2.0 * std::numbers::pi * fraction * static_cast<double>(tap));
        }

        Check(Near(Db(response / 32768.0), Db(ResamplerResponse(fraction)), 0.02),
              "the filter follows the resampler's mean response");
    }
}

void TestResamplerLowPassSine()
{
    // A sine at a quarter of the rate, with no loop. Away from the ends, the filter takes it down by the resampler's
    // mean response there, and the length stays.
    seqlib::Pcm pcm;
    pcm.channels.emplace_back();
    for (int i = 0; i < 4000; i++)
    {
        pcm.channels[0].push_back(
            static_cast<int16_t>(std::lround(16384.0 * std::sin(0.5 * std::numbers::pi * i + 0.3))));
    }

    const seqlib::Pcm out = ntdlib::sequence_export::n64::ApplyResamplerLowPass(pcm);

    Check(out.channels.size() == 1 && out.channels[0].size() == 4000 && !out.loop, "the length stays");
    double in_power = 0.0;
    double out_power = 0.0;
    for (std::size_t i = 100; i < 3900; i++)
    {
        in_power += static_cast<double>(pcm.channels[0][i]) * pcm.channels[0][i];
        out_power += static_cast<double>(out.channels[0][i]) * out.channels[0][i];
    }

    Check(Near(10.0 * std::log10(out_power / in_power), Db(ResamplerResponse(0.25)), 0.02), "a sine at a quarter");
}

void TestResamplerLowPassEnds()
{
    // A constant level of 1000 with no loop. Inside, it stays. At each end, the filter reads zeros past the wave.
    const std::array<int16_t, 5>& taps = ntdlib::sequence_export::n64::ResamplerLowPass();
    seqlib::Pcm pcm;
    pcm.channels.emplace_back(32, int16_t{1000});

    const seqlib::Pcm out = ntdlib::sequence_export::n64::ApplyResamplerLowPass(pcm);

    const std::vector<int16_t>& s = out.channels.at(0);
    Check(s.size() == 32 && std::all_of(s.begin() + 4, s.end() - 4, [](int16_t v) { return v == 1000; }),
          "a constant level stays");
    const int32_t half = taps[0] + taps[1] + taps[2] + taps[3] + taps[4];
    Check(s[0] == (1000 * half + 16384) >> 15 && s[31] == s[0], "zeros before the first sample and after the last");
}

void TestResamplerLowPassLoop()
{
    // An attack of 10 samples and a loop of 7, shorter than the filter. The filtered wave plays as the filtered wave
    // that repeats the loop 20 times: the loop's last samples and its first join as the loop wraps. Only the first
    // pass's first 4 samples of the loop differ. There they read the attack, and in the filtered wave the loop's end.
    seqlib::Pcm pcm;
    pcm.loop = true;
    pcm.loop_start = 10;
    pcm.loop_end = 17;
    pcm.channels.emplace_back();
    for (int i = 0; i < 17; i++)
    {
        pcm.channels[0].push_back(static_cast<int16_t>((i * 7919) % 20001 - 10000));
    }

    seqlib::Pcm unrolled;
    unrolled.channels.emplace_back(pcm.channels[0].begin(), pcm.channels[0].begin() + 10);
    for (int repeat = 0; repeat < 20; repeat++)
    {
        unrolled.channels[0].insert(unrolled.channels[0].end(), pcm.channels[0].begin() + 10, pcm.channels[0].end());
    }

    const seqlib::Pcm out = ntdlib::sequence_export::n64::ApplyResamplerLowPass(pcm);
    const seqlib::Pcm played = ntdlib::sequence_export::n64::ApplyResamplerLowPass(unrolled);

    Check(out.loop && out.loop_start == 10 && out.loop_end == 17 && out.channels.at(0).size() == 17, "the loop stays");
    bool same = true;
    for (std::size_t i = 0; i < 10 + 7 * 10; i++)
    {
        if (i >= 10 && i < 14)
        {
            continue;
        }

        const std::size_t looped = i < 10 ? i : 10 + (i - 10) % 7;
        same = same && out.channels[0][looped] == played.channels[0][i];
    }

    Check(same, "the attack and the loop play as the repeated loop");
}

void TestResamplerLowPassClips()
{
    // Three samples at the top between pairs at the bottom. The middle one goes past 16 bits, to 34343, and is
    // limited.
    seqlib::Pcm pcm;
    pcm.channels.push_back({-32768, -32768, 32767, 32767, 32767, -32768, -32768});
    std::size_t clipped = 0;

    const seqlib::Pcm out = ntdlib::sequence_export::n64::ApplyResamplerLowPass(pcm, &clipped);

    Check(clipped == 1 && out.channels.at(0)[3] == 32767, "a sample past 16 bits is limited");
    Check(out.channels[0][2] == out.channels[0][4] && out.channels[0][2] < 32767, "its neighbours stay inside");
}

// A bank of one instrument and one drum. The envelope rises in 4 updates and holds at `top`.
n64::EadBank SmallEadBank(int16_t top = 32767)
{
    n64::EadEnvelope envelope;
    envelope.points = {{4, top}, {1000, top}, {n64::kEnvelopeHang, 0}};

    n64::EadInstrument instrument;
    instrument.release_rate = 0x20;
    instrument.envelope = 0;
    instrument.normal = {.sample = 0, .tuning = 1.0F};

    n64::EadDrum drum;
    drum.release_rate = 0x20;
    drum.pan = 32;
    drum.sound = {.sample = 0, .tuning = 1.0F};
    drum.envelope = 0;

    n64::EadBank bank;
    bank.envelopes.push_back(envelope);
    bank.samples.emplace_back();
    bank.instruments.push_back(instrument);
    bank.program_instruments = {0};
    bank.drums.push_back(drum);
    bank.drum_notes = {0};

    return bank;
}

// The start of a sequence: channel 0 starts at 0x0D, after a tempo of 120, and the sequence's script ticks on for ever
// in steps of 0x60. The channel selects `program` and starts layer 0 at 0x20. It then waits for 96 ticks and ends.
std::vector<uint8_t> EadSequence(std::span<const uint8_t> channel_settings, std::span<const uint8_t> layer)
{
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0D, 0xDD, 0x78, 0xFD, 0x60, 0xFB, 0x00, 0x08};
    bytes.insert(bytes.end(), channel_settings.begin(), channel_settings.end());
    const std::array<uint8_t, 6> start_and_wait = {0x90, 0x00, 0x00, 0xFD, 0x60, 0xFF};
    const std::size_t layer_place = bytes.size() + start_and_wait.size();
    bytes.insert(bytes.end(), start_and_wait.begin(), start_and_wait.end());
    bytes[layer_place - 4] = static_cast<uint8_t>(layer_place);
    bytes.insert(bytes.end(), layer.begin(), layer.end());

    return bytes;
}

ntdlib::sequence_export::n64::EadPerformance PlayEad(
    const std::vector<uint8_t>& sequence, const n64::EadBank& bank, n64::EadEngine engine = n64::EadEngine::kEarly,
    ntdlib::sequence_export::n64::EadLevels levels = ntdlib::sequence_export::n64::EadLevels::kEarly)
{
    const std::map<int, n64::EadBank> banks = {{0, bank}};
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = engine;
    settings.levels = levels;
    settings.sequence = sequence;
    settings.bank_set = {0};
    settings.banks = &banks;

    return ntdlib::sequence_export::n64::PerformEad(settings, {});
}

void TestAmbrellaLoop()
{
    // Controllers 102 and 103 mark the loop. A note that's on at the loop's end ends there.
    const LibultraSettings settings = {.player = ntdlib::sequence_export::n64::LibultraPlayer::kAmbrella};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(10, 102, 0),
        {.delta = 20, .status = 0x90, .data1 = 60, .data2 = 100},
        Controller(100, 103, 0),
        EndOfTrack(0),
    };

    const LibultraPerformance played = Play(track, file, {}, settings);
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.loop.has_value() && performance.loop->start == 10 && performance.loop->end == 130, "the loop");
    Check(performance.end_tick == 130, "the end of the pass");
    const std::vector<midi::TrackEvent> offs = EventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 1 && offs[0].tick == 130, "the note ends with the loop");
}

void TestAmbrellaControllers()
{
    // A channel starts at a volume of 100. CC11 holds its value at a dry mix. An effect's send of 127 leaves no dry
    // level.
    const LibultraSettings settings = {.player = ntdlib::sequence_export::n64::LibultraPlayer::kAmbrella,
                                       .effect = ntdlib::sequence_export::n64::BusEffect::kCustom};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(0, 11, 64),
        Controller(0, 1, 90),
        NoteOn(0, 60, 100, 48),
        Controller(48, 91, 127),
        EndOfTrack(0),
    };

    const ntdlib::sequence_export::n64::Performance performance = Play(track, file, {}, settings).performance;
    Check(EventsOfKind(performance, midi::EventKind::kVolume).at(0).value == 100, "the first volume");
    const std::vector<midi::TrackEvent> expressions = EventsOfKind(performance, midi::EventKind::kExpression);
    Check(expressions.at(0).value == 64, "CC11 passes through");
    Check(expressions.back().value == 0, "the send takes the dry level");
    Check(EventsOfKind(performance, midi::EventKind::kModulation).at(0).value == 90, "CC1 passes through");
}

void TestAmbrellaSweep()
{
    // CC5 of 30 moves 768 / 31 = 24 cents in each update of 5.75 ms. At 480 ticks to a quarter note of 500000 us, an
    // update is 5.52 ticks. A step of two updates is 11 ticks in the file. A channel's last key starts at 64. The first
    // note, 60, therefore starts 400 cents up. A note 2 semitones above the last starts 200 cents down.
    const LibultraSettings settings = {.player = ntdlib::sequence_export::n64::LibultraPlayer::kAmbrella};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    LibultraSong song;
    song.division = 480;
    song.tracks.push_back({
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(0, 5, 30),
        Controller(0, 65, 127),
        NoteOn(0, 60, 100, 200),
        NoteOn(200, 62, 100, 200),
        EndOfTrack(200),
    });

    const ntdlib::sequence_export::n64::Performance performance =
        PerformLibultra(song, file, file.banks[0], settings, {}).performance;
    const std::vector<midi::TrackEvent> pitches = EventsOfKind(performance, midi::EventKind::kPitch);
    Check(!pitches.empty() && pitches[0].tick == 0 && Near(pitches[0].semitones, 4.0, 1e-5),
          "the first note sweeps from key 64");
    Check(!pitches.empty() && pitches.back().tick > 200, "the sweep runs on after the note starts");
}

void TestAmbrellaNoteOff()
{
    // Two notes of key 60 overlap on a channel. The first note-off, at tick 100, releases both voices.
    const LibultraSettings settings = {.player = ntdlib::sequence_export::n64::LibultraPlayer::kAmbrella};
    n64::BankFile file = SmallBank(kHeldEnvelope);
    file.envelopes[0].release_time = 100000;
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        NoteOn(0, 60, 100, 100),
        NoteOn(20, 60, 100, 200),
        EndOfTrack(300),
    };

    const std::vector<midi::TrackEvent> offs =
        EventsOfKind(Play(track, file, {}, settings).performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 2 && offs[0].tick == 100 && offs[1].tick == 100, "a note-off releases every voice of its key");

    // A sound without a release ends the newest voice at once, and the walk stops there. The older voice ends at the
    // next note-off of its key.
    file.envelopes[0].release_time = 0;
    const std::vector<midi::TrackEvent> sudden =
        EventsOfKind(Play(track, file, {}, settings).performance, midi::EventKind::kNoteOff);
    Check(sudden.size() == 2 && sudden[0].tick == 100 && sudden[1].tick == 220, "without a release, the newest ends");
}

void TestHalScales()
{
    // HAL's player: the player's scale starts at 120 and the channel's at 127, out of 128. CC21 moves every channel's
    // level, and CC8 one channel's.
    const LibultraSettings settings = {.player = ntdlib::sequence_export::n64::LibultraPlayer::kHal};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(10, 21, 100),
        Controller(10, 8, 64),
        EndOfTrack(0),
    };

    const LibultraPerformance played = Play(track, file, {}, settings);
    const std::vector<midi::TrackEvent> sent = EventsOfKind(played.performance, midi::EventKind::kExpression);

    // 120 * 127 / 128 is 119. 100 * 127 / 128 is 99. 100 * 64 / 128 is 50.
    Check(sent.size() == 3 && sent[0].tick == 0 && sent[0].value == 119, "the start");
    Check(sent[1].tick == 10 && sent[1].value == 99, "the player's scale");
    Check(sent[2].tick == 20 && sent[2].value == 50, "the channel's scale");

    // The sound's volume has no part in the level.
    n64::BankFile quiet = SmallBank(kHeldEnvelope);
    for (n64::BankSound& sound : quiet.sounds)
    {
        sound.volume = 64;
    }

    std::vector<int32_t> waves;
    const std::vector<Zone> full = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    waves.clear();
    const std::vector<Zone> same = ntdlib::sequence_export::n64::LibultraZones(quiet, 0, settings, waves);
    Check(Near(full[0].gain, same[0].gain), "no sound's volume");
}

void TestAmbrellaZones()
{
    // The player ignores the attack and any sustain short of nothing. The voice's top is 29769 / 32768, squared. CC1
    // sets the bank's vibrato, at 127 * 127 / (128 * 128) of its depth and a rate over 16 in hertz. The detune's sign
    // turns.
    n64::BankEnvelope envelope = {
        .attack_time = 50000, .decay_time = 100000, .release_time = 10000, .attack_volume = 64, .decay_volume = 32};
    n64::BankFile file = SmallBank(envelope);
    file.instruments[0].vibrato_type = 128;
    file.instruments[0].vibrato_rate = 80;
    file.instruments[0].vibrato_depth = 20;
    file.key_maps[0].detune = 10;

    const LibultraSettings settings = {.player = ntdlib::sequence_export::n64::LibultraPlayer::kAmbrella};
    std::vector<int32_t> waves;
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone& zone = zones.at(0);
    const double top = 29769.0 / 32768.0;
    Check(Near(zone.gain, top * top, 1e-9), "the voice's top");
    Check(zone.attack_ms == 0.0 && zone.delay_ms == 0.0 && zone.decay_ms == 0.0, "no attack and no decay");
    Check(Near(zone.tune_cents, -10.0), "the detune's sign");
    Check(zone.vibrato.has_value() && zone.vibrato->wheel, "CC1 moves the vibrato");
    Check(Near(zone.vibrato->hz, 5.0), "the vibrato's rate");
    Check(Near(zone.vibrato->depth, 20.0 * 127.0 * 127.0 / (128.0 * 128.0)), "the vibrato's depth");

    // A fall to nothing stays.
    file.envelopes[0].decay_volume = 0;
    const std::vector<Zone> falling = ntdlib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    Check(falling.at(0).decay_ms > 0.0, "a fall to nothing");
}

void TestEadNote()
{
    // A velocity of 100, a gate of 0x80, and a short note: note 39 for 48 ticks.
    const std::vector<uint8_t> sequence = EadSequence(std::array<uint8_t, 2>{0xC1, 0x00},
                                                      std::array<uint8_t, 7>{0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF});
    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = EventsOfKind(played.performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 1 && offs.size() == 1, "one note");
    Check(ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 100, "note 39 is key 60");

    // The note's gate takes half of its 48 ticks: 0x80 * 48 / 256 = 24 ticks stay after its release.
    Check(offs[0].tick == 24, "the gate ends the note");

    // 120 beats a minute in the engine's frames of 16.713 ms, on a video interface of 59.826 fields a second.
    Check(Near(played.performance.tempo.at(0).bpm, 120.0 * 59.826 * 0.016713, 1e-6), "tempo");
    Check(played.presets.size() == 1 && played.presets[0].program == 0, "one preset");
}

void TestEadLevels()
{
    // A channel volume of 64/127 and a scale of 64/128 act squared in MIDI, so the controllers hold their square roots.
    const std::vector<uint8_t> sequence = EadSequence(std::array<uint8_t, 6>{0xC1, 0x00, 0xDF, 64, 0xE0, 64},
                                                      std::array<uint8_t, 7>{0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF});
    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());
    Check(EventsOfKind(played.performance, midi::EventKind::kVolume).at(0).value == 90, "CC7 holds the root");
    Check(EventsOfKind(played.performance, midi::EventKind::kExpression).at(0).value == 90, "CC11 holds the root");

    // A build that squares the volumes acts as CC7 and CC11 do. The controllers hold the volumes.
    const ntdlib::sequence_export::n64::EadPerformance squared = PlayEad(
        sequence, SmallEadBank(), n64::EadEngine::kMiddle, ntdlib::sequence_export::n64::EadLevels::kVolumesSquared);
    Check(EventsOfKind(squared.performance, midi::EventKind::kVolume).at(0).value == 64, "CC7 holds the volume");
    Check(EventsOfKind(squared.performance, midi::EventKind::kExpression).at(0).value == 64, "CC11 holds the scale");
}

void TestEadForeverLoop()
{
    // The channel restarts its layer every 48 ticks, and the sequence's script comes round every 96. The state repeats
    // after a tick of set-up. The notes of that tick play at the end of each pass, so the loop moves back to tick 0.
    std::vector<uint8_t> sequence = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0D, 0xDD, 0x78, 0xFD, 0x60, 0xFB, 0x00, 0x08};
    const std::array<uint8_t, 10> channel = {0xC1, 0x00, 0x90, 0x00, 0x18, 0xFD, 0x30, 0xFB, 0x00, 0x0F};
    sequence.insert(sequence.end(), channel.begin(), channel.end());
    sequence.push_back(0x00);
    const std::array<uint8_t, 7> layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF};
    sequence.insert(sequence.end(), layer.begin(), layer.end());

    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());
    const ntdlib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.loop.has_value() && performance.loop->start == 0 && performance.loop->end == 96, "the loop");
    Check(performance.end_tick == 96, "the end of the pass");

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 2 && ons[0].tick == 0 && ons[1].tick == 48, "two notes in a pass");
}

void TestEadLegato()
{
    // A legato layer keeps its note for the same sound. The second note moves the pitch by 2 semitones.
    const std::vector<uint8_t> sequence =
        EadSequence(std::array<uint8_t, 2>{0xC1, 0x00},
                    std::array<uint8_t, 10>{0xC4, 0xC1, 100, 0xC9, 0x00, 0x27, 0x0A, 0x29, 0x0A, 0xFF});
    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 60, "one note");
    const std::vector<midi::TrackEvent> pitches = EventsOfKind(played.performance, midi::EventKind::kPitch);
    Check(pitches.size() == 1 && pitches[0].tick == 10 && Near(pitches[0].semitones, 2.0, 1e-5), "the pitch moves");
    const std::vector<midi::TrackEvent> offs = EventsOfKind(played.performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 1 && offs[0].tick == 20, "the layer's end ends the note");
}

void TestEadPortamento()
{
    // Mode 1, from note 43 to note 39, over 10 updates. A tick is 14359 / 5760 updates, so the sweep ends by tick 5.
    const std::vector<uint8_t> sequence =
        EadSequence(std::array<uint8_t, 2>{0xC1, 0x00},
                    std::array<uint8_t, 11>{0xC7, 0x01, 43, 0x0A, 0xC1, 100, 0xC9, 0x00, 0x27, 0x30, 0xFF});
    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 64, "the note starts at the sweep's first note");
    const std::vector<midi::TrackEvent> pitches = EventsOfKind(played.performance, midi::EventKind::kPitch);
    Check(!pitches.empty() && pitches.back().tick <= 5, "the sweep ends");
    Check(Near(pitches.back().semitones, -4.0, 1e-4), "the sweep ends at the note");
}

void TestEadDrums()
{
    // Drums on program 0x7F. A pan weight of 0 leaves the pan to the drum, which goes in the zone. Note 0 is key 21.
    const std::vector<uint8_t> sequence = EadSequence(std::array<uint8_t, 4>{0xC1, 0x7F, 0xDC, 0x00},
                                                      std::array<uint8_t, 7>{0xC1, 100, 0xC9, 0x80, 0x00, 0x30, 0xFF});
    const n64::EadBank bank = SmallEadBank();
    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(sequence, bank);

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 21, "the drum's key");
    Check(played.presets.at(0).program == -1 && played.presets.at(0).drum_pan_weight == 0, "the drums' preset");
    Check(EventsOfKind(played.performance, midi::EventKind::kPan).at(0).value == 64,
          "the controller stays at the centre");

    ntdlib::sequence_export::n64::EadPerformSettings settings;
    std::vector<int32_t> samples;
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::EadZones(played.presets[0], bank, settings, samples);
    Check(zones.size() == 1 && samples.size() == 1 && samples[0] == 0, "one zone");
    Check(zones[0].low_key == 21 && zones[0].high_key == 21 && !zones[0].follows_key, "a drum plays on its key");
    Check(Near(zones[0].pan, -0.5), "a drum's pan of 32 is halfway to the left");
}

void TestEadZones()
{
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    ntdlib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    std::vector<int32_t> samples;
    const n64::EadBank bank = SmallEadBank();
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples);

    // The resampler stops at 3.99992 times the output's rate: just under two octaves above key 60. Key 84 is past it.
    Check(zones.size() == 2, "a zone below the resampler's limit and one at it");
    Check(zones[0].low_key == 21 && zones[0].high_key == 83 && zones[0].follows_key, "the keys that follow");
    Check(zones[1].low_key == 84 && zones[1].high_key == 127 && !zones[1].follows_key, "the keys at the limit");
    Check(Near(zones[0].tune_cents, 0.0), "a tuning of 1");
    Check(Near(zones[1].tune_cents, 1200.0 * std::log2(3.99992), 1e-6), "the limit's pitch");

    // Four updates at 4 * 59.826 updates a second. The amplitude is the square of the line. A straight rise of the same
    // energy takes the last 0.6 of the time.
    const double rise_ms = 4.0 * 1000.0 / (4.0 * 59.826);
    Check(Near(zones[0].delay_ms, 0.4 * rise_ms, 1e-9) && Near(zones[0].attack_ms, 0.6 * rise_ms, 1e-9), "the rise");
    Check(Near(zones[0].gain, 1.0), "the top of the scale");
    Check(Near(zones[0].decay_ms, 0.0), "the level holds");

    // A release rate of 0x20 takes 0x20 * 24 from 32767 in each update. A fall of 100 dB of the same energy takes
    // 2 ln 10 times as long.
    const double release_ms = 32767.0 / (0x20 * 24.0) * 1000.0 / (4.0 * 59.826) * 2.0 * std::numbers::ln10;
    Check(Near(zones[0].release_ms, release_ms, 1e-6), "the release");
}

void TestEadEnvelopeScales()
{
    // The early engine's envelope is the line itself, and the amplitude squares the line. The next build squares the
    // values first, and the amplitude squares the line too. Its mixer volume at full velocity is 127 * 127 out of
    // 32768. A later one squares the values and takes the line as the amplitude, on a mixer volume of 1 at full
    // velocity.
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    ntdlib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    std::vector<int32_t> samples;
    const n64::EadBank bank = SmallEadBank(16384);
    const double half = 16384.0 / 32767.0;
    Check(Near(ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples).at(0).gain, half * half, 1e-9),
          "the early levels");
    settings.engine = n64::EadEngine::kMiddle;
    settings.levels = ntdlib::sequence_export::n64::EadLevels::kEnvelopeSquared;
    Check(Near(ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples).at(0).gain,
               half * half * half * half * 16129.0 / 32768.0, 1e-9),
          "the envelope squared");
    settings.levels = ntdlib::sequence_export::n64::EadLevels::kVolumesSquared;
    Check(Near(ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples).at(0).gain, half * half, 1e-9),
          "the volumes squared");
}

void TestEadVibrato()
{
    // A depth of 256 out of 4096 and the default rate. The curve peaks at 120 steps of 127 to the octave. A period is
    // 65536 / 0x800 = 32 updates. The LFO takes half the span and the zone's tuning takes the middle.
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    ntdlib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    key.vibrato_rate = 0x800;
    key.vibrato_depth = 256;
    std::vector<int32_t> samples;
    const n64::EadBank bank = SmallEadBank();
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples);
    const double up = 1200.0 * std::log2(1.0 + (256.0 / 4096.0) * (std::pow(2.0, 120.0 / 127.0) - 1.0));
    const double down = 1200.0 * std::log2(1.0 + (256.0 / 4096.0) * (std::pow(2.0, -120.0 / 127.0) - 1.0));
    Check(zones.at(0).vibrato.has_value(), "a vibrato");
    Check(Near(zones[0].vibrato->hz, 4.0 * 59.826 / 32.0, 1e-9), "the rate");
    Check(Near(zones[0].vibrato->depth, (up - down) / 2.0, 1e-9), "the depth");
    Check(Near(zones[0].tune_cents, (up + down) / 2.0, 1e-9), "the middle of the curve");
}

// A song of the library's early form: its lists start at offset 0 of the header, and it has no master track and no
// table of waves.
std::vector<uint8_t> MusEarlySong(const std::vector<MusChannel>& channels, const std::vector<uint8_t>& envelopes = {})
{
    constexpr uint32_t kLists = 0x18;
    const uint32_t count = static_cast<uint32_t>(channels.size());
    const auto put = [](std::vector<uint8_t>& bytes, std::size_t at, uint32_t value)
    {
        bytes[at] = static_cast<uint8_t>(value >> 24);
        bytes[at + 1] = static_cast<uint8_t>(value >> 16);
        bytes[at + 2] = static_cast<uint8_t>(value >> 8);
        bytes[at + 3] = static_cast<uint8_t>(value);
    };

    std::vector<uint8_t> song(kLists + 12 * count);
    const auto append = [&](const std::vector<uint8_t>& bytes)
    {
        const uint32_t at = static_cast<uint32_t>(song.size());
        song.insert(song.end(), bytes.begin(), bytes.end());

        return at;
    };

    put(song, 0x00, count);
    put(song, 0x04, kLists);
    put(song, 0x08, kLists + 4 * count);
    put(song, 0x0C, kLists + 8 * count);
    for (uint32_t i = 0; i < count; i++)
    {
        const MusChannel& channel = channels[i];
        put(song, kLists + 4 * i, append(channel.data));
        put(song, kLists + 4 * count + 4 * i, channel.volume.empty() ? 0 : append(channel.volume));
        put(song, kLists + 8 * count + 4 * i, channel.bend.empty() ? 0 : append(channel.bend));
    }

    put(song, 0x10, append(envelopes));

    return song;
}

ntdlib::sequence_export::n64::LibmusPerformance PlayEarlySong(const std::vector<uint8_t>& song)
{
    ntdlib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;

    return ntdlib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});
}

// The pitch that a song's last pitch command at or before `tick` sets, in semitones.
double PitchAt(const ntdlib::sequence_export::n64::Performance& performance, uint32_t tick)
{
    double pitch = 0.0;
    for (const midi::TrackEvent& event : EventsOfKind(performance, midi::EventKind::kPitch))
    {
        if (event.tick <= tick)
        {
            pitch = event.semitones;
        }
    }

    return pitch;
}

void TestLibmusEarlySong()
{
    // A channel starts without velocities, at a velocity of 127. Wave 1 is the bank's wave 1. An envelope's first byte
    // doesn't count. No wave's table or master track stands behind the note.
    const std::vector<uint8_t> envelopes = {5, 100, 6, 127, 28, 89, 54};
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        PlayEarlySong(MusEarlySong({{.data = {0x85, 120, 0x81, 1, 0x90, 0, 48, 96, 0x80}}}, envelopes));

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 60 && ons[0].value == 127, "the early form's note");
    Check(played.presets.size() == 1 && played.presets[0].wave == 1, "the early form's wave");
    const std::array<uint8_t, 7> expected = {5, 0, 6, 127, 28, 89, 54};
    Check(played.presets[0].envelope == expected, "the early form's envelope starts at nothing");
}

void TestLibmusEarlyZone()
{
    ntdlib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.song_volume = 128;
    settings.master_volume = 0x6FFF;

    // The envelope's first byte, its time unit, doesn't count: the attack takes 6 ticks and the release 54.
    const ntdlib::sequence_export::n64::LibmusPresetKey key = {.wave = 0, .envelope = {5, 0, 6, 127, 28, 89, 54}};
    const Zone zone = ntdlib::sequence_export::n64::LibmusZone(key, -48.0, settings, 96.0);
    Check(Near(zone.delay_ms + zone.attack_ms, 6.0 * 1000.0 / 96.0, 1e-6), "the early attack");
    Check(zone.exclusive_class == 0, "a note's release goes on beside the next note");
    Check(Near(zone.release_ms, 2.0 * std::numbers::ln10 * 54.0 * 1000.0 / 96.0, 1e-6), "the early release");

    // An attack of one tick starts at the peak.
    const ntdlib::sequence_export::n64::LibmusPresetKey sudden = {.wave = 0, .envelope = {1, 0, 1, 127, 255, 127, 1}};
    Check(Near(ntdlib::sequence_export::n64::LibmusZone(sudden, -48.0, settings, 96.0).attack_ms, 0.0), "no attack");
}

void TestLibmusEarlyBendRange()
{
    // The song's range of 0 takes the first pass's bend. The game's code at 0x80038790 sets the range back to 2/64 at
    // each new pass. A bend list's value of 80 stands 16 steps above the middle.
    const std::vector<uint8_t> data = {0xA8, 0, 0x95, 0xFF, 48, 96, 0x96};
    const std::vector<uint8_t> other = {0x95, 0xFF, 48, 0x81, 0x40, 0x96};
    const std::vector<uint8_t> bend = {0xD0, 0x83, 0x00};
    const ntdlib::sequence_export::n64::LibmusPerformance played =
        PlayEarlySong(MusEarlySong({{.data = data, .bend = bend}, {.data = other}}));
    Check(Near(PitchAt(played.performance, 95), 0.0), "the first pass has no bend");
    Check(Near(PitchAt(played.performance, 96), 16.0 / 32.0), "the second pass bends");
}

void TestLibmusPortamento()
{
    // A slide of 12 ticks from note 48 up to note 60.
    const std::vector<uint8_t> data = {48, 96, 0x82, 12, 60, 96, 0x80};
    const ntdlib::sequence_export::n64::LibmusPerformance played = PlayEarlySong(MusEarlySong({{.data = data}}));
    Check(Near(PitchAt(played.performance, 96), -12.0), "a slide starts at the last note");
    Check(Near(PitchAt(played.performance, 102), -6.0), "a slide's middle");
    Check(Near(PitchAt(played.performance, 108), 0.0), "a slide ends at the note");
}

void TestLibmusVibrato()
{
    // A period of 20 ticks and a depth of a semitone, after 10 ticks. The early form's count wraps before then, and the
    // pitch holds one value.
    const std::vector<uint8_t> data = {0x88, 10, 20, 50, 48, 96, 0x80};
    const ntdlib::sequence_export::n64::LibmusPerformance played = PlayEarlySong(MusEarlySong({{.data = data}}));
    Check(Near(PitchAt(played.performance, 3), PitchAt(played.performance, 8)), "no vibrato before its delay");
    Check(Near(PitchAt(played.performance, 15), 1.0, 0.1), "the vibrato's top");
    Check(Near(PitchAt(played.performance, 25), -1.0, 0.1), "the vibrato's bottom");
}

// A note of the later engine, in the terms that the performer and ntdlib's model share.
struct LaterNote
{
    double update = 0.0;
    int key = 0;
    int velocity = 0;
    int held = 0;
};

using LaterNotes = std::map<int, std::vector<LaterNote>>;
using LaterPorts = std::array<int, 8>;

constexpr LaterPorts kNoLaterPorts = {-1, -1, -1, -1, -1, -1, -1, -1};
constexpr int kLaterUpdatesPerFrame = 3;
constexpr int kLaterFrames = 600;

n64::EadLaterFiles LaterFiles(std::vector<std::vector<uint8_t>> sequences)
{
    n64::EadLaterFiles files;
    files.bank_sets.assign(sequences.size(), {0});
    files.sequences = std::move(sequences);
    files.banks = {SmallEadBank()};
    files.sample_places = {{0, 0}};

    return files;
}

// A sequence of the later engine. It starts channels 0 and 1, sets a tempo of 120 and runs `player`, then waits for 512
// ticks and ends. Each channel selects program 0, starts one layer with `layer_command`, waits and ends.
std::vector<uint8_t> LaterSequence(std::span<const uint8_t> player, std::span<const uint8_t> first_layer,
                                   std::span<const uint8_t> second_layer, uint8_t layer_command = 0x88)
{
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x03, 0x90, 0x00, 0x00, 0x91, 0x00, 0x00, 0xDD, 0x78};
    bytes.insert(bytes.end(), player.begin(), player.end());
    const std::array<uint8_t, 4> wait_and_end = {0xFD, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), wait_and_end.begin(), wait_and_end.end());

    const auto add_channel = [&](std::size_t start_place, std::span<const uint8_t> layer)
    {
        constexpr std::size_t kChannelSize = 9;
        const std::size_t place = bytes.size();
        const std::size_t layer_place = place + kChannelSize;
        bytes[start_place] = static_cast<uint8_t>(place >> 8);
        bytes[start_place + 1] = static_cast<uint8_t>(place);
        const std::array<uint8_t, kChannelSize> channel = {
            0xC1, 0x00, layer_command, static_cast<uint8_t>(layer_place >> 8), static_cast<uint8_t>(layer_place), 0xFD,
            0x82, 0x00, 0xFF};
        bytes.insert(bytes.end(), channel.begin(), channel.end());
        bytes.insert(bytes.end(), layer.begin(), layer.end());
    };

    add_channel(4, first_layer);
    add_channel(7, second_layer);

    return bytes;
}

LaterNotes LaterModelNotes(const n64::EadLaterFiles& files, int sequence, const LaterPorts& ports)
{
    constexpr uint32_t kOutputSize = 0x4000;
    constexpr std::size_t kWorkSize = 0x400000;
    constexpr int kSamplesPerFrame = 528;

    const std::vector<uint8_t> image(0x1000, 0);
    n64::AudioMemory memory(image, kWorkSize);
    n64::EadLaterPlayer player(n64::EadLaterConfig{}, files, memory);
    const uint32_t output = memory.Allocate(kOutputSize);

    LaterNotes notes;
    player.on_note = [&](const n64::EadNoteStart& note)
    {
        notes[note.channel].push_back({static_cast<double>(note.update), note.key + 21,
                                       static_cast<int>(std::lround(std::sqrt(note.velocity) * 127.0)),
                                       note.length - note.gate});
    };

    for (std::size_t port = 0; port < ports.size(); port++)
    {
        if (ports[port] != -1)
        {
            player.WritePlayerPort(0, static_cast<int>(port), static_cast<int8_t>(ports[port]));
        }
    }

    player.Start(0, sequence);
    n64::AudioList list;
    for (int frame = 0; frame < kLaterFrames && player.Enabled(0); frame++)
    {
        list.Clear();
        player.RunFrame(list, memory, output, kSamplesPerFrame);
    }

    return notes;
}

LaterNotes LaterPerformedNotes(const n64::EadLaterFiles& files, int sequence, const LaterPorts& ports)
{
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    std::vector<std::span<const uint8_t>> sequences;
    for (const std::vector<uint8_t>& bytes : files.sequences)
    {
        sequences.emplace_back(bytes);
    }

    ntdlib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLater;
    settings.levels = ntdlib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
    settings.sequence = files.sequences[static_cast<std::size_t>(sequence)];
    settings.bank_set = files.bank_sets[static_cast<std::size_t>(sequence)];
    settings.sequences = sequences;
    settings.bank_sets = files.bank_sets;
    settings.ports = ports;
    settings.banks = &banks;
    const ntdlib::sequence_export::n64::EadPerformance played = ntdlib::sequence_export::n64::PerformEad(settings, {});

    const auto& tempo = played.performance.tempo;
    const double updates_per_second = kLaterUpdatesPerFrame * settings.clock.frame_rate;
    const auto update_of = [&](uint32_t tick)
    {
        double seconds = 0.0;
        for (std::size_t i = 0; i < tempo.size(); i++)
        {
            const uint32_t end = i + 1 < tempo.size() ? tempo[i + 1].tick : tick;
            const uint32_t to = std::min(tick, end);
            if (to > tempo[i].tick)
            {
                seconds += (to - tempo[i].tick) * 60.0 / (tempo[i].bpm * 48.0);
            }
        }

        return seconds * updates_per_second;
    };

    LaterNotes notes;
    for (std::size_t channel = 0; channel < played.performance.tracks.size(); channel++)
    {
        std::map<uint32_t, uint32_t> offs;
        for (const midi::TrackEvent& event : played.performance.tracks[channel])
        {
            if (event.kind == midi::EventKind::kNoteOff)
            {
                offs[event.note] = event.tick;
            }
        }

        for (const midi::TrackEvent& event : played.performance.tracks[channel])
        {
            if (event.kind == midi::EventKind::kNoteOn)
            {
                notes[static_cast<int>(channel)].push_back({update_of(event.tick), event.key, event.value,
                                                            static_cast<int>(offs.at(event.note) - event.tick)});
            }
        }
    }

    return notes;
}

// Compares the performer with ntdlib's model, which gives the games' audio lists command for command. No value is
// recorded here. A note may start up to two frames from the model's, since a tick falls between the frame's updates.
void CheckSameAsLaterModel(const n64::EadLaterFiles& files, int sequence, const LaterPorts& ports, const char* what)
{
    constexpr double kMostUpdatesApart = 6.0;

    const LaterNotes model = LaterModelNotes(files, sequence, ports);
    const LaterNotes performed = LaterPerformedNotes(files, sequence, ports);
    Check(!model.empty(), what);
    Check(model.size() == performed.size(), what);
    for (const auto& [channel, notes] : model)
    {
        const std::vector<LaterNote>& others = performed.at(channel);
        Check(notes.size() == others.size(), what);
        for (std::size_t i = 0; i < notes.size(); i++)
        {
            Check(Near(notes[i].update, others[i].update, kMostUpdatesApart), what);
            Check(notes[i].key == others[i].key, what);
            Check(std::abs(notes[i].velocity - others[i].velocity) <= 1, what);
            Check(notes[i].held == others[i].held, what);
        }
    }
}

// A velocity of 100, a gate of 0x80 and two notes.
constexpr std::array<uint8_t, 9> kLaterLayer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0x2B, 0x18, 0xFF};

void TestLaterNotes()
{
    // After 48 ticks the player's tempo gains 16 * 48.
    const std::array<uint8_t, 4> player = {0xFD, 0x30, 0xDC, 0x10};
    const n64::EadLaterFiles files = LaterFiles({LaterSequence(player, kLaterLayer, kLaterLayer)});
    CheckSameAsLaterModel(files, 0, kNoLaterPorts, "the later engine's notes and tempo");
}

void TestLaterLayerNumbers()
{
    // A layer's number above 3 stands for layer 0.
    const n64::EadLaterFiles files = LaterFiles({LaterSequence({}, kLaterLayer, kLaterLayer, 0x8C)});
    CheckSameAsLaterModel(files, 0, kNoLaterPorts, "a layer's high number");
}

void TestLaterPorts()
{
    // The player reads port 4 and disables channel 1 unless the port holds 0.
    const std::array<uint8_t, 4> player = {0x84, 0xF3, 0x01, 0x41};
    const n64::EadLaterFiles files = LaterFiles({LaterSequence(player, kLaterLayer, kLaterLayer)});
    LaterPorts zero = kNoLaterPorts;
    zero[4] = 0;
    LaterPorts one = kNoLaterPorts;
    one[4] = 1;
    CheckSameAsLaterModel(files, 0, zero, "a port of 0");
    CheckSameAsLaterModel(files, 0, one, "a port of 1");
    Check(LaterPerformedNotes(files, 0, zero).contains(1), "a port of 0 keeps channel 1");
    Check(!LaterPerformedNotes(files, 0, one).contains(1), "a port of 1 disables channel 1");
}

void TestLaterChainedSequence()
{
    // The first sequence starts the second in its place.
    const std::array<uint8_t, 3> chain = {0xC4, 0xFF, 0x01};
    const n64::EadLaterFiles files =
        LaterFiles({LaterSequence(chain, kLaterLayer, kLaterLayer), LaterSequence({}, kLaterLayer, kLaterLayer)});
    CheckSameAsLaterModel(files, 0, kNoLaterPorts, "a chained sequence");
}

// The gain that the first note of a sequence's only channel has in its preset. The channel sets `gain`, and the layer
// starts with `layer_start`.
int LaterSampleGain(uint8_t gain, std::vector<uint8_t> layer_start)
{
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0A, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> channel = {0xED, gain, 0xC1, 0x00, 0x88, 0x00, 0x14, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    bytes.insert(bytes.end(), layer_start.begin(), layer_start.end());
    bytes.insert(bytes.end(), kLaterLayer.begin(), kLaterLayer.end());

    const n64::EadLaterFiles files = LaterFiles({bytes});
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLaterMajora;
    settings.levels = ntdlib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
    settings.sequence = files.sequences[0];
    settings.bank_set = files.bank_sets[0];
    const std::vector<std::span<const uint8_t>> sequences = {files.sequences[0]};
    settings.sequences = sequences;
    settings.bank_sets = files.bank_sets;
    settings.banks = &banks;

    return ntdlib::sequence_export::n64::PerformEad(settings, {}).presets.at(0).sample_gain;
}

void TestLaterChannelGain()
{
    Check(LaterSampleGain(34, {}) == 34, "a channel's gain reaches its preset");
    Check(LaterSampleGain(16, {}) == 0, "a gain of 1 changes nothing");
    Check(LaterSampleGain(8, {}) == 0, "the engine reads a lower gain as 1");

    const std::vector<uint8_t> clear_gain = {0xF0, 0x00, 0x40};
    Check(LaterSampleGain(34, clear_gain) == 0, "a layer can drop its channel's gain");
}

void TestLaterChannelLayers()
{
    // A channel starts two layers, by numbers 0 and 4, that each play two notes.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0A, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> channel = {0xC1, 0x00, 0x88, 0x00, 0x17, 0x8C, 0x00, 0x20, 0xFD, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    bytes.push_back(0xFF);
    bytes.insert(bytes.end(), kLaterLayer.begin(), kLaterLayer.end());
    const std::vector<uint8_t> second = {0xC1, 100, 0xC9, 0x80, 0x30, 0x30, 0x34, 0x18, 0xFF};
    bytes.insert(bytes.end(), second.begin(), second.end());

    const n64::EadLaterFiles files = LaterFiles({bytes});
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    const std::vector<std::span<const uint8_t>> sequences = {files.sequences[0]};
    const auto notes = [&](std::size_t layers)
    {
        ntdlib::sequence_export::n64::EadPerformSettings settings;
        settings.engine = n64::EadEngine::kLaterMajora;
        settings.levels = ntdlib::sequence_export::n64::EadLevels::kVolumesSquared;
        settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
        settings.channel_layers = layers;
        settings.sequence = files.sequences[0];
        settings.bank_set = files.bank_sets[0];
        settings.sequences = sequences;
        settings.bank_sets = files.bank_sets;
        settings.banks = &banks;

        return AllEventsOfKind(ntdlib::sequence_export::n64::PerformEad(settings, {}).performance,
                               midi::EventKind::kNoteOn);
    };

    // Ocarina of Time's and Majora's Mask's channels hold four layers. Number 4 stands for the first.
    const std::vector<midi::TrackEvent> four = notes(4);
    Check(four.size() == 2, "number 4 replaces the first layer of four");

    // Pokémon Stadium 2's code at 0x8001EA54 keeps numbers below 6.
    const std::vector<midi::TrackEvent> six = notes(6);
    Check(six.size() == 4, "number 4 is a layer of its own among six");
}

ntdlib::sequence_export::n64::EadPerformance PlayLater(const std::vector<uint8_t>& bytes,
                                                       const ntdlib::sequence_export::n64::EadGameCall& call = {})
{
    const n64::EadLaterFiles files = LaterFiles({bytes});
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    const std::vector<std::span<const uint8_t>> sequences = {files.sequences[0]};
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLater;
    settings.levels = ntdlib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
    settings.sequence = files.sequences[0];
    settings.bank_set = files.bank_sets[0];
    settings.sequences = sequences;
    settings.bank_sets = files.bank_sets;
    settings.banks = &banks;
    settings.call = call;

    return ntdlib::sequence_export::n64::PerformEad(settings, {});
}

void TestLaterFilter()
{
    // 0xB0 sets the place of the channel's filter. 0xB3 then loads low-pass row 5 and high-pass row 2 there. The note
    // plays through row 5, and the high-pass filter is left out.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0C, 0xDD, 0x78, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> channel = {0xC1, 0x00, 0xB0, 0x00, 0x40, 0xB3, 0x52,
                                          0x88, 0x00, 0x1A, 0xFD, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    const std::vector<uint8_t> layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF};
    bytes.insert(bytes.end(), layer.begin(), layer.end());

    const ntdlib::sequence_export::n64::EadPerformance played = PlayLater(bytes);

    Check(played.presets.size() == 1 && played.presets[0].low_pass == 5, "the note plays through the loaded row");
    Check(played.performance.approximations.contains("a channel's high-pass filter left out"), "no high-pass filter");
}

void TestLaterStopOtherChannel()
{
    // Channel 0 stops channel 1 at tick 16 with 0xCD 0x01. Channel 1's note ends there. Channel 0's note ends at its
    // gate, 24 ticks in.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x03, 0x90, 0x00, 0x0F, 0x91, 0x00,
                                  0x23, 0xDD, 0x78, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> first = {0xC1, 0x00, 0x88, 0x00, 0x1C, 0xFD, 0x10, 0xCD, 0x01, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> short_layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF};
    const std::vector<uint8_t> second = {0xC1, 0x00, 0x88, 0x00, 0x2C, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> long_layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x81, 0x00, 0xFF};
    for (const std::vector<uint8_t>* part : {&first, &short_layer, &second, &long_layer})
    {
        bytes.insert(bytes.end(), part->begin(), part->end());
    }

    const ntdlib::sequence_export::n64::Performance performance = PlayLater(bytes).performance;

    const auto off_of = [&](std::size_t track)
    {
        const auto found = std::ranges::find_if(performance.tracks.at(track), [](const midi::TrackEvent& event)
                                                { return event.kind == midi::EventKind::kNoteOff; });
        return found != performance.tracks.at(track).end() ? static_cast<int>(found->tick) : -1;
    };

    Check(off_of(1) == 16, "the other channel stops");
    Check(off_of(0) == 24, "the channel that stops it plays on");
}

void TestLaterGameCall()
{
    // Channel 0 sets its pan to 0 and its reverb to 0x10, and writes 0x40 to its IO port 5. Layer 0 holds a note with a
    // pan of 127. 96 ticks in, the channel reads IO port 4 of channel 15. Unless that's -1, layer 1 plays a second
    // note. The call of Ganon's Tower's level 1 comes on audio frame 30, about 48 ticks in (Ocarina of Time's code at
    // 0x800C5CD8 and 0x800C5D84). Its pan weight of 0 takes the pan from the layer. The volume of 80 gives the player a
    // scale of 80/127, and a reverb of 0x40 - 80 + 127 for the channel's port. Its cutoff of 0x50 goes to channel 15.
    const std::vector<uint8_t> bytes = {
        0xD7, 0x00, 0x01, 0x90, 0x00, 0x0C, 0xDD, 0x78, 0xFD, 0x82, 0x00, 0xFF,                         // the sequence
        0xC1, 0x00, 0xDD, 0x00, 0xCC, 0x40, 0x75, 0xD4, 0x10, 0x88, 0x00, 0x26, 0xFD, 0x60, 0x4F, 0x04, // channel 0
        0xF9, 0x00, 0x25, 0x89, 0x00, 0x30, 0xFD, 0x81, 0x00, 0xFF,                                     //
        0xC1, 0x64, 0xC9, 0x80, 0xCA, 0x7F, 0x27, 0x82, 0x00, 0xFF,                                     // layer 0
        0xC1, 0x64, 0xC9, 0x80, 0x29, 0x81, 0x00, 0xFF};                                                // layer 1
    const ntdlib::sequence_export::n64::EadGameCall level_1 = {
        .frame = 30, .pan_weight = 0, .volume = 80, .io_channel = 15, .io_port = 4, .io_value = 0x50, .reverb_port = 5};

    const ntdlib::sequence_export::n64::Performance plain = PlayLater(bytes).performance;
    const ntdlib::sequence_export::n64::Performance called = PlayLater(bytes, level_1).performance;

    const std::vector<midi::TrackEvent> reverbs = EventsOfKind(called, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> pans = EventsOfKind(called, midi::EventKind::kPan);
    const std::vector<midi::TrackEvent> levels = EventsOfKind(called, midi::EventKind::kExpression);
    Check(!reverbs.empty() && reverbs.front().value == 0x10 && reverbs.back().value == 0x40 - 80 + 127,
          "the call gives the reverb from IO port 5");
    Check(pans.size() >= 2 && pans[0].value == 0 && pans[1].value == 127, "a pan weight of 0 takes the layer's pan");
    Check(levels.size() >= 2 && levels[1].value == 80, "the player's volume scale");
    Check(levels.size() >= 2 && pans.size() >= 2 && levels[1].tick == pans[1].tick && pans[1].tick > 40 &&
              pans[1].tick < 56,
          "the call comes on frame 30");
    Check(EventsOfKind(called, midi::EventKind::kNoteOn).size() == 2, "the call writes IO port 4 of channel 15");
    Check(EventsOfKind(plain, midi::EventKind::kNoteOn).size() == 1, "without the call, the port stays at -1");
    Check(EventsOfKind(plain, midi::EventKind::kReverb).back().value == 0x10, "without the call, the reverb stays");

    // The entrance's level, 7, sets the pan weight and waits 2 game frames, 6 audio frames, for the rest.
    ntdlib::sequence_export::n64::EadGameCall level_7 = level_1;
    level_7.volume = 60;
    level_7.volume_delay = 6;
    const ntdlib::sequence_export::n64::Performance entrance = PlayLater(bytes, level_7).performance;
    const std::vector<midi::TrackEvent> entrance_pans = EventsOfKind(entrance, midi::EventKind::kPan);
    const std::vector<midi::TrackEvent> entrance_levels = EventsOfKind(entrance, midi::EventKind::kExpression);
    Check(entrance_levels.size() >= 2 && entrance_pans.size() >= 2 && entrance_levels[1].value == 60 &&
              entrance_levels[1].tick > entrance_pans[1].tick,
          "the entrance's volume comes later");
    Check(EventsOfKind(entrance, midi::EventKind::kReverb).back().value == 127, "a reverb past 127 stays at 127");
}

void TestLoudZoneThatNoNotePlays()
{
    // Preset 0 plays at full level below velocity 64 and 10.4 dB above it there. A note at velocity 40 plays the first
    // zone only, so the SoundFont keeps its level. A note at velocity 100 plays the loud zone, and every zone then
    // takes the cut.
    const auto build = [](uint8_t velocity)
    {
        ntdlib::sequence_export::n64::Performance performance;
        performance.timebase = 48;
        ntdlib::sequence_export::n64::Instrument preset;
        preset.name = "p0";
        auto wave = std::make_shared<ntdlib::sequence_export::n64::Wave>();
        wave->name = "w";
        wave->rate = 32000;
        wave->pcm.channels = {std::vector<int16_t>(64, 1000)};
        ntdlib::sequence_export::n64::Zone quiet;
        quiet.wave = wave;
        quiet.high_velocity = 63;
        ntdlib::sequence_export::n64::Zone loud;
        loud.wave = wave;
        loud.low_velocity = 64;
        loud.gain = 3.31;
        preset.zones = {quiet, loud};
        performance.presets.push_back(preset);
        performance.tracks[0] = {{.tick = 0, .kind = midi::EventKind::kPreset, .preset = 0},
                                 {.tick = 0, .kind = midi::EventKind::kNoteOn, .key = 60, .value = velocity, .note = 1},
                                 {.tick = 48, .kind = midi::EventKind::kNoteOff, .key = 60, .note = 1}};

        return ntdlib::sequence_export::n64::BuildFiles(performance, "test", "test").performance.approximations;
    };

    const auto cut = [](const auto& approximations)
    {
        return std::ranges::any_of(approximations, [](const auto& entry)
                                   { return entry.first.starts_with("instrument volume above full level"); });
    };

    Check(!cut(build(40)), "a loud zone that no note plays cuts nothing");
    Check(cut(build(100)), "a loud zone that a note plays cuts every zone");
}

void TestMiddleFadeIn()
{
    // 0xDA 0x01 sets a fade-in of 100 updates, and 0xDB then fades the song in to full volume. At 120 bpm, a tick takes
    // 4 * 3590 / 5760 = 2.493 updates. The fade therefore ends in the 41st tick, at tick 40. The note holds for 128
    // ticks, past the end of the fade.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x13, 0xDA, 0x01, 0x00, 0x64,
                                  0xDB, 0x7F, 0xDD, 0x78, 0xFD, 0x60, 0xFB, 0x00, 0x0E};
    const std::vector<uint8_t> channel = {0xC1, 0x00, 0x90, 0x00, 0x1C, 0xFD, 0x81, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    const std::vector<uint8_t> layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x81, 0x00, 0xFF};
    bytes.insert(bytes.end(), layer.begin(), layer.end());

    const ntdlib::sequence_export::n64::EadPerformance played = PlayEad(
        bytes, SmallEadBank(), n64::EadEngine::kMiddle, ntdlib::sequence_export::n64::EadLevels::kVolumesSquared);

    const std::vector<midi::TrackEvent> levels = EventsOfKind(played.performance, midi::EventKind::kExpression);
    Check(!levels.empty() && levels.back().value == 127, "the song fades in to full volume");
    Check(!levels.empty() && levels.back().tick == 40, "the fade takes 100 updates");
}

void TestLaterZones()
{
    ntdlib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLater;
    settings.levels = ntdlib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;

    // A release rate of 200 falls for 251 - 200 = 51 frames. A straight line of the amplitude reaches -100 dB at 10/3
    // of ln(10) of its time.
    ntdlib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    key.release_rate = 200;
    std::vector<int32_t> samples;
    n64::EadBank bank = SmallEadBank();
    const double seconds = 51.0 / settings.clock.frame_rate;
    const std::vector<Zone> zones = ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples);
    Check(Near(zones.at(0).release_ms, 10.0 / 3.0 * std::numbers::ln10 * seconds * 1000.0, 1.0), "a release's time");

    // The filter's row 5 cuts at 4360 Hz, and its gain at no frequency reaches 1.
    key.low_pass = 5;
    const std::vector<Zone> filtered = ntdlib::sequence_export::n64::EadZones(key, bank, settings, samples);
    Check(Near(filtered.at(0).low_pass_hz, 4360.0), "the filter's cutoff");
    Check(Near(filtered.at(0).gain, zones.at(0).gain * 0.854, 1e-9), "the filter's gain");

    // Pokémon Stadium 2's code at 0x8001BF34 reads a delay of -4 as a command that takes no time. Its code at
    // 0x8000E810 starts a note whose list opens that way at its first update's level, with no ramp from silence.
    n64::EadBank skipping = SmallEadBank();
    std::vector<n64::EadEnvelope::Point>& points = skipping.envelopes.at(0).points;
    points.insert(points.begin(), {n64::kEnvelopeSkip, 0});
    settings.envelope_skip = true;
    const std::vector<Zone> skipped = ntdlib::sequence_export::n64::EadZones(key, skipping, settings, samples);
    Check(Near(skipped.at(0).gain, filtered.at(0).gain, 1e-9), "an envelope's point that takes no time");
    Check(skipped.at(0).attack_ms < filtered.at(0).attack_ms, "a list that opens with that point rises from a level");

    // Bank 0x17 of the game holds (-4, 0), then (1, 32700): the note starts at its top.
    points.at(1).delay = 1;
    const std::vector<Zone> at_once = ntdlib::sequence_export::n64::EadZones(key, skipping, settings, samples);
    Check(Near(at_once.at(0).attack_ms, 0.0, 1e-9), "a first line of one update");

    // The point later in a list leaves the note's start alone.
    n64::EadBank later_skip = SmallEadBank();
    std::vector<n64::EadEnvelope::Point>& later_points = later_skip.envelopes.at(0).points;
    later_points.insert(later_points.begin() + 1, {n64::kEnvelopeSkip, 0});
    const std::vector<Zone> later_zones = ntdlib::sequence_export::n64::EadZones(key, later_skip, settings, samples);
    settings.envelope_skip = false;
    Check(Near(later_zones.at(0).attack_ms, filtered.at(0).attack_ms, 1e-9), "the point later in a list");

    // Pokémon Stadium 2's code at 0x8001BC5C reads a vibrato as 65536 / (wave * depth / 4096 + 65536), for a wave of up
    // to 32767 either way. A depth of 1024 gives scales of 1 / (1 + 1/8) and 1 / (1 - 1/8).
    ntdlib::sequence_export::n64::EadPresetKey shaking = key;
    shaking.vibrato_rate = 0x400;
    shaking.vibrato_depth = 1024;
    const std::vector<Zone> plain_vibrato = ntdlib::sequence_export::n64::EadZones(shaking, bank, settings, samples);
    settings.inverse_vibrato = true;
    const std::vector<Zone> inverse_vibrato = ntdlib::sequence_export::n64::EadZones(shaking, bank, settings, samples);
    settings.inverse_vibrato = false;
    const double up = -1200.0 * std::log2(1.0 - 1.0 / 8.0);
    const double down = -1200.0 * std::log2(1.0 + 1.0 / 8.0);
    Check(plain_vibrato.at(0).vibrato.has_value() &&
              Near(plain_vibrato.at(0).vibrato->depth, 1200.0 * std::log2(1.25), 1e-6),
          "the depth of the usual vibrato");
    Check(inverse_vibrato.at(0).vibrato.has_value() &&
              Near(inverse_vibrato.at(0).vibrato->depth, (up - down) / 2.0, 1e-6),
          "the depth of Pokémon Stadium 2's vibrato");
    Check(Near(inverse_vibrato.at(0).tune_cents - plain_vibrato.at(0).tune_cents, (up + down) / 2.0, 1e-6),
          "the middle of Pokémon Stadium 2's vibrato");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64",
                                    []()
                                    {
                                        TestImageOrders();
                                        TestZippedImage();
                                        TestNotesAndTempo();
                                        TestLibultraRate();
                                        TestUnusedInstrumentsStayOut();
                                        TestForeverLoop();
                                        TestCountedLoop();
                                        TestControllers();
                                        TestZones();
                                        TestEnvelope();
                                        TestLineRampEnvelope();
                                        TestOscillators();
                                        TestAmbrellaLoop();
                                        TestAmbrellaControllers();
                                        TestAmbrellaSweep();
                                        TestAmbrellaNoteOff();
                                        TestAmbrellaZones();
                                        TestHalScales();
                                        TestEadNote();
                                        TestEadLevels();
                                        TestEadForeverLoop();
                                        TestEadLegato();
                                        TestEadPortamento();
                                        TestEadDrums();
                                        TestEadZones();
                                        TestEadEnvelopeScales();
                                        TestEadVibrato();
                                        TestLaterNotes();
                                        TestLaterLayerNumbers();
                                        TestLaterPorts();
                                        TestLaterChainedSequence();
                                        TestLaterChannelGain();
                                        TestLaterChannelLayers();
                                        TestLaterFilter();
                                        TestLaterStopOtherChannel();
                                        TestLaterGameCall();
                                        TestLoudZoneThatNoNotePlays();
                                        TestMiddleFadeIn();
                                        TestLaterZones();
                                        TestLibmusNote();
                                        TestLibmusVelocity();
                                        TestLibmusDrums();
                                        TestLibmusLists();
                                        TestLibmusLoops();
                                        TestLibmusZone();
                                        TestLibmusSharedTrack();
                                        TestLibmusEarlySong();
                                        TestLibmusEarlyZone();
                                        TestLibmusEarlyBendRange();
                                        TestLibmusPortamento();
                                        TestLibmusVibrato();
                                        TestUnpackDirectoryFile();
                                        TestLibmusTempoScale();
                                        TestLibmusRepeats();
                                        TestLibmusSongTables();
                                        TestLibmusTablesOutsideImage();
                                        TestLibmusSend();
                                        TestSbnClock();
                                        TestSbnShortNote();
                                        TestSbnVoices();
                                        TestSbnComposition();
                                        TestSbnMix();
                                        TestSbnLinked();
                                        TestSbnChanges();
                                        TestSbnLevels();
                                        TestSbnMasterAndInstrument();
                                        TestSbnPresets();
                                        TestSbnZones();
                                        TestSbnDrumZone();
                                        TestSbnDrumNote();
                                        TestResamplerLowPassResponse();
                                        TestResamplerLowPassSine();
                                        TestResamplerLowPassEnds();
                                        TestResamplerLowPassLoop();
                                        TestResamplerLowPassClips();
                                    });
}
