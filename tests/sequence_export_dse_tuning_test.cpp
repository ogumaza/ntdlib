// SPDX-License-Identifier: MIT

// Tests of the DSE conversion's key groups, tuning and loops (ntdlib/sequence_export/dse): the keys that the zones play
// at the driver's timers, the bends of the timers, the parts that bent notes take, the pass test that finds a
// sequence's loop, and the conversion of a bank. The expected values are worked out by hand from the driver's keys
// and timers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sfz/track_file.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export_dse_cases.h"

namespace
{

using namespace sequence_export_dse_cases;

// Key group 1 of the test's bank allows two voices of a channel. With one, a note of the group takes the voice of the
// group's last note on the channel, and the zones of program 1's splits take the group as their exclusive class.
void TestKeyGroups()
{
    TestFiles test;
    Check(std::ranges::all_of(PresetZones(test.bank, {.bank = files::kBankId, .program = 1}),
                              [](const PresetZone& z) { return z.exclusive_class == 0; }),
          "a key group of two voices has no exclusive class");
    test.bank.instruments->key_groups.at(1)[2] = 1;
    const std::vector<PresetZone> one = PresetZones(test.bank, {.bank = files::kBankId, .program = 1});
    const std::vector<PresetZone> two = PresetZones(test.bank, {.bank = files::kBankId, .program = 2});
    Check(one.size() == 2 && std::ranges::all_of(one, [](const PresetZone& z) { return z.exclusive_class == 1; }) &&
              std::ranges::all_of(two, [](const PresetZone& z) { return z.exclusive_class == 0; }),
          "the zones of a key group of one voice take it as their exclusive class");

    // Key 60 of program 0 sounds from tick 0 to 96. Key 64 of program 1 starts at 0 for 48 ticks, and key 65 at 12.
    // Both play program 1's second split, whose wave loops. Key 65 takes key 64's voice at 12. The three notes share a
    // part, and keys 64 and 65 a preset: key 65's exclusive class cuts key 64 where key 60 still sounds.
    const Bytes track = files::Join({{1, 0, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40},
                                     files::Note(0x60, 0, 1, 0x60, 1),
                                     {0xAC, 0x01},
                                     files::Note(0x60, 4, 0, 0x30, 1),
                                     {0x92, 0x0C},
                                     files::Note(0x60, 5, 0, 0x0C, 1),
                                     {0x92, 0x60, 0x98}});
    const Conversion c = ConvertEffect(test, track);
    CheckFiles(c, "the exclusive cut");
    const std::vector<std::pair<uint32_t, int>> offs = NoteOffs(c.performance, 0);
    Check(std::ranges::find(offs, std::pair<uint32_t, int>{12, 64}) != offs.end() &&
              std::ranges::find(offs, std::pair<uint32_t, int>{96, 60}) != offs.end(),
          "key 65 cuts key 64");
    Check(!c.performance.approximations.contains(kCut), "the exclusive class cuts the note");

    const seqlib::sf2::Records sf2 = seqlib::sf2::Read(c.sf2);
    std::vector<uint16_t> classes;
    for (std::size_t zone = 0; zone + 1 < sf2.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = sf2.ZoneGens(zone);
        if (const auto found = gens.find(seqlib::sf2::kExclusiveClass); found != gens.end())
        {
            classes.push_back(found->second);
        }
    }

    // Each split's keys may split into runs of their tunings (TunedZones). Each run of a split keeps its class.
    Check(classes.size() >= 2 && std::ranges::all_of(classes, [](uint16_t each) { return each == 1; }),
          "program 1's zones have exclusive class 1");
    std::size_t groups = 0;
    std::size_t any_group = 0;
    for (const seqlib::sfz::File& file : c.sfz.files)
    {
        for (std::size_t at = 0; (at = file.text.find(" group=1 off_by=1\n", at)) != std::string::npos; at++)
        {
            groups++;
        }
        for (std::size_t at = 0; (at = file.text.find(" group=", at)) != std::string::npos; at++)
        {
            any_group++;
        }
    }
    Check(groups >= 2 && groups == any_group, "program 1's regions have group 1 and off_by 1");
}

// A key that a channel played in a run of the driver's code, and the timer that the driver's command gave the channel.
// Explorers of Sky's ARM9 and ARM7 code (North America) played keys 0 to 127 in turn on a split of each tuning,
// with no bend (on ntdlib's DS machine, with a song and its bank in place of bgm0001's).
struct PlayedKey
{
    int16_t tuning = 0; // the split's tuning and coarse tuning, in 256ths of a semitone
    uint8_t format = 0; // 2 for IMA-ADPCM, 3 for the PSG
    int key = 0;
    int32_t timer = 0;
};

constexpr PlayedKey kPlayedKeys[] = {
    {0, 2, 60, 512},     {0, 2, 84, 128},     {0, 2, 96, 64},      {0, 2, 100, 50},     {0, 2, 102, 45},
    {0, 2, 108, 32},     {0, 2, 110, 28},     {0, 2, 112, 25},     {0, 2, 116, 20},     {0, 2, 118, 16396},
    {-1743, 2, 60, 758}, {-1743, 2, 100, 75}, {-1743, 2, 110, 42}, {-1743, 2, 118, 26}, {0, 3, 0, 3839},
    {0, 3, 24, 177},     {0, 3, 60, 8194},    {0, 3, 100, 812},    {0, 3, 116, 322},    {-4814, 2, 72, 758},
    {-4814, 2, 96, 189}, {-4814, 2, 120, 47}, {-4814, 2, 127, 31},
};

// The cents above `sample_rate` at which a channel plays at `timer`: the PSG takes 16 samples of its wave a period.
double TimerCents(int32_t timer, uint8_t format, uint32_t sample_rate)
{
    const double rate = ntdlib::ntr::kSystemClockHz / 2.0 / timer * (format >= 3 ? 16.0 : 1.0);
    return 1200.0 * std::log2(rate / sample_rate);
}

// The cents above the sample's rate at which a zone of `tuning` plays `key`.
double ZoneCents(const ZoneTuning& tuning, int key)
{
    return 100.0 * (key - tuning.root_key) + 100.0 * tuning.coarse + tuning.fine;
}

// Program 0 of the test's bank plays wave 0 (32,000 Hz) with the tuning of -4814. The game's code gave keys 72, 96 and
// 120 these timers (kPlayedKeys).
constexpr uint32_t kWaveZeroRate = 32000;
constexpr std::pair<int, int32_t> kWaveZeroTimers[] = {{72, 758}, {96, 189}, {120, 47}};

// A zone or a region that plays one sample: its keys, and the cents above the sample's rate at which it plays key 0.
// It plays each key 100 cents above the key below.
struct SampleZone
{
    int low_key = 0;
    int high_key = 127;
    double key_zero_cents = 0.0;
};

// The zones of `sf2` that play the sample named `sample`, in the order of the instruments' zones.
std::vector<SampleZone> SoundFontZones(const std::vector<uint8_t>& sf2, const std::string& sample)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<SampleZone> zones;
    for (std::size_t z = 0; z + 1 < records.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> gens = records.ZoneGens(z);
        const auto keys = gens.find(seqlib::sf2::kKeyRange);
        const auto id = gens.find(seqlib::sf2::kSampleId);
        if (keys == gens.end() || id == gens.end())
        {
            continue;
        }

        const uint8_t* header = records.Record("shdr", 46, id->second);
        if (std::string(reinterpret_cast<const char*>(header)) != sample)
        {
            continue;
        }

        const auto gen = [&](uint16_t op, int otherwise)
        {
            const auto found = gens.find(op);
            return found != gens.end() ? static_cast<int16_t>(found->second) : otherwise;
        };
        const int root = gen(seqlib::sf2::kOverridingRootKey, header[40]);
        zones.push_back({.low_key = keys->second & 0xFF,
                         .high_key = keys->second >> 8,
                         .key_zero_cents = -100.0 * root + 100.0 * gen(seqlib::sf2::kCoarseTune, 0) +
                                           gen(seqlib::sf2::kFineTune, 0) + static_cast<int8_t>(header[41])});
    }

    return zones;
}

// The regions of the SFZ files that play the WAV file `sample`, in the order of the files: their lokey, hikey,
// pitch_keycenter, transpose and tune.
std::vector<SampleZone> SfzZones(const seqlib::sfz::Instruments& sfz, const std::string& sample)
{
    std::vector<SampleZone> zones;
    for (const seqlib::sfz::File& file : sfz.files)
    {
        std::size_t start = 0;
        while (start < file.text.size())
        {
            const std::size_t end = std::min(file.text.find('\n', start), file.text.size());
            const std::string line = file.text.substr(start, end - start);
            start = end + 1;
            if (!line.starts_with("<region>"))
            {
                continue;
            }

            std::map<std::string, std::string> opcodes;
            std::size_t at = line.find(' ');
            while (at != std::string::npos)
            {
                const std::size_t next = line.find(' ', at + 1);
                const std::string token = line.substr(at + 1, next == std::string::npos ? next : next - at - 1);
                const std::size_t equals = token.find('=');
                if (equals != std::string::npos)
                {
                    opcodes[token.substr(0, equals)] = token.substr(equals + 1);
                }
                at = next;
            }
            const std::string& name = opcodes["sample"];
            if (name != sample && !name.ends_with("/" + sample))
            {
                continue;
            }

            const auto value = [&](const std::string& opcode, int otherwise)
            {
                const auto found = opcodes.find(opcode);
                return found != opcodes.end() ? std::stoi(found->second) : otherwise;
            };
            zones.push_back({.low_key = value("lokey", 0),
                             .high_key = value("hikey", 127),
                             .key_zero_cents = -100.0 * value("pitch_keycenter", 60) + 100.0 * value("transpose", 0) +
                                               value("tune", 0)});
        }
    }

    return zones;
}

// The first of `zones` that holds `key`, or nothing.
std::optional<std::size_t> ZoneAt(const std::vector<SampleZone>& zones, int key)
{
    for (std::size_t z = 0; z < zones.size(); z++)
    {
        if (zones[z].low_key <= key && key <= zones[z].high_key)
        {
            return z;
        }
    }

    return std::nullopt;
}

// Whether `zones` play wave 0's keys 72, 96 and 120 at the game's timers, within half a cent.
bool PlaysAtTimers(const std::vector<SampleZone>& zones)
{
    for (const auto& [key, timer] : kWaveZeroTimers)
    {
        const std::optional<std::size_t> z = ZoneAt(zones, key);
        if (!z || std::abs(zones[*z].key_zero_cents + 100.0 * key - TimerCents(timer, 2, kWaveZeroRate)) > 0.5)
        {
            return false;
        }
    }

    return true;
}

// Each key takes the tuning at which the driver's timer plays it, within half a cent (R-CDD). The timers depart from
// equal semitones in the top octaves, pitches from 117 on take the first octave's, and the PSG's lowest keys pass the
// 16 bits of the channel's timer.
void TestKeyTuning()
{
    constexpr uint32_t kRate = 22055;
    bool alone = true;
    bool runs = true;
    for (const PlayedKey& played : kPlayedKeys)
    {
        const PresetZone zone{.wave = 0, .format = played.format, .tuning = played.tuning};
        const double game = TimerCents(played.timer, played.format, kRate);
        alone = alone && std::abs(ZoneCents(TuningOf(zone, kRate, played.key), played.key) - game) <= 0.5;
        for (const TunedZone& run : TunedZones(zone, kRate))
        {
            if (run.zone.low_key <= played.key && played.key <= run.zone.high_key)
            {
                runs = runs && std::abs(ZoneCents(run.tuning, played.key) - game) <= 0.5;
            }
        }
    }
    Check(alone, "a zone tuned at a key plays it at the game's timer");
    Check(runs, "every key's run plays it at the game's timer");

    // Only the played keys take their tuning. Key 101 joins key 100's run, and a zone without a played key keeps the
    // root key's tuning.
    const PresetZone zone{.format = 2};
    const std::set<uint8_t> keys = {100, 108};
    const std::vector<TunedZone> tuned = TunedZones(zone, kRate, &keys);
    const auto run_of = [&](int key)
    {
        return std::ranges::find_if(
            tuned, [key](const TunedZone& t) { return t.zone.low_key <= key && key <= t.zone.high_key; });
    };
    Check(tuned.size() == 2 && run_of(101) == run_of(100) && run_of(0) == run_of(100) && run_of(127) == run_of(108) &&
              run_of(100) != tuned.end() && run_of(108) != tuned.end() &&
              std::abs(ZoneCents(run_of(108)->tuning, 108) - TimerCents(32, 2, kRate)) <= 0.5 &&
              std::abs(ZoneCents(run_of(100)->tuning, 100) - TimerCents(50, 2, kRate)) <= 0.5,
          "only the played keys take their tuning");
    const std::set<uint8_t> none = {};
    const std::vector<TunedZone> untuned = TunedZones(zone, kRate, &none);
    Check(untuned.size() == 1 && untuned[0].tuning == TuningOf(zone, kRate), "a zone without a played key");

    // Program 0 of the test's bank plays wave 0 (32,000 Hz) with the tuning of -4814. Keys 72, 96 and 120 play at the
    // timers that the game's code gave them.
    const TestFiles test;
    const Bytes track = files::Join({{1, 0, 0, 0},
                                     {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40, 0xA0, 0x06},
                                     files::Note(0x60, 0, 0, 0x18, 1),
                                     {0x92, 0x18, 0xA0, 0x08},
                                     files::Note(0x60, 0, 0),
                                     {0x92, 0x18, 0xA0, 0x0A},
                                     files::Note(0x60, 0, 0),
                                     {0x92, 0x30, 0x98}});
    const Conversion c = ConvertEffect(test, track);
    const std::vector<SampleZone> sf2_zones = SoundFontZones(c.sf2, "w32512_0");

    CheckFiles(c, "the tuned keys");
    Check(PlaysAtTimers(sf2_zones), "the SoundFont plays each played key at the game's timer");
    Check(PlaysAtTimers(SfzZones(c.sfz, "w32512_0.wav")), "the SFZ files play each played key at the game's timer");

    // In the SoundFont, a key that no note plays joins the run of the played key below it, and gets no zone.
    Check(!sf2_zones.empty() && ZoneAt(sf2_zones, 0) && ZoneAt(sf2_zones, 127) &&
              ZoneAt(sf2_zones, 73) == ZoneAt(sf2_zones, 72) && ZoneAt(sf2_zones, 97) == ZoneAt(sf2_zones, 96) &&
              ZoneAt(sf2_zones, 121) == ZoneAt(sf2_zones, 120),
          "the SoundFont tunes only the played keys");
}

// The tracks of the timer bend's anchor, on program 0 of the test's bank (wave 0, a tuning of -4814/256), one after
// another: track 1 holds key 120 under steps of the key bend (0xD7) at a range of 12, track 2 holds key 96 under a
// tuning slide (0xD4) of 24 semitones in 48 ticks, track 3 strikes keys 108 and 113 together under a tuning of 128/256
// (0xD3), and track 4 plays key 24 under a tuning of -12 semitones (0xD0): its pitch wraps below 0.
std::vector<Bytes> AnchorTracks()
{
    const Bytes start = {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE3, 0x7F, 0xE8, 0x40};
    return {
        files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x93, 0x4C, 0x04, 0x98}}),
        files::Join({{1, 1, 0, 0},
                     start,
                     {0xDB, 0x0C, 0xA0, 0x0A},
                     files::Note(0x7F, 0, 0, 230, 2),
                     {0x92, 0x18, 0xD7, 0x10, 0x00, 0x92, 0x18, 0xD7, 0x08, 0x00, 0x92, 0x18, 0xD7, 0xF8, 0x00},
                     {0x92, 0x18, 0xD7, 0xEA, 0xAB, 0x92, 0x18, 0xD7, 0xE0, 0x00, 0x92, 0x18, 0xD7, 0x1F, 0xFF},
                     {0x92, 0x18, 0xD7, 0x00, 0x00, 0x92, 0x18, 0x92, 0x28, 0x98}}),
        files::Join({{2, 2, 0, 0},
                     start,
                     {0x93, 0x2C, 0x01, 0xA0, 0x08},
                     files::Note(0x7F, 0, 0, 130, 1),
                     {0x92, 0x0C, 0xD4, 0x30, 0x00, 0x18, 0x92, 0x82, 0x98}}),
        files::Join({{3, 3, 0, 0},
                     start,
                     {0x93, 0x58, 0x02, 0xD3, 0x80, 0x00, 0xA0, 0x09},
                     files::Note(0x7F, 0, 0, 60, 1),
                     files::Note(0x7F, 5, 0),
                     {0x92, 0x5A, 0x98}}),
        files::Join({{4, 4, 0, 0},
                     start,
                     {0x93, 0x84, 0x03, 0xD0, 0xF4, 0xA0, 0x02},
                     files::Note(0x7F, 0, 0, 48, 1),
                     {0x92, 0x5A, 0x98}}),
    };
}

// The timers that Explorers of Sky's ARM9 and ARM7 code (North America) gave the channels of the anchor's notes, in
// order. The run on ntdlib's DS machine played the tracks of AnchorTracks in a song in place of bgm0001's, on wave 73
// of the game's main bank (IMA-ADPCM), with the same tuning. The timer depends on the pitch and on whether the wave is
// the PSG.
constexpr int32_t kStepTimers[] = {47, 33, 39, 56, 75, 94, 23, 47};
constexpr int32_t kSlideTimers[] = {189, 184, 178, 173, 168, 164, 159, 154, 150, 146, 142, 138, 134, 130, 126, 122, 119,
                                    116, 112, 109, 106, 103, 100, 97,  94,  92,  89,  86,  84,  82,  79,  77,  75,  73,
                                    71,  69,  67,  65,  63,  61,  59,  58,  56,  54,  53,  51,  50,  48,  47};
constexpr int32_t kChordTimers[] = {92, 69}; // keys 108 and 113
constexpr int32_t kWrapTimers[] = {15303};

// The pitches in semitones that part `part` of track `track` sends, in the order of the MIDI file.
std::vector<float> PartPitches(const Performance& p, std::size_t track, uint8_t part)
{
    std::vector<TrackEvent> events;
    for (const TrackEvent& e : EventsOf(p, track, EventKind::kPitch))
    {
        if (e.part == part)
        {
            events.push_back(e);
        }
    }
    std::ranges::stable_sort(events, {}, [](const TrackEvent& e) { return std::pair(e.tick, e.after); });

    std::vector<float> pitches;
    for (const TrackEvent& e : events)
    {
        pitches.push_back(e.semitones);
    }

    return pitches;
}

// The cents above wave 0's rate at which program 0's zone plays `key` at `pitch` semitones.
double PlayedCents(int key, float pitch)
{
    const PresetZone zone{.format = 1, .tuning = -4814};
    return ZoneCents(TuningOf(zone, kWaveZeroRate, key), key) + 100.0 * pitch;
}

// Whether `pitches` of a part that plays `key` follow `timers`: each pitch plays one of the timers, within `tolerance`
// cents, in their order, and the last pitch plays the last timer. A MIDI tick keeps the last of the driver's ticks in
// it. A pitch can therefore pass over a timer.
bool FollowsTimers(const std::vector<float>& pitches, int key, std::span<const int32_t> timers, double tolerance)
{
    std::size_t at = 0;
    for (const float pitch : pitches)
    {
        while (at < timers.size() &&
               std::abs(PlayedCents(key, pitch) - TimerCents(timers[at], 1, kWaveZeroRate)) > tolerance)
        {
            at++;
        }
        if (at == timers.size())
        {
            return false;
        }
    }

    return !pitches.empty() && at + 1 == timers.size();
}

// A song of one track on program 0 of the test's bank: the pitches that part 0 of the track sends while its note
// sounds, the pitch that the part sends once the note has ended, and the timers that the driver's model gives the
// track's channels, in order: each channel's start and each change of its timer.
struct LoneNote
{
    std::vector<float> pitches;
    std::optional<float> rest;
    std::vector<int32_t> timers;
};

// Plays a track of `events` after the track's start, through the performer and through the driver's model alone.
LoneNote PlayLoneNote(const Bytes& events)
{
    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
        {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
         files::Join({{1, 1, 0, 0}, {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40}, events})}));
    SequenceFiles lone_files = test.Song();
    lone_files.song = &song;
    LoneNote lone{.pitches = PartPitches(Perform(lone_files), 1, 0)};
    if (!lone.pitches.empty())
    {
        lone.rest = lone.pitches.back();
        lone.pitches.pop_back();
    }

    // Every hardware channel plays. No voice therefore ends before its note.
    ntdlib::dse::Driver driver;
    if (const std::optional<uint16_t> id = driver.AddBank(test.bank, 0))
    {
        driver.PlaceWaves(*id, test.main_bank, 0);
    }
    driver.PlaySong(song);
    for (int tick = 0; tick < 1000 && driver.Playing(); tick++)
    {
        for (const ntdlib::ntr::snd::Command& command : driver.Tick(0xFFFF))
        {
            if (command.id == ntdlib::ntr::snd::CommandId::kSetUpPcmChannel)
            {
                lone.timers.push_back(static_cast<int32_t>(command.args[0] >> 16));
            }
            else if (command.id == ntdlib::ntr::snd::CommandId::kChannelTimer)
            {
                lone.timers.push_back(static_cast<int32_t>(command.args[1]));
            }
        }
    }

    return lone;
}

// A part that sounds one note sends the bend at which the voice's timer plays the note through its zone. The
// timer's bend against the zone's whole cents gives the game's timer within a hundredth of a cent. A note without a
// bend keeps a pitch of 0, where the zone gives the timer within half a cent. A note that the channel's bend plays more
// than 5 cents from its timer takes a part to itself, and the next note goes to another part.
void TestTimerBend()
{
    // TimerBend at the anchor's pitches: key 120 under a key bend of +12 semitones, key 96 at the slide's end, and key
    // 24 below pitch 0.
    const PresetZone zone{.format = 1, .tuning = -4814};
    const auto timer_bend = [&](int key, int bend)
    {
        const auto pitch = static_cast<uint16_t>(zone.tuning + key * 256 + bend);
        return TimerBend(zone, kWaveZeroRate, key, pitch).value_or(0.0);
    };
    Check(std::abs(PlayedCents(120, static_cast<float>(timer_bend(120, 3071))) -
                   TimerCents(kStepTimers[std::size(kStepTimers) - 2], 1, kWaveZeroRate)) < 0.01 &&
              std::abs(PlayedCents(96, static_cast<float>(timer_bend(96, 6144))) -
                       TimerCents(kSlideTimers[std::size(kSlideTimers) - 1], 1, kWaveZeroRate)) < 0.01 &&
              std::abs(PlayedCents(24, static_cast<float>(timer_bend(24, -3072))) -
                       TimerCents(kWrapTimers[std::size(kWrapTimers) - 1], 1, kWaveZeroRate)) < 0.01,
          "the timer's bend plays the game's timer through the zone's whole cents");

    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(AnchorTracks()));
    SequenceFiles anchor = test.Song();
    anchor.song = &song;
    const Performance p = Perform(anchor);

    const std::vector<float> steps = PartPitches(p, 1, 0);
    Check(FollowsTimers(steps, 120, kStepTimers, 0.5),
          "a note under steps of the key bend plays each at the game's timer");
    Check(!steps.empty() && std::abs(steps.front() - 6.0f) < 0.5f, "the note's start without a bend sends no bend");
    Check(FollowsTimers(PartPitches(p, 2, 0), 96, kSlideTimers, 0.5),
          "a note under a tuning slide follows the game's timer tick by tick");
    Check(FollowsTimers(PartPitches(p, 4, 0), 24, kWrapTimers, 0.5),
          "a note whose pitch wraps below 0 plays the game's timer, octaves from the bend");

    Check(std::abs(PlayedCents(108, 0.5f) - TimerCents(kChordTimers[0], 1, kWaveZeroRate)) > 5.0 &&
              std::abs(PlayedCents(113, 0.5f) - TimerCents(kChordTimers[1], 1, kWaveZeroRate)) < 1.0,
          "the channel's bend plays key 108 over 5 cents from its timer, and key 113 within a cent");
    Check(FollowsTimers(PartPitches(p, 3, 0), 108, std::span(kChordTimers).first(1), 0.5) &&
              FollowsTimers(PartPitches(p, 3, 1), 113, std::span(kChordTimers).last(1), 0.5),
          "a note over 5 cents from its timer at the channel's bend takes a part to itself");

    // Key 96 under a random pitch of up to a quarter of a semitone (0xD6). The timer contains the note's random pitch.
    // Once each note has ended, its part sends the channel's bend.
    const LoneNote random =
        PlayLoneNote(files::Join({{0xD6, 0x40, 0x00, 0xA0, 0x08}, files::Note(0x7F, 0, 0, 48, 1), {0x92, 0x60, 0x98}}));
    const int32_t plain_timer = ntdlib::dse::VoiceTimer(static_cast<uint16_t>(zone.tuning + 96 * 256), 1);
    Check(random.timers.size() == 1 && random.timers[0] != plain_timer &&
              FollowsTimers(random.pitches, 96, random.timers, 0.5) && random.rest == 0.0f,
          "a lone note with a random pitch plays the game's timer");

    // Key 96 under a tuning of 128/256 (0xD3). A legato (0xC0) moves it to key 100. The MIDI note keeps key 96, and its
    // part plays key 100's timer.
    const LoneNote legato = PlayLoneNote(files::Join({{0xD3, 0x80, 0x00, 0xA0, 0x08},
                                                      files::Note(0x7F, 0, 0, 96, 1),
                                                      {0x92, 0x18, 0xC0},
                                                      files::Note(0x7F, 4, 0, 24, 1),
                                                      {0x92, 0x60, 0x98}}));
    Check(legato.timers.size() == 2 && FollowsTimers(legato.pitches, 96, legato.timers, 0.5) &&
              std::abs(legato.pitches.back() - 4.5f) < 0.1f && legato.rest == 0.5f,
          "a lone bent note that a legato moves plays the new key's timer");
}

// The anchor's chord in the other order: key 113 starts first, alone on its part. The channel's bend plays key 108 over
// 5 cents from its timer. Key 108 leaves that part for one of its own. Each part plays its note at the timer.
void TestOwnPartForALaterNote()
{
    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
        {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
         files::Join({{1, 1, 0, 0},
                      {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40, 0xD3, 0x80, 0x00, 0xA0, 0x09},
                      files::Note(0x7F, 5, 0, 60, 1),
                      files::Note(0x7F, 0, 0),
                      {0x92, 0x5A, 0x98}})}));
    SequenceFiles files = test.Song();
    files.song = &song;
    const Performance p = Perform(files);

    Check(FollowsTimers(PartPitches(p, 1, 0), 113, std::span(kChordTimers).last(1), 0.5) &&
              FollowsTimers(PartPitches(p, 1, 1), 108, std::span(kChordTimers).first(1), 0.5),
          "a later note over 5 cents from its timer leaves a sounding part");
}

// The voices of a note that play it at pitches more than half a cent apart send the channel's bend. Program 1 plays key
// 63 on two splits: wave 1 with a tuning of 100/256, and wave 2 with -556/256. A bend of a semitone plays them 5 cents
// apart, and a bend of 12 semitones within half a cent.
void TestTimerBendOfLayers()
{
    const TestFiles test;
    const Bytes start = {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x01, 0xE0, 0x7F, 0xE8, 0x40, 0xA0, 0x05};
    std::vector<float> pitches;
    for (const uint8_t semitones : {uint8_t{1}, uint8_t{12}})
    {
        const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
            {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
             files::Join(
                 {{1, 1, 0, 0}, start, {0xD0, semitones}, files::Note(0x7F, 3, 0, 48, 1), {0x92, 0x60, 0x98}})}));
        SequenceFiles files = test.Song();
        files.song = &song;
        const std::vector<float> sent = PartPitches(Perform(files), 1, 0);
        pitches.push_back(sent.empty() ? 0.0f : sent.front());
    }

    Check(pitches[0] == 1.0f, "voices 5 cents apart: the channel's bend");
    Check(pitches[1] != 12.0f && std::abs(pitches[1] - 12.0f) < 0.01f, "voices within half a cent: the timer's bend");
}

// A loop whose first pass plays a note on a part that holds another, and whose later passes play it alone. Under a
// tuning of 128/256, key 113 holds from tick 0 to 96, and the loop from tick 24 plays key 102 for 24 ticks and waits
// 300. Key 113 alone plays at its timer's bend, below the channel's. The first pass plays key 102 at the channel's
// bend, and the later passes at the timer's, 2.1 cents higher. The
// pass test takes two pitches as the same where both stand for the same bend and are within the timer's departure
// from it: the loop starts at the first pass.
void TestLoopOverATimerBend()
{
    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
        {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
         files::Join({{1, 1, 0, 0},
                      {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40, 0xD3, 0x80, 0x00, 0xA0, 0x09},
                      files::Note(0x7F, 5, 0, 96, 1),
                      {0x92, 0x18, 0x99},
                      files::Note(0x7F, 6, -1, 24, 1),
                      {0xA0, 0x09, 0x93, 0x2C, 0x01, 0x98}})}));
    SequenceFiles files = test.Song();
    files.song = &song;
    const Performance p = Perform(files);

    const std::vector<float> pitches = PartPitches(p, 1, 0);
    Check(pitches.size() == 4 && pitches[0] != 0.5f && pitches[1] == 0.5f && pitches[3] > 0.5f && pitches[3] < 0.53f,
          "the first pass's note at the channel's bend, and the second's at the timer's");
    Check(p.loop && p.loop->start == 24 && p.loop->end == 324, "the loop starts at the first pass");
}

// A loop whose first pass plays a note shorter than the later passes do. Track 1: key 0 for `first` ticks and a wait of
// 48, then the loop: key 0 without a length (the track's previous length), a wait of 48, key 4 for 24 ticks and a
// wait of 48. The first pass plays key 0 for `first` ticks, and the later passes for 24. Every pass strikes the same
// notes. A note 12 ticks shorter in the first pass moves the loop to the second pass, whose next passes repeat it with
// their lengths. A note 4 ticks shorter keeps it on the first: 6 ticks at 48 a quarter note are within the tolerance.
void TestLoopOnASettledPass()
{
    const TestFiles test;
    const auto loop = [&test](uint32_t first)
    {
        const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(
            files::SongFile({files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
                             files::Join({{1, 1, 0, 0},
                                          {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40, 0xA0, 0x04},
                                          files::Note(0x7F, 0, 0, first, 1),
                                          {0x92, 0x30, 0x99},
                                          files::Note(0x7F, 0, 0),
                                          {0x92, 0x30},
                                          files::Note(0x7F, 4, 0, 24, 1),
                                          {0x92, 0x30, 0x98}})}));
        SequenceFiles files = test.Song();
        files.song = &song;
        return Perform(files).loop;
    };

    const auto shorter = loop(12);
    const auto close = loop(20);

    Check(shorter && shorter->start == 144 && shorter->end == 240, "a first pass 12 ticks shorter: the second pass");
    Check(close && close->start == 48 && close->end == 144, "a first pass 4 ticks shorter: the first pass");
}

// Tracks that loop at other lengths. Track 1 loops on key 0 for 24 ticks and a wait of `owner` ticks, and track 2 on
// key 4 for 12 ticks and a wait of `other`. Track 1 has the longer loop and owns the main loop. Loops of 48 and 40
// ticks line up every 240 ticks, 5 of track 1's passes: the loop is that block. Loops of 4,800 and 4,700 ticks line up
// only every 47 passes, about 39 minutes, past the file's limit: the loop stays on the first pass, and the performance
// lists the period.
void TestWholePeriodOfTheTracks()
{
    const TestFiles test;
    const auto perform = [&test](uint16_t owner, uint16_t other)
    {
        const Bytes kStart = {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40, 0xA0, 0x04, 0x99};
        const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(
            {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
             files::Join({{1, 1, 0, 0},
                          kStart,
                          files::Note(0x7F, 0, 0, 24, 1),
                          {0x93, static_cast<uint8_t>(owner), static_cast<uint8_t>(owner >> 8), 0x98}}),
             files::Join({{2, 2, 0, 0},
                          kStart,
                          files::Note(0x7F, 4, 0, 12, 1),
                          {0x93, static_cast<uint8_t>(other), static_cast<uint8_t>(other >> 8), 0x98}})}));
        SequenceFiles files = test.Song();
        files.song = &song;
        return Perform(files);
    };

    const Performance lined_up = perform(48, 40);
    const Performance apart = perform(4800, 4700);

    const auto listed = apart.approximations.find("tracks loop at other lengths (their loops line up every 47 passes)");
    Check(lined_up.loop && lined_up.loop->start == 0 && lined_up.loop->end == 240 && lined_up.approximations.empty(),
          "loops that line up every 5 passes: the 5 passes");
    Check(apart.loop && apart.loop->start == 0 && apart.loop->end == 4800 && listed != apart.approximations.end() &&
              listed->second > 0,
          "loops that line up past the file's limit: the first pass, and the approximation");
}

// A run on fewer parts tests the first run's loop again on its parts. Tracks 2 to 16 each play one note, and track 1
// requires a second part: 16 MIDI tracks and 17 parts, one more than MIDI's channels. The run on 16 parts gives track 1
// one part.
//
// Track 1, under a tuning of 128/256: 0: a wait of 12; 12: key 113 for 24 ticks; 24: the loop: a wait of 6, key 108 for
// 6 ticks, a wait of 12, program 1, key 113 for 6 ticks, a wait of 18, program 0, a wait of 18 and the loop's end. Each
// pass takes 54 ticks. In the first pass, key 108 starts while key 113 sounds, and the channel's bend plays it over 5
// cents from its timer. On two parts it takes the second part. That part takes program 0 there. On one part it keeps
// program 0 without an event. The loop's end leaves program 1 on that part. The second pass sends program 0 with key
// 108, after the first tick of the pass. A loop's start sends the settings of that tick again. On one part, a repeat of
// the first pass plays key 108 with program 1. The loop starts at the second pass.
void TestLoopCheckedOnFewerParts()
{
    const Bytes kStart = {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xE8, 0x40};
    std::vector<Bytes> tracks = {files::Join({{0, 0, 0, 0, 0xA4, 0x78, 0x98}}),
                                 files::Join({{1, 1, 0, 0},
                                              kStart,
                                              {0xD3, 0x80, 0x00, 0xA0, 0x09, 0x92, 0x0C},
                                              files::Note(0x7F, 5, 0, 24, 1),
                                              {0x92, 0x0C, 0x99, 0x92, 0x06},
                                              files::Note(0x7F, 0, 0, 6, 1),
                                              {0x92, 0x0C, 0xAC, 0x01},
                                              files::Note(0x7F, 5, 0, 6, 1),
                                              {0x92, 0x12, 0xAC, 0x00, 0x92, 0x12, 0x98}})};
    for (uint8_t track = 2; track <= 16; track++)
    {
        const auto channel = static_cast<uint8_t>(track % 16);
        tracks.push_back(
            files::Join({{track, channel, 0, 0}, kStart, files::Note(0x7F, 0, 0, 4, 1), {0x92, 0x04, 0x98}}));
    }
    const TestFiles test;
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(files::SongFile(tracks));
    SequenceFiles fewer = test.Song();
    fewer.song = &song;

    const Performance p = Perform(fewer);

    Check(p.approximations.contains("note off its timer's pitch on a shared part (no MIDI channel free)"),
          "the run on fewer parts plays key 108 on track 1's one part");
    Check(p.loop && p.loop->start == 78 && p.loop->end == 132,
          "a pass that repeats only on the first run's parts: the loop starts at the second pass");
}

// The conversion of the instruments of a bank. Program 0 plays wave 0 of the main bank, and program 1 waves 1 and 2.
// Program 2 plays the PSG and the noise with three LFOs: a pitch LFO, a volume LFO of type 3 that a level of 0 leaves
// out, and a pan LFO. Program 3 plays wave 5. The main bank has no wave 5.
void TestConvertBank()
{
    const TestFiles test;
    const std::vector<uint32_t> every = {0, 1, 2, 3, 9, 200};
    const ntdlib::sequence_export::BankConversion c =
        ConvertBank(test.bank, &test.main_bank, every, kTenMs, "B_TEST", kProgram);
    Check(c.sound_fonts.size() == 1 && c.sound_fonts[0].first == 0 && c.sound_fonts[0].last == 3,
          "a bank: one SoundFont of programs 0 to 3");
    const std::vector<uint8_t>& sf2 = c.sound_fonts.empty() ? std::vector<uint8_t>() : c.sound_fonts[0].sf2;
    Check(seqlib::sf2::Check(sf2).empty(), "a bank: seqlib's check of the SoundFont");
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::vector<std::pair<uint16_t, uint16_t>> presets;
    for (std::size_t p = 0; p + 1 < records.Count("phdr", 38); p++)
    {
        const uint8_t* r = records.Record("phdr", 38, p);
        presets.emplace_back(xsflib::LoadLe16(r + 22), xsflib::LoadLe16(r + 20));
    }
    Check(presets == std::vector<std::pair<uint16_t, uint16_t>>{{0, 0}, {0, 1}, {0, 2}, {0, 3}},
          "a bank: a preset at each program that plays");

    // Programs 0 and 1 take waves 0, 1 and 2. Program 2 takes the PSG at wave 3 and the noise at wave 4.
    Check(c.sound_fonts.size() == 1 && c.sound_fonts[0].samples == 5 && c.sfz.samples.size() == 5,
          "a bank: a sample for each wave, and a WAV file for each");
    std::vector<std::string> names = {"conductor"};
    for (const seqlib::sfz::File& file : c.sfz.files)
    {
        names.push_back(file.name.substr(0, file.name.size() - 4));
    }
    Check(names == std::vector<std::string>{"conductor", "B_TEST_0", "B_TEST_1", "B_TEST_2", "B_TEST_3"},
          "a bank: an SFZ file of each program that plays");
    Check(ntdlib::testing::SfzProblem(c.sfz, names).empty(), "a bank: the SFZ files' samples and loops");
    const std::string& first = c.sfz.files.size() == 4 ? c.sfz.files[0].text : std::string();
    Check(first.find("// Program 0 of B_TEST, converted by xyz2midi.") == 0 &&
              first.find("bend_up=200 bend_down=-200") != std::string::npos &&
              first.find("<region> sample=w32512_0.wav lokey=0 ") != std::string::npos,
          "a bank: program 0's file plays its wave");
    Check(PlaysAtTimers(SoundFontZones(sf2, "w32512_0")) && PlaysAtTimers(SfzZones(c.sfz, "w32512_0.wav")),
          "a bank: the SoundFont and the SFZ file play program 0's keys at the game's timers");
    const std::string& third = c.sfz.files.size() == 4 ? c.sfz.files[2].text : std::string();
    Check(third.find(" lfo1_pitch=") != std::string::npos && third.find(" lfo2_pan=") != std::string::npos &&
              third.find("_volume=") == std::string::npos,
          "a bank: the program's LFOs");
    Check(c.warnings == std::vector<std::string>{"program 9 of bank 32512 plays nothing",
                                                 "program 200 of bank 32512 plays nothing",
                                                 "w32512_5: no bank holds the wave's samples"},
          "a bank: the programs that play nothing, and the wave that no bank holds");

    // One program by itself, and programs given out of order and twice.
    const ntdlib::sequence_export::BankConversion one =
        ConvertBank(test.bank, &test.main_bank, std::vector<uint32_t>{0}, kTenMs, "B_TEST", kProgram);
    Check(one.sound_fonts.size() == 1 && one.sound_fonts[0].first == 0 && one.sfz.files.size() == 1 &&
              one.sfz.files[0].name == "B_TEST_0.sfz" && one.sfz.samples.size() == 1,
          "a bank: one program by itself");
    const ntdlib::sequence_export::BankConversion unsorted =
        ConvertBank(test.bank, &test.main_bank, std::vector<uint32_t>{3, 0, 3}, kTenMs, "B_TEST", kProgram);
    Check(unsorted.sound_fonts.size() == 1 && unsorted.sound_fonts[0].first == 0 && unsorted.sound_fonts[0].last == 3 &&
              unsorted.sfz.files.size() == 2 && unsorted.sfz.files[0].name == "B_TEST_0.sfz" &&
              unsorted.sfz.files[1].name == "B_TEST_3.sfz",
          "a bank: programs out of order convert in ascending order, each once");

    // A bank without its main bank plays nothing of its waves.
    const ntdlib::sequence_export::BankConversion without =
        ConvertBank(test.bank, nullptr, std::vector<uint32_t>{0}, kTenMs, "B_TEST", kProgram);
    Check(without.sfz.samples.empty() && !without.warnings.empty(), "a bank without its main bank has no samples");

    Check(
        ntdlib::dse::ProgramLfos(test.bank, 2)[0].type == 1 && ntdlib::dse::ProgramLfos(test.bank, 2)[0].output == 1 &&
            ntdlib::dse::ProgramLfos(test.bank, 2)[0].phase_ms == 40 &&
            ntdlib::dse::ProgramLfos(test.bank, 2)[1].type == 3 && ntdlib::dse::ProgramLfos(test.bank, 2)[3].type == 0,
        "a program's LFOs");
    Check(ntdlib::dse::ProgramLfos(test.bank, 0)[0].type == 0 && ntdlib::dse::ProgramLfos(test.bank, 9)[0].type == 0,
          "a program without LFOs, and a program that the bank lacks");
}

// The frequency of the first LFO in an SFZ file, or 0.
double FirstLfoHz(const std::string& sfz)
{
    const std::size_t at = sfz.find("lfo1_freq=");
    return at == std::string::npos ? 0.0 : std::stod(sfz.substr(at + 10));
}

// A bank in the oldest build. Program 1's first split has a multiplier of 60. The oldest build plays it as 1. Program
// 2's third LFO is of type 2, and the oldest build ignores that type. The first LFO of program 2, of waveform 3, runs
// at half the rate.
void TestConvertBankOldest()
{
    const TestFiles test;
    const auto with_multiplier = [&test](uint8_t multiplier)
    {
        ntdlib::dse::WaveBank bank = test.bank;
        bank.instruments->programs[1]->splits[0][ntdlib::dse::SplitView::kEnvelopeMultiplier] = multiplier;
        bank.instruments->programs[2]->lfos[2][1] = 2;
        return bank;
    };
    const ntdlib::dse::WaveBank steep = with_multiplier(60);
    const ntdlib::dse::WaveBank one = with_multiplier(1);
    const std::vector<uint32_t> programs = {1, 2};

    const ntdlib::sequence_export::BankConversion oldest =
        ConvertBank(steep, &test.main_bank, programs, kTenMs, "B_TEST", kProgram, ntdlib::dse::Build::kOldest);
    const ntdlib::sequence_export::BankConversion later =
        ConvertBank(steep, &test.main_bank, programs, kTenMs, "B_TEST", kProgram);
    const ntdlib::sequence_export::BankConversion later_one =
        ConvertBank(one, &test.main_bank, std::vector<uint32_t>{1}, kTenMs, "B_TEST", kProgram);
    const ntdlib::sequence_export::BankConversion oldest_one = ConvertBank(
        steep, &test.main_bank, std::vector<uint32_t>{1}, kTenMs, "B_TEST", kProgram, ntdlib::dse::Build::kOldest);

    Check(oldest.sfz.files.size() == 2 && later.sfz.files.size() == 2 && oldest_one.sfz.files.size() == 1 &&
              later_one.sfz.files.size() == 1,
          "the oldest build's bank: a file of each program");
    if (oldest.sfz.files.size() != 2 || later.sfz.files.size() != 2 || oldest_one.sfz.files.size() != 1 ||
        later_one.sfz.files.size() != 1)
    {
        return;
    }

    Check(oldest_one.sfz.files[0].text == later_one.sfz.files[0].text &&
              oldest_one.sound_fonts.at(0).sf2 == later_one.sound_fonts.at(0).sf2 &&
              oldest.sfz.files[0].text != later.sfz.files[0].text,
          "the oldest build's bank: a multiplier of 60 plays as 1 in the SFZ file and the SoundFont");
    const double later_hz = FirstLfoHz(later.sfz.files[1].text);
    Check(later_hz > 0.0 && FirstLfoHz(oldest.sfz.files[1].text) == later_hz / 2,
          "the oldest build's bank: waveform 3 at half the rate");
    Check(ntdlib::dse::ProgramLfos(steep, 2)[2].type == 2 &&
              ntdlib::dse::ProgramLfos(steep, 2, ntdlib::dse::Build::kOldest)[2].type == 0 &&
              ntdlib::dse::ProgramLfos(steep, 2, ntdlib::dse::Build::kOldest)[0].type == 1,
          "the oldest build takes no LFO of type 2");
}

} // namespace

int main()
{
    TestKeyGroups();
    TestKeyTuning();
    TestTimerBend();
    TestOwnPartForALaterNote();
    TestTimerBendOfLayers();
    TestLoopOverATimerBend();
    TestLoopOnASettledPass();
    TestWholePeriodOfTheTracks();
    TestLoopCheckedOnFewerParts();
    TestConvertBank();
    TestConvertBankOldest();

    return xsflib::testing::Summary("DSE conversion: tuning, loops and banks");
}
